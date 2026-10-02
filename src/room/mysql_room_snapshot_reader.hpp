/// @file mysql_room_snapshot_reader.hpp
/// @brief 房间快照的 MySQL 读取实现（TASK-014）。
///
/// 与 MysqlRoomSnapshotWriter 分开的实现，理由见 room_snapshot_reader.hpp 顶部。
///
/// 关于单位：时间列在 rooms 表里存的是 Unix 毫秒（BIGINT），不是 TIMESTAMP，
/// 因此读出来不需要任何时区换算。这是 TASK-013 选 BIGINT 而不是 TIMESTAMP 的
/// 直接收益——match_results 用的是 TIMESTAMP，读取时必须过一遍 UNIX_TIMESTAMP，
/// 多一处容易出错的换算。

#ifndef RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_READER_HPP
#define RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_READER_HPP

#include "common/mysql_connection.hpp"
#include "room_snapshot_reader.hpp"

namespace rgbt::room {

class MysqlRoomSnapshotReader final : public RoomSnapshotReader {
public:
    /// @param connection 连接。可以为 nullptr（此时读取一律失败），
    ///        用于不关心恢复的测试。
    explicit MysqlRoomSnapshotReader(rgbt::common::MysqlConnection* connection);

    [[nodiscard]] bool LoadUnfinished(std::vector<RoomSnapshotRow>* out_rows) override;
    [[nodiscard]] bool IsHealthy() override;

private:
    rgbt::common::MysqlConnection* connection_;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_MYSQL_ROOM_SNAPSHOT_READER_HPP
