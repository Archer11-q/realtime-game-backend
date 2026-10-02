#include "common/mysql_connection.hpp"

// mysql.h 内部已经包含 mariadb_stmt.h（预处理语句与 MYSQL_BIND 的定义都在那里），
// 因此这里只包含 mysql.h；重复包含会触发类型重定义错误。
//
// 为什么写 <mysql.h> 而不是 <mysql/mysql.h>：vcpkg 的 unofficial::libmariadb 把
// `${prefix}/include/mysql` 作为 INTERFACE_INCLUDE_DIRECTORIES 暴露出来（见
// share/unofficial-libmariadb/unofficial-libmariadb-targets.cmake），因此正确的
// 引用形式就是 <mysql.h>。
//
// 该模块原先在 src/gateway/ 时写的是 <mysql/mysql.h>，能编过是因为 gateway 同时
// 链接了 brpc/protobuf，那些包额外把 `${prefix}/include` 加进了搜索路径——属于
// 靠传递依赖碰巧成立。移到公共目录后不再享有这个副作用，因此修正为正确写法。
#include <mysql.h>

#include <array>
#include <cstdio>
#include <mutex>
#include <utility>

namespace rgbt::common {
namespace {

/// 预处理语句的参数个数上限。
///
/// 2026-10-02 由 8 提升到 32（TASK-013）。原值 8 是按当时的最大用量定的
/// （「Room 写 match_results 时需要 6 个参数」），但当 TASK-013 要写 15 列的
/// 房间快照时，这个上限直接变成 `too many params` 失败，一个参数也绑不上。
///
/// 为什么用「提高上限」而不是「拆小语句」解决：这个上限只是
/// `param_binds` 定长数组的容量，32 个 MYSQL_BIND 约 2 KB 栈空间，代价可忽略；
/// 而拆语句会把一条原子 upsert 变成多条需要自己保证顺序的语句，
/// 引入的复杂度远大于收益。
///
/// 取值 32 而不是刚好 15：给后续新增列留出余量，避免每加一列就要改这里。
/// 仍保留上限本身是有价值的——它把「参数个数失控」变成一次明确失败，
/// 而不是让定长数组越界。
constexpr unsigned int kMaxBindParams = 32;

/// 一行结果允许的最大列数。players 与 match_results 都不超过 10 列。
constexpr unsigned int kMaxResultColumns = 16;

/// 单列读取缓冲区大小。player_id/account/display_name 均不超过 64 字符。
constexpr unsigned long kColumnBufferSize = 256;

/// 判断错误码是否表示「连接已失效」。
///
/// 容器重启、网络中断或服务端主动断开时，已建立的连接会失效。此时必须换一条
/// 新连接重试，而不是把错误直接返回给调用方——否则依赖恢复后服务仍会持续失败
/// （这正是实现初期观察到的问题）。
bool IsConnectionLostError(unsigned int error_code) {
    switch (error_code) {
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2013:  // CR_SERVER_LOST
        case 2003:  // CR_CONN_HOST_ERROR
        case 2002:  // CR_CONNECTION_ERROR
            return true;
        default:
            return false;
    }
}

}  // namespace

MysqlConnection::MysqlConnection(std::string service_prefix, MysqlOptions options)
    : service_prefix_(std::move(service_prefix)), options_(std::move(options)) {
    // 说明：不使用自动重连选项。它会掩盖连接状态，且不同客户端行为不一致；
    // 本实现改为**不做预检**，只在语句真的失败且错误码表示"连接已失效"时
    // 显式换一条新连接重试一次（见 QueryLocked）。
    (void)service_prefix_;
}

MysqlConnection::~MysqlConnection() {
    // 加锁只为让"析构时的关闭"与其他线程的查询互斥。按契约，对象析构时不应
    // 还有别的线程在用同一个对象；但那属于调用方的责任，本类不该假设它一定成立。
    const std::lock_guard<std::mutex> lock(mutex_);
    Disconnect();
}

std::string MysqlConnection::last_error() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void MysqlConnection::Disconnect() {
    if (connection_ != nullptr) {
        mysql_close(connection_);
        connection_ = nullptr;
    }
}

bool MysqlConnection::EnsureConnected() {
    // 已有连接就直接用，**不做 mysql_ping 预检**。
    //
    // 为什么去掉预检（TASK-014 实测，有 gdb 栈为证）：
    //   在**一次成功的预处理查询之后**再调用 mysql_ping，会让进程 abort：
    //
    //     OPENSSL_die("refcount error")        <- libmariadb 内嵌 OpenSSL
    //     SSL_CTX_free <- SSL_free <- ma_tls_close
    //     <- ma_pvio_close <- end_server <- ma_net_safe_read <- mysql_ping
    //
    //   即 mysql_ping 的读失败后关闭 TLS 连接时，libmariadb 与 OpenSSL 之间
    //   的引用计数对不上，直接 abort。这是 libmariadb 3.4.8 + vcpkg OpenSSL
    //   在「先查后 ping」这个顺序下的问题，不是本项目的代码错误。
    //
    //   触发它需要"成功查询 → ping"这个顺序。本项目原有代码从不产生这个顺序
    //   （探活只在启动时做一次，那时还没有查询），是 TASK-014 的
    //   `RoomManager::Restore` 先读快照、紧接着启动探活才把它暴露出来。
    //
    // 去掉预检不会降低可靠性：连接失效时 `QueryOnce` 会在
    // `mysql_stmt_prepare` / `mysql_stmt_execute` 上拿到 2002/2003/2006/2013，
    //   `IsConnectionLostError` 把它们判为"连接已失效"，`Query` 随即换一条
    //   新连接重试一次。这条路径本来就是为依赖恢复设计的，比 ping 更贴近真实
    //   使用（它检验的是"能不能真的执行语句"，而不只是"套接字还在不在"）。
    if (connection_ != nullptr) {
        return true;
    }

    connection_ = mysql_init(nullptr);
    if (connection_ == nullptr) {
        last_error_ = "mysql_init failed";
        return false;
    }

    // 超时必须通过 mysql_options 设置：MYSQL 结构体未暴露 options 成员
    // （libmariadb 的实现细节，不能直接访问）。
    const unsigned int connect_timeout = options_.connect_timeout_seconds;
    const unsigned int read_timeout = options_.read_timeout_seconds;
    const unsigned int write_timeout = options_.write_timeout_seconds;
    mysql_options(connection_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(connection_, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    mysql_options(connection_, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);

    if (mysql_real_connect(connection_, options_.host.c_str(), options_.user.c_str(),
                           options_.password.c_str(), options_.database.c_str(),
                           static_cast<unsigned int>(options_.port), nullptr, 0) == nullptr) {
        // 只记录服务端返回的说明，不拼接密码或查询参数，避免敏感信息进日志。
        last_error_ = "connect failed: ";
        last_error_ += mysql_error(connection_);
        std::fprintf(stderr, "[mysql] %s:%d %s\n", options_.host.c_str(), options_.port,
                     last_error_.c_str());
        Disconnect();
        return false;
    }

    // 统一 utf8mb4，与建表语句的字符集一致。
    if (mysql_set_character_set(connection_, "utf8mb4") != 0) {
        last_error_ = "set charset failed";
        Disconnect();
        return false;
    }
    return true;
}

bool MysqlConnection::Ping() {
    // 探活走**与真实查询完全相同**的代码路径，而不是 mysql_ping。
    //
    // 两个理由：
    //   1. `mysql_ping` 走 libmariadb 的旧协议路径，而本类所有业务查询都走预处理
    //      接口。用一条路径探活、另一条路径干活，"探活通过"并不保证查询能成功。
    //   2. 更要紧的是 `mysql_ping` 在本环境下会崩（原因见 EnsureConnected 的注释）。
    //      与其在调用顺序上绕开它，不如根本不碰它。
    //
    // `QueryLocked` 内部已经包含"连接失效则换新连接重试一次"的逻辑，因此这里
    // 不需要任何额外处理。
    //
    // 注意这里**不能**调用 public 的 `Query`：它也要加同一把锁，会自锁死。
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SqlRow> rows;
    return QueryLocked("SELECT 1", {}, &rows);
}

bool MysqlConnection::QueryOnce(const std::string& sql, const std::vector<std::string>& params,
                                std::vector<SqlRow>* out_rows, bool* retry_on_dead_connection) {
    *retry_on_dead_connection = false;

    if (!EnsureConnected()) {
        // 连接阶段就失败：属于依赖不可用，无需在同一请求内重试。
        return false;
    }

    MYSQL_STMT* statement = mysql_stmt_init(connection_);
    if (statement == nullptr) {
        last_error_ = "stmt_init failed";
        return false;
    }

    bool ok = false;
    bool failed = false;
    do {
        if (mysql_stmt_prepare(statement, sql.c_str(), static_cast<unsigned long>(sql.size())) !=
            0) {
            last_error_ = "prepare failed: ";
            last_error_ += mysql_stmt_error(statement);
            if (IsConnectionLostError(mysql_stmt_errno(statement))) {
                *retry_on_dead_connection = true;
            }
            failed = true;
            break;
        }

        // 参数绑定：所有值都作为字符串传递，绝不拼接 SQL。
        std::array<MYSQL_BIND, kMaxBindParams> param_binds{};
        if (!params.empty()) {
            for (std::size_t i = 0; i < params.size(); ++i) {
                // const_cast 在此安全：mysql_stmt_bind_param 只读取该缓冲区。
                param_binds[i].buffer_type = MYSQL_TYPE_STRING;
                param_binds[i].buffer = const_cast<char*>(params[i].data());
                param_binds[i].buffer_length = static_cast<unsigned long>(params[i].size());
            }
            if (mysql_stmt_bind_param(statement, param_binds.data()) != 0) {
                last_error_ = "bind_param failed: ";
                last_error_ += mysql_stmt_error(statement);
                failed = true;
                break;
            }
        }

        if (mysql_stmt_execute(statement) != 0) {
            last_error_ = "execute failed: ";
            last_error_ += mysql_stmt_error(statement);
            // 连接被服务端关闭时（例如容器重启）错误码为 2006/2013，
            // 此时应换新连接重试，而不是直接失败。
            if (IsConnectionLostError(mysql_stmt_errno(statement))) {
                *retry_on_dead_connection = true;
            }
            failed = true;
            break;
        }

        MYSQL_RES* metadata = mysql_stmt_result_metadata(statement);
        const unsigned int column_count = metadata != nullptr ? mysql_num_fields(metadata) : 0;
        if (metadata != nullptr) {
            mysql_free_result(metadata);
        }
        if (column_count > kMaxResultColumns) {
            last_error_ = "too many columns";
            failed = true;
            break;
        }

        if (column_count > 0) {
            std::array<MYSQL_BIND, kMaxResultColumns> result_binds{};
            std::array<std::array<char, kColumnBufferSize>, kMaxResultColumns> buffers{};
            std::array<unsigned long, kMaxResultColumns> lengths{};
            std::array<my_bool, kMaxResultColumns> null_flags{};

            for (unsigned int i = 0; i < column_count; ++i) {
                result_binds[i].buffer_type = MYSQL_TYPE_STRING;
                result_binds[i].buffer = buffers[i].data();
                result_binds[i].buffer_length = kColumnBufferSize - 1;
                result_binds[i].length = &lengths[i];
                result_binds[i].is_null = &null_flags[i];
            }
            if (mysql_stmt_bind_result(statement, result_binds.data()) != 0) {
                last_error_ = "bind_result failed: ";
                last_error_ += mysql_stmt_error(statement);
                failed = true;
                break;
            }

            while (true) {
                const int fetch_status = mysql_stmt_fetch(statement);
                if (fetch_status == MYSQL_NO_DATA) {
                    break;
                }
                if (fetch_status != 0) {
                    // MYSQL_DATA_TRUNCATED 表示列值超出缓冲区；
                    // 宁可报错，也不返回被截断的数据。
                    last_error_ = "fetch failed or value truncated";
                    if (mysql_stmt_errno(statement) != 0 &&
                        IsConnectionLostError(mysql_stmt_errno(statement))) {
                        *retry_on_dead_connection = true;
                    }
                    failed = true;
                    break;
                }
                SqlRow row;
                row.reserve(column_count);
                for (unsigned int i = 0; i < column_count; ++i) {
                    if (null_flags[i] != 0) {
                        row.emplace_back(std::nullopt);
                    } else {
                        row.emplace_back(std::string(buffers[i].data(), lengths[i]));
                    }
                }
                out_rows->push_back(std::move(row));
            }
            if (failed) {
                break;
            }
        }
        ok = true;
    } while (false);

    mysql_stmt_close(statement);

    if (!ok) {
        out_rows->clear();
        return false;
    }
    return true;
}

bool MysqlConnection::Query(const std::string& sql, const std::vector<std::string>& params,
                            std::vector<SqlRow>* out_rows) {
    // 整个查询（含可能的重连重试）在一把锁内完成。理由与代价见头文件里
    // mutex_ 的说明：一条连接是有状态的字节流，并发使用会打乱协议。
    const std::lock_guard<std::mutex> lock(mutex_);
    return QueryLocked(sql, params, out_rows);
}

bool MysqlConnection::QueryLocked(const std::string& sql, const std::vector<std::string>& params,
                                  std::vector<SqlRow>* out_rows) {
    if (out_rows == nullptr) {
        last_error_ = "null output";
        return false;
    }
    out_rows->clear();

    if (params.size() > kMaxBindParams) {
        // 错误信息必须带上实际值与上限。原来的 "too many params" 两头都没有，
        // 调用方只看到一句"参数太多"，既不知道该减到多少，也不知道这个限制
        // 来自哪里——TASK-013 为此多花了好几轮排查。
        last_error_ = "too many params: got " + std::to_string(params.size()) + ", limit is " +
                      std::to_string(kMaxBindParams);
        return false;
    }

    bool retry_on_dead_connection = false;
    if (QueryOnce(sql, params, out_rows, &retry_on_dead_connection)) {
        last_error_.clear();
        return true;
    }

    if (retry_on_dead_connection) {
        // 连接已失效：丢弃它并用新连接重试一次。
        //
        // 为什么需要这一步：mysql_ping 在服务端刚恢复时可能返回成功（TCP 可建立），
        // 真正失效要等到第一条语句才暴露。若不在此重试，服务将长期返回 503，
        // 无法在依赖恢复后自愈。
        std::fprintf(stderr, "[mysql] connection lost, reconnecting and retrying once\n");
        Disconnect();
        bool retry_result = false;
        if (QueryOnce(sql, params, out_rows, &retry_result)) {
            last_error_.clear();
            return true;
        }
    }

    return false;
}

}  // namespace rgbt::common
