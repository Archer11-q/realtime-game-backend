/// @file mysql_connection.hpp
/// @brief MySQL 连接管理（基于 libmariadb）与参数化查询辅助。
///
/// 设计：
///   * 采用与 RedisSessionStore 相同的策略——构造时不因连接失败而失败，
///     而是在每个请求上返回 kUnavailable。这样 MySQL 抖动不会导致服务反复重启，
///     MySQL 恢复后无需重启服务即可继续工作。
///   * 所有 SQL 都通过 mysql_stmt 预处理接口执行，字符串参数走参数绑定而不是
///     字符串拼接，避免 SQL 注入。这是本模块的硬约束。
///   * 连接、读超时、写超时均为显式配置，避免 MySQL 不可用时请求长时间挂起。
///
/// 退出条件提示：Phase 1 期间 Gateway 直接读 players 表，依据 ADR-0002；
/// Player/State 服务落地后本模块应从 Gateway 移除。

#ifndef RGBT_GATEWAY_MYSQL_CONNECTION_HPP
#define RGBT_GATEWAY_MYSQL_CONNECTION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 前置声明，避免把 mariadb 头文件泄漏给使用方。
struct st_mysql;

namespace rgbt::gateway {

/// MySQL 连接参数。
struct MysqlOptions {
    std::string host = "127.0.0.1";
    std::int32_t port = 3306;
    std::string user;
    std::string password;
    std::string database;

    /// 连接与读写超时（秒）。取正值，避免依赖不可用时长时间挂起。
    std::uint32_t connect_timeout_seconds = 3;
    std::uint32_t read_timeout_seconds = 3;
    std::uint32_t write_timeout_seconds = 3;
};

/// 一行查询结果。按列顺序存放，NULL 用 std::nullopt 表示。
using SqlRow = std::vector<std::optional<std::string>>;

class MysqlConnection {
public:
    MysqlConnection(std::string service_prefix, MysqlOptions options);
    ~MysqlConnection();

    MysqlConnection(const MysqlConnection&) = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;

    /// 执行参数化查询。
    /// @param sql 使用 '?' 作为参数占位符。
    /// @param params 按占位符顺序提供的字符串参数。
    /// @param out_rows 输出结果行。
    /// @return true 表示查询成功（可能返回 0 行）；false 表示连接或语句失败。
    [[nodiscard]] bool Query(const std::string& sql, const std::vector<std::string>& params,
                             std::vector<SqlRow>* out_rows);

    /// @brief 探测当前能否与 MySQL 通信，供启动日志与健康检查使用。
    [[nodiscard]] bool Ping();

    /// @brief 最近一次失败的说明。仅用于日志，不含参数值，避免泄露敏感数据。
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    /// 确保连接可用，必要时重连。返回 false 表示当前不可用。
    bool EnsureConnected();

    /// 关闭并按需释放连接。
    void Disconnect();

    /// 执行一次查询尝试，不做重试。Query 负责在连接失效时重试。
    ///
    /// @param retry_on_dead_connection 遇到「连接已失效」类错误时置为 true，
    ///        调用方据此决定是否用新连接重试一次。
    bool QueryOnce(const std::string& sql, const std::vector<std::string>& params,
                   std::vector<SqlRow>* out_rows, bool* retry_on_dead_connection);

    std::string service_prefix_;
    MysqlOptions options_;
    st_mysql* connection_ = nullptr;
    std::string last_error_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_MYSQL_CONNECTION_HPP
