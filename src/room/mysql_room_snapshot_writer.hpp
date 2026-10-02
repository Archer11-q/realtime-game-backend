/// @file mysql_room_snapshot_writer.hpp
/// @brief 房间快照的 MySQL 实现（TASK-013）。
///
/// 与 MysqlMatchResultWriter 分开的实现，理由见 room_snapshot_writer.hpp 顶部：
/// 对局结果不可丢弃、房间快照可以丢弃，两者的失败策略相反。

#ifndef RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_WRITER_HPP
#define RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_WRITER_HPP

#include "common/mysql_connection.hpp"
#include "room_snapshot_writer.hpp"

namespace rgbt::room {

class MysqlRoomSnapshotWriter final : public RoomSnapshotWriter {
public:
    /// @param connection 连接。可以为 nullptr（此时任何写入都返回 kUnavailable），
    ///        用于不关心持久化的测试。
    explicit MysqlRoomSnapshotWriter(rgbt::common::MysqlConnection* connection);

    [[nodiscard]] SnapshotWriteStatus Save(const RoomSnapshotRecord& record) override;
    [[nodiscard]] bool IsHealthy() override;

private:
    rgbt::common::MysqlConnection* connection_;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_WRITER_HPP
