/// @file metrics_service.hpp
/// @brief `/metrics` 端点的服务实现（TASK-019）。
///
/// 三个服务在各自的端口上注册同一个实现，因此三个服务都对外提供
/// Prometheus 文本格式的指标，而实现只有一份。

#ifndef RGBT_COMMON_METRICS_SERVICE_HPP
#define RGBT_COMMON_METRICS_SERVICE_HPP

#include <brpc/server.h>

#include "metrics.pb.h"

namespace rgbt::common {

/// 只做一件事：把 `Metrics()` 里的全部指标按 Prometheus 文本格式写回响应体。
///
/// 为什么不是 brpc 的 restful 映射函数而是一个 Service：restful 映射只能把
/// **已有的 RPC** 暴露成 HTTP 路径，它本身不产生 RPC。要让 `GET /metrics`
/// 在三个服务上都可用，最省的做法就是让三者注册同一个 Service 实现。
class MetricsServiceImpl : public rgbt::metrics::v1::MetricsService {
public:
    MetricsServiceImpl() = default;
    MetricsServiceImpl(const MetricsServiceImpl&) = delete;
    MetricsServiceImpl& operator=(const MetricsServiceImpl&) = delete;

    void Scrape(::google::protobuf::RpcController* controller,
                const rgbt::metrics::v1::ScrapeRequest* request,
                rgbt::metrics::v1::ScrapeResponse* response,
                ::google::protobuf::Closure* done) override;
};

}  // namespace rgbt::common

#endif  // RGBT_COMMON_METRICS_SERVICE_HPP
