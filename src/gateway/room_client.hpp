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
    bool connected = false;
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
    /// 房间或结果不存在，对外返回 404。
    kNotFound,
    /// 对局已结束，不再接受该操作，对外返回 409。
    kAlreadyFinished,
    /// 对局已结束但结果尚未落库，对外返回 503 result_pending（可稍后重试）。
    kResultPending,
    /// 对端存储不可用，对外返回 503。
    kStoreUnavailable,
    /// 对端返回了未分类错误，对外返回 500。
    kInternal,
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
                                RoomSnapshot* out_snapshot) = 0;

    /// @brief 提交一次攻击输入。
    ///
    /// 不带输入类型参数：TASK-008 只有「攻击」一种输入，提前暴露一个只有一个取值的
    /// 枚举参数属于未经验证的抽象。新增输入类型时再扩展本接口。
    virtual RoomCallStatus SubmitAttack(const std::string& room_id, const std::string& player_id,
                                        RoomSnapshot* out_snapshot) = 0;

    /// @brief 查询房间权威状态快照。
    virtual RoomCallStatus GetState(const std::string& room_id, RoomSnapshot* out_snapshot) = 0;

    /// @brief 查询对局结果。
    virtual RoomCallStatus GetResult(const std::string& match_id, MatchResultView* out_view) = 0;

    /// @brief Room 当前是否可用。用于启动日志，不做真实往返调用。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_ROOM_CLIENT_HPP
