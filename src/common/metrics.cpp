/// @file metrics.cpp
/// @brief 指标登记表与 Prometheus 文本暴露的实现。设计取舍见 metrics.hpp。

#include "common/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace rgbt::common {
namespace {

/// 直方图的默认桶（秒）。取值的理由：
///   * 本项目最关键的是**服务间 RPC**（超时 500 ms）与**HTTP 接口**；
///   * 从 1 ms 到 2 s 覆盖了"正常"到"已经超时"的整个区间；
///   * 桶边界是**累计对外暴露**的（`le`），因此这里只需要给出上界序列。
const std::vector<double>& DefaultBounds() {
    static const std::vector<double> bounds = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1,
                                               0.25,  0.5,   1.0,  2.0,   5.0};
    return bounds;
}

/// counter 的规范化键：`名字{标签1="值1",标签2="值2"}`。
///
/// 为什么把标签拼进键而不是嵌套 map：本项目的标签组合是固定枚举的小集合，
/// 拼串后一次哈希即可，且**导出的顺序可以直接由键的字典序决定**，
/// 不需要额外排序结构。
std::string LabelKey(std::string_view name, const std::vector<MetricLabel>& labels) {
    std::string key(name);
    if (labels.empty()) {
        return key;
    }
    key.push_back('{');
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) {
            key.push_back(',');
        }
        key += labels[i].key;
        key.push_back('=');
        key += labels[i].value;
    }
    key.push_back('}');
    return key;
}

/// 转义标签值里的 `"`、`\` 与换行（Prometheus 文本格式的要求）。
///
/// 与日志模块同样的理由：标签值一旦含未转义的引号或换行，整个抓取响应就会变成
/// 非法格式，而采集端只会报"解析失败"，看不出是哪条指标坏了。
std::string EscapeLabelValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

/// 渲染标签集合：`{k="v",k2="v2"}`；无标签时返回空串。
std::string RenderLabels(const std::vector<MetricLabel>& labels) {
    if (labels.empty()) {
        return {};
    }
    std::string out = "{";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += labels[i].key;
        out += "=\"";
        out += EscapeLabelValue(labels[i].value);
        out.push_back('"');
    }
    out.push_back('}');
    return out;
}

/// 把浮点渲染成 Prometheus 认得的十进制（不使用科学计数法，避免解析歧义）。
std::string RenderNumber(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

}  // namespace

void CounterHandle::Add(std::uint64_t delta) const noexcept {
    if (cell_ == nullptr) {
        return;
    }
    // Relaxed 足够：指标只要求最终被读到，不参与任何同步决策。
    // 用原子而不是普通变量，是因为同一个 counter 会被多个 brpc 工作线程累加。
    cell_->fetch_add(delta, std::memory_order_relaxed);
}

std::uint64_t CounterHandle::Value() const noexcept {
    if (cell_ == nullptr) {
        return 0;
    }
    return cell_->load(std::memory_order_relaxed);
}

CounterHandle MetricsRegistry::Counter(std::string_view name, std::string_view help,
                                       std::initializer_list<MetricLabel> labels) {
    std::vector<MetricLabel> label_list(labels.begin(), labels.end());
    const std::string key = LabelKey(name, label_list);

    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = counter_index_.find(key);
    if (it != counter_index_.end()) {
        return CounterHandle(&counters_[it->second]->value);
    }

    auto cell = std::make_unique<CounterCell>();
    cell->name = std::string(name);
    cell->help = std::string(help);
    cell->labels = std::move(label_list);
    // **注册即存在**：即使一次也没 Add，也要导出为 0。
    // 否则"某个计数一直是 0"与"这个指标还没被创建"在采集端无法区分，
    // 而这两种情况对排障的含义完全不同。
    const std::size_t index = counters_.size();
    std::atomic<std::uint64_t>* raw = &cell->value;
    counters_.push_back(std::move(cell));
    counter_index_.emplace(key, index);
    return CounterHandle(raw);
}

void MetricsRegistry::Gauge(std::string_view name, std::string_view help,
                            std::function<std::uint64_t()> read) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const GaugeEntry& entry : gauges_) {
        if (entry.name == name) {
            return;  // 同名重复登记：保留第一次，避免回调被调用两次
        }
    }
    GaugeEntry entry;
    entry.name = std::string(name);
    entry.help = std::string(help);
    entry.read = std::move(read);
    gauges_.push_back(std::move(entry));
}

void MetricsRegistry::Observe(std::string_view name, std::string_view help, double value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Histogram* target = nullptr;
    for (Histogram& histogram : histograms_) {
        if (histogram.name == name) {
            target = &histogram;
            break;
        }
    }
    if (target == nullptr) {
        Histogram histogram;
        histogram.name = std::string(name);
        histogram.help = std::string(help);
        // **必须把桶边界存下来**：只记计数的话，`Observe` 就无从判断一次观测落在
        // 哪个桶里。漏了这一行的表现是"每个桶都等于观测次数，而 `+Inf` 却正确"
        // （`+Inf` 取自 count）——实测踩到，被 HistogramBucketsAreCumulative 抓到。
        histogram.bounds = DefaultBounds();
        histogram.bucket_counts.assign(histogram.bounds.size(), 0);
        histograms_.push_back(std::move(histogram));
        target = &histograms_.back();
    }

    // 负数没有意义（耗时不可能为负）：归零而不是抛错，指标不该让业务失败。
    const double observed = value < 0.0 ? 0.0 : value;
    target->count += 1;
    target->sum += observed;
    // 只累加**上界大于等于观测值的最小桶**；导出时再做累计。
    // 用 `lower_bound` 找第一个 `bound >= observed`；若超出所有上界则不落任何桶
    // （`+Inf` 桶由 count 得到）。写成"遍历所有桶、每个都加"是错的——那是每个桶
    // 都变成总次数，等价于没有任何分桶信息。
    const auto it = std::lower_bound(target->bounds.begin(), target->bounds.end(), observed);
    if (it != target->bounds.end()) {
        ++target->bucket_counts[static_cast<std::size_t>(it - target->bounds.begin())];
    }
}

std::string MetricsRegistry::TextExposure() {
    // 先做快照：**不在持锁期间渲染**。Gauge 回调会去问业务对象（可能拿它们自己的
    // 锁），在指标表的锁内调用它们会把两把锁串起来，早晚踩到锁序问题。
    struct CounterSample {
        std::string name;
        std::string help;
        std::vector<MetricLabel> labels;
        std::uint64_t value = 0;
    };
    struct GaugeSample {
        std::string name;
        std::string help;
        std::function<std::uint64_t()> read;
    };
    struct HistogramSample {
        std::string name;
        std::string help;
        std::vector<double> bounds;
        std::vector<std::uint64_t> bucket_counts;
        std::uint64_t count = 0;
        double sum = 0.0;
    };

    std::vector<CounterSample> counters;
    std::vector<GaugeSample> gauges;
    std::vector<HistogramSample> histograms;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        counters.reserve(counters_.size());
        for (const auto& cell : counters_) {
            counters.push_back(CounterSample{cell->name, cell->help, cell->labels,
                                             CounterHandle(&cell->value).Value()});
        }
        for (const GaugeEntry& entry : gauges_) {
            gauges.push_back(GaugeSample{entry.name, entry.help, entry.read});
        }
        for (const Histogram& histogram : histograms_) {
            histograms.push_back(HistogramSample{histogram.name, histogram.help, histogram.bounds,
                                                 histogram.bucket_counts, histogram.count,
                                                 histogram.sum});
        }
    }

    // 输出顺序：counter/gauge 按名字排，同名内按标签键排；histogram 单独排在后面。
    // 固定顺序让 `diff` 两个时间点的抓取结果时有意义。
    std::sort(counters.begin(), counters.end(),
              [](const CounterSample& left, const CounterSample& right) {
                  if (left.name != right.name) {
                      return left.name < right.name;
                  }
                  return RenderLabels(left.labels) < RenderLabels(right.labels);
              });
    std::sort(gauges.begin(), gauges.end(), [](const GaugeSample& left, const GaugeSample& right) {
        return left.name < right.name;
    });
    std::sort(histograms.begin(), histograms.end(),
              [](const HistogramSample& left, const HistogramSample& right) {
                  return left.name < right.name;
              });

    std::ostringstream out;
    // HELP/TYPE 每个名字只写一次；同名多标签的样本紧跟其后。
    std::string last_name;
    for (const CounterSample& sample : counters) {
        if (sample.name != last_name) {
            out << "# HELP " << sample.name << ' ' << sample.help << '\n';
            out << "# TYPE " << sample.name << " counter\n";
            last_name = sample.name;
        }
        out << sample.name << RenderLabels(sample.labels) << ' ' << sample.value << '\n';
    }
    for (const GaugeSample& sample : gauges) {
        if (sample.name != last_name) {
            out << "# HELP " << sample.name << ' ' << sample.help << '\n';
            out << "# TYPE " << sample.name << " gauge\n";
            last_name = sample.name;
        }
        const std::uint64_t value = sample.read ? sample.read() : 0;
        out << sample.name << ' ' << value << '\n';
    }
    for (const HistogramSample& sample : histograms) {
        out << "# HELP " << sample.name << ' ' << sample.help << '\n';
        out << "# TYPE " << sample.name << " histogram\n";
        // 累计桶：`le` 是"小于等于该上界"的累计计数，这是 Prometheus 的约定。
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < sample.bounds.size(); ++i) {
            cumulative += sample.bucket_counts[i];
            out << sample.name << "_bucket{le=\"" << RenderNumber(sample.bounds[i]) << "\"} "
                << cumulative << '\n';
        }
        out << sample.name << "_bucket{le=\"+Inf\"} " << sample.count << '\n';
        out << sample.name << "_sum " << RenderNumber(sample.sum) << '\n';
        out << sample.name << "_count " << sample.count << '\n';
    }
    return out.str();
}

std::size_t MetricsRegistry::MetricCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return counters_.size() + gauges_.size() + histograms_.size();
}

MetricsRegistry& Metrics() {
    // 函数内静态：首次调用时构造，线程安全由 C++11 保证。
    // 不做"进程退出时销毁"的处理——指标表的析构顺序与业务对象纠缠，
    // 让它在进程生命周期内存活反而更安全。
    static MetricsRegistry* registry = new MetricsRegistry();
    return *registry;
}

}  // namespace rgbt::common
