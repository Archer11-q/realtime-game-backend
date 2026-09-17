/// @file gateway_main.cpp
/// @brief Gateway 服务入口（TASK-005 登录切片）。
///
/// 提供接口（docs/05-api-and-data.md 第 2 节）：
///   POST /api/v1/login
///   GET  /api/v1/players/me
///   POST /api/v1/logout
///   GET  /health
///
/// 启动失败与依赖不可用采用不同策略：
///   * 端口被占用等启动错误 -> 直接退出并返回非 0。
///   * Redis 暂时不可用 -> 服务照常启动，请求返回 503 UNAVAILABLE，
///     Redis 恢复后无需重启服务。这样避免依赖抖动导致服务反复重启。

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
#include "gateway.pb.h"
#include "gateway_service.hpp"
#include "player_directory.hpp"
#include "redis_session_store.hpp"

DEFINE_int32(port, 8080, "HTTP 监听端口，默认与 .env.example 的 GATEWAY_HTTP_PORT 一致");
DEFINE_string(redis_host, "127.0.0.1", "Redis 主机");
DEFINE_int32(redis_port, 6379, "Redis 端口");
DEFINE_int32(redis_timeout_ms, 500, "Redis 连接与命令超时（毫秒）");
DEFINE_int32(session_ttl_seconds, 604800, "会话有效期（秒），默认 7 天");
DEFINE_string(env_prefix, "dev", "Redis Key 的环境前缀，取值示例 dev / test / prod");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

/// 健康检查服务。
///
/// 依据 docs/04-quality-and-observability.md，健康检查需要区分存活与就绪：
///   /health        存活：进程能响应即返回 200。
///   /health/ready  就绪：依赖（Redis）可用才返回 200，否则返回 503。
class HealthServiceImpl : public rgbt::gateway::v1::GatewayService {
public:
    explicit HealthServiceImpl(rgbt::gateway::RedisSessionStore* sessions) : sessions_(sessions) {}

    void Login(::google::protobuf::RpcController*, const rgbt::gateway::v1::LoginRequest*,
               rgbt::gateway::v1::LoginResponse*, ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
    }
    void GetCurrentPlayer(::google::protobuf::RpcController*,
                          const rgbt::gateway::v1::GetCurrentPlayerRequest*,
                          rgbt::gateway::v1::GetCurrentPlayerResponse*,
                          ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
    }
    void Logout(::google::protobuf::RpcController*, const rgbt::gateway::v1::LogoutRequest*,
                rgbt::gateway::v1::LogoutResponse*, ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
    }

private:
    rgbt::gateway::RedisSessionStore* sessions_;
};

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    const std::string env_prefix = FLAGS_env_prefix;

    rgbt::gateway::RedisOptions redis_options;
    redis_options.host = FLAGS_redis_host;
    redis_options.port = FLAGS_redis_port;
    redis_options.connect_timeout_ms = FLAGS_redis_timeout_ms;
    redis_options.command_timeout_ms = FLAGS_redis_timeout_ms;

    auto sessions = std::make_unique<rgbt::gateway::RedisSessionStore>(env_prefix, redis_options,
                                                                       FLAGS_session_ttl_seconds);
    const rgbt::gateway::PlayerDirectory players =
        rgbt::gateway::PlayerDirectory::WithBuiltinTestAccounts();

    rgbt::gateway::GatewayServiceImpl service(sessions.get(), &players, FLAGS_session_ttl_seconds);

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

    const bool redis_ok = sessions->IsHealthy();
    std::printf("Gateway 已启动\n");
    std::printf("  版本: %s\n", rgbt::common::kVersionString);
    std::printf("  监听: http://127.0.0.1:%d\n", FLAGS_port);
    std::printf("  登录: POST http://127.0.0.1:%d/api/v1/login\n", FLAGS_port);
    std::printf("  当前玩家: GET http://127.0.0.1:%d/api/v1/players/me\n", FLAGS_port);
    std::printf("  Redis: %s:%d (%s)\n", FLAGS_redis_host.c_str(), FLAGS_redis_port,
                redis_ok ? "可用" : "当前不可用，请求将返回 503");
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
