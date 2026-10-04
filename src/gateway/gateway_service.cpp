#include "gateway_service.hpp"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/http_status_code.h>
#include <brpc/progressive_attachment.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "common/logging.hpp"
#include "common/metrics.hpp"
#include "common/token.hpp"
#include "error.hpp"

namespace rgbt::gateway {
namespace {

using rgbt::gateway::v1::ErrorCode;

/// 把 SSE 事件写进 brpc 的持续响应体。
///
/// 这是 EventSink 的唯一生产实现。它必须持有 ProgressiveAttachment 的
/// intrusive_ptr——`brpc::Controller` 在请求返回后就不再可用，而文档明确要求
/// 「发送完毕后确保所有 intrusive_ptr 都析构以释放资源」。
/// 因此释放最后一个引用（Close）就是结束这条响应。
class ProgressiveAttachmentSink final : public EventSink {
public:
    explicit ProgressiveAttachmentSink(butil::intrusive_ptr<brpc::ProgressiveAttachment> attachment)
        : attachment_(std::move(attachment)) {}

    [[nodiscard]] bool Write(const std::string& data) override {
        if (attachment_ == nullptr) {
            return false;
        }
        // 返回 0 表示写成功；非 0 表示连接已断或响应已关闭，此时调用方清理订阅。
        return attachment_->Write(data.data(), data.size()) == 0;
    }

    void Close() override { attachment_.reset(); }

private:
    butil::intrusive_ptr<brpc::ProgressiveAttachment> attachment_;
};

/// 把业务状态码同步到 HTTP 响应上。
///
/// 背景：brpc 把 protobuf 响应序列化为 JSON 作为 body，但 HTTP 状态码默认恒为
/// 200，业务状态只体现在 body 里。依据 docs/05-api-and-data.md 第 3 节，错误
/// 必须能被调用方在传输层识别，因此这里显式设置 HTTP 状态码。
///
/// 仅在 HTTP 请求上下文中有意义：通过 RPC 调用本服务时没有 HTTP 响应，
/// 此时跳过设置。
void ApplyHttpStatus(::google::protobuf::RpcController* controller, std::int32_t status_code) {
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return;
    }
    cntl->http_response().set_status_code(status_code);
}

/// 从 HTTP 请求中取出 Token。
///
/// 为什么需要这个函数：brpc 只把 HTTP body（JSON）映射到 protobuf 字段，
/// **不会**把 HTTP 头映射进任何字段（见官方文档 http_service.md 中 headers 与
/// query string 的说明）。因此 `Authorization` 头必须由服务自己读取，否则请求中的
/// token 字段永远是空字符串，表现为「所有携带 Token 的请求都报 token_required」。
///
/// 取值顺序：
///   1. 请求体中的 token 字段（纯 RPC 调用或显式传参时使用）；
///   2. `Authorization: Bearer <token>`，HTTP 客户端的标准做法；
///   3. query string 的 `token`，便于用浏览器或 curl 直接调试。
///
/// 无 HTTP 上下文时（例如单元测试直接调用服务）只使用请求体字段。
std::string ExtractToken(::google::protobuf::RpcController* controller,
                         const std::string& body_token) {
    if (!body_token.empty()) {
        return body_token;
    }
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return {};
    }
    const brpc::HttpHeader& header = cntl->http_request();

    if (const std::string* auth = header.GetHeader("Authorization"); auth != nullptr) {
        // 接受 "Bearer <token>" 与直接给出 token 两种形式，前缀大小写不敏感。
        constexpr std::string_view kBearer = "bearer ";
        if (auth->size() > kBearer.size()) {
            std::string prefix = auth->substr(0, kBearer.size());
            std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (prefix == kBearer) {
                return auth->substr(kBearer.size());
            }
        }
        return *auth;
    }

    if (const std::string* query_token = header.uri().GetQuery("token"); query_token != nullptr) {
        return *query_token;
    }

    return {};
}

/// 从 query string 中取一个参数。
///
/// 与 ExtractToken 同样的理由：brpc 只把 HTTP body（JSON）映射到 protobuf 字段，
/// **query string 不会被映射进任何字段**。TASK-008 的房间与结果接口把 room_id /
/// match_id 放在查询参数里（因为 brpc 的 restful 映射不支持 `{name}` 路径参数，
/// 而 `*` 通配又会与 /rooms/join 这类固定子路径冲突），因此必须在这里显式读取，
/// 否则请求里的同名字段永远是空字符串，表现为「所有查询都报 xxx_required」。
///
/// 取值顺序与 ExtractToken 一致：请求体优先（纯 RPC 调用），其次 query string。
/// 无 HTTP 上下文时（单元测试直接调用服务）只使用请求体字段。
std::string ExtractQueryParam(::google::protobuf::RpcController* controller, const char* name,
                              const std::string& body_value) {
    if (!body_value.empty()) {
        return body_value;
    }
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return {};
    }
    if (const std::string* value = cntl->http_request().uri().GetQuery(name); value != nullptr) {
        return *value;
    }
    return {};
}

/// `Last-Event-ID` 的解析结果（TASK-017）。
///
/// **不需要单独一个 `has_header` 字段**：调用方（以及 StreamHub 的 SubscribeOptions）
/// 只关心两件事——"有没有一个可用的帧号"（`value`）与"头是不是存在但坏了"
/// （`malformed`）。再加一个布尔值只会多出一个必须保持一致的状态组合。
struct LastEventIdHeader {
    /// 解析出来的帧号；`malformed == true` 时无意义。
    std::int64_t value = 0;
    /// 头存在，但解析不成合法的非负整数。
    bool malformed = false;
};

/// 从 HTTP 请求中取出 SSE 的 `Last-Event-ID` 头（TASK-017）。
///
/// `Last-Event-ID` 是 SSE 规范定义的请求头：浏览器/客户端重连时把它设为**最后收到的
/// 那个 `id:` 值**，服务端据此补发断线期间错过的事件。本项目的 `id:` 就是房间帧号，
/// 因此这里解析成一个非负整数。
///
/// 解析规则与理由：
///   * 头不存在 → `value == 0 && !malformed`。这是"第一次订阅"，不是错误。
///   * 头存在但为空或不是合法的非负十进制整数 → `malformed == true`。
///     明确区分它与"头不存在"：前者是客户端在说自己有状态却说不清是哪个，
///     静默当成首次订阅会让它以为自己拿到了连续的事件。
///   * 负数、带符号、超长数字按非法处理：帧号由服务端从 0 单调生成，不可能是负数。
///
/// 为什么在这里解析而不是在 StreamHub 里：只有这里能读到 HTTP 头，而 StreamHub
/// 不依赖 brpc（它要能被单元测试直接驱动）。把 HTTP 细节留在服务层是本项目一贯的分层。
LastEventIdHeader ParseLastEventId(::google::protobuf::RpcController* controller) {
    LastEventIdHeader result;
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return result;  // 无 HTTP 上下文（单元测试直接调用服务）：等同"没有这个头"。
    }
    const std::string* raw = cntl->http_request().GetHeader("Last-Event-ID");
    if (raw == nullptr) {
        return result;
    }
    if (raw->empty() || raw->size() > 18) {  // 18 位十进制足够表达任何 int64 帧号
        result.malformed = true;
        return result;
    }
    std::int64_t value = 0;
    for (const char ch : *raw) {
        if (ch < '0' || ch > '9') {
            result.malformed = true;
            return result;
        }
        value = value * 10 + (ch - '0');
    }
    result.value = value;
    return result;
}

/// 校验登录请求的输入。返回空字符串表示通过，否则返回失败原因。
std::string ValidateLoginRequest(const rgbt::gateway::v1::LoginRequest& request) {
    if (request.account().empty()) {
        return "account_required";
    }
    if (request.account().size() > kMaxAccountLength) {
        return "account_too_long";
    }
    if (request.password().empty()) {
        return "password_required";
    }
    if (request.password().size() > kMaxPasswordLength) {
        return "password_too_long";
    }
    if (request.request_id().size() > kMaxRequestIdLength) {
        return "request_id_too_long";
    }
    // client_type 允许为空，视为 web；非空时只接受已知取值。
    const std::string& client_type = request.client_type();
    if (!client_type.empty() && client_type != "web" && client_type != "bot") {
        return "unsupported_client_type";
    }
    return {};
}

/// 校验 Token 格式。返回空字符串表示通过。
std::string ValidateToken(const std::string& token) {
    if (token.empty()) {
        return "token_required";
    }
    if (token.size() > kMaxTokenLength) {
        return "token_too_long";
    }
    if (!rgbt::common::IsValidTokenFormat(token)) {
        return "token_malformed";
    }
    return {};
}

/// 匹配状态 -> 对外字符串。
///
/// 对外用字符串而不是枚举数字：浏览器直接可用，且新增状态时不需要客户端同步升级
/// 枚举定义。字符串取值与 api/proto/match.proto 的枚举名一一对应。
std::string MatchStateName(MatchState state) {
    switch (state) {
        case MatchState::kIdle:
            return "idle";
        case MatchState::kQueued:
            return "queued";
        case MatchState::kMatched:
            return "matched";
        case MatchState::kTimeout:
            return "timeout";
    }
    return "idle";
}

}  // namespace

/// TASK-021：HTTP 状态码 -> 日志级别。
///
/// 为什么按状态码分级而不是一律 `info`：trace 的用途是"顺着一条链把一次请求看
/// 完"，而 `grep 'level=error'` 是最常用的第一刀。全记成 info 会让这一刀失效
/// ——4xx/5xx 混在几百条成功行里，等于把"出问题了"这件事藏起来。
rgbt::common::LogLevel LogLevelForStatus(std::int32_t status_code) {
    if (status_code >= 500) {
        return rgbt::common::LogLevel::kError;
    }
    if (status_code >= 400) {
        return rgbt::common::LogLevel::kWarn;
    }
    return rgbt::common::LogLevel::kInfo;
}

/// TASK-021：把 restful 路径映射成稳定的操作名（`op=` 字段）。
///
/// **为什么要有 op，而不只记 `path=`**：`path` 是**接口路径**，`op` 是**动作名**。
/// 排障时的问法是"这次登录为什么失败"，而不是"这个 URI 为什么失败"；动作名也是
/// 唯一能在三个服务的日志之间对齐的粒度（Room 记的是 `room_joined`，
/// 与 Gateway 的 `op=join_room` 指的是同一次调用）。
///
/// 取值与 `gateway_main.cpp` 的 `restful_mappings` **一一对应**，那张表是本仓库
/// 路径的唯一真相来源；这里是它的只读视图。因此新增接口时必须同时加两处——
/// 与 TASK-019 的路径标签是同一个约束。写错的后果是 `op=unknown`（而不是崩溃
/// 或一条假的操作名），因此它是可发现的、不会误导。
///
/// 找不到就返回 `unknown`：**不编造**一个看起来合理的名字。`unknown` 本身
/// 就是"这里漏了一个映射"的信号。
const char* OperationForPath(std::string_view path) {
    struct Mapping {
        std::string_view path;
        const char* op;
    };
    static constexpr Mapping kMappings[] = {
        {"/api/v1/login", "login"},
        {"/api/v1/players/me", "get_current_player"},
        {"/api/v1/logout", "logout"},
        {"/api/v1/matches", "enqueue_match"},
        {"/api/v1/matches/current", "get_match_status"},
        {"/api/v1/matches/current/cancel", "cancel_match"},
        {"/api/v1/rooms/join", "join_room"},
        {"/api/v1/rooms/input", "submit_input"},
        {"/api/v1/rooms/state", "get_room_state"},
        {"/api/v1/results", "get_match_result"},
        {"/api/v1/stream", "stream_events"},
    };
    for (const Mapping& mapping : kMappings) {
        if (mapping.path == path) {
            return mapping.op;
        }
    }
    return "unknown";
}

/// TASK-019：把一次 HTTP 请求记到指标上。
///
/// 路径标签取 **restful 映射里的模板路径**（`/api/v1/matches`、`/api/v1/rooms/state`…），
/// 而不是原始 URI。原始 URI 带 query string（`?room_id=...`），把它当标签会让标签
/// 基数随房间数无限增长，最终把 Prometheus 拖垮——这是"标签基数"最常见的坑。
///
/// 只对 `/api/v1/` 前缀的路径记账：brpc 内置端点（`/status`、`/vars`、
/// `/metrics`）也会走到这里，混进业务 QPS 会让面板虚高。
///
/// 句柄按 (path, status) 缓存：`Metrics().Counter()` 每次都要加锁查表，
/// 直接放在请求路径上会在高 QPS 时成为热点。缓存后只剩一次哈希查找 + 一次原子加。
void GatewayServiceImpl::RecordHttpRequest(::google::protobuf::RpcController* controller,
                                           std::int32_t status_code) noexcept {
    if (!metrics_ready_ || controller == nullptr) {
        return;
    }
    const auto* cntl = static_cast<const brpc::Controller*>(controller);
    if (!cntl->has_http_request()) {
        return;
    }
    const std::string path(cntl->http_request().uri().path());
    if (path.rfind("/api/v1/", 0) != 0) {
        return;
    }

    static std::mutex cache_mutex;
    static std::unordered_map<std::string, rgbt::common::CounterHandle> cache;
    const std::string key = path + "|" + std::to_string(status_code);

    rgbt::common::CounterHandle handle;
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(key);
        if (it == cache.end()) {
            handle = rgbt::common::Metrics().Counter(
                rgbt::common::kMetricHttpRequestsTotal,
                "Gateway 处理的 HTTP 请求数，按映射路径与 HTTP 状态码分组",
                {{"path", path}, {"status", std::to_string(status_code)}});
            cache.emplace(key, handle);
        } else {
            handle = it->second;
        }
    }
    handle.Add();
}

/// TASK-021：每个 HTTP 请求结束时留一条可按 trace 检索的记录。
///
/// **为什么放在收敛点而不是每个处理函数里**：`ApplyHttpStatusAndRecord` 是
/// Gateway 所有 HTTP 响应（含成功路径）的唯一出口，放在这里"漏记"就不可能发生。
/// 此前 15 个接口里只有 2 个有结构化日志——登录、进房、结算三条关键路径在
/// Gateway 侧一条可检索记录都没有，"按 trace 取出一条完整调用序"因此做不到。
///
/// 无 HTTP 上下文时（单元测试直接调用服务）读不到路径，此时记 `op=unknown`：
/// 仍然留痕，只是没有路径可推导。这不是错误路径，不需要特殊处理。
///
/// 显式构造 `LogRecord` 而不是用 `LogInfo(...)` 便捷入口：级别由状态码决定，
/// 便捷入口是按级别的三个函数，这里不想把同一个调用点复制三份。
void GatewayServiceImpl::RecordRequestDone(::google::protobuf::RpcController* controller,
                                           std::int32_t status_code,
                                           const std::string& request_id) noexcept {
    std::string op = "unknown";
    if (const auto* cntl = static_cast<const brpc::Controller*>(controller);
        cntl != nullptr && cntl->has_http_request()) {
        op = OperationForPath(cntl->http_request().uri().path());
    }

    rgbt::common::LogRecord record;
    record.trace_id = request_id;
    record.level = LogLevelForStatus(status_code);
    record.event = "request_done";
    record.fields = {{"op", op}, {"status", std::to_string(status_code)}};
    rgbt::common::Log(record);
}

/// TASK-019 + TASK-021 + TASK-022：设置状态码、记账、留一条 `request_done`、
/// 记录耗时。所有处理函数都走这里，因此这四件事都不会被漏掉。
///
/// **TASK-022 的耗时为什么必须由调用方传开始时刻**：brpc 服务端**拿不到**请求的
/// 开始时间——`start_realtime_us` 只是客户端 `IssueRPC(int64_t)` 的参数，不是
/// Controller 的访问器；而 `latency_us()` 在服务端的语义是"处理前的排队时间"
/// （头文件原话：`it gets queue time before server processes the RPC call`），
/// 不能当处理耗时用。因此开始时刻只能在每个处理函数入口取一次。
/// `start_us == 0` 表示调用方没计时（单元测试路径），此时不观测，也不写
/// `duration_ms`——**不做"假装记了 0 秒"**那种会污染分位数的处理。
void GatewayServiceImpl::ApplyHttpStatusAndRecord(::google::protobuf::RpcController* controller,
                                                  std::int32_t status_code,
                                                  const std::string& request_id,
                                                  std::int64_t start_us) {
    ApplyHttpStatus(controller, status_code);
    RecordHttpRequest(controller, status_code);
    RecordRequestDone(controller, status_code, request_id);
    RecordHttpLatency(controller, start_us);
}

/// TASK-022：把一次 HTTP 请求的处理耗时写进直方图。
///
/// 与 `RecordHttpRequest` 一样只对 `/api/v1/` 记账：brpc 内置端点（`/status`、
/// `/metrics`、`/vars`）不是业务请求，混进延迟分位数会让 SLO 读数字变好看，
/// 而那不是被测对象。
///
/// 用**自定义桶**（`kHttpLatencyBucketBounds`）而不是默认桶：后者的粒度让
/// P95 < 100 ms 这条 SLO 无法判定，理由见 `metrics.hpp` 里该常量的注释。
///
/// 不带 `status` 维度：Prometheus 的惯例是"延迟由一个无状态标签的直方图描述，
/// 错误率另由一个计数器描述"。把状态码拼进直方图会立刻带来 3 倍的时间序列数，
/// 却只能回答"错误请求更快还是更慢"这种没人问的问题。
void GatewayServiceImpl::RecordHttpLatency(::google::protobuf::RpcController* controller,
                                           std::int64_t start_us) noexcept {
    if (start_us <= 0 || controller == nullptr) {
        return;
    }
    const auto* cntl = static_cast<const brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return;
    }
    const std::string path(cntl->http_request().uri().path());
    if (path.rfind("/api/v1/", 0) != 0) {
        return;
    }

    const std::int64_t elapsed_us = rgbt::common::NowUs() - start_us;
    // 负值只可能来自时钟回退。归零会让它落进第一个桶、把分位数拉低，
    // 因此比"不记"更糟；这里直接丢弃这一次观测（指标层另有负值归零的兜底，
    // 那条兜底是给"调用方忘了判断"用的，不该被我们自己依赖）。
    if (elapsed_us < 0) {
        return;
    }

    rgbt::common::Metrics().Observe(
        rgbt::common::kMetricHttpRequestSeconds, "Gateway 的 HTTP 处理耗时（秒），按业务路径计",
        static_cast<double>(elapsed_us) / 1'000'000.0,
        {rgbt::common::kHttpLatencyBucketBounds[0], rgbt::common::kHttpLatencyBucketBounds[1],
         rgbt::common::kHttpLatencyBucketBounds[2], rgbt::common::kHttpLatencyBucketBounds[3],
         rgbt::common::kHttpLatencyBucketBounds[4], rgbt::common::kHttpLatencyBucketBounds[5],
         rgbt::common::kHttpLatencyBucketBounds[6], rgbt::common::kHttpLatencyBucketBounds[7],
         rgbt::common::kHttpLatencyBucketBounds[8], rgbt::common::kHttpLatencyBucketBounds[9],
         rgbt::common::kHttpLatencyBucketBounds[10]},
        // `path` 是**固定枚举**（11 条映射），与 `rgbt_http_requests_total` 用的是
        // 同一个值，因此可以直接据此对齐两个指标、算"每个端点的 P95"。
        // 带上它的理由见 metrics.hpp：没有它就只能给出"所有端点混在一起"的分位数，
        // 而"哪个端点慢"正是容量报告要回答的第一个问题（TASK-022 实测踩到）。
        {{"path", path}});
}

GatewayServiceImpl::GatewayServiceImpl(SessionStore* sessions, PlayerDirectory* players,
                                       MatchClient* match, RoomClient* room, StreamHub* stream,
                                       std::int32_t session_ttl_seconds)
    : sessions_(sessions),
      players_(players),
      match_(match),
      room_(room),
      stream_(stream),
      session_ttl_seconds_(session_ttl_seconds > 0 ? session_ttl_seconds
                                                   : kDefaultSessionTtlSeconds) {
    // TASK-019：SSE 相关指标。全部用**回调式 gauge** 读 StreamHub 已有的累计量
    // （理由见本文件顶部关于"单一真相来源"的说明）。
    rgbt::common::Metrics().Gauge(
        rgbt::common::kMetricSseConnections, "当前 SSE 连接数", [this]() -> std::uint64_t {
            return stream_ == nullptr ? 0 : static_cast<std::uint64_t>(stream_->ConnectionCount());
        });

    // 名字里不带 `_total`：它按惯例表示 counter，而这里读的是别人维护的累计量，
    // 类型上仍声明为 gauge。宁可名字朴素，也不制造第二份计数。
    rgbt::common::Metrics().Gauge(
        rgbt::common::kMetricPushBackfilledFrames, "累计补发出去的推送帧数",
        [this]() -> std::uint64_t {
            return stream_ == nullptr ? 0
                                      : static_cast<std::uint64_t>(stream_->BackfilledFrameCount());
        });

    // 每个 reason 都登记（即使从未发生）：否则"这个原因还没出现过"在面板上表现为
    // "这条曲线不存在"，无法与"指标坏了"区分。
    for (const char* reason : {"id_malformed", "id_ahead", "id_out_of_window", "id_current"}) {
        rgbt::common::Metrics().Gauge(
            rgbt::common::kMetricPushResetTotal, "累计发出 stream.reset 的次数，按原因分组",
            [this, reason]() -> std::uint64_t {
                return stream_ == nullptr
                           ? 0
                           : static_cast<std::uint64_t>(stream_->ResetEventCount(reason));
            },
            {{"reason", reason}});
    }

    // TASK-019：构造完成后才开始记账。登记表是进程级的，句柄在首次使用时解析，
    // 这里只是把"可以记账了"这个事实记下来。
    metrics_ready_ = true;
}

std::int32_t GatewayServiceImpl::FillError(rgbt::gateway::v1::Error* error,
                                           rgbt::gateway::v1::ErrorCode code,
                                           const std::string& reason, const std::string& message,
                                           const std::string& request_id) {
    if (error != nullptr) {
        error->set_code(code);
        error->set_reason(reason);
        error->set_message(message);
        error->set_request_id(request_id);
    }
    return rgbt::gateway::HttpStatusOf(code);
}

void GatewayServiceImpl::Login(::google::protobuf::RpcController* controller,
                               const rgbt::gateway::v1::LoginRequest* request,
                               rgbt::gateway::v1::LoginResponse* response,
                               ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string request_id = request->request_id();

    // 第一步：输入校验。输入错误不重试。
    const std::string input_error = ValidateLoginRequest(*request);
    if (!input_error.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, input_error,
                      "登录请求参数不合法", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // 第二步：凭据校验。先鉴权再碰依赖，避免为无效凭据产生会话写入。
    rgbt::gateway::v1::PlayerInfo player;
    const CredentialStatus credential =
        players_->Authenticate(request->account(), request->password(), &player);
    if (credential == CredentialStatus::kInvalidCredential) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAUTHENTICATED,
                                              "invalid_credential", "账号或密码不正确", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }
    if (credential == CredentialStatus::kAccountDisabled) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAUTHENTICATED,
                                              "account_disabled", "账号已被禁用", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }
    if (credential == CredentialStatus::kUnavailable) {
        // 玩家档案来自 MySQL：依赖不可用时返回 503。该判断必须排在凭据判断之后，
        // 避免用依赖故障掩盖「密码错误」这类确定性结果。
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE, "player_store_unavailable",
                      "玩家档案暂时不可用，请稍后重试", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // 第三步：创建会话。request_id 为空时不做幂等（视为一次性请求），
    // 非空时 SessionStore 保证同一 request_id 返回同一 Token。
    const std::string client_type = request->client_type().empty() ? "web" : request->client_type();
    std::string token;
    const StoreStatus created = sessions_->CreateSession(request_id, player.player_id(),
                                                         client_type, session_ttl_seconds_, &token);
    if (created != StoreStatus::kOk) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                      "session_store_unavailable", "会话存储暂时不可用，请稍后重试", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    response->set_token(token);
    response->set_expires_in_seconds(session_ttl_seconds_);
    *response->mutable_player() = player;
}

void GatewayServiceImpl::GetCurrentPlayer(::google::protobuf::RpcController* controller,
                                          const rgbt::gateway::v1::GetCurrentPlayerRequest* request,
                                          rgbt::gateway::v1::GetCurrentPlayerResponse* response,
                                          ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    // 注意：Token 不能只从 request->token() 取。brpc 不把 HTTP 头映射进 protobuf
    // 字段，携带 Authorization 头的请求在这里会是空字符串。
    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    // 注意：错误体先写进局部变量，成功时不拷贝回 response。直接传
    // response->mutable_error() 会**提前创建** error 子消息，导致成功响应里也带一个
    // 空 error 字段，客户端据此会误判为失败。
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    const auto player = players_->FindByPlayerId(player_id);
    if (!player.has_value()) {
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::NOT_FOUND, "player_not_found",
                      "会话对应的玩家不存在", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatusAndRecord(controller, http_status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    *response->mutable_player() = player.value();
}

void GatewayServiceImpl::Logout(::google::protobuf::RpcController* controller,
                                const rgbt::gateway::v1::LogoutRequest* request,
                                rgbt::gateway::v1::LogoutResponse* response,
                                ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    // 同 GetCurrentPlayer：Token 需要从 HTTP 头或 query string 中提取。
    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    const std::string token_error = ValidateToken(token);
    if (!token_error.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, token_error,
                      "Token 缺失或格式不合法", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    const StoreStatus status = sessions_->DeleteSession(token);
    if (status == StoreStatus::kUnavailable) {
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                      "session_store_unavailable", "会话存储暂时不可用，请稍后重试", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatusAndRecord(controller, http_status, request_id, start_us);
        return;
    }

    // 会话本就不存在时也返回成功：登出是幂等操作，重复登出不应报错。
    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
}

std::int32_t GatewayServiceImpl::ResolvePlayerId(const std::string& token,
                                                 const std::string& request_id,
                                                 std::string* out_player_id,
                                                 rgbt::gateway::v1::Error* error) {
    const std::string token_error = ValidateToken(token);
    if (!token_error.empty()) {
        return FillError(error, ErrorCode::INVALID_ARGUMENT, token_error, "Token 缺失或格式不合法",
                         request_id);
    }

    SessionRecord session;
    const StoreStatus status = sessions_->GetSession(token, &session);
    if (status == StoreStatus::kUnavailable) {
        return FillError(error, ErrorCode::UNAVAILABLE, "session_store_unavailable",
                         "会话存储暂时不可用，请稍后重试", request_id);
    }
    if (status == StoreStatus::kNotFound) {
        // Token 格式合法但会话不存在：可能是过期或伪造，按未认证处理。
        return FillError(error, ErrorCode::UNAUTHENTICATED, "session_not_found",
                         "会话不存在或已过期", request_id);
    }

    *out_player_id = session.player_id;
    return 200;
}

void GatewayServiceImpl::FillMatchStatus(const MatchSnapshot& snapshot,
                                         rgbt::gateway::v1::MatchStatusInfo* out) {
    if (out == nullptr) {
        return;
    }
    out->set_state(MatchStateName(snapshot.state));
    out->set_match_id(snapshot.match_id);
    out->set_room_id(snapshot.room_id);
    for (const std::string& player_id : snapshot.player_ids) {
        out->add_player_ids(player_id);
    }
    out->set_queued_at_ms(snapshot.queued_at_ms);
    out->set_queue_size(snapshot.queue_size);
}

std::int32_t GatewayServiceImpl::HandleMatchFailure(MatchCallStatus status,
                                                    const std::string& request_id,
                                                    rgbt::gateway::v1::Error* error) {
    switch (status) {
        case MatchCallStatus::kOk:
            return 200;
        case MatchCallStatus::kUnavailable:
            // 对端未启动、超时或连接失败。返回 503 而不是 500：这是依赖不可用，
            // 不是本服务代码错误，恢复后无需重启 Gateway 即可继续匹配。
            return FillError(error, ErrorCode::UNAVAILABLE, "match_unavailable",
                             "匹配服务暂时不可用，请稍后重试", request_id);
        case MatchCallStatus::kInvalidArgument:
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "match_invalid_argument",
                             "匹配请求参数不合法", request_id);
        case MatchCallStatus::kQueueFull:
            return FillError(error, ErrorCode::RESOURCE_EXHAUSTED, "match_queue_full",
                             "匹配队列已满，请稍后重试", request_id);
        case MatchCallStatus::kInternal:
            return FillError(error, ErrorCode::INTERNAL, "match_internal", "匹配服务返回未分类错误",
                             request_id);
    }
    return FillError(error, ErrorCode::INTERNAL, "match_internal", "匹配服务返回未知结果",
                     request_id);
}

namespace {

/// 房间状态的稳定字符串表示。与 api/proto/gateway.proto 的 RoomStateInfo.state 一致。
const char* RoomStateName(RoomState state) {
    switch (state) {
        case RoomState::kWaiting:
            return "waiting";
        case RoomState::kPlaying:
            return "playing";
        case RoomState::kFinishing:
            return "finishing";
        case RoomState::kFinished:
            return "finished";
        case RoomState::kAborted:
            return "aborted";
        case RoomState::kCreated:
        default:
            return "created";
    }
}

}  // namespace

void GatewayServiceImpl::FillRoomState(const RoomSnapshot& snapshot,
                                       rgbt::gateway::v1::RoomStateInfo* out) {
    if (out == nullptr) {
        return;
    }
    out->set_room_id(snapshot.room_id);
    out->set_match_id(snapshot.match_id);
    out->set_state(RoomStateName(snapshot.state));
    out->set_frame(snapshot.frame);
    out->set_finish_reason(snapshot.finish_reason);
    out->set_winner_id(snapshot.winner_id);
    out->set_started_at_ms(snapshot.started_at_ms);
    out->set_finished_at_ms(snapshot.finished_at_ms);

    for (const RoomPlayerSnapshot& player : snapshot.players) {
        rgbt::gateway::v1::RoomPlayerInfo* item = out->add_players();
        item->set_player_id(player.player_id);
        item->set_hp(player.hp);
        item->set_connected(player.connected);
        // TASK-016：轮询兜底接口同样要暴露 online，否则自动重连的前端在
        // 推送断开、退回轮询的这段时间里看不到"对方是否还在"。
        item->set_online(player.online);
    }
}

std::int32_t GatewayServiceImpl::HandleRoomFailure(RoomCallStatus status,
                                                   const std::string& request_id,
                                                   rgbt::gateway::v1::Error* error,
                                                   const char* not_found_reason) {
    switch (status) {
        case RoomCallStatus::kOk:
            return 200;
        case RoomCallStatus::kUnavailable:
            // 对端未启动、超时或连接失败。503 而不是 500：这是依赖不可用，
            // 恢复后无需重启 Gateway 即可继续。
            return FillError(error, ErrorCode::UNAVAILABLE, "room_unavailable",
                             "房间服务暂时不可用，请稍后重试", request_id);
        case RoomCallStatus::kStoreUnavailable:
            // Room 在，但它的存储（MySQL）不可用。与 room_unavailable 分开：
            // 排障时「哪个依赖挂了」是第一个要回答的问题。
            return FillError(error, ErrorCode::UNAVAILABLE, "result_store_unavailable",
                             "对局结果存储暂时不可用，请稍后重试", request_id);
        case RoomCallStatus::kResultPending:
            // 已分出胜负但结果尚未落库。503 而不是 404：结果**存在**，只是还查不到，
            // 调用方稍后重试即可。返回 404 会让客户端以为这局没有结果。
            return FillError(error, ErrorCode::UNAVAILABLE, "result_pending",
                             "对局已结束，结果仍在写入，请稍后重试", request_id);
        case RoomCallStatus::kNotFound:
            return FillError(error, ErrorCode::NOT_FOUND, not_found_reason, "房间或对局结果不存在",
                             request_id);
        case RoomCallStatus::kAlreadyFinished:
            return FillError(error, ErrorCode::CONFLICT, "room_already_finished",
                             "对局已结束，不再接受该操作", request_id);
        case RoomCallStatus::kInvalidArgument:
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "room_invalid_argument",
                             "房间请求参数不合法", request_id);
        case RoomCallStatus::kNotAMember:
            // 与 room_invalid_argument 分开：房间确实存在，只是这个玩家不在名单里。
            // 合并成一个 reason 会让排障时无法区分「请求写错了」和「走错房间了」。
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "not_a_member",
                             "该玩家不是这一局的成员", request_id);
        case RoomCallStatus::kNotPlaying:
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "room_not_playing",
                             "对局尚未开始，此时不能提交输入", request_id);
        case RoomCallStatus::kInternal:
            return FillError(error, ErrorCode::INTERNAL, "room_internal", "房间服务返回未分类错误",
                             request_id);
    }
    return FillError(error, ErrorCode::INTERNAL, "room_internal", "房间服务返回未知结果",
                     request_id);
}

void GatewayServiceImpl::EnqueueMatch(::google::protobuf::RpcController* controller,
                                      const rgbt::gateway::v1::EnqueueMatchRequest* request,
                                      rgbt::gateway::v1::EnqueueMatchResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    // player_id 来自会话，请求体里就算带了这个字段也不会被读取。
    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->Enqueue(player_id, request_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        // 失败也要记：否则"客户端说入队失败"在 Gateway 侧查不到任何痕迹。
        rgbt::common::LogWarn("match_enqueue_failed", request_id,
                              {{"player", player_id},
                               {"reason", error.reason()},
                               {"http_status", std::to_string(status)}});
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // TASK-018：与 Match 侧的 `match_enqueued` 共用同一个 request_id，
    // 于是"点开始匹配"这件事在两个服务的日志里能用同一个键串起来——这正是
    // Phase 3 退出标准第一条要求的能力。
    rgbt::common::LogInfo("match_enqueue_ok", request_id,
                          {{"player", player_id},
                           {"match_state", std::string(MatchStateName(snapshot.state))},
                           {"match_id", snapshot.match_id},
                           {"room_id", snapshot.room_id}});

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::GetMatchStatus(::google::protobuf::RpcController* controller,
                                        const rgbt::gateway::v1::GetMatchStatusRequest* request,
                                        rgbt::gateway::v1::GetMatchStatusResponse* response,
                                        ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->GetStatus(player_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::CancelMatch(::google::protobuf::RpcController* controller,
                                     const rgbt::gateway::v1::CancelMatchRequest* request,
                                     rgbt::gateway::v1::CancelMatchResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    // 取消是幂等操作：玩家本来就不在队列中时，Match 同样返回成功状态。
    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->Cancel(player_id, request_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::JoinRoom(::google::protobuf::RpcController* controller,
                                  const rgbt::gateway::v1::JoinRoomRequest* request,
                                  rgbt::gateway::v1::JoinRoomResponse* response,
                                  ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    RoomSnapshot snapshot;
    const RoomCallStatus call = room_->Join(room_id, player_id, request_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::SubmitInput(::google::protobuf::RpcController* controller,
                                     const rgbt::gateway::v1::SubmitInputRequest* request,
                                     rgbt::gateway::v1::SubmitInputResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    RoomSnapshot snapshot;
    // player_id 来自会话，请求体里即使带同名字段也会被忽略：
    // 否则任何登录用户都能替别人提交输入。
    const RoomCallStatus call = room_->SubmitAttack(room_id, player_id, request_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::GetRoomState(::google::protobuf::RpcController* controller,
                                      const rgbt::gateway::v1::GetRoomStateRequest* request,
                                      rgbt::gateway::v1::GetRoomStateResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }
    // 这里只校验身份，不使用 player_id：房间快照对同局玩家是共享信息，
    // 而「谁能看这个房间」的判定在 Room 侧（非成员无法加入，因此也拿不到匹配结果）。
    (void)player_id;

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    RoomSnapshot snapshot;
    const RoomCallStatus call = room_->GetState(room_id, request_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::GetMatchResult(::google::protobuf::RpcController* controller,
                                        const rgbt::gateway::v1::GetMatchResultRequest* request,
                                        rgbt::gateway::v1::GetMatchResultResponse* response,
                                        ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string match_id = ExtractQueryParam(controller, "match_id", request->match_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }
    (void)player_id;

    if (match_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "match_id_required",
                      "缺少 match_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    MatchResultView view;
    const RoomCallStatus call = room_->GetResult(match_id, request_id, &view);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error, "result_not_found");

    // 房间快照在成功与「结果待落库」两种情况下都可能存在，先填上。
    if (view.has_room) {
        FillRoomState(view.room, response->mutable_room());
    }

    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
    if (view.has_result) {
        rgbt::gateway::v1::MatchResultInfo* result = response->mutable_result();
        result->set_match_id(view.result.match_id);
        result->set_room_id(view.result.room_id);
        result->set_winner_id(view.result.winner_id);
        result->set_player_count(view.result.player_count);
        result->set_started_at_ms(view.result.started_at_ms);
        result->set_finished_at_ms(view.result.finished_at_ms);
    }
}

void GatewayServiceImpl::StreamEvents(::google::protobuf::RpcController* controller,
                                      const rgbt::gateway::v1::StreamEventsRequest* request,
                                      rgbt::gateway::v1::StreamEventsResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // TASK-022：处理耗时从这一行开始计。brpc 服务端拿不到请求开始时间，
    // 因此只能在每个处理函数入口取一次（见 ApplyHttpStatusAndRecord 的注释）。
    const std::int64_t start_us = rgbt::common::NowUs();

    const std::string token = ExtractToken(controller, request->token());
    // TASK-018：`request_id` 也从 query string 读。GET /stream 没有请求体，而 brpc
    // **不会**把 query string 映射进 protobuf 字段（与 token/room_id 同一个坑），
    // 于是这里原来永远是空串——订阅链路因此无法按 id 与其它服务关联。
    // 这是排查入口，不是契约变更：取值顺序仍是"请求体优先，其次 query"。
    const std::string request_id =
        ExtractQueryParam(controller, "request_id", request->request_id());
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatusAndRecord(controller, auth, request_id, start_us);
        return;
    }

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // 成员校验：房间快照里必须包含调用者。
    //
    // 没有这一步，任何登录用户只要猜到（或用别人给的）room_id，就能长期订阅到
    // 别人房间的血量与胜负。room_id 由服务端随机生成，但「难猜」不是访问控制。
    RoomSnapshot snapshot;
    const RoomCallStatus lookup = room_->GetState(room_id, request_id, &snapshot);
    if (lookup != RoomCallStatus::kOk) {
        rgbt::common::LogWarn(
            "subscribe_rejected", request_id,
            {{"reason", "room_lookup_failed"}, {"room", room_id}, {"player", player_id}});
        const std::int32_t status = HandleRoomFailure(lookup, request_id, &error);
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }
    bool is_member = false;
    for (const RoomPlayerSnapshot& player : snapshot.players) {
        if (player.player_id == player_id) {
            is_member = true;
            break;
        }
    }
    if (!is_member) {
        // TASK-018：拒绝也要留痕。此前这条路径**什么都不记**，于是"客户端说订阅被拒"
        // 在服务端查不到任何证据——而这正是最需要日志的时刻（可能是攻击，也可能是
        // 前端把 room_id 传错了）。
        rgbt::common::LogWarn(
            "subscribe_rejected", request_id,
            {{"reason", "not_a_member"}, {"room", room_id}, {"player", player_id}});
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "not_a_member",
                      "该玩家不是这一局的成员", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    if (stream_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "stream_unavailable", "推送服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        // 无 HTTP 上下文（单元测试直接调用服务）时无法建立流式响应。
        // 明确失败，而不是登记一条永远写不出去的订阅。
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INTERNAL, "stream_requires_http",
                      "推送接口只能通过 HTTP 访问", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // 进入 SSE 模式：先定响应头，再取持续写入口。
    // done 释放之后写出的数据会立刻以 chunked 形式发出
    // （brpc 文档 http_service.md「持续发送」一节）。
    cntl->http_response().set_content_type("text/event-stream");
    cntl->http_response().set_status_code(200);
    cntl->http_response().SetHeader("Connection", "keep-alive");
    cntl->http_response().SetHeader("Cache-Control", "no-cache");
    // 反向代理常会缓冲响应体，把 SSE 事件攒到最后一起发。这一行是通用的
    // 关闭缓冲提示，对直连没有副作用。
    cntl->http_response().SetHeader("X-Accel-Buffering", "no");

    butil::intrusive_ptr<brpc::ProgressiveAttachment> attachment =
        cntl->CreateProgressiveAttachment();
    if (attachment == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::INTERNAL,
                                              "stream_unavailable", "无法创建推送通道", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // TASK-017：解析 Last-Event-ID。它决定订阅建立时是否补发缺的帧。
    // 解析必须在这里完成（只有这里能读到 HTTP 头），StreamHub 只接受结论。
    //
    // 注意：**补发结果不在这里**。补发出现在订阅建立之后的第一次 Tick 里
    // （理由见 StreamHub::Subscribe 的注释），因此返回值里只有订阅 id；
    // 补发与 stream.reset 的结果由 StreamHub 自己记日志，也可以事后用
    // `StreamHub::SubscribeReport(id)` 查到。
    const LastEventIdHeader last_event_id = ParseLastEventId(controller);

    SubscribeOptions subscribe_options;
    subscribe_options.last_event_id = last_event_id.value;
    subscribe_options.last_event_id_malformed = last_event_id.malformed;
    // TASK-018：把本次 HTTP 请求的 request_id 交给订阅表，让这条连接引发的所有
    // Room 调用（上报 presence、轮询状态、补发历史）都带同一个关联键。
    // 漏了这一行时的表现是：Gateway 自己的日志有 trace=，而 Room 侧那几条
    // `presence_reported` 没有——跨服务链路断在中间（实测踩到）。
    subscribe_options.request_id = request_id;

    const SubscriptionReport subscription = stream_->Subscribe(
        player_id, room_id, std::make_unique<ProgressiveAttachmentSink>(attachment),
        subscribe_options);
    if (subscription.id == 0) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::INTERNAL,
                                              "stream_unavailable", "订阅注册失败", request_id);
        response->set_status_code(status);
        ApplyHttpStatusAndRecord(controller, status, request_id, start_us);
        return;
    }

    // 成功。不写 error、也不写响应体：后续内容全部由 StreamHub 持续写出，
    // 直到对局结束或客户端断开。
    //
    // 这一行日志是排障时判断"客户端到底有没有带 Last-Event-ID"的最直接依据：
    // 补发是否发生由 StreamHub 记，但"请求里有没有那个头"只有这里知道。
    // （实测踩过同类问题：HTTP 头不会被 brpc 映射进 protobuf 字段，漏读一次
    // 就表现为"永远不补发"，而链路上没有任何报错。）
    //
    // 三态要能分辨：malformed（头坏了）/ none（没带头，第一次订阅）/ 具体帧号。
    // 把 "none" 与 "0" 分开是必要的——0 是一个**合法帧号**（对局刚建好时就是它），
    // 而"没带头"是完全不同的意思，混在一起排障时会误判。
    const std::string last_event_id_text =
        last_event_id.malformed ? std::string("malformed") : std::to_string(last_event_id.value);
    rgbt::common::LogInfo("subscribe_ready", request_id,
                          {{"sub", std::to_string(subscription.id)},
                           {"room", room_id},
                           {"player", player_id},
                           {"last_event_id", last_event_id_text}});
    response->set_status_code(200);
    ApplyHttpStatusAndRecord(controller, 200, request_id, start_us);
}

bool GatewayServiceImpl::DependenciesHealthy() {
    // 只检查会话存储：账号目录是进程内数据，匹配服务在每次请求时单独判断
    // （它不可用只影响匹配接口，不应让整个 Gateway 显示为不健康）。
    return sessions_ != nullptr;
}

}  // namespace rgbt::gateway
