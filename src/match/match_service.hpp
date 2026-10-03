/// @file match_service.hpp
/// @brief MatchService 的 brpc 实现。
///
/// 职责边界：本文件只做「协议转换 + 错误码映射」，队列与配对逻辑全在 MatchQueue 中。
/// 这样做的原因是配对规则需要被大量单元测试覆盖，而 brpc 服务类不方便直接测试；
/// 把逻辑放在不依赖框架的类里，测试就不需要起服务器。
///
/// 时间由 `clock_` 注入，默认取系统时间。测试可以注入固定时钟，从而精确验证超时
/// 与结果过期，而不需要 sleep。

#ifndef RGBT_MATCH_MATCH_SERVICE_HPP
#define RGBT_MATCH_MATCH_SERVICE_HPP

#include <cstdint>
#include <functional>

#include "common/metrics.hpp"
#include "match.pb.h"
#include "match_queue.hpp"

namespace rgbt::match {

/// @brief 当前 Unix 毫秒时间戳。
[[nodiscard]] std::int64_t NowUnixMillis();

class MatchServiceImpl : public rgbt::match::v1::MatchService {
public:
    /// @param queue 队列，生命周期由调用方保证，不能为空。
    explicit MatchServiceImpl(MatchQueue* queue, std::function<std::int64_t()> clock = {});

    void EnqueueMatch(::google::protobuf::RpcController* controller,
                      const rgbt::match::v1::EnqueueMatchRequest* request,
                      rgbt::match::v1::EnqueueMatchResponse* response,
                      ::google::protobuf::Closure* done) override;

    void GetMatchStatus(::google::protobuf::RpcController* controller,
                        const rgbt::match::v1::GetMatchStatusRequest* request,
                        rgbt::match::v1::GetMatchStatusResponse* response,
                        ::google::protobuf::Closure* done) override;

    void CancelMatch(::google::protobuf::RpcController* controller,
                     const rgbt::match::v1::CancelMatchRequest* request,
                     rgbt::match::v1::CancelMatchResponse* response,
                     ::google::protobuf::Closure* done) override;

    /// @brief 队列是否可用。当前队列在进程内存中，只要对象存在就是可用。
    [[nodiscard]] bool Healthy() const { return queue_ != nullptr; }

    /// TASK-019：只读的指标取值口，供单元测试断言"记账真的发生了"。
    /// 测试不解析文本导出——那会把格式与语义耦合在一起。
    [[nodiscard]] std::uint64_t EnqueuedCount() const { return event_enqueued_.Value(); }
    [[nodiscard]] std::uint64_t RejectedCount() const { return event_rejected_.Value(); }
    [[nodiscard]] std::uint64_t PairedCount() const { return event_paired_.Value(); }
    [[nodiscard]] std::uint64_t CanceledCount() const { return event_canceled_.Value(); }

private:
    /// 写统一错误体。返回值恒为 false，便于在调用点直接 return。
    static bool FillError(rgbt::match::v1::MatchError* error, rgbt::match::v1::MatchErrorCode code,
                          const std::string& reason, const std::string& message,
                          const std::string& request_id);

    /// 把队列快照写入 proto 响应。
    static void FillStatus(const MatchStatusSnapshot& snapshot, rgbt::match::v1::MatchStatus* out);

    // TASK-019：事件计数。记账放在**服务层**而不是 MatchQueue 里：
    // 四种入队结果在这里是一个统一的 `outcome` 变量，一处插入就能覆盖全部分支；
    // 而队列内部的返回点分散在多条路径上（实测：分散到 4 处以上，且形式不统一），
    // 漏掉任何一处都不会报错，只会让计数偏低。服务层是这次调用的唯一出口。
    rgbt::common::CounterHandle event_enqueued_;
    rgbt::common::CounterHandle event_rejected_;
    rgbt::common::CounterHandle event_paired_;
    rgbt::common::CounterHandle event_canceled_;
    bool metrics_ready_ = false;

    MatchQueue* queue_;
    std::function<std::int64_t()> clock_;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_MATCH_SERVICE_HPP
