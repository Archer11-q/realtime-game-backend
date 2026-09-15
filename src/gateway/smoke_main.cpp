/// @file smoke_main.cpp
/// @brief brpc 冒烟验证程序（TASK-004）。
///
/// 目的：证明 vcpkg 提供的 brpc 能在本工程中编译、链接、启动 HTTP 服务、
/// 响应健康检查，并在收到停止信号后优雅退出。
///
/// 本程序**不是**正式的 Gateway 实现。正式实现会在 brpc 验证通过后单独设计。
///
/// 设计说明：
///   brpc 并不存在 `brpc::HttpService` 这个类；HTTP 能力来自两个方面：
///     1. RPC 服务通过 restful_mappings 暴露为 HTTP 路径；
///     2. 内置运维服务（index/status/health/version/vars 等），由
///        Server::AddBuiltinServices() 一次性注册。
///   本程序启用内置服务，因此可直接访问：
///     /health  健康检查
///     /status  运行状态
///     /version 版本信息
///   这样既验证了 brpc 的 HTTP 链路，又不需要额外的服务类。
///
/// 行为：
///   1. 启用 brpc 内置服务。
///   2. 启动 Server 并打印监听地址。
///   3. 等待 SIGINT / SIGTERM，收到后调用 Stop(0) 优雅退出。

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>

#include "common/version.hpp"

DEFINE_int32(port, 8090, "HTTP 监听端口");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    brpc::Server server;
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    // ServerOptions::has_builtin_services 默认为 true，因此 Start() 之后即可访问
    // /health、/status、/version 等内置路径，无需（也无法）手工调用私有的
    // AddBuiltinServices()。
    options.has_builtin_services = true;

    if (server.Start(FLAGS_port, &options) != 0) {
        std::fprintf(stderr, "启动 brpc Server 失败，端口 %d 可能已被占用\n", FLAGS_port);
        return 1;
    }

    std::printf("brpc 冒烟服务已启动\n");
    std::printf("  构建版本: %s\n", rgbt::common::kVersionString);
    std::printf("  健康检查: http://127.0.0.1:%d/health\n", FLAGS_port);
    std::printf("  运行状态: http://127.0.0.1:%d/status\n", FLAGS_port);
    std::printf("  版本信息: http://127.0.0.1:%d/version\n", FLAGS_port);
    std::printf("  进程号: %d\n", static_cast<int>(::getpid()));
    std::fflush(stdout);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    while (!g_stopping.load()) {
        // 以 100 ms 为粒度轮询停止标志，避免忙等。
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
