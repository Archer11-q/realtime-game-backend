/// @file metrics_service.cpp
/// @brief `/metrics` 端点的服务实现（TASK-019）。契约见 api/proto/metrics.proto。

#include "common/metrics_service.hpp"

#include <string>

#include "common/logging.hpp"
#include "common/metrics.hpp"

namespace rgbt::common {

void MetricsServiceImpl::Scrape(::google::protobuf::RpcController* controller,
                                const rgbt::metrics::v1::ScrapeRequest* request,
                                rgbt::metrics::v1::ScrapeResponse* response,
                                ::google::protobuf::Closure* done) {
    // 无论成功失败都要把 done 跑掉，否则 brpc 的这次调用永远不返回。
    ::brpc::ClosureGuard done_guard(done);

    auto* cntl = static_cast<brpc::Controller*>(controller);

    // **响应体必须写进 attachment，不能写 `response->set_body()`。**
    // brpc 的 HTTP 响应体是 `Controller::response_attachment()`；通过 restful 映射
    // 暴露的 protobuf 响应会被**序列化成 JSON**。实测踩到：只设 protobuf 字段时，
    // `GET /metrics` 回的是 HTTP 200 + `{}` + 我们设的 Content-Type，
    // 而 Prometheus 会把它当成"空指标集"静默接受——最难发现的那类错误。
    // 既然 Prometheus 的文本暴露格式不是 protobuf 的 JSON 映射，就绕过协议转换，
    // 自己写原始响应体（brpc 文档 cn/http_service.md 第 3 节）。
    cntl->http_response().set_content_type("text/plain; version=0.0.4; charset=utf-8");

    // 失败**不影响业务**：这个端点与业务端点共用同一个 brpc 服务器，
    // 采集本身不该有任何机会拖慢或中断对局。
    const std::string body = Metrics().TextExposure();
    if (body.empty() && Metrics().MetricCount() > 0) {
        // 有指标却导出为空，只可能是内存分配失败——如实报错而不是回一个空的 200。
        // 空的 200 会让采集端把"服务没在报指标"和"指标本来就该是 0 条"混起来。
        cntl->http_response().set_status_code(500);
        response->set_error("导出指标失败（内存不足）");
        rgbt::common::LogError("metrics_expose_failed", request->request_id(),
                               {{"metrics", std::to_string(Metrics().MetricCount())}});
        return;
    }

    cntl->response_attachment().append(body);
}

}  // namespace rgbt::common
