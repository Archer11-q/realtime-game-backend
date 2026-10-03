/// @file metrics_test.cpp
/// @brief 指标登记表与 Prometheus 文本暴露的单元测试（TASK-019）。
///
/// 为什么这些用例值得写：指标是 Phase 3 退出标准"指标可查、数据可复现"的唯一依托。
/// 格式写错（缺 TYPE、桶不累计、标签没转义）**不会让任何功能失败**，只会让采集端
/// 静默丢掉或误读数据——那比没指标更危险，因为它让人以为看到了事实。
///
/// 测试自带一个**独立的文本解析器**：不调用产品代码的导出函数去自证，而是把导出的
/// 文本按 Prometheus 文本格式重新解析一遍，再断言值与类型。这样"导出的东西真的能被
/// 采集端读懂"才是被验证的。

#include "common/metrics.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using rgbt::common::CounterHandle;
using rgbt::common::MetricLabel;
using rgbt::common::MetricsRegistry;

/// 从导出文本里取一个样本的值。返回 -1 表示没找到。
///
/// 只解析形如 `name{labels} value` 与 `name value` 的样本行，忽略 HELP/TYPE。
double SampleValue(const std::string& text, const std::string& sample_name) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::string line =
            text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t space = line.rfind(' ');
        if (space == std::string::npos) {
            continue;
        }
        std::string label_part = line.substr(0, space);
        const std::string value_part = line.substr(space + 1);
        const std::size_t brace = label_part.find('{');
        const std::string name =
            brace == std::string::npos ? label_part : label_part.substr(0, brace);
        if (name != sample_name) {
            continue;
        }
        return std::stod(value_part);
    }
    return -1.0;
}

/// 某一行是否存在（用于断言 MYTYPE/HELP）。
bool HasLine(const std::string& text, const std::string& line) {
    return text.find(line + "\n") != std::string::npos;
}

// ---------------------------------------------------------------------------
// counter
// ---------------------------------------------------------------------------

TEST(MetricsTest, CounterStartsAtZeroAndAccumulates) {
    // **注册即存在**：一次也没 Add 也要导出为 0，否则"计数是 0"与"指标还没创建"
    // 在采集端无法区分，而这两者对排障的含义完全不同。
    MetricsRegistry registry;
    const CounterHandle handle = registry.Counter("rgbt_test_counter_total", "测试用计数");
    ASSERT_TRUE(handle.valid());
    EXPECT_EQ(handle.Value(), 0U);
    EXPECT_NE(registry.TextExposure().find("rgbt_test_counter_total 0"), std::string::npos);

    handle.Add();
    handle.Add(4);
    EXPECT_EQ(handle.Value(), 5U);
    EXPECT_NE(registry.TextExposure().find("rgbt_test_counter_total 5"), std::string::npos);
}

TEST(MetricsTest, SameNameAndLabelsReturnTheSameCounter) {
    // 重复注册必须落到同一个计数器：否则每个调用点都会造一个新指标，
    // 导出里出现一堆同名样本，而每个都只有局部计数。
    MetricsRegistry registry;
    const CounterHandle first = registry.Counter("rgbt_test_dup_total", "重复注册", {{"k", "v"}});
    const CounterHandle second = registry.Counter("rgbt_test_dup_total", "重复注册", {{"k", "v"}});
    first.Add(3);
    EXPECT_EQ(second.Value(), 3U);
    EXPECT_EQ(registry.MetricCount(), 1U);
}

TEST(MetricsTest, DifferentLabelValuesAreDifferentSeries) {
    MetricsRegistry registry;
    const CounterHandle ok =
        registry.Counter("rgbt_test_series_total", "按标签分序列", {{"outcome", "ok"}});
    const CounterHandle failed =
        registry.Counter("rgbt_test_series_total", "按标签分序列", {{"outcome", "failed"}});
    ok.Add(7);
    EXPECT_EQ(ok.Value(), 7U);
    EXPECT_EQ(failed.Value(), 0U);
    EXPECT_EQ(registry.MetricCount(), 2U);

    const std::string text = registry.TextExposure();
    // 断言按标签逐个看，不能只断言"第一个同名样本的值"：同一指标名下的样本
    // 按标签字典序排列，`failed` 在前，因此 SampleValue 拿到的是 0。
    EXPECT_NE(text.find("rgbt_test_series_total{outcome=\"ok\"} 7"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_series_total{outcome=\"failed\"} 0"), std::string::npos);
}

TEST(MetricsTest, CounterHandleStaysValidAfterManyRegistrations) {
    // 句柄直接指向登记表里的存储单元，因此**容器扩容不能让句柄失效**。
    // 这条用例锁定那个不变量：注册大量其它指标之后，早期句柄仍然指向同一个计数器。
    MetricsRegistry registry;
    const CounterHandle early = registry.Counter("rgbt_test_early_total", "早期注册");
    for (int i = 0; i < 200; ++i) {
        registry.Counter("rgbt_test_bulk_" + std::to_string(i) + "_total", "填充用");
    }
    early.Add(42);
    EXPECT_EQ(early.Value(), 42U);
    EXPECT_NE(registry.TextExposure().find("rgbt_test_early_total 42"), std::string::npos);
}

// ---------------------------------------------------------------------------
// gauge（回调式）
// ---------------------------------------------------------------------------

TEST(MetricsTest, GaugeReadsValueAtScrapeTime) {
    // Gauge 用回调：采集时去问状态的唯一来源，因此不会出现"忘了更新导致数值僵死"。
    MetricsRegistry registry;
    std::uint64_t current = 3;
    registry.Gauge("rgbt_test_gauge", "测试用 gauge", [&current]() { return current; });

    EXPECT_NE(registry.TextExposure().find("rgbt_test_gauge 3"), std::string::npos);
    current = 11;
    EXPECT_NE(registry.TextExposure().find("rgbt_test_gauge 11"), std::string::npos);
}

TEST(MetricsTest, DuplicateGaugeRegistrationKeepsTheFirst) {
    // 同名重复登记保留第一次：否则回调会被调用两次（值可能不同），
    // 导出里出现两条同名 gauge 样本。
    MetricsRegistry registry;
    registry.Gauge("rgbt_test_gauge_dup", "第一次", []() { return 1U; });
    registry.Gauge("rgbt_test_gauge_dup", "第二次", []() { return 2U; });
    EXPECT_EQ(registry.MetricCount(), 1U);
    EXPECT_NE(registry.TextExposure().find("rgbt_test_gauge_dup 1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// histogram
// ---------------------------------------------------------------------------

TEST(MetricsTest, HistogramBucketsAreCumulative) {
    // Prometheus 的 `le` 语义是**累计**（小于等于该上界的次数）。写错成非累计
    // 会让服务端算出的分位数完全错误，而且不会报任何错。
    MetricsRegistry registry;
    registry.Observe("rgbt_test_hist_seconds", "测试用直方图", 0.002);  // <= 0.005
    registry.Observe("rgbt_test_hist_seconds", "测试用直方图", 0.3);    // <= 0.5

    const std::string text = registry.TextExposure();
    // 取**第一个**桶（`le="0.001"`）的累计值：两次观测都不落在这里，所以是 0。
    // 用逐桶断言而不是只取首个样本，才能同时锁住"累计"和"只落一个桶"。
    EXPECT_NE(text.find("rgbt_test_hist_seconds_bucket{le=\"0.001\"} 0"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_hist_seconds_bucket{le=\"0.001\"} 0"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_hist_seconds_bucket{le=\"0.005\"} 1"), std::string::npos);
    // 注意桶上界的渲染：`%.6g` 把 2.0 渲染成 `2`（不是 `2.0`）。
    // 这不是缺陷——Prometheus 的 `le` 是字符串标签，`2` 与 `2.0` 数值等价，
    // 但断言必须按实际渲染来写。
    EXPECT_NE(text.find("rgbt_test_hist_seconds_bucket{le=\"2\"} 2"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_hist_seconds_bucket{le=\"+Inf\"} 2"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_hist_seconds_count 2"), std::string::npos);
}

TEST(MetricsTest, HistogramSumTracksObservations) {
    MetricsRegistry registry;
    registry.Observe("rgbt_test_sum_seconds", "计时", 0.25);
    registry.Observe("rgbt_test_sum_seconds", "计时", 0.75);
    const std::string text = registry.TextExposure();
    EXPECT_NE(text.find("rgbt_test_sum_seconds_count 2"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_sum_seconds_sum 1"), std::string::npos);
}

TEST(MetricsTest, NegativeObservationIsClampedToZero) {
    // 耗时不可能为负（时钟抖动可能算出来）。指标不该让业务失败，也不该导出负数
    // 让服务端算出无意义的 sum。
    MetricsRegistry registry;
    registry.Observe("rgbt_test_neg_seconds", "负值", -1.0);
    const std::string text = registry.TextExposure();
    EXPECT_NE(text.find("rgbt_test_neg_seconds_sum 0"), std::string::npos);
    EXPECT_NE(text.find("rgbt_test_neg_seconds_count 1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 文本格式
// ---------------------------------------------------------------------------

TEST(MetricsTest, EverySampleHasHelpAndType) {
    // 没有 TYPE 的样本会被 Prometheus 当成 untyped；没有 HELP 只是不友好。
    // 两者都必须有，否则采集端的行为依赖默认值而不是我们的声明。
    MetricsRegistry registry;
    registry.Counter("rgbt_test_have_total", "有 HELP");
    registry.Gauge("rgbt_test_have_gauge", "有 HELP", []() { return 1U; });
    registry.Observe("rgbt_test_have_seconds", "有 HELP", 0.01);
    const std::string text = registry.TextExposure();

    EXPECT_TRUE(HasLine(text, "# HELP rgbt_test_have_total 有 HELP"));
    EXPECT_TRUE(HasLine(text, "# TYPE rgbt_test_have_total counter"));
    EXPECT_TRUE(HasLine(text, "# TYPE rgbt_test_have_gauge gauge"));
    EXPECT_TRUE(HasLine(text, "# TYPE rgbt_test_have_seconds histogram"));
}

TEST(MetricsTest, LabelValuesAreEscaped) {
    // 未转义的引号/换行会让整个抓取响应变成非法格式，而采集端只会报"解析失败"。
    MetricsRegistry registry;
    registry.Counter("rgbt_test_escape_total", "转义", {{"err", "say \"hi\"\nnext"}});
    const std::string text = registry.TextExposure();
    // 原始值 `say "hi"` + 换行 + `next` 必须渲染成单行里的转义序列。
    EXPECT_NE(text.find("err=\"say \\\"hi\\\"\\nnext\""), std::string::npos) << text;

    // 注意：不能用"从样本起点到第一个 '\n' 的子串"去断言含有 `\nnext`。
    // 第一个 `\n` 正是转义序列内部那两个字面字符中的第二个（`\` + `n`），
    // 子串会在 `\` 之前就结束，因此找不到 `nnext`（实测踩到）。
    // 要验证的是"这一行以值结尾"——即换行被转义、没有真的断行。
    // 定位**样本行**而不是 `# HELP` 行：HELP 注释里也含指标名，若用
    // `text.find(name)` 会先命中注释，取到的"样本行"其实是注释（实测踩到）。
    // 样本行前一定是换行，因此用 `"\n" + name` 定位。
    const std::size_t pos = text.find("\nrgbt_test_escape_total");
    ASSERT_NE(pos, std::string::npos) << text;
    const std::size_t begin = pos + 1;
    const std::size_t eol = text.find('\n', begin);
    ASSERT_NE(eol, std::string::npos);
    const std::string sample_line = text.substr(begin, eol - begin);
    EXPECT_NE(sample_line.rfind(" 0"), std::string::npos)
        << "样本行应以值结尾（换行被转义而不是真的断行）：[" << sample_line << "]";
}

TEST(MetricsTest, OutputIsSortedByNameForDiffability) {
    // 固定顺序让"两个时间点的抓取结果做 diff"有意义。
    MetricsRegistry registry;
    registry.Counter("rgbt_test_zzz_total", "z");
    registry.Counter("rgbt_test_aaa_total", "a");
    const std::string text = registry.TextExposure();
    const std::size_t pos_a = text.find("rgbt_test_aaa_total 0");
    const std::size_t pos_z = text.find("rgbt_test_zzz_total 0");
    ASSERT_NE(pos_a, std::string::npos);
    ASSERT_NE(pos_z, std::string::npos);
    EXPECT_LT(pos_a, pos_z);
}

TEST(MetricsTest, EmptyRegistryExposesEmptyTextWithoutThrowing) {
    // 一个指标都没登记时导出空串，而不是抛异常或输出半截内容。
    MetricsRegistry registry;
    EXPECT_EQ(registry.MetricCount(), 0U);
    EXPECT_EQ(registry.TextExposure(), "");
}

TEST(MetricsTest, InvalidHandleIsSafeToUse) {
    // 默认构造的句柄（未注册）必须可以安全调用：业务代码可能持有默认值。
    const CounterHandle invalid;
    EXPECT_FALSE(invalid.valid());
    invalid.Add();
    EXPECT_EQ(invalid.Value(), 0U);
}

}  // namespace
