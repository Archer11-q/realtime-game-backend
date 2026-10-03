/// @file logging.hpp
/// @brief 结构化日志：单行 `key=value`、服务名固定、按关联 id 串起一次请求。
///
/// 为什么需要它（TASK-018）：Phase 2 排查问题时最耗时的一环是**把一次请求在三份
/// stderr 日志里对上**。此前是 30 处 `std::fprintf(stderr, "[service] ...")`，
/// 人眼可读但机器不可解析，而且没有任何一个 id 是三个服务共有的。Phase 3 的退出
/// 标准第一条是"任一错误可以定位到服务、请求、会话或房间"——那时做不到。
///
/// 为什么自己写而不是引入 spdlog/glog：
///   * glog 已经在产物里（brpc 依赖它），但它面向的是"人读的文本 + 级别过滤"，
///     固定 `I20261003 ...` 前缀与自由格式文本，不是键值结构；
///   * spdlog 是新依赖，而本模块需要的只有"格式化 + 互斥 + 写 stderr"，
///     多一套日志库只会扩大迁移面与构建时间（`CLAUDE.md` 第 6 条：
///     不以"技术先进"为理由增加组件）。
///
/// 输出格式（单行，字段之间一个空格）：
///
/// ```text
/// ts=2026-10-03T01:02:03.456Z service=gateway level=info event=subscribe_ready sub=7 room=r-1
/// player=p-0001 trace=abc123
/// ```
///
/// **为什么用 `ts=` 而不是裸 ISO8601 时间戳打头**：整行要么全是 `k=v`、要么完全
/// 不是，解析器不需要为主键开特例；`grep 'trace=abc123'` 也能直接命中。
///
/// **恒定的身份字段**（`ts` / `service` / `level` / `event` / `trace`）永远出现在
/// 前面且顺序固定，其余字段按调用方给出的顺序追加。这样人眼扫前几列就能定位
/// "哪个服务、什么级别、哪件事、哪个 trace"。

#ifndef RGBT_COMMON_LOGGING_HPP
#define RGBT_COMMON_LOGGING_HPP

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rgbt::common {

/// 日志级别。取值会写进 `level=` 字段，**是稳定标识**（不翻译成中文）。
enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError,
};

/// 级别名。用于 `level=` 字段与测试断言。
[[nodiscard]] const char* ToString(LogLevel level);

/// 一个附加字段。`value` 是**未转义的原值**，格式化时才处理。
struct LogField {
    std::string key;
    std::string value;
};

/// 一次日志调用的全部输入。
///
/// 用结构体而不是 `Log(level, event, {fields})`：调用点经常是"把已有变量塞进去"，
/// 具名成员比位置参数更难写错；也让格式化与输出这两步都能被单元测试直接调用。
struct LogRecord {
    std::string service;

    /// 关联 id。为空时**不输出** `trace=` 字段，而不是输出 `trace=-`：
    /// 没有 id 与"id 是空串"是两件事，前者不该在日志里伪装成一个值。
    std::string trace_id;

    LogLevel level = LogLevel::kInfo;

    /// 事件名。稳定标识，用 snake_case，例如 `restore_failed`、`backfill_done`。
    /// 排障时先看它，再看字段。
    std::string event;

    std::vector<LogField> fields;

    /// 覆盖时间戳（Unix 毫秒）。0 表示取当前时间。
    ///
    /// 存在的唯一目的是让测试能断言确定的时间；生产路径不需要设置它。
    std::int64_t timestamp_ms = 0;
};

/// 把值转义成**单行且可解析**的形式。
///
/// 规则：
///   * 非空、不含空格、不含 `=`、不含 `"`、不含控制字符 → 原样返回；
///   * 否则用双引号包起来，并转义 `"` 与 `\`，控制字符转成 `\n` / `\r` / `\t`
///     或 `\xNN`。
///
/// 为什么必须转义而不是"直接塞进去"：本模块的字段值大量来自配置、数据库错误
/// 信息与 MySQL/Redis 的原始报文，里面出现空格、引号甚至换行都很正常。
/// 少一次转义就会让**一条坏字段把整行日志变成不可解析的两行**，而那恰好发生在
/// 排查最需要日志的时候。
[[nodiscard]] std::string EscapeFieldValue(std::string_view value);

/// 把一条记录格式化成单行。
///
/// 这是纯函数：不读时钟（除 `timestamp_ms == 0` 时）、不加锁、不写任何输出。
/// 因此它可以在单元测试里被直接断言，也不需要为了测试去捕获 stderr。
[[nodiscard]] std::string FormatLogLine(const LogRecord& record);

/// 设置当前进程的服务名（`service=` 字段）。应由各服务入口调用一次。
///
/// 线程安全：与写入共用同一把锁。
void SetServiceName(std::string name);

/// 当前服务名（未设置时为空字符串）。仅供测试与排障使用。
[[nodiscard]] std::string ServiceName();

/// 写一条日志到 stderr。
///
/// 与今天的 `fprintf(stderr, ...)` 行为一致：**不抛异常、不影响调用方**。
/// 日志是旁路，任何写失败都只能被忽略——为了一条日志让业务路径失败是本末倒置。
void Log(const LogRecord& record);

// 便捷入口。`fields` 是 `{"key", "value"}` 的初始化列表。
void LogInfo(std::string_view event, std::initializer_list<LogField> fields = {});
void LogWarn(std::string_view event, std::initializer_list<LogField> fields = {});
void LogError(std::string_view event, std::initializer_list<LogField> fields = {});

/// 便捷入口（带 trace_id）。`trace_id` 为空时不输出该字段。
void LogInfo(std::string_view event, std::string_view trace_id,
             std::initializer_list<LogField> fields = {});
void LogWarn(std::string_view event, std::string_view trace_id,
             std::initializer_list<LogField> fields = {});
void LogError(std::string_view event, std::string_view trace_id,
              std::initializer_list<LogField> fields = {});

// ---------------------------------------------------------------------------
// 关于"解析"
// ---------------------------------------------------------------------------
//
// 本模块**只负责产生**日志，不提供解析函数。理由：解析的消费者只有两个——
// 单元测试（断言"格式化的结果能不能被解析回同样的字段"）与验收脚本
// （`scripts/verify-observability.sh` 要按 trace_id 从三份日志里捞出同一条链路）。
// 前者的解析器写在测试里更有价值（它顺带校验了"格式是否真的自洽"）；
// 后者用 shell/awk 实现更直接。把解析器放进产品代码会让 `logging` 承担两个方向的
// 责任，而没有任何运行时消费者需要它。
}  // namespace rgbt::common

#endif  // RGBT_COMMON_LOGGING_HPP
