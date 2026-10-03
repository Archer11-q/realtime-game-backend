/// @file metrics.hpp
/// @brief 最小指标登记表 + Prometheus 文本暴露（TASK-019）。
///
/// 为什么需要它（TASK-019）：`docs/04-quality-and-observability.md` 第 4 节列了
/// 8 类指标，但 Phase 2 结束时**一个都没有暴露**。`StreamHub` 虽然有可断言的
/// 计数器，却没有出口——于是"指标可查、数据可复现"这条 Phase 3 退出标准无从谈起。
///
/// 为什么自己写而不是引入 prometheus-cpp：
///   * 本项目需要的类型只有三种：counter、gauge、histogram；
///   * Prometheus 的文本暴露格式本身就是协议（几行 HELP/TYPE + 样本行），
///     不需要客户端库；
///   * 新依赖会扩大构建时间与迁移面（`CLAUDE.md` 第 6 条：不以"技术先进"为
///     理由增加组件）。
///
/// 设计取舍：
///   * **不做标签基数控制**：本项目所有标签都是固定枚举（路径、状态码、阶段），
///     没有用户输入的标签。真要防的是"把 room_id/player_id 当标签"——
///     这条约束写在文档与验收脚本里，而不是靠库来兜。
///   * **不做直方图的分位数计算**：Prometheus 的惯例是暴露桶（bucket）由服务端
///     算分位数。这里只累计桶计数，不引入 HDR/tdigest。
///   * **采集失败不影响业务**：`TextExposure()` 只读快照，任何内部异常都不抛出。
///
/// 命名约定：所有指标以 `rgbt_` 开头，单位写进名字（`_total` 表示累计计数，
/// `_seconds` 表示秒）。这与 Prometheus 的惯例一致，避免同一指标在不同服务里
/// 用不同单位。

#ifndef RGBT_COMMON_METRICS_HPP
#define RGBT_COMMON_METRICS_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rgbt::common {

/// 一个标签。键与值都是短字符串（本项目里全部来自固定枚举）。
struct MetricLabel {
    std::string key;
    std::string value;
};

/// 指标句柄。拿到后可以高频 `Add`，不需要每次按名字查找。
///
/// 为什么用句柄而不是"每次调用都按名字查表"：业务路径上（例如每个 HTTP 请求）
/// 加指标必须便宜。句柄在首次注册时解析一次，之后只做一次原子加。
class CounterHandle {
public:
    CounterHandle() = default;

    /// 累加。`delta` 通常为 1。
    void Add(std::uint64_t delta = 1) const noexcept;

    /// 当前值。仅测试与排障使用。
    [[nodiscard]] std::uint64_t Value() const noexcept;

    [[nodiscard]] bool valid() const noexcept { return cell_ != nullptr; }

private:
    friend class MetricsRegistry;
    explicit CounterHandle(std::atomic<std::uint64_t>* cell) : cell_(cell) {}

    /// 指向登记表里的原子单元。**地址在注册后不再变化**：单元由
    /// `unique_ptr` 持有，容器扩容不会移动它。
    std::atomic<std::uint64_t>* cell_ = nullptr;
};

/// 指标登记表。每个进程一个（见 `Metrics()`）。
///
/// 状态直接放在类里（不用 pimpl）：它只依赖标准库，没有 ABI 或依赖隔离的需要，
/// 多一层间接只会让"到底存了什么"更难看清。
class MetricsRegistry {
public:
    MetricsRegistry() = default;
    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    /// @brief 取一个 counter 的句柄。同名同标签重复调用返回同一个计数器。
    ///
    /// 线程安全：内部加锁。**首次注册的键集合在进程内固定**——我们没有删除接口，
    /// 这是有意的：能删的登记表会让"某个路径第一次出现时指标才存在"这种
    /// 无法解释的现象成为可能。
    CounterHandle Counter(std::string_view name, std::string_view help,
                          std::initializer_list<MetricLabel> labels = {});

    /// @brief 登记一个 gauge，并在**每个采集周期调用回调**取当前值。
    ///
    /// 为什么 gauge 用回调而不是 `Set`：gauge 天然是"此刻的状态"（连接数、房间数），
    /// 而状态本来就有唯一来源（`StreamHub`、`RoomManager`）。让指标在采集时去问那个
    /// 来源，就永远不会出现"忘了更新 gauge"导致数值僵死；`Set` 则要求每个状态变更点
    /// 都记得调用，那恰恰最容易漏。
    ///
    /// @param read 采集时调用，返回当前值。**不应阻塞**（会在采集线程上执行）。
    void Gauge(std::string_view name, std::string_view help, std::function<std::uint64_t()> read,
               std::initializer_list<MetricLabel> labels = {});

    /// @brief 记录一次观测到直方图里。单位由调用方保证（惯例是秒）。
    void Observe(std::string_view name, std::string_view help, double value);

    /// @brief 导出 Prometheus 文本格式。
    ///
    /// 指标按名字排序输出，便于人眼比对与 `diff`。内容不含时间戳（采集时由
    /// Prometheus 打），也不含进程级标签（由抓取配置决定）。
    [[nodiscard]] std::string TextExposure();

    /// @brief 已登记的指标名数量。仅供测试与排障。
    [[nodiscard]] std::size_t MetricCount() const;

private:
    /// counter 的存储单元。句柄直接指向 `value`，因此**地址必须稳定**：
    /// 单元由 `unique_ptr` 持有，容器扩容时指针不变。
    struct CounterCell {
        std::string name;
        std::string help;
        std::vector<MetricLabel> labels;
        std::atomic<std::uint64_t> value{0};
    };

    /// 一个直方图。桶边界固定，由注册时决定。
    struct Histogram {
        std::string name;
        std::string help;
        std::vector<double> bounds;
        std::vector<std::uint64_t> bucket_counts;  // 与 bounds 对齐，非累计
        std::uint64_t count = 0;
        double sum = 0.0;
    };

    /// 每个指标名的 gauge 回调集合（同名 gauge 目前只有一个实例；
    /// 需要多实例时用标签区分，见命名约定）。
    struct GaugeEntry {
        std::string name;
        std::string help;
        std::vector<MetricLabel> labels;
        std::function<std::uint64_t()> read;
    };

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<CounterCell>> counters_;
    /// 名字 + 标签 -> counter 下标。Key 用规范化字符串，见 `LabelKey`。
    std::unordered_map<std::string, std::size_t> counter_index_;
    std::vector<GaugeEntry> gauges_;
    std::vector<Histogram> histograms_;
};

/// 进程级登记表。各服务用同一份，因此 `/metrics` 一次导出全部指标。
[[nodiscard]] MetricsRegistry& Metrics();

// ---------------------------------------------------------------------------
// 常用指标名（集中定义，避免各服务各写一遍字符串）
// ---------------------------------------------------------------------------

/// HTTP 请求总数。标签：`path`（restful 路径模板）、`status`（HTTP 状态码）。
inline constexpr const char* kMetricHttpRequestsTotal = "rgbt_http_requests_total";

/// HTTP 请求耗时（秒）。标签与上面一致。
inline constexpr const char* kMetricHttpRequestSeconds = "rgbt_http_request_seconds";

/// 当前 SSE 连接数（gauge）。
inline constexpr const char* kMetricSseConnections = "rgbt_sse_connections";

/// 累计补发出去的推送帧数。
///
/// 名字里**没有** `_total`：Prometheus 的惯例是 `_total` 表示 counter，而本进程
/// 读的是 StreamHub 维护的累计量、类型上仍声明为 gauge。宁可名字朴素，也不为了
/// 命名惯例去制造第二份计数（见 gateway_service.cpp 里的说明）。
inline constexpr const char* kMetricPushBackfilledFrames = "rgbt_push_backfilled_frames";

/// `stream.reset` 次数。标签：`reason`（稳定标识）。
inline constexpr const char* kMetricPushResetTotal = "rgbt_push_reset_total";

/// 服务间 brpc 调用总数。标签：`target`（match/room）、`outcome`（ok/failed）。
inline constexpr const char* kMetricRpcCallsTotal = "rgbt_rpc_calls_total";

/// 匹配队列当前长度（gauge）。
inline constexpr const char* kMetricMatchQueueLength = "rgbt_match_queue_length";

/// 匹配事件计数。标签：`event`（enqueue/pair/cancel/timeout）。
inline constexpr const char* kMetricMatchEventsTotal = "rgbt_match_events_total";

/// 房间当前数量。标签：`phase`（created/waiting/playing/finishing/finished/aborted）。
inline constexpr const char* kMetricRooms = "rgbt_rooms";

/// 房间推进的帧总数。
inline constexpr const char* kMetricRoomFramesAdvancedTotal = "rgbt_room_frames_advanced_total";

/// 房间生命周期事件。标签：`event`（created/joined/aborted/disconnect_finish）。
inline constexpr const char* kMetricRoomEventsTotal = "rgbt_room_events_total";

/// 对局结果落库尝试。标签：`outcome`（ok/failed）。
inline constexpr const char* kMetricResultPersistTotal = "rgbt_result_persist_total";

/// 房间快照写入尝试。标签：`outcome`（ok/failed）。
inline constexpr const char* kMetricSnapshotWriteTotal = "rgbt_snapshot_write_total";

/// 队列快照（Match）写入尝试。标签：`outcome`（ok/failed）。
inline constexpr const char* kMetricQueueSnapshotTotal = "rgbt_queue_snapshot_total";

}  // namespace rgbt::common

#endif  // RGBT_COMMON_METRICS_HPP
