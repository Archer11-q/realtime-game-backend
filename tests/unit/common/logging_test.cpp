/// @file logging_test.cpp
/// @brief 结构化日志的单元测试（TASK-018）。
///
/// 为什么这些用例值得写：日志本身不影响业务，但它**是 Phase 3 退出标准第一条
/// （"任一错误可以定位到服务、请求、会话或房间"）的唯一依托**。字段拼错、少一个
/// trace、或者一条带空格的错误信息把整行撑成两行，都不会让任何功能失败——
/// 只会让排查在最需要日志的时候失效。这类缺陷只能靠断言格式拦住。
///
/// 测试自带一个**独立实现的解析器**（不调用产品代码的函数，因为产品代码只负责
/// 产生）：这样"格式化的结果能不能被解析回同样的字段"才是真的被验证，
/// 而不是拿同一套实现自证。验收脚本用的也是同样的解析语义。

#include "common/logging.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using rgbt::common::EscapeFieldValue;
using rgbt::common::FormatLogLine;
using rgbt::common::LogField;
using rgbt::common::LogLevel;
using rgbt::common::LogRecord;
using rgbt::common::ServiceName;
using rgbt::common::SetServiceName;
using rgbt::common::ToString;

/// 固定时间戳：2026-10-03T01:02:03.456Z
constexpr std::int64_t kFixedMs = 1'790'989'323'456LL;

/// 测试自己的解析器：把一行拆成 `key -> value`（已反转义）。
///
/// 独立实现是本文件刻意的设计：见文件头注释。
struct Parsed {
    bool ok = false;
    std::vector<std::pair<std::string, std::string>> entries;

    [[nodiscard]] std::string Get(const std::string& key) const {
        for (const auto& entry : entries) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return {};
    }
    /// 键是否存在。**不能**用"Get 是否为空"代替：空值是合法的
    /// （转义成 `""`），与"字段不存在"是两件事。
    [[nodiscard]] bool HasKey(const std::string& key) const {
        for (const auto& entry : entries) {
            if (entry.first == key) {
                return true;
            }
        }
        return false;
    }
};

/// 解析一行 `k=v` 文本。值可带双引号（内部支持 \" \\ \n \r \t \xNN）。
Parsed ParseLine(const std::string& line) {
    Parsed parsed;
    if (line.empty() || line.back() != '\n') {
        return parsed;  // 必须以换行结束：否则说明上一行没写完就被截断
    }
    const std::string body = line.substr(0, line.size() - 1);
    if (body.empty()) {
        return parsed;
    }

    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t eq = body.find('=', pos);
        if (eq == std::string::npos) {
            return parsed;
        }
        const std::string key = body.substr(pos, eq - pos);
        if (key.empty()) {
            return parsed;
        }
        pos = eq + 1;

        std::string value;
        if (pos < body.size() && body[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < body.size()) {
                const char ch = body[pos];
                if (ch == '\\' && pos + 1 < body.size()) {
                    const char next = body[pos + 1];
                    switch (next) {
                        case 'n':
                            value.push_back('\n');
                            break;
                        case 'r':
                            value.push_back('\r');
                            break;
                        case 't':
                            value.push_back('\t');
                            break;
                        case '"':
                            value.push_back('"');
                            break;
                        case '\\':
                            value.push_back('\\');
                            break;
                        case 'x': {
                            if (pos + 3 >= body.size()) {
                                return parsed;
                            }
                            const int code = std::stoi(body.substr(pos + 2, 2), nullptr, 16);
                            value.push_back(static_cast<char>(code));
                            pos += 2;
                            break;
                        }
                        default:
                            value.push_back(next);
                            break;
                    }
                    pos += 2;
                    continue;
                }
                if (ch == '"') {
                    closed = true;
                    ++pos;
                    break;
                }
                value.push_back(ch);
                ++pos;
            }
            if (!closed) {
                return parsed;
            }
        } else {
            const std::size_t space = body.find(' ', pos);
            const std::size_t end = space == std::string::npos ? body.size() : space;
            value = body.substr(pos, end - pos);
            pos = end;
        }
        parsed.entries.emplace_back(key, value);
        if (pos < body.size()) {
            if (body[pos] != ' ') {
                return parsed;
            }
            ++pos;
        }
    }
    parsed.ok = true;
    return parsed;
}

LogRecord Sample() {
    LogRecord record;
    record.service = "gateway";
    record.trace_id = "abc123";
    record.level = LogLevel::kInfo;
    record.event = "subscribe_ready";
    record.timestamp_ms = kFixedMs;
    record.fields = {{"sub", "7"}, {"room", "r-1"}, {"player", "p-0001"}};
    return record;
}

// ---------------------------------------------------------------------------
// 级别名
// ---------------------------------------------------------------------------

TEST(LoggingTest, LevelNamesAreStableIdentifiers) {
    // 这四个字符串是稳定标识（会被写进日志与被脚本匹配），改动即破坏兼容。
    EXPECT_STREQ(ToString(LogLevel::kDebug), "debug");
    EXPECT_STREQ(ToString(LogLevel::kInfo), "info");
    EXPECT_STREQ(ToString(LogLevel::kWarn), "warn");
    EXPECT_STREQ(ToString(LogLevel::kError), "error");
}

// ---------------------------------------------------------------------------
// 格式化：恒定字段与顺序
// ---------------------------------------------------------------------------

TEST(LoggingTest, FixedFieldsAppearFirstInFixedOrder) {
    const std::string line = FormatLogLine(Sample());
    const Parsed parsed = ParseLine(line);
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.entries.size(), 8U);
    // 前五个是恒定的身份字段，顺序固定；其余按调用方给出的顺序。
    EXPECT_EQ(parsed.entries[0].first, "ts");
    EXPECT_EQ(parsed.entries[1].first, "service");
    EXPECT_EQ(parsed.entries[2].first, "level");
    EXPECT_EQ(parsed.entries[3].first, "event");
    EXPECT_EQ(parsed.entries[4].first, "trace");
    EXPECT_EQ(parsed.entries[5].first, "sub");
    EXPECT_EQ(parsed.entries[6].first, "room");
    EXPECT_EQ(parsed.entries[7].first, "player");
}

TEST(LoggingTest, TimestampIsUtcWithMillisAndZSuffix) {
    // 固定时间戳必须格式化成确定的 UTC 串——日志时间的可比较性依赖它。
    const Parsed parsed = ParseLine(FormatLogLine(Sample()));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.Get("ts"), "2026-10-03T01:02:03.456Z");
}

TEST(LoggingTest, EmptyTraceIdOmitsTheFieldEntirely) {
    // "没有 id"与"id 是空串"是两件事：前者不该在日志里伪装成一个值。
    LogRecord record = Sample();
    record.trace_id.clear();
    const Parsed parsed = ParseLine(FormatLogLine(record));
    ASSERT_TRUE(parsed.ok);
    EXPECT_FALSE(parsed.HasKey("trace"));
    EXPECT_EQ(parsed.Get("event"), "subscribe_ready");
}

TEST(LoggingTest, SingleLineEndsWithExactlyOneNewline) {
    // 每条记录恰好一行：多一个换行会把日志切成两条，少一个会让两条黏在一起。
    const std::string line = FormatLogLine(Sample());
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.back(), '\n');
    EXPECT_EQ(line.find('\n'), line.size() - 1);
}

TEST(LoggingTest, NoFieldsStillProducesAParsableLine) {
    LogRecord record;
    record.service = "room";
    record.event = "tick";
    record.timestamp_ms = kFixedMs;
    const Parsed parsed = ParseLine(FormatLogLine(record));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.entries.size(), 4U);
    EXPECT_EQ(parsed.Get("service"), "room");
    EXPECT_EQ(parsed.Get("level"), "info");  // 默认级别是 info
}

// ---------------------------------------------------------------------------
// 格式化：转义（本模块最容易出错的地方）
// ---------------------------------------------------------------------------

TEST(LoggingTest, PlainValuesAreNotQuoted) {
    // 常见值保持可读：给每个值都加引号会让日志难扫，也会让 grep 变麻烦。
    EXPECT_EQ(EscapeFieldValue("r-1"), "r-1");
    EXPECT_EQ(EscapeFieldValue("p-0001"), "p-0001");
    EXPECT_EQ(EscapeFieldValue("42"), "42");
    EXPECT_EQ(EscapeFieldValue("hp_zero"), "hp_zero");
}

TEST(LoggingTest, ValueWithSpaceIsQuoted) {
    EXPECT_EQ(EscapeFieldValue("Access denied"), "\"Access denied\"");
}

TEST(LoggingTest, ValueWithEqualsIsQuoted) {
    // 不转义的话会被解析器当成新的 `key=`，把后面的内容吃成另一个字段。
    EXPECT_EQ(EscapeFieldValue("a=b"), "\"a=b\"");
}

TEST(LoggingTest, EmptyValueIsQuotedSoItStaysVisible) {
    // 空值必须写成 `""`：否则 `key=` 后面直接跟空格，解析器能过但人眼看不出
    // "这个字段存在但为空"还是"这个字段根本没有"。
    EXPECT_EQ(EscapeFieldValue(""), "\"\"");
}

TEST(LoggingTest, QuotesAndBackslashesAreEscaped) {
    EXPECT_EQ(EscapeFieldValue("say \"hi\""), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(EscapeFieldValue("C:\\path"), "\"C:\\\\path\"");
}

TEST(LoggingTest, NewlineInsideValueDoesNotSplitTheLine) {
    // 这是本模块存在的核心理由之一：MySQL/Redis 的原始错误信息里带换行很常见，
    // 少一次转义就会把一条日志变成两条不可解析的行。
    const std::string escaped = EscapeFieldValue("line1\nline2");
    EXPECT_EQ(escaped, "\"line1\\nline2\"");
    EXPECT_EQ(escaped.find('\n'), std::string::npos);

    LogRecord record = Sample();
    record.fields = {{"err", "ERROR 1045\nAccess denied"}};
    const std::string line = FormatLogLine(record);
    EXPECT_EQ(line.find('\n'), line.size() - 1);  // 只有结尾那一个换行
    const Parsed parsed = ParseLine(line);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.Get("err"), "ERROR 1045\nAccess denied");
}

TEST(LoggingTest, ControlCharactersAreEscaped) {
    EXPECT_EQ(EscapeFieldValue(std::string("a\x01", 2)), "\"a\\x01\"");
    EXPECT_EQ(EscapeFieldValue("tab\there"), "\"tab\\there\"");
    EXPECT_EQ(EscapeFieldValue("cr\rhere"), "\"cr\\rhere\"");
}

TEST(LoggingTest, RoundTripPreservesTrickyValues) {
    // 往返断言：格式化 → 解析 → 值必须一致。这是"日志可解析"的定义。
    const std::vector<std::string> values = {
        "plain",         "with space",  "with\t tab", "with \"quote\"",     "with \\ backslash",
        "with\nnewline", "with=equals", "",           "混合 中文 与 ascii",
    };
    for (const std::string& value : values) {
        LogRecord record = Sample();
        record.fields = {{"payload", value}};
        const Parsed parsed = ParseLine(FormatLogLine(record));
        ASSERT_TRUE(parsed.ok) << "无法解析 value=[" << value << "]";
        EXPECT_EQ(parsed.Get("payload"), value) << "往返丢失: [" << value << "]";
    }
}

// ---------------------------------------------------------------------------
// 服务名
// ---------------------------------------------------------------------------

TEST(LoggingTest, ServiceNameCanBeSetAndRead) {
    const std::string original = ServiceName();
    SetServiceName("gateway");
    EXPECT_EQ(ServiceName(), "gateway");
    SetServiceName("room");
    EXPECT_EQ(ServiceName(), "room");
    SetServiceName(original);  // 复原，避免影响其它用例
}

TEST(LoggingTest, RecordServiceOverridesProcessServiceName) {
    // 记录里显式给了服务名就以它为准；只有为空时才回落到进程级服务名。
    LogRecord record = Sample();
    record.service.clear();
    record.timestamp_ms = kFixedMs;
    SetServiceName("match");
    const Parsed parsed = ParseLine(FormatLogLine(record));
    ASSERT_TRUE(parsed.ok);
    // FormatLogLine 是纯函数，不做回落；回落发生在 Log() 里。
    EXPECT_TRUE(parsed.HasKey("service"));
    EXPECT_EQ(parsed.Get("service"), "");
    SetServiceName("");
}

}  // namespace
