/// @file match_main.cpp
/// @brief Match Service 进程入口。
///
/// 服务边界（docs/01-architecture.md 第 3 节）：
///   Match 维护匹配队列、处理进入/取消/超时、选择玩家并请求创建房间。
///   它不保存战斗状态，也不直接向客户端推送消息——客户端通过 Gateway 轮询结果
///   （TASK-007 决策 4 选 A），WebSocket 推送属 TASK-009。
///
/// 本进程只通过 brpc 对外提供服务，没有 restful 映射：它不面向浏览器。
/// brpc 的内置运维服务（`/health`、`/status`）由 `has_builtin_services` 提供，
/// 验收脚本用 `/health` 探活。
///
/// Phase 1 的已知限制：
///   * 队列在进程内存中，进程重启即丢失排队状态（快照与恢复属 Phase 2）。
///   * 队列由单把互斥锁保护，只在单实例下正确（多实例属 Phase 4）。

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>

#include "common/version.hpp"
#include "match_queue.hpp"
#include "match_service.hpp"
#include "room_allocator.hpp"

DEFINE_int32(match_port, 8082, "Match Service 监听端口");
DEFINE_int32(match_timeout_seconds, 30, "排队超时（秒），超过后被惰性淘汰");
DEFINE_int32(match_result_ttl_seconds, 120, "匹配结果保留时长（秒），供客户端轮询领取");
DEFINE_int32(match_max_queue_size, 1000, "匹配队列长度上限，超过后拒绝新请求");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    // 房间分配：Phase 1 用占位实现，只保证同一局的双方拿到同一个 room_id，
    // 不产生任何房间状态。TASK-008 落地 Room Service 后替换这里即可。
    rgbt::match::DerivedRoomAllocator room_allocator;
    rgbt::match::MatchQueue queue(&room_allocator, {}, FLAGS_match_timeout_seconds * 1000LL,
                                  FLAGS_match_result_ttl_seconds * 1000LL,
                                  static_cast<std::size_t>(FLAGS_match_max_queue_size));

    rgbt::match::MatchServiceImpl service(&queue);

    brpc::Server server;
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.has_builtin_services = true;

    if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        std::fprintf(stderr, "注册 MatchService 失败\n");
        return 1;
    }

    if (server.Start(FLAGS_match_port, &options) != 0) {
        std::fprintf(stderr, "启动 Match 失败，端口 %d 可能已被占用\n", FLAGS_match_port);
        return 1;
    }

    std::printf("Match 已启动\n");
    std::printf("  版本: %s\n", rgbt::common::kVersionString);
    std::printf("  监听: brpc 端口 %d\n", FLAGS_match_port);
    std::printf("  健康检查: http://127.0.0.1:%d/health\n", FLAGS_match_port);
    std::printf("  队列: 进程内存，两人一局，超时 %d 秒，结果保留 %d 秒，上限 %d\n",
                FLAGS_match_timeout_seconds, FLAGS_match_result_ttl_seconds,
                FLAGS_match_max_queue_size);
    std::printf("  房间分配: 占位实现（room_id 由 match_id 派生，无房间状态）\n");
    std::printf("  进程号: %d\n", static_cast<int>(::getpid()));
    std::fflush(stdout);

    ::signal(SIGINT, HandleSignal);
    ::signal(SIGTERM, HandleSignal);

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
