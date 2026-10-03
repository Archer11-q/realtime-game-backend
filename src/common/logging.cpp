/// @file logging.cpp
/// @brief 结构化日志的实现。见 logging.hpp 的格式说明与取舍理由。

#include "common/logging.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <utility>

namespace rgbt::common {
namespace {

/// 保护 `service_name_` 与整行写出。
///
/// 为什么整行写成也要持锁：`fprintf` 对**单个 FILE*` 是线程安全的，但一条日志
/// 往往由多次写入组成（前缀 + 各字段 + 换行）。若不加锁，两个线程的行会互相穿插，
/// 产生**半截属于 A、半截属于 B** 的记录——那正是"机器不可解析"的来源。
/// 用一把锁把"组装好的整行一次写出去"变成原子操作。
std::mutex g_mutex;

std::string g_service_name = "unknown";

bool NeedsQuoting(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == ' ' || ch == '=' || ch == '"' || ch == '\\' || c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

/// 把 Unix 毫秒格式化成 `2026-10-03T01:02:03.456Z`。
///
/// 用 UTC 而不是本地时间：三个服务可能在不同时区/容器里运行，日志时间必须能直接
/// 横向比较；带 `Z` 后缀也让"这是 UTC"不需要靠约定去记。
std::string FormatTimestamp(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    const int millis = static_cast<int>(timestamp_ms % 1000);
    std::tm parts{};
    gmtime_r(&seconds, &parts);

    // 缓冲留足（最坏 `%04d` 在极端年份会写 5 位以上），并按 snprintf 的返回值截断：
    // 直接 `return buffer` 会踩到 `-Werror=format-truncation`，而那个告警值得听——
    // 它指出的是"潜在未终止缓冲区"这个真实类别的问题。
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                      parts.tm_hour, parts.tm_min, parts.tm_sec, millis);
    if (written <= 0) {
        return "-";
    }
    const auto length = static_cast<std::size_t>(written);
    return std::string(buffer, length < sizeof(buffer) ? length : sizeof(buffer) - 1);
}

std::int64_t SystemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void AppendField(std::string* line, std::string_view key, std::string_view value) {
    line->push_back(' ');
    line->append(key);
    line->push_back('=');
    line->append(EscapeFieldValue(value));
}

LogRecord MakeRecord(LogLevel level, std::string_view event, std::string_view trace_id,
                     std::initializer_list<LogField> fields) {
    LogRecord record;
    record.level = level;
    record.event = std::string(event);
    record.trace_id = std::string(trace_id);
    record.fields.assign(fields.begin(), fields.end());
    return record;
}

}  // namespace

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kError:
            return "error";
    }
    return "info";
}

std::string EscapeFieldValue(std::string_view value) {
    if (!NeedsQuoting(value)) {
        return std::string(value);
    }

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
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
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 ||
                    static_cast<unsigned char>(ch) == 0x7f) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\x%02x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string FormatLogLine(const LogRecord& record) {
    const std::int64_t timestamp = record.timestamp_ms != 0 ? record.timestamp_ms : SystemNowMs();

    std::string line;
    line.reserve(128 + record.event.size());
    line += "ts=";
    line += FormatTimestamp(timestamp);
    AppendField(&line, "service", record.service);
    AppendField(&line, "level", ToString(record.level));
    AppendField(&line, "event", record.event);

    // 空 id 不输出字段：见头文件说明（"没有 id"与"id 是空串"是两件事）。
    if (!record.trace_id.empty()) {
        AppendField(&line, "trace", record.trace_id);
    }

    for (const LogField& field : record.fields) {
        AppendField(&line, field.key, field.value);
    }
    line.push_back('\n');
    return line;
}

void SetServiceName(std::string name) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_service_name = std::move(name);
}

std::string ServiceName() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_service_name;
}

void Log(const LogRecord& record) {
    LogRecord with_service = record;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (with_service.service.empty()) {
        with_service.service = g_service_name;
    }
    const std::string line = FormatLogLine(with_service);
    // 一次写出整行，保证不同线程的记录不会互相穿插。
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void LogInfo(std::string_view event, std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kInfo, event, {}, fields));
}

void LogWarn(std::string_view event, std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kWarn, event, {}, fields));
}

void LogError(std::string_view event, std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kError, event, {}, fields));
}

void LogInfo(std::string_view event, std::string_view trace_id,
             std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kInfo, event, trace_id, fields));
}

void LogWarn(std::string_view event, std::string_view trace_id,
             std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kWarn, event, trace_id, fields));
}

void LogError(std::string_view event, std::string_view trace_id,
              std::initializer_list<LogField> fields) {
    Log(MakeRecord(LogLevel::kError, event, trace_id, fields));
}

}  // namespace rgbt::common
