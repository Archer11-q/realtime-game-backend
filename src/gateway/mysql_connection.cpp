#include "mysql_connection.hpp"

// mysql.h 内部已经包含 mariadb_stmt.h（预处理语句与 MYSQL_BIND 的定义都在那里），
// 因此这里只包含 mysql.h；重复包含会触发类型重定义错误。
#include <mysql/mysql.h>

#include <array>
#include <cstdio>
#include <utility>

namespace rgbt::gateway {
namespace {

/// 预处理语句的参数个数上限。当前查询最多 1 个参数，留出余量。
constexpr unsigned int kMaxBindParams = 4;

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
    // 本实现改为每次查询前探测，并在连接失效时显式重试一次（见 Query）。
    (void)service_prefix_;
}

MysqlConnection::~MysqlConnection() {
    Disconnect();
}

void MysqlConnection::Disconnect() {
    if (connection_ != nullptr) {
        mysql_close(connection_);
        connection_ = nullptr;
    }
}

bool MysqlConnection::EnsureConnected() {
    if (connection_ != nullptr && mysql_ping(connection_) == 0) {
        return true;
    }
    // 连接不可用（或尚未建立）：关闭后重建。
    if (connection_ != nullptr) {
        Disconnect();
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
    return EnsureConnected();
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
    if (out_rows == nullptr) {
        last_error_ = "null output";
        return false;
    }
    out_rows->clear();

    if (params.size() > kMaxBindParams) {
        last_error_ = "too many params";
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

}  // namespace rgbt::gateway
