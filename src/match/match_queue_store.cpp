#include "match_queue_store.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rgbt::match {
namespace {

/// 标识类字段的长度上限。与 match_queue.hpp 的 kMaxPlayerIdLength 一致；
/// 这里重新写一个常量而不是包含那个头，是为了让本文件（编解码）不依赖队列实现。
constexpr std::size_t kMaxFieldLength = 64;

void AppendString(std::string* out, std::string_view value) {
    out->append(std::to_string(value.size()));
    out->push_back(':');
    out->append(value);
}

void AppendInt(std::string* out, std::int64_t value) {
    out->append(std::to_string(value));
    out->push_back(';');
}

/// 顺序读取器。所有读取都做边界检查，**越界即失败**，绝不返回部分数据。
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    [[nodiscard]] bool ReadChar(char* out) {
        if (pos_ >= text_.size()) {
            return false;
        }
        *out = text_[pos_++];
        return true;
    }

    /// 读 `<十进制长度>:<原始字节>`。长度为 0 允许（调用方按需再校验非空）。
    [[nodiscard]] bool ReadString(std::string* out) {
        std::size_t length = 0;
        const char* begin = text_.data() + pos_;
        const char* end = text_.data() + text_.size();
        const std::from_chars_result parsed = std::from_chars(begin, end, length);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || parsed.ptr >= end ||
            *parsed.ptr != ':') {
            return false;
        }
        pos_ = static_cast<std::size_t>(parsed.ptr - text_.data()) + 1;
        if (length > text_.size() - pos_) {
            return false;
        }
        out->assign(text_.data() + pos_, length);
        pos_ += length;
        return true;
    }

    /// 读 `<十进制整数>;`。
    [[nodiscard]] bool ReadInt(std::int64_t* out) {
        const char* begin = text_.data() + pos_;
        const char* end = text_.data() + text_.size();
        std::int64_t value = 0;
        const std::from_chars_result parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || parsed.ptr >= end ||
            *parsed.ptr != ';') {
            return false;
        }
        pos_ = static_cast<std::size_t>(parsed.ptr - text_.data()) + 1;
        *out = value;
        return true;
    }

    [[nodiscard]] bool AtEnd() const { return pos_ == text_.size(); }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

bool ValidIdentifier(const std::string& value) {
    return !value.empty() && value.size() <= kMaxFieldLength;
}

/// 字段合法性校验。**编码与解码共用**：两边对"什么叫合法"必须只有一个答案，
/// 否则会出现"写得进去、读不出来"的记录。
bool ValidateEntry(const MatchQueueEntry& entry, std::string* out_problem) {
    if (entry.kind == SnapshotEntryKind::kQueued) {
        if (!ValidIdentifier(entry.player_id)) {
            *out_problem = "player_id 缺失或超长";
            return false;
        }
        if (entry.queued_at_ms <= 0) {
            *out_problem = "queued_at_ms 非正";
            return false;
        }
        return true;
    }

    if (!ValidIdentifier(entry.match_id)) {
        *out_problem = "match_id 缺失或超长";
        return false;
    }
    if (!ValidIdentifier(entry.room_id)) {
        *out_problem = "room_id 缺失或超长";
        return false;
    }
    if (entry.stamp_ms <= 0) {
        *out_problem = "stamp_ms 非正";
        return false;
    }
    if (entry.player_ids.empty() || entry.player_ids.size() > kMaxPlayersPerSnapshotGroup) {
        *out_problem = "玩家数不在 [1, " + std::to_string(kMaxPlayersPerSnapshotGroup) + "]";
        return false;
    }
    for (const std::string& player_id : entry.player_ids) {
        if (!ValidIdentifier(player_id)) {
            *out_problem = "玩家标识缺失或超长";
            return false;
        }
    }
    return true;
}

}  // namespace

bool EncodeMatchQueueEntry(const MatchQueueEntry& entry, std::string* out_line) {
    if (out_line == nullptr) {
        return false;
    }
    std::string problem;
    if (!ValidateEntry(entry, &problem)) {
        return false;
    }

    std::string line;
    line.push_back(kSnapshotFormatVersion);
    if (entry.kind == SnapshotEntryKind::kQueued) {
        line.push_back('Q');
        AppendString(&line, entry.player_id);
        AppendInt(&line, entry.queued_at_ms);
    } else {
        line.push_back('M');
        AppendString(&line, entry.match_id);
        AppendString(&line, entry.room_id);
        AppendInt(&line, entry.stamp_ms);
        AppendInt(&line, static_cast<std::int64_t>(entry.player_ids.size()));
        for (const std::string& player_id : entry.player_ids) {
            AppendString(&line, player_id);
        }
    }

    *out_line = std::move(line);
    return true;
}

MatchQueueSnapshotRow DecodeMatchQueueEntry(std::string_view line) {
    MatchQueueSnapshotRow row;
    Reader reader(line);

    char version = 0;
    if (!reader.ReadChar(&version)) {
        row.problem = "空行";
        return row;
    }
    if (version != kSnapshotFormatVersion) {
        // 版本不认识就到此为止。**不要**尝试按当前格式继续解析：
        // 那正是"解析出一支乱七八糟的队列"的来源。
        row.problem = std::string("未知的快照格式版本：") + version;
        return row;
    }

    char kind = 0;
    if (!reader.ReadChar(&kind)) {
        row.problem = "缺少条目类型";
        return row;
    }

    if (kind == 'Q') {
        row.entry.kind = SnapshotEntryKind::kQueued;
        if (!reader.ReadString(&row.entry.player_id)) {
            row.problem = "player_id 字段截断";
            return row;
        }
        if (!reader.ReadInt(&row.entry.queued_at_ms)) {
            row.problem = "queued_at_ms 字段截断";
            return row;
        }
    } else if (kind == 'M') {
        row.entry.kind = SnapshotEntryKind::kMatched;
        if (!reader.ReadString(&row.entry.match_id)) {
            row.problem = "match_id 字段截断";
            return row;
        }
        if (!reader.ReadString(&row.entry.room_id)) {
            row.problem = "room_id 字段截断";
            return row;
        }
        if (!reader.ReadInt(&row.entry.stamp_ms)) {
            row.problem = "stamp_ms 字段截断";
            return row;
        }
        std::int64_t count = 0;
        if (!reader.ReadInt(&count)) {
            row.problem = "玩家数字段截断";
            return row;
        }
        // 先卡上界再循环：损坏或恶意构造的计数不能变成一次长循环。
        if (count <= 0 || count > static_cast<std::int64_t>(kMaxPlayersPerSnapshotGroup)) {
            row.problem = "玩家数越界：" + std::to_string(count);
            return row;
        }
        row.entry.player_ids.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
            std::string player_id;
            if (!reader.ReadString(&player_id)) {
                row.problem = "玩家标识字段截断";
                return row;
            }
            row.entry.player_ids.push_back(std::move(player_id));
        }
    } else {
        row.problem = std::string("未知的条目类型：") + kind;
        return row;
    }

    if (!reader.AtEnd()) {
        // 行尾还有东西，说明这一行与当前格式不一致（例如写入端改过格式）。
        row.problem = "行尾有多余字节";
        return row;
    }

    // 与编码端共用同一套字段校验，避免"写得进去、读不出来"。
    std::string problem;
    if (!ValidateEntry(row.entry, &problem)) {
        row.problem = problem;
    }
    return row;
}

}  // namespace rgbt::match
