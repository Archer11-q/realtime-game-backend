#include "mysql_match_result_writer.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rgbt::room {
namespace {

/// 幂等写入。
///
/// 为什么用 ON DUPLICATE KEY UPDATE 而不是先查后写：先查后写之间存在竞态，
/// 两个并发请求可能都查到「不存在」然后都插入，第二个会以主键冲突失败。
/// 交给唯一约束裁决才是原子的。
///
/// `match_id = match_id` 是一个空更新：命中重复键时不改变任何列，
/// 因此**不会覆盖首次写入的结果**。这一点很重要——重复写入必须返回第一次的结果，
/// 而不是把后来的值写进去。
///
/// NULLIF(?, '') 把空字符串转成 NULL：平局时 winner_id 必须落库为 NULL，
/// 而不是空字符串，否则「平局」和「胜者标识为空」两种状态在数据库里无法区分。
constexpr const char* kInsertSql =
    "INSERT INTO match_results "
    "(match_id, room_id, winner_id, player_count, started_at, finished_at) "
    "VALUES (?, ?, NULLIF(?, ''), ?, FROM_UNIXTIME(? / 1000), FROM_UNIXTIME(? / 1000)) "
    "ON DUPLICATE KEY UPDATE match_id = match_id";

/// 按主键读取。
///
/// UNIX_TIMESTAMP 直接返回秒，避免把 TIMESTAMP 当作字符串解析时的时区歧义。
constexpr const char* kSelectSql =
    "SELECT match_id, room_id, winner_id, player_count, "
    "UNIX_TIMESTAMP(started_at), UNIX_TIMESTAMP(finished_at) "
    "FROM match_results WHERE match_id = ? LIMIT 1";

/// 把可选列解析成整数。NULL 或空串返回 0，解析失败同样返回 0。
///
/// 用 from_chars 而不是 stoll：前者不抛异常，也不依赖当前的 locale。
std::int64_t ParseInt64(const std::optional<std::string>& value) {
    if (!value.has_value() || value->empty()) {
        return 0;
    }
    std::int64_t result = 0;
    const char* begin = value->data();
    const char* end = begin + value->size();
    const std::from_chars_result parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{}) {
        return 0;
    }
    return result;
}

/// 把可选列安全地取成字符串。NULL 返回空串。
std::string ParseString(const std::optional<std::string>& value) {
    return value.has_value() ? *value : std::string();
}

}  // namespace

MysqlMatchResultWriter::MysqlMatchResultWriter(rgbt::common::MysqlConnection* connection)
    : connection_(connection) {}

WriteStatus MysqlMatchResultWriter::Write(const MatchResultRecord& record) {
    if (connection_ == nullptr) {
        return WriteStatus::kUnavailable;
    }
    // 对局结果的主键必须存在，否则写入没有意义。
    if (record.match_id.empty()) {
        return WriteStatus::kUnavailable;
    }

    const std::vector<std::string> params = {
        record.match_id,
        record.room_id,
        record.winner_id,
        std::to_string(record.player_count),
        std::to_string(record.started_at_ms),
        std::to_string(record.finished_at_ms),
    };

    std::vector<rgbt::common::SqlRow> rows;
    if (!connection_->Query(kInsertSql, params, &rows)) {
        return WriteStatus::kUnavailable;
    }
    return WriteStatus::kOk;
}

ReadStatus MysqlMatchResultWriter::Read(const std::string& match_id,
                                        MatchResultRecord* out_record) {
    if (connection_ == nullptr || out_record == nullptr) {
        return ReadStatus::kUnavailable;
    }
    if (match_id.empty()) {
        return ReadStatus::kNotFound;
    }

    std::vector<rgbt::common::SqlRow> rows;
    if (!connection_->Query(kSelectSql, {match_id}, &rows)) {
        return ReadStatus::kUnavailable;
    }
    if (rows.empty()) {
        return ReadStatus::kNotFound;
    }

    const rgbt::common::SqlRow& row = rows.front();
    // 期望 6 列。列数不符说明查询与实现不一致，按依赖故障处理而不是猜测。
    if (row.size() != 6) {
        return ReadStatus::kUnavailable;
    }

    MatchResultRecord record;
    record.match_id = ParseString(row[0]);
    record.room_id = ParseString(row[1]);
    // winner_id 为 NULL 表示平局，这里保持空字符串，由上层决定如何呈现。
    record.winner_id = ParseString(row[2]);
    record.player_count = static_cast<std::int32_t>(ParseInt64(row[3]));
    // UNIX_TIMESTAMP 返回秒，统一转成毫秒，与协议字段的时间单位一致。
    record.started_at_ms = ParseInt64(row[4]) * 1000;
    record.finished_at_ms = ParseInt64(row[5]) * 1000;

    *out_record = std::move(record);
    return ReadStatus::kOk;
}

bool MysqlMatchResultWriter::IsHealthy() {
    return connection_ != nullptr && connection_->Ping();
}

}  // namespace rgbt::room
