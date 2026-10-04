/// @file room_client.hpp
/// @brief Gateway 调用 Room/Battle Service 的接口。
///
/// 为什么抽成接口（与 SessionStore、PlayerDirectory、MatchClient 的做法一致）：
///   1. 服务层的单元测试必须能在**不启动 Room 进程**的情况下覆盖成功、房间不存在、
///      结果待落库、存储不可用等全部分支，否则这些错误路径只能靠端到端脚本碰运气。
///   2. 传输方式（brpc channel、超时策略）是会变的东西，而「Gateway 如何解释房间
///      与对局结果」相对稳定，两者应该分开。
///
/// 这个接口刻意使用本服务自己的枚举，不暴露 room.pb.h：
/// Gateway 里除 brpc 实现外，没有任何代码需要知道 Room 的契约细节。

#ifndef RGBT_GATEWAY_ROOM_CLIENT_HPP
#define RGBT_GATEWAY_ROOM_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace rgbt::gateway {

/// 房间生命周期状态。取值与 api/proto/room.proto 的 RoomState 一一对应。
enum class RoomState {
    kCreated,
    kWaiting,
    kPlaying,
    kFinishing,
    kFinished,
    kAborted,
};

/// 玩家在对局中的权威状态。
struct RoomPlayerSnapshot {
    std::string player_id;
    std::int32_t hp = 0;
    /// 是否在房间内（已加入且未离开）。**不是**"网络是否连通"。
    bool connected = false;
    /// TASK-016：推送连接是否在线。断线后进入宽限期，期内对局暂停推进。
    bool online = true;
};

/// 房间权威状态快照。
struct RoomSnapshot {
    std::string room_id;
    std::string match_id;
    RoomState state = RoomState::kCreated;
    std::int64_t frame = 0;
    std::vector<RoomPlayerSnapshot> players;
    /// 结束原因的字符串形式：none / hp_zero / timeout / aborted。
    std::string finish_reason;
    /// 胜者 player_id。平局或未结束时为空。
    std::string winner_id;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
};

/// 对局结果。
struct MatchResultSnapshot {
    std::string match_id;
    std::string room_id;
    /// 胜者 player_id。**空字符串表示平局**，与「结果不存在」是两回事。
    std::string winner_id;
    std::int32_t player_count = 0;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
};

/// 查询对局结果的返回视图。
///
/// 用两个显式的 has_* 而不是把「没有结果」编码成空结构：平局的结果本身就是一个
/// 合法的、winner_id 为空的记录，用空值表示「不存在」会与平局混淆。
struct MatchResultView {
    bool has_result = false;
    MatchResultSnapshot result;
    /// 结果尚未落库时，Room 会一并返回房间快照，便于调用方判断对局是否已结束。
    bool has_room = false;
    RoomSnapshot room;
};

/// 调用 Room 的结果。
///
/// 必须区分 kUnavailable / kStoreUnavailable 与确定性失败：前者是对端或存储暂时
/// 故障，对外是 503 且可重试；后者重试没有意义。混在一起会让客户端做出错误的重试决策。
enum class RoomCallStatus {
    /// 调用成功。
    kOk,
    /// 传输失败或对端未启动，对外返回 503 room_unavailable。
    kUnavailable,
    /// 对端判定参数不合法，对外返回 400。
    kInvalidArgument,
    /// 该玩家不是这一局的成员，对外返回 400 not_a_member。
    ///
    /// 与 kInvalidArgument 分开的理由：Room 用**独立错误码**表达这件事
    /// （见 api/proto/room.proto 的 RoomErrorCode）。若在这里合并成一个值，
    /// 「不是本局成员」就会在跨进程后退化成「参数不合法」——调用方只映射错误码。
    kNotAMember,
    /// 对局尚未开始，此时不接受输入，对外返回 400 room_not_playing。
    kNotPlaying,
    /// 房间或结果不存在，对外返回 404。
    kNotFound,
    /// TASK-026：对端正在排空（`ROOM_SHUTTING_DOWN`）。理由同 MatchCallStatus。
    kShuttingDown,
    /// 对局已结束，不再接受该操作，对外返回 409。
    kAlreadyFinished,
    /// 对局已结束但结果尚未落库，对外返回 503 result_pending（可稍后重试）。
    kResultPending,
    /// 对端存储不可用，对外返回 503。
    kStoreUnavailable,
    /// 对端返回了未分类错误，对外返回 500。
    kInternal,
};

/// 历史快照查询的结果状态（TASK-017）。
///
/// 与 `RoomCallStatus` 是两个维度：后者说"这次调用成不成功"，本枚举说
/// "成功的前提下，缺的帧补得齐吗"。**不能合并**——「Room 不可用」（可重试）与
/// 「这段历史已经不在内存缓冲里了」（重试一万次也一样）对客户端是完全不同的结论。
enum class SnapshotWindowStatus {
    /// 区间被完整覆盖，`snapshots` 里是全部缺失帧（帧号严格递增且不重复）。
    kReady,
    /// 需要的起点早于环形缓冲：中间有一段已经不可恢复。
    kIncomplete,
    /// 客户端已有的帧号晚于服务端当前帧号（状态不一致）。
    kAhead,
};

/// 一段历史快照。
struct SnapshotRange {
    SnapshotWindowStatus status = SnapshotWindowStatus::kReady;
    /// 帧号严格递增且不重复。仅 `kReady` 时保证完整。
    std::vector<RoomSnapshot> snapshots;
    /// 环形缓冲当前的帧号区间，用于向客户端解释"为什么补不齐"。缓冲为空时为 0。
    std::int64_t oldest_frame = 0;
    std::int64_t latest_frame = 0;
};

/// Gateway 侧的房间客户端接口。
class RoomClient {
public:
    RoomClient() = default;
    RoomClient(const RoomClient&) = delete;
    RoomClient& operator=(const RoomClient&) = delete;
    RoomClient(RoomClient&&) = delete;
    RoomClient& operator=(RoomClient&&) = delete;
    virtual ~RoomClient() = default;

    /// @brief 加入房间。
    /// @param player_id 必须来自会话，不能来自客户端请求体。
    virtual RoomCallStatus Join(const std::string& room_id, const std::string& player_id,
                                const std::string& request_id, RoomSnapshot* out_snapshot) = 0;

    /// @brief 提交一次攻击输入。
    ///
    /// 不带输入类型参数：TASK-008 只有「攻击」一种输入，提前暴露一个只有一个取值的
    /// 枚举参数属于未经验证的抽象。新增输入类型时再扩展本接口。
    virtual RoomCallStatus SubmitAttack(const std::string& room_id, const std::string& player_id,
                                        const std::string& request_id,
                                        RoomSnapshot* out_snapshot) = 0;

    /// @brief 查询房间权威状态快照。
    virtual RoomCallStatus GetState(const std::string& room_id, const std::string& request_id,
                                    RoomSnapshot* out_snapshot) = 0;

    /// @brief 查询对局结果。
    virtual RoomCallStatus GetResult(const std::string& match_id, const std::string& request_id,
                                     MatchResultView* out_view) = 0;

    /// @brief 查询帧号大于 `since_frame` 的历史快照（TASK-017）。
    ///
    /// 由 StreamHub 在建立带 `Last-Event-ID` 的订阅时调用，用于补发客户端错过的帧。
    /// 返回 kOk 表示调用成功；返回其他值时 `out_range` 的内容不可用。
    virtual RoomCallStatus GetSnapshotsSince(const std::string& room_id, std::int64_t since_frame,
                                             const std::string& request_id,
                                             SnapshotRange* out_range) = 0;

    /// @brief 上报某个玩家的推送连接状态（TASK-016）。
    ///
    /// 由 StreamHub 在订阅建立/移除时调用。**这不是幂等键语义上的"请求"**：
    /// Room 侧对同一状态重复上报无副作用，因此调用方不需要重试或补偿——
    /// 下一次连接状态变化会自然覆盖它。
    virtual RoomCallStatus SetPresence(const std::string& room_id, const std::string& player_id,
                                       bool online, const std::string& request_id,
                                       RoomSnapshot* out_snapshot) = 0;

    /// @brief Room 当前是否可用。用于启动日志，不做真实往返调用。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_ROOM_CLIENT_HPP
