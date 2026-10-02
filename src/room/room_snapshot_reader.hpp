/// @file room_snapshot_reader.hpp
/// @brief 房间快照的读取接口与快照合法性校验（TASK-014）。
///
/// 为什么不给 RoomSnapshotWriter 加一个 Load 方法，而是新增独立接口：
///   writer 的契约是「写失败可以丢弃、不重试」（见 room_snapshot_writer.hpp）；
///   而读取是**启动路径上的前置条件**，失败必须让调用方能区分三种情况——
///   没有快照、存储不可用、快照损坏——并各自做出不同处置。
///   把语义相反的两种失败策略塞进同一组方法，调用方早晚会用错其中一个。
///
/// 本文件里的校验是**纯函数**，不依赖 MySQL。因此「各类损坏快照如何处理」
/// 可以在单元测试里逐条构造，而不是只能靠往数据库里塞脏数据来碰运气。

#ifndef RGBT_ROOM_ROOM_SNAPSHOT_READER_HPP
#define RGBT_ROOM_ROOM_SNAPSHOT_READER_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "room_types.hpp"

namespace rgbt::room {

/// 读取一行快照的结果。
struct RoomSnapshotRow {
    RoomSnapshotRecord record;

    /// 这条快照**不可用**的原因。空字符串表示可用。
    ///
    /// 为什么把两种来源的原因合并到一个字段里：
    ///   * 解析层能发现的问题（`state` 或 `finish_reason` 是未知字符串）
    ///     只有 MySQL 实现知道，因为内部类型存的是枚举而不是字符串；
    ///   * 语义层能发现的问题（血量越界、双方都没加入、frame 超范围）
    ///     由 ValidateRoomSnapshot 判断。
    /// 调用方只需要一个信号——「这条能不能用」——因此两层都往同一个字段写，
    /// 由 Restore 统一按「problem 非空即拒绝」处理。
    std::string problem;
};

/// 把快照里的状态字符串解析成枚举。
/// @return 未知取值返回 nullopt。**不要**把未知状态当成 CREATED：
///         那会让一条损坏的快照被当成正常房间恢复出来。
[[nodiscard]] std::optional<RoomPhase> ParseRoomPhase(std::string_view text);

/// 把结束原因字符串解析成枚举。未知取值返回 nullopt。
[[nodiscard]] std::optional<FinishReason> ParseFinishReason(std::string_view text);

/// @brief 校验一条已解析的快照是否可用于恢复。
/// @return 空字符串表示可用；否则返回不可用的原因（用于日志与 ABORTED 记录）。
///
/// 校验项与理由：
///   * `match_id` / `room_id` 非空且不超长——它们是主键与外键，缺了无法重建索引。
///   * `frame` 在 [0, kMaxFrames]——超出说明这一局本来就不该存在。
///   * 两个玩家标识非空且**互不相同**——同一人占两个位置的对局无法继续。
///   * 血量在 [0, kInitialHp]——负血或超过初始值都说明快照被写坏了。
///   * `winner_id` 要么为空（未结束/平局），要么是场上某个玩家。
///   * **PLAYING / FINISHING 的房间必须双方都已加入**。这两类状态意味着对局
///     真的打过，而"双方都不在房间内"与之矛盾。CREATED / WAITING 不受这条约束：
///     那时本来就没人加入，0 人是正常状态。
[[nodiscard]] std::string ValidateRoomSnapshot(const RoomSnapshotRecord& record);

/// 房间快照读取器。
class RoomSnapshotReader {
public:
    RoomSnapshotReader() = default;
    RoomSnapshotReader(const RoomSnapshotReader&) = delete;
    RoomSnapshotReader& operator=(const RoomSnapshotReader&) = delete;
    RoomSnapshotReader(RoomSnapshotReader&&) = delete;
    RoomSnapshotReader& operator=(RoomSnapshotReader&&) = delete;
    virtual ~RoomSnapshotReader() = default;

    /// @brief 读取所有**未结束**的房间快照。
    ///
    /// 只扫未结束的（state 不是 finished / aborted）：已结束的房间没有恢复价值，
    /// 扫回来只会白占内存并让启动日志充满噪音。
    ///
    /// **损坏的行也会被返回**（`problem` 非空）。读取层自行丢弃损坏行
    /// 就等于"静默丢数据"，而本任务明确禁止这一点：调用方需要知道
    /// 「有 N 个房间因为快照损坏而无法恢复」，并把它们标记为 ABORTED。
    ///
    /// @return false 表示存储不可用（连接失败或查询失败），此时 `out_rows`
    ///         的内容无意义。
    [[nodiscard]] virtual bool LoadUnfinished(std::vector<RoomSnapshotRow>* out_rows) = 0;

    /// @brief 存储当前是否可用。用于启动日志。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_ROOM_SNAPSHOT_READER_HPP
