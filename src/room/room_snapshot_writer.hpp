/// @file room_snapshot_writer.hpp
/// @brief 房间快照的写入接口（TASK-013）。
///
/// 为什么与 MatchResultWriter 分成两个接口，而不是合成一个 "RoomRepository"：
///   两者虽然都写 MySQL，**失败语义却完全相反**。
///
///     对局结果：**不可丢弃**。写入失败必须持续推进到成功，否则玩家拿不到胜负。
///               RoomManager 为此维护 FINISHING 状态并按固定间隔无限重试。
///     房间快照：**可以丢弃**。写入失败只记一条日志，下一个快照周期天然覆盖它。
///
///   放同一个接口里就等于邀请调用方用同一套策略处理两者——而"给快照加上无限
///   重试"正是这里最需要避免的错误：那会让一次 MySQL 抖动在内存里堆起一批
///   过期快照，且毫无收益。接口分开，这个区分就有地方写下来。
///
/// 接口目前只有写入。读取路径属于 TASK-014（房间重启恢复），届时再加一个
/// 独立的读取接口——一次引入两处未验证的复杂度不符合本项目的拆分原则。

#ifndef RGBT_ROOM_ROOM_SNAPSHOT_WRITER_HPP
#define RGBT_ROOM_ROOM_SNAPSHOT_WRITER_HPP

#include "room_types.hpp"

namespace rgbt::room {

/// 快照写入结果。
enum class SnapshotWriteStatus {
    /// 写入成功。
    kOk,
    /// 存储暂时不可用。**调用方不需要重试**：下一个快照周期会覆盖它。
    kUnavailable,
};

/// 房间快照写入器。
class RoomSnapshotWriter {
public:
    RoomSnapshotWriter() = default;
    RoomSnapshotWriter(const RoomSnapshotWriter&) = delete;
    RoomSnapshotWriter& operator=(const RoomSnapshotWriter&) = delete;
    RoomSnapshotWriter(RoomSnapshotWriter&&) = delete;
    RoomSnapshotWriter& operator=(RoomSnapshotWriter&&) = delete;
    virtual ~RoomSnapshotWriter() = default;

    /// @brief 写入（或覆盖）一条房间快照，以 match_id 为键。
    ///
    /// 覆盖而不是追加：这张表只保留每个房间的最新一份快照。
    /// 因此 `snapshot_at_ms` 更旧的一次写入**不应该**覆盖更新的那次——
    /// 这个判断由实现负责（比较 snapshot_at_ms），调用方不需要关心乱序。
    [[nodiscard]] virtual SnapshotWriteStatus Save(const RoomSnapshotRecord& record) = 0;

    /// @brief 存储当前是否可用。用于启动日志，不做真实往返保证。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_ROOM_SNAPSHOT_WRITER_HPP
