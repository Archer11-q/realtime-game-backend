/// @file match_main.cpp
/// @brief Match Service 进程入口。
///
/// 服务边界（docs/01-architecture.md 第 3 节）：
///   Match 维护匹配队列、处理进入/取消/超时、选择玩家并请求创建房间。
///   它不保存战斗状态，也不直接向客户端推送消息——客户端通过 Gateway 轮询结果
///   （TASK-007 决策 4 选 A），服务端推送（SSE）属 TASK-009。
///
/// 本进程只通过 brpc 对外提供服务，没有 restful 映射：它不面向浏览器。
/// brpc 的内置运维服务（`/health`、`/status`）由 `has_builtin_services` 提供，
/// 验收脚本用 `/health` 探活。
///
/// Phase 1 的已知限制：
///   * 队列在进程内存中，进程重启即丢失排队状态（快照与恢复属 Phase 2）。
///   * 队列由单把互斥锁保护，只在单实例下正确。本项目**不做多实例**
///     （见 docs/adr/0003-scope-reduction.md），因此该限制不会在项目范围内触发。

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "brpc_room_allocator.hpp"
#include "common/logging.hpp"
#include "common/metrics_service.hpp"
#include "common/version.hpp"
#include "match_queue.hpp"
#include "match_queue_store.hpp"
#include "match_service.hpp"
#include "redis_match_queue_store.hpp"
#include "room_allocator.hpp"

DEFINE_int32(match_port, 8082, "Match Service 监听端口");
DEFINE_int32(match_timeout_seconds, 30, "排队超时（秒），超过后被惰性淘汰");
DEFINE_int32(match_result_ttl_seconds, 120, "匹配结果保留时长（秒），供客户端轮询领取");
DEFINE_int32(match_max_queue_size, 1000, "匹配队列长度上限，超过后拒绝新请求");
// Room/Battle Service 地址。TASK-008 起匹配成功后由 Match 调用它创建房间。
DEFINE_string(room_host, "127.0.0.1", "Room/Battle Service 主机");
DEFINE_int32(room_port, 8083, "Room/Battle Service 端口");
DEFINE_int32(room_timeout_ms, 500, "调用 Room/Battle Service 的超时（毫秒）");
DEFINE_string(redis_host, "127.0.0.1", "Redis 主机（队列快照）");
DEFINE_int32(redis_port, 6379, "Redis 端口（队列快照）");
DEFINE_int32(redis_timeout_ms, 500, "Redis 连接与命令超时（毫秒）");
DEFINE_string(env_prefix, "dev", "Key 前缀的环境标识，取值示例 dev / test / prod");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");

namespace {

std::atomic<bool> g_stopping{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

/// 当前墙钟毫秒。恢复时需要它来重算"停机期间流逝了多久"。
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    // TASK-018：结构化日志的 service 字段。必须在任何日志之前设置，
    // 否则进程启动阶段（最容易出问题的那一段）的日志会缺服务名。
    rgbt::common::SetServiceName("match");

    // 房间分配：TASK-008 起调用真实的 Room/Battle Service。
    //
    // 分配发生在**匹配队列的锁之外**（见 match_queue.hpp 的第三条设计决定）：
    // 一次 brpc 超时不应该把整个队列卡住。
    //
    // 分配失败时 MatchQueue 会把玩家退回队首，因此 Room 不可用表现为
    // 「暂时匹配不上」，而不是「玩家被莫名其妙判为超时」。
    rgbt::match::RoomAllocatorOptions room_options;
    room_options.host = FLAGS_room_host;
    room_options.port = FLAGS_room_port;
    room_options.timeout_ms = FLAGS_room_timeout_ms;
    rgbt::match::BrpcRoomAllocator room_allocator(room_options);

    // 队列快照存储（TASK-015）。**队列的权威状态仍在内存里**，Redis 只是旁路
    // 快照：Redis 不可用时匹配照常工作，只是失去"重启可恢复"（降级为纯内存）。
    // 这条失败策略由项目所有者确认，理由见 docs/TASKS.md 的 TASK-015。
    rgbt::match::MatchRedisOptions redis_options;
    redis_options.host = FLAGS_redis_host;
    redis_options.port = FLAGS_redis_port;
    redis_options.connect_timeout_ms = FLAGS_redis_timeout_ms;
    redis_options.command_timeout_ms = FLAGS_redis_timeout_ms;
    rgbt::match::RedisMatchQueueStore queue_store(FLAGS_env_prefix, redis_options);

    rgbt::match::MatchQueue queue(&room_allocator, {}, FLAGS_match_timeout_seconds * 1000LL,
                                  FLAGS_match_result_ttl_seconds * 1000LL,
                                  static_cast<std::size_t>(FLAGS_match_max_queue_size),
                                  &queue_store);

    // 恢复必须在**开始接受请求之前**完成：否则一个刚入队的玩家要与一个
    // 尚未恢复的队列竞争，队列里可能已经有他的旧记录。
    const rgbt::match::MatchRestoreReport restore = queue.Restore(NowMs());
    std::printf(
        "队列恢复：扫描 %zu 行，重建排队 %zu 人、已配对 %zu 人，"
        "跳过 %zu 行、已超时未恢复 %zu 人、结果已过期未恢复 %zu 人%s\n",
        restore.scanned, restore.queued, restore.matched, restore.dropped, restore.timed_out,
        restore.expired_results,
        restore.load_failed ? "（快照读取失败，本次未恢复任何排队状态）" : "");
    std::fflush(stdout);

    rgbt::match::MatchServiceImpl service(&queue);

    brpc::Server server;
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.has_builtin_services = true;

    if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        rgbt::common::LogError("service_register_failed", {});
        return 1;
    }

    // TASK-019：`/metrics`。同一个端口上再注册一个服务——brpc 允许这样做，
    // 因此不需要为指标单开端口。`SERVER_DOESNT_OWN_SERVICE` 表示生命周期由本函数
    // 的栈对象管理，服务器不负责释放。
    rgbt::common::MetricsServiceImpl metrics_service;
    // 必须给它一条 restful 映射：brpc 收到 `GET /metrics` 时是按**路径**在
    // restful 映射表里找方法的（不是按 service/method 名找）。没有映射时的
    // 表现是 404 `Fail to find method on '/metrics'`（实测踩到）。
    brpc::ServiceOptions metrics_options;
    metrics_options.restful_mappings = "/metrics => Scrape";
    if (server.AddService(&metrics_service, metrics_options) != 0) {
        // 指标端点注册失败**不阻止启动**：指标是旁路，业务可用性优先。
        rgbt::common::LogWarn("metrics_register_failed", {});
    }

    if (server.Start(FLAGS_match_port, &options) != 0) {
        rgbt::common::LogError("service_start_failed", {{"port", std::to_string(FLAGS_match_port)},
                                                        {"hint", "端口可能已被占用"}});
        return 1;
    }

    std::printf("Match 已启动\n");
    std::printf("  版本: %s\n", rgbt::common::kVersionString);
    std::printf("  监听: brpc 端口 %d\n", FLAGS_match_port);
    std::printf("  健康检查: http://127.0.0.1:%d/health\n", FLAGS_match_port);
    std::printf("  队列: 进程内存，两人一局，超时 %d 秒，结果保留 %d 秒，上限 %d\n",
                FLAGS_match_timeout_seconds, FLAGS_match_result_ttl_seconds,
                FLAGS_match_max_queue_size);
    std::printf("  房间分配: Room/Battle Service %s:%d (%s)，超时 %d ms\n", FLAGS_room_host.c_str(),
                FLAGS_room_port, room_allocator.IsHealthy() ? "已配置" : "地址不合法",
                FLAGS_room_timeout_ms);
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
