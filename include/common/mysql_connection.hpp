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
///   * **线程安全**：本类的 public 方法可以被多个线程并发调用，内部用一把互斥锁
///     串行化。这不是"顺手加的保险"——TASK-014 期间实测到并发使用同一条连接会
///     打乱 MySQL 协议流（`Received malformed packet`），进而在重连关闭 TLS 时
///     触发 `OpenSSL internal error: refcount error` 让进程 abort。
///     详见成员 `mutex_` 的说明。
///   * 采用与 RedisSessionStore 相同的策略——构造时不因连接失败而失败，
///     而是在每个请求上返回失败。这样 MySQL 抖动不会导致服务反复重启，
///     MySQL 恢复后无需重启服务即可继续工作。
///   * 所有 SQL 都通过 mysql_stmt 预处理接口执行，字符串参数走参数绑定而不是
///     字符串拼接，避免 SQL 注入。这是本模块的硬约束。
///   * 连接、读超时、写超时均为显式配置，避免依赖不可用时请求长时间挂起。

#ifndef RGBT_COMMON_MYSQL_CONNECTION_HPP
#define RGBT_COMMON_MYSQL_CONNECTION_HPP

#include <cstdint>
#include <mutex>
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
    ///
    /// **按值返回，不返回引用**（TASK-014 修正）。本类现在允许被多线程共用，
    /// 返回 `const std::string&` 会让调用方在另一个线程正写这个字符串时读它——
    /// 那是一处真实的数据竞争，代价是日志里出现撕裂的字符串。
    /// 拷贝一份的成本只有几十字节，换来的是"日志随时可以安全打印"。
    [[nodiscard]] std::string last_error() const;

private:
    /// 确保连接可用，必要时重连。返回 false 表示当前不可用。
    /// **调用方必须已持有 mutex_**：它读写 connection_ 与 last_error_。
    bool EnsureConnected();

    /// 关闭并按需释放连接。**调用方必须已持有 mutex_**。
    void Disconnect();

    /// 执行一次查询尝试，不做重试。Query 负责在连接失效时重试。
    /// **调用方必须已持有 mutex_**。
    ///
    /// @param retry_on_dead_connection 遇到「连接已失效」类错误时置为 true，
    ///        调用方据此决定是否用新连接重试一次。
    bool QueryOnce(const std::string& sql, const std::vector<std::string>& params,
                   std::vector<SqlRow>* out_rows, bool* retry_on_dead_connection);

    /// @brief 已持锁版本的查询实现。Query 与 Ping 都在锁内调用它。
    ///
    /// 为什么要拆出这一层：`Ping()` 想复用 `Query()` 的逻辑，但两者都是 public
    /// 入口、都要自己加锁。若 `Ping` 直接调 `Query`，同一把非递归互斥锁会被
    /// 加锁两次——那是死锁，不是"重复加锁的开销"。拆出一个约定"调用方已持锁"
    /// 的私有函数，是唯一不会自锁的写法。
    bool QueryLocked(const std::string& sql, const std::vector<std::string>& params,
                     std::vector<SqlRow>* out_rows);

    std::string service_prefix_;
    MysqlOptions options_;
    st_mysql* connection_ = nullptr;
    std::string last_error_;

    /// @brief 串行化对这条连接的访问。**本类是线程安全的，靠的就是它。**
    ///
    /// 为什么必须有（TASK-014 实测崩溃，有栈为证）：
    ///   一个 `MYSQL*` 是一条**有状态的字节流**，同一时刻只能有一个语句在上面
    ///   跑。本项目的部署方式却是**多个线程共用一个 `MysqlConnection` 对象**：
    ///     * Room：进程入口创建一条连接，交给 `result_writer` / `snapshot_writer` /
    ///       `snapshot_reader` 三者共用；ticker 线程、brpc worker 线程和启动时的
    ///       主线程都会碰它。TASK-014 的 `Restore` 让「启动瞬间就有一个房间要写
    ///       快照」成为常态，于是 ticker 线程的写快照与主线程的启动探活第一次
    ///       真正并发，协议流被打乱：
    ///         `execute failed: Received malformed packet`
    ///       随后走到"连接已失效"的重连分支，`mysql_close()` 拆除 TLS 时
    ///       OpenSSL 引用计数校验失败，进程直接 abort：
    ///         `OpenSSL internal error: refcount error` → SIGABRT
    ///     * Gateway：一条连接被所有 brpc worker 线程共用做登录查询，
    ///       并发登录踩的是同一个坑，只是验收脚本一直顺序登录而没暴露。
    ///
    /// 为什么是加锁而不是"每个线程一条连接"：
    ///   连接的**数量**是另一件事（连接池属 Phase 3，当前没有容量证据）。
    ///   这里修的是"同一对象被并发使用"这一确定性错误，最小改动就是互斥。
    ///
    /// 已知代价：锁覆盖整个查询（含重连重试），因此慢查询会串行化这条连接上的
    ///   其他调用。这**不是**本锁引入的限制——一条连接本来就无法并发执行两个
    ///   语句。真正的扩容手段是连接池，不是去掉这把锁。
    mutable std::mutex mutex_;
};

}  // namespace rgbt::common

#endif  // RGBT_COMMON_MYSQL_CONNECTION_HPP
