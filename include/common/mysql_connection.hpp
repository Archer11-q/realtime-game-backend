/// @file mysql_connection.hpp
/// @brief MySQL 连接管理（基于 libmariadb）与参数化查询辅助。
///
/// 位置说明（TASK-008）：
///   本模块原先位于 src/gateway/。TASK-008 的 Room/Battle Service 也需要写
///   match_results 表，于是它成为**被两个服务使用**的代码。按
///   docs/01-architecture.md 第 11 节的放置规则（判据是依赖方向，不是"它是不是
///   头文件"），必须移入 include/common/，避免服务之间通过源码目录互相依赖。
///
/// 构建说明：本模块依赖 vcpkg 提供的 libmariadb，而默认预设（debug/release/asan）
/// 没有 vcpkg。因此它单独构成 rgbt_common_mysql 目标，只在启用 vcpkg 时构建，
/// 不并入 rgbt_common（见 src/common/CMakeLists.txt）。
///
/// 设计：
///   * 采用与 RedisSessionStore 相同的策略——构造时不因连接失败而失败，
///     而是在每个请求上返回失败。这样 MySQL 抖动不会导致服务反复重启，
///     MySQL 恢复后无需重启服务即可继续工作。
///   * 所有 SQL 都通过 mysql_stmt 预处理接口执行，字符串参数走参数绑定而不是
///     字符串拼接，避免 SQL 注入。这是本模块的硬约束。
///   * 连接、读超时、写超时均为显式配置，避免依赖不可用时请求长时间挂起。

#ifndef RGBT_COMMON_MYSQL_CONNECTION_HPP
#define RGBT_COMMON_MYSQL_CONNECTION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 前置声明，避免把 mariadb 头文件泄漏给使用方。
struct st_mysql;

namespace rgbt::common {

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
    /// @param service_prefix 仅用于日志前缀，便于区分是哪个服务在报错。
    MysqlConnection(std::string service_prefix, MysqlOptions options);
    ~MysqlConnection();

    MysqlConnection(const MysqlConnection&) = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;

    /// 执行参数化查询。
    /// @param sql 使用 '?' 作为参数占位符。
    /// @param params 按占位符顺序提供的字符串参数。
    /// @param out_rows 输出结果行。
    /// @return true 表示查询成功（可能返回 0 行）；false 表示连接或语句失败。
    ///
    /// 写语句（INSERT/UPDATE）同样走本方法：成功时 out_rows 为空。
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

}  // namespace rgbt::common

#endif  // RGBT_COMMON_MYSQL_CONNECTION_HPP
