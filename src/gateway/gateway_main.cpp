/// @file gateway_main.cpp
/// @brief Gateway 服务入口（TASK-005 登录切片 + TASK-006 玩家档案走数据库）。
///
/// 提供接口（docs/05-api-and-data.md 第 2 节）：
///   POST /api/v1/login
///   GET  /api/v1/players/me
///   POST /api/v1/logout
///   GET  /health
///
/// 启动失败与依赖不可用采用不同策略：
///   * 端口被占用等启动错误 -> 直接退出并返回非 0。
///   * Redis 或 MySQL 暂时不可用 -> 服务照常启动，请求返回 503 UNAVAILABLE，
///     依赖恢复后无需重启服务。这样避免依赖抖动导致服务反复重启。
///
/// 玩家档案来源：MySQL 的 players 表（Phase 1 期间 Gateway 只读，见 ADR-0002）；
/// 账号密码保留在代码中作为测试身份，以 SHA-256 摘要比较（见 D-002）。

#include <brpc/closure_guard.h>
#include <brpc/server.h>
#include <gflags/gflags.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>

#include "common/version.hpp"
#include "database_player_directory.hpp"
#include "gateway.pb.h"
#include "gateway_service.hpp"
#include "mysql_connection.hpp"
#include "mysql_player_reader.hpp"
#include "player_directory.hpp"
#include "redis_session_store.hpp"

// 默认端口与 .env.example 的 GATEWAY_HTTP_PORT 保持一致（8080）。
// 本地 8080 可能被其它开发服务占用，端口冲突时网关会启动失败，而请求会被打到
// 占用端口的那个服务上、表现为难以定位的 404。这是**环境问题，不是默认值问题**：
// 因此不把非约定端口硬编码进程序，而由验收脚本在启动前预检并从候选列表中挑选
// 空闲端口，再用 -port 显式传入（见 scripts/verify-login.sh）。
DEFINE_int32(port, 8080, "HTTP 监听端口");
DEFINE_string(redis_host, "127.0.0.1", "Redis 主机");
DEFINE_int32(redis_port, 6379, "Redis 端口");
DEFINE_int32(redis_timeout_ms, 500, "Redis 连接与命令超时（毫秒）");
DEFINE_int32(session_ttl_seconds, 604800, "会话有效期（秒），默认 7 天");
DEFINE_string(env_prefix, "dev", "Key 前缀的环境标识，取值示例 dev / test / prod");
DEFINE_string(mysql_host, "127.0.0.1", "MySQL 主机");
DEFINE_int32(mysql_port, 3306, "MySQL 端口");
DEFINE_string(mysql_user, "realtime_game", "MySQL 账号");
DEFINE_string(mysql_password, "change_me", "MySQL 密码");
DEFINE_string(mysql_database, "realtime_game", "MySQL 库名");
DEFINE_int32(mysql_timeout_seconds, 3, "MySQL 连接与读写超时（秒）");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    const std::string env_prefix = FLAGS_env_prefix;

    // --- 会话存储（Redis）---
    rgbt::gateway::RedisOptions redis_options;
    redis_options.host = FLAGS_redis_host;
    redis_options.port = FLAGS_redis_port;
    redis_options.connect_timeout_ms = FLAGS_redis_timeout_ms;
    redis_options.command_timeout_ms = FLAGS_redis_timeout_ms;

    auto sessions = std::make_unique<rgbt::gateway::RedisSessionStore>(env_prefix, redis_options,
                                                                       FLAGS_session_ttl_seconds);

    // --- 玩家档案（MySQL）---
    rgbt::gateway::MysqlOptions mysql_options;
    mysql_options.host = FLAGS_mysql_host;
    mysql_options.port = FLAGS_mysql_port;
    mysql_options.user = FLAGS_mysql_user;
    mysql_options.password = FLAGS_mysql_password;
    mysql_options.database = FLAGS_mysql_database;
    mysql_options.connect_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.read_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.write_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);

    auto mysql_connection =
        std::make_unique<rgbt::gateway::MysqlConnection>(env_prefix, mysql_options);
    auto player_reader = std::make_unique<rgbt::gateway::MysqlPlayerReader>(mysql_connection.get());
    std::unique_ptr<rgbt::gateway::PlayerDirectory> players =
        std::make_unique<rgbt::gateway::DatabasePlayerDirectory>(player_reader.get());

    rgbt::gateway::GatewayServiceImpl service(sessions.get(), players.get(),
                                              FLAGS_session_ttl_seconds);

    brpc::Server server;
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.has_builtin_services = true;

    // restful 映射把 RPC 暴露为 docs/05-api-and-data.md 约定的 HTTP 路径。
    const std::string mappings =
        "/api/v1/login => Login,"
        "/api/v1/players/me => GetCurrentPlayer,"
        "/api/v1/logout => Logout";

    brpc::ServiceOptions service_options;
    service_options.restful_mappings = mappings;
    if (server.AddService(&service, service_options) != 0) {
        std::fprintf(stderr, "注册 GatewayService 失败：restful 映射可能不合法\n");
        return 1;
    }

    if (server.Start(FLAGS_port, &options) != 0) {
        std::fprintf(stderr, "启动 Gateway 失败，端口 %d 可能已被占用\n", FLAGS_port);
        return 1;
    }

    // 依赖状态在启动时探测一次并打印，便于排障；不因不可用而拒绝启动。
    const bool redis_ok = sessions->IsHealthy();
    const bool mysql_ok = player_reader->IsHealthy();

    std::printf("Gateway 已启动\n");
    std::printf("  版本: %s\n", rgbt::common::kVersionString);
    std::printf("  监听: http://127.0.0.1:%d\n", FLAGS_port);
    std::printf("  登录: POST http://127.0.0.1:%d/api/v1/login\n", FLAGS_port);
    std::printf("  当前玩家: GET http://127.0.0.1:%d/api/v1/players/me\n", FLAGS_port);
    std::printf("  Redis(会话): %s:%d (%s)\n", FLAGS_redis_host.c_str(), FLAGS_redis_port,
                redis_ok ? "可用" : "当前不可用，请求将返回 503");
    std::printf("  MySQL(玩家档案): %s:%d/%s (%s)\n", FLAGS_mysql_host.c_str(), FLAGS_mysql_port,
                FLAGS_mysql_database.c_str(), mysql_ok ? "可用" : "当前不可用，登录将返回 503");
    std::printf("  Key 前缀: %s:gateway:\n", env_prefix.c_str());
    std::printf("  进程号: %d\n", static_cast<int>(::getpid()));
    std::fflush(stdout);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    while (!g_stopping.load()) {
        ::usleep(100 * 1000);
    }

    std::printf("收到停止信号，开始优雅退出\n");
    std::fflush(stdout);
    server.Stop(0);
    server.Join();
    std::printf("已优雅退出\n");
    std::fflush(stdout);
    return 0;
}
