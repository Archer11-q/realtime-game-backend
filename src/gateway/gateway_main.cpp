/// @file gateway_main.cpp
/// @brief Gateway 服务入口（TASK-005 登录切片 + TASK-006 玩家档案走数据库
///        + TASK-007 匹配接口转发）。
///
/// 提供接口（docs/05-api-and-data.md 第 2 节）：
///   POST /api/v1/login
///   GET  /api/v1/players/me
///   POST /api/v1/logout
///   POST /api/v1/matches
///   GET  /api/v1/matches/current
///   POST /api/v1/matches/current/cancel
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

#include "brpc_match_client.hpp"
#include "brpc_room_client.hpp"
#include "common/logging.hpp"
#include "common/metrics_service.hpp"
#include "common/mysql_connection.hpp"
#include "common/version.hpp"
#include "database_player_directory.hpp"
#include "gateway.pb.h"
#include "gateway_service.hpp"
#include "mysql_player_reader.hpp"
#include "player_directory.hpp"
#include "redis_session_store.hpp"
#include "room_client.hpp"
#include "stream_hub.hpp"

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
// SSE 推送参数。轮询间隔取 100 ms，与 Room 的帧长对齐：更密不会产生新帧，
// 更疏则推送比帧率还慢。心跳用于房间长时间没有帧变化时保持连接。
DEFINE_int32(stream_poll_interval_ms, 100, "SSE 推送时轮询房间状态的间隔（毫秒）");
DEFINE_int32(stream_heartbeat_interval_ms, 15000, "SSE 心跳间隔（毫秒）");
DEFINE_string(mysql_host, "127.0.0.1", "MySQL 主机");
DEFINE_int32(mysql_port, 3306, "MySQL 端口");
DEFINE_string(mysql_user, "realtime_game", "MySQL 账号");
DEFINE_string(mysql_password, "change_me", "MySQL 密码");
DEFINE_string(mysql_database, "realtime_game", "MySQL 库名");
DEFINE_int32(mysql_timeout_seconds, 3, "MySQL 连接与读写超时（秒）");
// Match Service 地址。TASK-007 起 Gateway 会调用它，这是本项目第一次发起服务间
// RPC。超时设得比客户端超时短，保证依赖故障表现为快速失败的 503 而不是请求悬挂。
DEFINE_string(match_host, "127.0.0.1", "Match Service 主机");
DEFINE_int32(match_port, 8082, "Match Service 端口");
DEFINE_int32(match_timeout_ms, 500, "调用 Match Service 的超时（毫秒）");
// Room/Battle Service 地址。TASK-008 起 Gateway 用它查询房间状态与对局结果。
// 注意：Gateway **不直读** match_results 表——该表的所有者是 Room/Battle
// （见 docs/adr/0003-scope-reduction.md），直读会绕过所有者。
DEFINE_string(room_host, "127.0.0.1", "Room/Battle Service 主机");
DEFINE_int32(room_port, 8083, "Room/Battle Service 端口");
DEFINE_int32(room_timeout_ms, 500, "调用 Room/Battle Service 的超时（毫秒）");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    // TASK-018：结构化日志的 service 字段。必须在任何日志之前设置，
    // 否则进程启动阶段（最容易出问题的那一段）的日志会缺服务名。
    rgbt::common::SetServiceName("gateway");

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
    rgbt::common::MysqlOptions mysql_options;
    mysql_options.host = FLAGS_mysql_host;
    mysql_options.port = FLAGS_mysql_port;
    mysql_options.user = FLAGS_mysql_user;
    mysql_options.password = FLAGS_mysql_password;
    mysql_options.database = FLAGS_mysql_database;
    mysql_options.connect_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.read_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.write_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);

    auto mysql_connection =
        std::make_unique<rgbt::common::MysqlConnection>(env_prefix, mysql_options);
    auto player_reader = std::make_unique<rgbt::gateway::MysqlPlayerReader>(mysql_connection.get());
    std::unique_ptr<rgbt::gateway::PlayerDirectory> players =
        std::make_unique<rgbt::gateway::DatabasePlayerDirectory>(player_reader.get());

    // --- 匹配（Match Service，TASK-007）---
    rgbt::gateway::MatchClientOptions match_options;
    match_options.host = FLAGS_match_host;
    match_options.port = FLAGS_match_port;
    match_options.timeout_ms = FLAGS_match_timeout_ms;
    auto match = std::make_unique<rgbt::gateway::BrpcMatchClient>(match_options);

    // --- 房间与对局结果（Room/Battle Service，TASK-008）---
    rgbt::gateway::RoomClientOptions room_options;
    room_options.host = FLAGS_room_host;
    room_options.port = FLAGS_room_port;
    room_options.timeout_ms = FLAGS_room_timeout_ms;
    auto room = std::make_unique<rgbt::gateway::BrpcRoomClient>(room_options);

    // --- 服务端推送（SSE，TASK-009）---
    //
    // 推送线程与订阅表在这里创建，生命周期覆盖整个进程：订阅表由 GatewayServiceImpl
    // 使用，推进线程按固定间隔轮询 Room 并把变化推给订阅者。
    rgbt::gateway::StreamHubOptions stream_options;
    stream_options.poll_interval_ms = FLAGS_stream_poll_interval_ms;
    stream_options.heartbeat_interval_ms = FLAGS_stream_heartbeat_interval_ms;
    rgbt::gateway::StreamHub stream_hub(room.get(), stream_options);

    rgbt::gateway::GatewayServiceImpl service(sessions.get(), players.get(), match.get(),
                                              room.get(), &stream_hub, FLAGS_session_ttl_seconds);

    brpc::Server server;
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.has_builtin_services = true;

    // restful 映射把 RPC 暴露为 docs/05-api-and-data.md 约定的 HTTP 路径。
    //
    // 注意取消匹配的路径：文档原先写的是 DELETE /api/v1/matches/current，与查询
    // 共用同一路径。brpc 的 restful 映射按**路径**分派，不支持按 HTTP 方法分派，
    // 因此两者不能共用路径，改为 /api/v1/matches/current/cancel。
    // 房间与对局结果的路径说明（TASK-008）：
    //   文档原写 `GET /api/v1/rooms/{room_id}`。实测确认 **brpc 的 restful 映射不支持
    //   `{name}` 路径参数**，只支持 `*` 通配符（见 brpc docs/cn/http_service.md
    //   的 Restful URL 一节）。而 `/api/v1/rooms/*` 会与 `/api/v1/rooms/join`、
    //   `/api/v1/rooms/input` 这类固定子路径产生歧义，因此 room_id 与 match_id
    //   改走**查询参数**。这不是偏好问题，是框架约束，与 TASK-007 取消匹配
    //   路径的调整性质相同；docs/05-api-and-data.md 第 2 节已同步修正。
    const std::string mappings =
        "/api/v1/login => Login,"
        "/api/v1/players/me => GetCurrentPlayer,"
        "/api/v1/logout => Logout,"
        "/api/v1/matches => EnqueueMatch,"
        "/api/v1/matches/current => GetMatchStatus,"
        "/api/v1/matches/current/cancel => CancelMatch,"
        "/api/v1/rooms/join => JoinRoom,"
        "/api/v1/rooms/input => SubmitInput,"
        "/api/v1/rooms/state => GetRoomState,"
        "/api/v1/results => GetMatchResult,"
        // SSE 订阅。它是唯一一个响应体长度不确定的接口：成功时返回
        // text/event-stream 长连接，由服务端持续写事件。
        // room_id 同样走查询参数（brpc 不支持 {name} 路径参数）。
        "/api/v1/stream => StreamEvents";

    // `/metrics` 由 MetricsService 提供（见下方 AddService）：它不属于 Gateway 的
    // 业务能力，而是与 brpc 内置的 `/status` 同级的运维接口，因此不混进
    // GatewayService，而是**单独注册一个服务并单独给一条 restful 映射**。
    const std::string metrics_mapping = "/metrics => Scrape";

    brpc::ServiceOptions service_options;
    service_options.restful_mappings = mappings;
    if (server.AddService(&service, service_options) != 0) {
        rgbt::common::LogError("service_register_failed", {{"detail", "restful 映射可能不合法"}});
        return 1;
    }

    // TASK-019：`/metrics`。单独一个服务 + 单独一条 restful 映射，理由见
    // api/proto/metrics.proto 的头部说明。
    rgbt::common::MetricsServiceImpl metrics_service;
    brpc::ServiceOptions metrics_options;
    metrics_options.restful_mappings = metrics_mapping;
    if (server.AddService(&metrics_service, metrics_options) != 0) {
        // 指标端点注册失败**不阻止启动**：指标是旁路，业务可用性优先。
        rgbt::common::LogWarn("metrics_register_failed", {});
    }

    if (server.Start(FLAGS_port, &options) != 0) {
        rgbt::common::LogError("service_start_failed", {{"port", std::to_string(FLAGS_port)},
                                                        {"hint", "端口可能已被占用"}});
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
    std::printf("  推送(SSE): GET http://127.0.0.1:%d/api/v1/stream?room_id=...\n", FLAGS_port);
    std::printf("  Redis(会话): %s:%d (%s)\n", FLAGS_redis_host.c_str(), FLAGS_redis_port,
                redis_ok ? "可用" : "当前不可用，请求将返回 503");
    std::printf("  MySQL(玩家档案): %s:%d/%s (%s)\n", FLAGS_mysql_host.c_str(), FLAGS_mysql_port,
                FLAGS_mysql_database.c_str(), mysql_ok ? "可用" : "当前不可用，登录将返回 503");
    std::printf("  Match(匹配): %s:%d (%s)\n", FLAGS_match_host.c_str(), FLAGS_match_port,
                match->IsHealthy() ? "已配置" : "地址不合法");
    std::printf("  Room(房间): %s:%d (%s)\n", FLAGS_room_host.c_str(), FLAGS_room_port,
                room->IsHealthy() ? "已配置" : "地址不合法");
    std::printf("  Key 前缀: %s:gateway:\n", env_prefix.c_str());
    std::printf("  进程号: %d\n", static_cast<int>(::getpid()));
    std::fflush(stdout);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    // 推送线程在服务注册完成之后启动，避免它先于订阅表可用而空转。
    stream_hub.Start();

    while (!g_stopping.load()) {
        ::usleep(100 * 1000);
    }

    std::printf("收到停止信号，开始优雅退出\n");
    std::fflush(stdout);
    // 顺序很重要：先停推送线程，再关闭所有 SSE 长连接，最后停服务器。
    //   * 不先停线程，它会继续往正在被关闭的连接里写。
    //   * 不关闭长连接，server.Stop() 要等这些响应结束，进程会挂住不退出——
    //     SSE 是长连接，不会自己结束。
    stream_hub.Stop();
    stream_hub.CloseAll();
    server.Stop(0);
    server.Join();
    std::printf("已优雅退出\n");
    std::fflush(stdout);
    return 0;
}
