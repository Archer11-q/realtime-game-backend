/// @file brpc_match_client.hpp
/// @brief MatchClient 的 brpc 实现。
///
/// 这是本项目**第一次真正发起服务间 RPC**：此前的 brpc 只用于把 Gateway 自己
/// 暴露成 HTTP 服务，没有任何对外调用。因此这里显式处理三件以前没遇到过的事：
///
///   1. **超时**必须设置。brpc 默认超时是 500ms；依赖抖动时应尽快失败并返回 503，
///      而不是把 Gateway 的工作线程挂在一次调用上。
///   2. **连接失败与业务错误必须分开**。`controller.Failed()` 表示传输层失败
///      （对端未启动、超时），要映射成 503；对端正常返回的 error 字段是业务结果，
///      要按各自的语义映射。混在一起会把「队列已满」误报成「服务不可用」。
///   3. **不重试**。入队不是天然幂等的写操作吗？是——但重试策略应由调用方按错误
///      类型决定，而不是在这里无脑重放。当前 Gateway 对 Match 的调用不做自动重试，
///      失败即返回 503，由客户端决定是否重新发起。

#ifndef RGBT_GATEWAY_BRPC_MATCH_CLIENT_HPP
#define RGBT_GATEWAY_BRPC_MATCH_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "match_client.hpp"

namespace rgbt::gateway {

/// brpc 连接的参数。
struct MatchClientOptions {
    std::string host = "127.0.0.1";

    std::int32_t port = 8082;

    /// 单次 RPC 超时（毫秒）。取值必须明显小于客户端自己的超时，
    /// 否则依赖故障会表现为「请求悬挂」而不是「快速失败」。
    std::int32_t timeout_ms = 500;

    /// 连接超时（毫秒）。
    std::int32_t connect_timeout_ms = 300;
};

/// 通过 brpc channel 调用 MatchService。
class BrpcMatchClient final : public MatchClient {
public:
    explicit BrpcMatchClient(MatchClientOptions options);

    ~BrpcMatchClient() override;

    BrpcMatchClient(const BrpcMatchClient&) = delete;
    BrpcMatchClient& operator=(const BrpcMatchClient&) = delete;

    MatchCallStatus Enqueue(const std::string& player_id, const std::string& request_id,
                            MatchSnapshot* out_snapshot) override;

    MatchCallStatus GetStatus(const std::string& player_id, MatchSnapshot* out_snapshot) override;

    MatchCallStatus Cancel(const std::string& player_id, const std::string& request_id,
                           MatchSnapshot* out_snapshot) override;

    [[nodiscard]] bool IsHealthy() override;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_BRPC_MATCH_CLIENT_HPP
