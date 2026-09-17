#include "redis_session_store.hpp"

#include <hiredis/hiredis.h>

#include <chrono>
#include <cstdarg>
#include <string>
#include <utility>

#include "common/token.hpp"

namespace rgbt::gateway {
namespace {

/// 幂等映射的 TTL。取值与登录会话一致：保证「同一 request_id 在会话有效期内
/// 返回同一 Token」这一语义成立。
constexpr std::int32_t kIdempotencyTtlSeconds = 7 * 24 * 60 * 60;

std::int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 从 Redis 回复中安全取出字符串。reply 为空或类型不符时返回 false。
bool ReplyAsString(void* raw_reply, std::string* out) {
    auto* reply = static_cast<redisReply*>(raw_reply);
    if (reply == nullptr) {
        return false;
    }
    if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
        out->assign(reply->str, static_cast<std::size_t>(reply->len));
        return true;
    }
    return false;
}

void FreeReply(void* raw_reply) {
    if (raw_reply != nullptr) {
        freeReplyObject(raw_reply);
    }
}

}  // namespace

RedisSessionStore::RedisSessionStore(std::string env_prefix, RedisOptions options,
                                     std::int32_t session_ttl_seconds)
    : env_prefix_(std::move(env_prefix)),
      options_(std::move(options)),
      session_ttl_seconds_(session_ttl_seconds > 0 ? session_ttl_seconds : 60) {}

RedisSessionStore::~RedisSessionStore() {
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
}

std::string RedisSessionStore::KeyForSession(const std::string& token) const {
    // 依据 docs/05-api-and-data.md：Key 必须带环境和服务前缀。
    return env_prefix_ + ":gateway:session:" + token;
}

std::string RedisSessionStore::KeyForLoginIdempotency(const std::string& request_id) const {
    return env_prefix_ + ":gateway:idem:login:" + request_id;
}

bool RedisSessionStore::EnsureConnected() {
    if (context_ != nullptr && context_->err == 0) {
        return true;
    }
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }

    timeval connect_timeout{};
    connect_timeout.tv_sec = options_.connect_timeout_ms / 1000;
    connect_timeout.tv_usec = (options_.connect_timeout_ms % 1000) * 1000;

    context_ = redisConnectWithTimeout(options_.host.c_str(), options_.port, connect_timeout);
    if (context_ == nullptr || context_->err != 0) {
        if (context_ != nullptr) {
            redisFree(context_);
            context_ = nullptr;
        }
        return false;
    }

    timeval command_timeout{};
    command_timeout.tv_sec = options_.command_timeout_ms / 1000;
    command_timeout.tv_usec = (options_.command_timeout_ms % 1000) * 1000;
    redisSetTimeout(context_, command_timeout);
    return true;
}

void* RedisSessionStore::ExecuteCommand(const char* format, ...) {
    if (!EnsureConnected()) {
        return nullptr;
    }
    va_list args;
    va_start(args, format);
    void* reply = redisvCommand(context_, format, args);
    va_end(args);

    // 连接层错误（例如 Redis 中途停止）后标记为不可用，下次调用会重连。
    if (reply == nullptr && context_ != nullptr && context_->err != 0) {
        redisFree(context_);
        context_ = nullptr;
    }
    return reply;
}

StoreStatus RedisSessionStore::CreateSession(const std::string& request_id,
                                             const std::string& player_id,
                                             const std::string& client_type,
                                             std::int32_t ttl_seconds, std::string* out_token) {
    if (out_token == nullptr) {
        return StoreStatus::kUnavailable;
    }
    const std::int32_t ttl = ttl_seconds > 0 ? ttl_seconds : session_ttl_seconds_;
    const std::string idem_key = KeyForLoginIdempotency(request_id);

    // 第一步：先看该 request_id 是否已经创建过会话（幂等命中）。
    void* existing_reply = ExecuteCommand("GET %s", idem_key.c_str());
    if (existing_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    std::string existing_token;
    const bool has_existing = ReplyAsString(existing_reply, &existing_token);
    FreeReply(existing_reply);
    if (has_existing && !existing_token.empty()) {
        *out_token = existing_token;
        return StoreStatus::kOk;
    }

    // 第二步：生成新 Token，并写入会话 Hash。
    const std::string token = rgbt::common::GenerateToken();
    const std::string session_key = KeyForSession(token);

    void* write_reply =
        ExecuteCommand("HSET %s token %s player_id %s client_type %s created_at %lld",
                       session_key.c_str(), token.c_str(), player_id.c_str(), client_type.c_str(),
                       static_cast<long long>(NowUnixSeconds()));
    if (write_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    FreeReply(write_reply);

    void* expire_reply = ExecuteCommand("EXPIRE %s %d", session_key.c_str(), ttl);
    if (expire_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    FreeReply(expire_reply);

    // 第三步：写入幂等映射。SET NX 保证并发下只有一个请求能写入，
    // 若竞争失败则读取别人写入的 Token，仍然保证同一 request_id 对应同一 Token。
    void* idem_reply = ExecuteCommand("SET %s %s EX %d NX", idem_key.c_str(), token.c_str(),
                                      ttl < kIdempotencyTtlSeconds ? ttl : kIdempotencyTtlSeconds);
    if (idem_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    const bool idem_written = (static_cast<redisReply*>(idem_reply)->type == REDIS_REPLY_STATUS);
    FreeReply(idem_reply);

    if (!idem_written) {
        void* winner_reply = ExecuteCommand("GET %s", idem_key.c_str());
        if (winner_reply == nullptr) {
            return StoreStatus::kUnavailable;
        }
        std::string winner_token;
        const bool got_winner = ReplyAsString(winner_reply, &winner_token);
        FreeReply(winner_reply);
        if (got_winner && !winner_token.empty()) {
            // 竞争失败：清理自己刚写入的会话，返回胜出者的 Token。
            void* cleanup = ExecuteCommand("DEL %s", session_key.c_str());
            FreeReply(cleanup);
            *out_token = winner_token;
            return StoreStatus::kOk;
        }
        return StoreStatus::kUnavailable;
    }

    *out_token = token;
    return StoreStatus::kOk;
}

StoreStatus RedisSessionStore::GetSession(const std::string& token, SessionRecord* out_session) {
    if (out_session == nullptr) {
        return StoreStatus::kUnavailable;
    }
    const std::string session_key = KeyForSession(token);

    void* reply =
        ExecuteCommand("HMGET %s token player_id client_type created_at", session_key.c_str());
    if (reply == nullptr) {
        return StoreStatus::kUnavailable;
    }

    auto* array = static_cast<redisReply*>(reply);
    if (array->type != REDIS_REPLY_ARRAY || array->elements != 4) {
        FreeReply(reply);
        return StoreStatus::kUnavailable;
    }
    // 第一个字段为 nil 说明 Key 不存在。
    if (array->element[0]->type == REDIS_REPLY_NIL) {
        FreeReply(reply);
        return StoreStatus::kNotFound;
    }

    SessionRecord record;
    if (!ReplyAsString(array->element[0], &record.token) ||
        !ReplyAsString(array->element[1], &record.player_id) ||
        !ReplyAsString(array->element[2], &record.client_type)) {
        FreeReply(reply);
        return StoreStatus::kUnavailable;
    }
    if (array->element[3]->type == REDIS_REPLY_STRING) {
        record.created_at_unix = std::stoll(
            std::string(array->element[3]->str, static_cast<std::size_t>(array->element[3]->len)));
    }
    FreeReply(reply);

    *out_session = std::move(record);
    return StoreStatus::kOk;
}

StoreStatus RedisSessionStore::DeleteSession(const std::string& token) {
    const std::string session_key = KeyForSession(token);

    void* exists_reply = ExecuteCommand("EXISTS %s", session_key.c_str());
    if (exists_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    const bool existed = (static_cast<redisReply*>(exists_reply)->integer == 1);
    FreeReply(exists_reply);

    void* del_reply = ExecuteCommand("DEL %s", session_key.c_str());
    if (del_reply == nullptr) {
        return StoreStatus::kUnavailable;
    }
    FreeReply(del_reply);

    // 幂等映射不在这里删除：它有自己的 TTL，保留它可以让重复的登出请求
    // 依然得到「已登出」的结果，而不是报会话不存在。
    return existed ? StoreStatus::kOk : StoreStatus::kNotFound;
}

bool RedisSessionStore::IsHealthy() {
    void* reply = ExecuteCommand("PING");
    if (reply == nullptr) {
        return false;
    }
    auto* typed = static_cast<redisReply*>(reply);
    const bool ok = (typed->type == REDIS_REPLY_STATUS);
    FreeReply(reply);
    return ok;
}

}  // namespace rgbt::gateway
