#include "redis_match_queue_store.hpp"

#include <hiredis/hiredis.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace rgbt::match {
namespace {

/// 队列快照在 Redis 中的键名后缀。前缀由 env_prefix 提供
/// （见 docs/05-api-and-data.md：Key 必须带环境和服务前缀）。
constexpr const char* kQueueKeySuffix = ":match:queue";

}  // namespace

RedisMatchQueueStore::RedisMatchQueueStore(std::string env_prefix, MatchRedisOptions options)
    : env_prefix_(std::move(env_prefix)), options_(std::move(options)) {}

RedisMatchQueueStore::~RedisMatchQueueStore() {
    const std::lock_guard<std::mutex> lock(mutex_);
    DiscardConnection();
}

std::string RedisMatchQueueStore::queue_key() const {
    return env_prefix_ + kQueueKeySuffix;
}

void RedisMatchQueueStore::DiscardConnection() {
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
}

bool RedisMatchQueueStore::EnsureConnected() {
    if (context_ != nullptr && context_->err == 0) {
        return true;
    }
    DiscardConnection();

    timeval connect_timeout{};
    connect_timeout.tv_sec = options_.connect_timeout_ms / 1000;
    connect_timeout.tv_usec = (options_.connect_timeout_ms % 1000) * 1000;

    context_ = redisConnectWithTimeout(options_.host.c_str(), options_.port, connect_timeout);
    if (context_ == nullptr || context_->err != 0) {
        DiscardConnection();
        return false;
    }

    timeval command_timeout{};
    command_timeout.tv_sec = options_.command_timeout_ms / 1000;
    command_timeout.tv_usec = (options_.command_timeout_ms % 1000) * 1000;
    redisSetTimeout(context_, command_timeout);
    return true;
}

bool RedisMatchQueueStore::Save(const MatchQueueSnapshot& snapshot) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureConnected()) {
        return false;
    }

    // 先编码。编码失败的单条只跳过并记日志——**不能让一条坏记录毁掉整份快照**，
    // 那会让队列在重启后整体丢失，损失远大于少一条。
    std::vector<std::string> lines;
    lines.reserve(snapshot.queued.size() + snapshot.matched.size());
    for (const MatchQueueEntry& entry : snapshot.queued) {
        std::string line;
        if (EncodeMatchQueueEntry(entry, &line)) {
            lines.push_back(std::move(line));
        } else {
            std::fprintf(stderr, "[match] 队列快照条目编码失败，已跳过：player_id=%s\n",
                         entry.player_id.c_str());
        }
    }
    for (const MatchQueueEntry& entry : snapshot.matched) {
        std::string line;
        if (EncodeMatchQueueEntry(entry, &line)) {
            lines.push_back(std::move(line));
        } else {
            std::fprintf(stderr, "[match] 队列快照条目编码失败，已跳过：match_id=%s\n",
                         entry.match_id.c_str());
        }
    }

    const std::string key = queue_key();

    // 整份替换用 MULTI/EXEC 包住：读取端永远看到完整的一份。
    // 用管道（redisAppendCommand）而不是逐条 redisCommand：队列上千行时
    // 逐条会变成上千次往返，而快照写入发生在入队路径上，不能拖住它。
    bool failed = false;
    auto push_replies = [&](std::size_t expected) {
        for (std::size_t i = 0; i < expected; ++i) {
            void* reply = nullptr;
            if (redisGetReply(context_, &reply) != REDIS_OK) {
                failed = true;
                return;
            }
            freeReplyObject(reply);
        }
    };

    redisAppendCommand(context_, "MULTI");
    redisAppendCommand(context_, "DEL %s", key.c_str());
    std::size_t queued_replies = 2;  // MULTI + DEL
    for (const std::string& line : lines) {
        // %b 是二进制安全的（长度 + 指针），因此编码行里出现任何字节都不会出问题。
        redisAppendCommand(context_, "RPUSH %s %b", key.c_str(), line.data(), line.size());
        ++queued_replies;
    }
    redisAppendCommand(context_, "EXEC");
    ++queued_replies;
    push_replies(queued_replies);

    if (failed || context_ == nullptr || context_->err != 0) {
        // **管道出错时连接已经不可信**：必须整条丢掉，否则残留的未读回复会让
        // 下一次调用读到别人的回复（TASK-014 实测过这类错位）。
        DiscardConnection();
        return false;
    }
    return true;
}

bool RedisMatchQueueStore::Load(std::vector<MatchQueueSnapshotRow>* out_rows) {
    if (out_rows == nullptr) {
        return false;
    }
    out_rows->clear();

    const std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureConnected()) {
        return false;
    }

    const std::string key = queue_key();
    void* raw_reply = redisCommand(context_, "LRANGE %s 0 -1", key.c_str());
    if (raw_reply == nullptr) {
        DiscardConnection();
        return false;
    }
    auto* reply = static_cast<redisReply*>(raw_reply);
    if (reply->type != REDIS_REPLY_ARRAY) {
        freeReplyObject(raw_reply);
        DiscardConnection();
        return false;
    }

    out_rows->reserve(reply->elements);
    for (std::size_t i = 0; i < reply->elements; ++i) {
        const redisReply* element = reply->element[i];
        if (element == nullptr || element->type != REDIS_REPLY_STRING) {
            // 非字符串元素说明这个 Key 被别的写入者污染了。把它作为一条损坏记录
            // 交上去，而不是在这里静默跳过。
            MatchQueueSnapshotRow row;
            row.problem = "Key 中的元素不是字符串（被别的写入者污染？）";
            out_rows->push_back(std::move(row));
            continue;
        }
        out_rows->push_back(DecodeMatchQueueEntry(
            std::string_view(element->str, static_cast<std::size_t>(element->len))));
    }
    freeReplyObject(raw_reply);
    return true;
}

bool RedisMatchQueueStore::IsHealthy() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureConnected()) {
        return false;
    }
    void* raw_reply = redisCommand(context_, "PING");
    if (raw_reply == nullptr) {
        DiscardConnection();
        return false;
    }
    auto* reply = static_cast<redisReply*>(raw_reply);
    const bool ok = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(raw_reply);
    return ok;
}

}  // namespace rgbt::match
