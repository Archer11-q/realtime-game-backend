#include "mysql_room_snapshot_reader.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rgbt::room {
namespace {

/// 只取**未结束**的房间。已结束的房间没有恢复价值。
///
/// 没有绑定参数，因此不触及 MysqlConnection 的参数个数上限。
/// 按 snapshot_at_ms 升序：先恢复更早的房间，启动日志的顺序因此是稳定的，
/// 排障时不用在两行之间来回对照时间。
constexpr const char* kSelectSql =
    "SELECT match_id, room_id, state, frame, "
    "p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined, "
    "winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms "
    "FROM rooms WHERE state NOT IN ('finished', 'aborted') ORDER BY snapshot_at_ms ASC";

/// 期望的列数。与上面的 SELECT 一一对应。
constexpr std::size_t kColumnCount = 15;

/// 把列解析成整数。NULL 或空串返回 0，解析失败同样返回 0。
///
/// 用 from_chars 而不是 stoll：前者不抛异常，也不依赖当前 locale。
/// 与 mysql_match_result_writer.cpp 里的同名函数是有意重复的——
/// 抽成公共头会让三个文件都依赖一个二十行的工具头，收益不抵这份耦合。
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

std::string ParseString(const std::optional<std::string>& value) {
    return value.has_value() ? *value : std::string();
}

/// 解析 MySQL 的 0/1 布尔列。
///
/// 只认 "0" 与 "1"：出现别的值说明列被写坏了，按 false 处理并让上层
/// 的 ValidateRoomSnapshot 去判断——而不是在这里悄悄编一个布尔值。
bool ParseBool(const std::optional<std::string>& value) {
    return value.has_value() && *value == "1";
}

}  // namespace

MysqlRoomSnapshotReader::MysqlRoomSnapshotReader(rgbt::common::MysqlConnection* connection)
    : connection_(connection) {}

bool MysqlRoomSnapshotReader::LoadUnfinished(std::vector<RoomSnapshotRow>* out_rows) {
    if (connection_ == nullptr || out_rows == nullptr) {
        return false;
    }
    out_rows->clear();

    std::vector<rgbt::common::SqlRow> rows;
    if (!connection_->Query(kSelectSql, {}, &rows)) {
        return false;
    }

    for (const rgbt::common::SqlRow& row : rows) {
        RoomSnapshotRow parsed;
        if (row.size() != kColumnCount) {
            // 列数不符说明 SELECT 与解析不一致（例如有人改了表结构）。
            // 这条不能丢：一旦丢了，恢复出来的房间就是"少了一半字段"的，
            // 而调用方完全不知道。因此把它作为一条损坏记录交上去。
            parsed.problem = "列数不是 " + std::to_string(kColumnCount) + "（实际 " +
                             std::to_string(row.size()) + "）";
            out_rows->push_back(std::move(parsed));
            continue;
        }

        RoomSnapshotRecord& record = parsed.record;
        record.match_id = ParseString(row[0]);
        record.room_id = ParseString(row[1]);
        record.frame = ParseInt64(row[3]);
        record.winner_id = ParseString(row[10]);
        record.started_at_ms = ParseInt64(row[12]);
        record.finished_at_ms = ParseInt64(row[13]);
        record.snapshot_at_ms = ParseInt64(row[14]);

        record.players[0].player_id = ParseString(row[4]);
        record.players[0].hp = static_cast<std::int32_t>(ParseInt64(row[5]));
        record.players[0].joined = ParseBool(row[6]);
        record.players[1].player_id = ParseString(row[7]);
        record.players[1].hp = static_cast<std::int32_t>(ParseInt64(row[8]));
        record.players[1].joined = ParseBool(row[9]);

        // 状态与结束原因是枚举，解析失败必须留下痕迹而不是退回默认值：
        // 把未知状态当成 CREATED，会让一条损坏的快照被当成正常房间恢复出来。
        const std::optional<RoomPhase> phase = ParseRoomPhase(ParseString(row[2]));
        if (!phase.has_value()) {
            parsed.problem = "state 是未知取值：" + ParseString(row[2]);
            out_rows->push_back(std::move(parsed));
            continue;
        }
        record.phase = *phase;

        const std::optional<FinishReason> reason = ParseFinishReason(ParseString(row[11]));
        if (!reason.has_value()) {
            parsed.problem = "finish_reason 是未知取值：" + ParseString(row[11]);
            out_rows->push_back(std::move(parsed));
            continue;
        }
        record.finish_reason = *reason;

        // 解析层能查的都查完了，剩下的交给纯函数（可单元测试的那一层）。
        parsed.problem = ValidateRoomSnapshot(record);
        out_rows->push_back(std::move(parsed));
    }

    return true;
}

bool MysqlRoomSnapshotReader::IsHealthy() {
    return connection_ != nullptr && connection_->Ping();
}

}  // namespace rgbt::room
