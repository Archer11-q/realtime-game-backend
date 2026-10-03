#include "mysql_room_snapshot_writer.hpp"

#include <string>
#include <vector>

#include "common/logging.hpp"

namespace rgbt::room {
namespace {

/// 写入（或覆盖）房间快照。
///
/// 为什么是 INSERT ... ON DUPLICATE KEY UPDATE 而不是先查后写：先查后写之间存在
/// 竞态，两个写入可能都查到「不存在」然后都插入，第二个以主键冲突失败。
/// 交给唯一约束裁决才是原子的。
///
/// **每一列都套了 `IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, ...)`**。
/// 为什么不直接覆盖：这是一张"最新一份快照"表，如果一次迟到的旧快照把新快照
/// 覆盖掉，恢复出来的房间就会**倒退**，而且从数据库里完全看不出发生过这件事。
/// 把新旧判断交给数据库表达，就不依赖"调用方一定按顺序写"这个外部不变量；
/// 单线程的 Tick 目前确实保证了顺序，但那是巧合而非契约。
///
/// **为什么用 `VALUES()` 而不是更现代的 `AS new` 行别名**：本机 MySQL 8.4.11 上
/// 两种写法都能被 `mysql` 命令行正确执行，因此这不是语法问题；而 `AS new` 在
/// 本项目的 `MysqlConnection`（走预处理接口）下曾经报错，排查时一并换成了
/// 兼容性更广的 `VALUES()`。**注意：那次报错的真实原因不是别名**，而是
/// `MysqlConnection` 里一个 8 参数的硬上限（见 src/common/mysql_connection.cpp
/// 的 kMaxBindParams），与 SQL 写法无关。本注释保留这段经过，是因为它记录了
/// 一次"看起来像 A 问题、实际是 B 问题"的误判，避免下次重犯。
///
/// `NULLIF(?, '')` 把空字符串转成 NULL：平局与"未结束"都必须落库为 NULL，
/// 而不是空字符串，否则「平局」和「胜者标识为空」两种状态在数据库里无法区分。
constexpr const char* kUpsertSql =
    "INSERT INTO rooms "
    "(match_id, room_id, state, frame, "
    " p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined, "
    " winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, ''), ?, ?, ?, ?) "
    "ON DUPLICATE KEY UPDATE "
    " room_id        = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(room_id), room_id), "
    " state          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(state), state), "
    " frame          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(frame), frame), "
    " p1_id          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p1_id), p1_id), "
    " p1_hp          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p1_hp), p1_hp), "
    " p1_joined      = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p1_joined), p1_joined), "
    " p2_id          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p2_id), p2_id), "
    " p2_hp          = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p2_hp), p2_hp), "
    " p2_joined      = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(p2_joined), p2_joined), "
    " winner_id      = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(winner_id), winner_id), "
    " finish_reason  = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(finish_reason), "
    "finish_reason), "
    " started_at_ms  = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(started_at_ms), "
    "started_at_ms), "
    " finished_at_ms = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(finished_at_ms), "
    "finished_at_ms), "
    " snapshot_at_ms = IF(VALUES(snapshot_at_ms) >= snapshot_at_ms, VALUES(snapshot_at_ms), "
    "snapshot_at_ms)";

/// 把布尔值写成 MySQL 的 0/1。
const char* BoolLiteral(bool value) {
    return value ? "1" : "0";
}

}  // namespace

MysqlRoomSnapshotWriter::MysqlRoomSnapshotWriter(rgbt::common::MysqlConnection* connection)
    : connection_(connection) {}

SnapshotWriteStatus MysqlRoomSnapshotWriter::Save(const RoomSnapshotRecord& record) {
    if (connection_ == nullptr) {
        return SnapshotWriteStatus::kUnavailable;
    }
    // 幂等键必须存在。它是空的说明调用方传错了，不该往库里写一条无主记录。
    if (record.match_id.empty() || record.room_id.empty()) {
        return SnapshotWriteStatus::kUnavailable;
    }

    const std::vector<std::string> params = {
        record.match_id,
        record.room_id,
        ToString(record.phase),
        std::to_string(record.frame),
        record.players[0].player_id,
        std::to_string(record.players[0].hp),
        BoolLiteral(record.players[0].joined),
        record.players[1].player_id,
        std::to_string(record.players[1].hp),
        BoolLiteral(record.players[1].joined),
        record.winner_id,
        ToString(record.finish_reason),
        std::to_string(record.started_at_ms),
        std::to_string(record.finished_at_ms),
        std::to_string(record.snapshot_at_ms),
    };

    std::vector<rgbt::common::SqlRow> rows;
    if (!connection_->Query(kUpsertSql, params, &rows)) {
        // 失败原因在这里记，因为**只有这一层知道 MySQL 的错误**。
        // 调用方按次计数即可，重复记一遍同样的信息只会淹没日志。
        //
        // 顺便说明为什么这条日志是 stderr 而不是丢弃：快照可以丢弃，
        // 但"一直写不进去"必须被看见——那意味着"重启可恢复"已经不成立了。
        rgbt::common::LogWarn("room_snapshot_write_failed", record.match_id,
                              {{"frame", std::to_string(record.frame)},
                               {"err", connection_->last_error()},
                               {"policy", "可丢弃、不重试，下个周期覆盖"}});
        return SnapshotWriteStatus::kUnavailable;
    }
    return SnapshotWriteStatus::kOk;
}

bool MysqlRoomSnapshotWriter::IsHealthy() {
    return connection_ != nullptr && connection_->Ping();
}

}  // namespace rgbt::room
