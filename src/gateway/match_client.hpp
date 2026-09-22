/// @file match_client.hpp
/// @brief Gateway 调用 Match Service 的接口。
///
/// 为什么抽成接口（与 SessionStore、PlayerDirectory 的做法一致）：
///   1. 服务层的单元测试必须能在**不启动 Match 进程**的情况下覆盖成功、队列已满、
///      依赖不可用等全部分支，否则这些错误路径只能靠端到端脚本碰运气。
///   2. 匹配的传输方式（brpc channel、连接池、超时策略）是会变的东西，
///      而「Gateway 如何解释匹配结果」相对稳定，两者应该分开。
///
/// 这个接口刻意使用本服务自己的枚举，不暴露 match.pb.h：
/// Gateway 里除 brpc 实现外，没有任何代码需要知道 Match 的契约细节。

#ifndef RGBT_GATEWAY_MATCH_CLIENT_HPP
#define RGBT_GATEWAY_MATCH_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace rgbt::gateway {

/// 匹配状态。取值与 api/proto/match.proto 的 MatchState 一一对应。
enum class MatchState {
    /// 不在队列中，也没有待领取的结果。
    kIdle,
    /// 排队中。
    kQueued,
    /// 已配对成功，match_id 与 room_id 有效。
    kMatched,
    /// 排队超时被淘汰，需要重新入队。
    kTimeout,
};

/// 匹配状态快照。
struct MatchSnapshot {
    MatchState state = MatchState::kIdle;
    std::string match_id;
    std::string room_id;
    std::vector<std::string> player_ids;
    std::int64_t queued_at_ms = 0;
    std::int32_t queue_size = 0;
};

/// 调用 Match 的结果。
///
/// 必须区分 kUnavailable 与其他失败：前者是依赖故障，对外是 503 且可重试；
/// 后者是确定性结果，重试没有意义。混在一起会让客户端做出错误的重试决策。
enum class MatchCallStatus {
    /// 调用成功。包含 Match 侧返回的「已在队列」，因为那是幂等的正常结果。
    kOk,
    /// 传输失败或对端未启动，对外返回 503 match_unavailable。
    kUnavailable,
    /// 对端判定参数不合法，对外返回 400。
    kInvalidArgument,
    /// 队列已满，属限流类错误，对外返回 429。
    kQueueFull,
    /// 对端返回了未分类错误，对外返回 500。
    kInternal,
};

/// Gateway 侧的匹配客户端接口。
class MatchClient {
public:
    MatchClient() = default;
    MatchClient(const MatchClient&) = delete;
    MatchClient& operator=(const MatchClient&) = delete;
    MatchClient(MatchClient&&) = delete;
    MatchClient& operator=(MatchClient&&) = delete;
    virtual ~MatchClient() = default;

    /// @brief 进入匹配队列。
    /// @param player_id 必须来自会话，不能来自客户端请求体。
    virtual MatchCallStatus Enqueue(const std::string& player_id, const std::string& request_id,
                                    MatchSnapshot* out_snapshot) = 0;

    /// @brief 查询匹配状态。
    virtual MatchCallStatus GetStatus(const std::string& player_id,
                                      MatchSnapshot* out_snapshot) = 0;

    /// @brief 取消匹配。
    virtual MatchCallStatus Cancel(const std::string& player_id, const std::string& request_id,
                                   MatchSnapshot* out_snapshot) = 0;

    /// @brief Match 当前是否可用。用于启动日志与健康检查，不做真实往返调用。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_MATCH_CLIENT_HPP
