/// @file room_main.cpp
/// @brief Room/Battle Service 进程入口。
///
/// 服务边界（docs/01-architecture.md 第 3 节）：
///   Room/Battle 拥有房间生命周期、权威状态、快照与 match_results 表。
///   它不处理登录与连接管理，也不做全局匹配策略。
///
/// 本进程通过 brpc 对外提供服务，没有 restful 映射：它不面向浏览器，
/// 请求来自 Match（创建房间）与 Gateway（加入、输入、查询）。
/// brpc 的内置运维服务（`/health`、`/status`）由 `has_builtin_services` 提供，
/// 验收脚本用 `/health` 探活。
///
/// 定时推进：
///   房间的帧推进不依赖外部请求，必须由本进程自己驱动。因此这里起一个轻量线程，
///   每 50 ms 调用一次 RoomManager::Tick。50 ms 的调用间隔小于 100 ms 的帧长，
///   保证帧号推进的时间误差不会被调用间隔放大。
///   **不做** worker 池或定时器框架：一个线程、一个间隔常量，就是当前需求的全貌。

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "common/logging.hpp"
#include "common/metrics_service.hpp"
#include "common/mysql_connection.hpp"
#include "common/version.hpp"
#include "mysql_match_result_writer.hpp"
#include "mysql_room_snapshot_reader.hpp"
#include "mysql_room_snapshot_writer.hpp"
#include "room_manager.hpp"
#include "room_service.hpp"
#include "room_types.hpp"

// 默认端口与 .env.example 的 ROOM_HTTP_PORT 保持一致（8083），与 Gateway 的
// 8080/8081、Match 的 8082 错开。端口冲突属环境问题：由验收脚本在启动前预检并
// 从候选列表中挑选空闲端口，再用 -port 显式传入，不改这里的约定值。
DEFINE_int32(port, 8083, "brpc 监听端口");
DEFINE_string(env_prefix, "dev", "Key 前缀的环境标识，取值示例 dev / test / prod");
DEFINE_string(mysql_host, "127.0.0.1", "MySQL 主机");
DEFINE_int32(mysql_port, 3306, "MySQL 端口");
DEFINE_string(mysql_user, "realtime_game", "MySQL 账号");
DEFINE_string(mysql_password, "change_me", "MySQL 密码");
DEFINE_string(mysql_database, "realtime_game", "MySQL 库名");
DEFINE_int32(mysql_timeout_seconds, 3, "MySQL 连接与读写超时（秒）");
DEFINE_int32(tick_interval_ms, 50, "房间推进线程的调用间隔（毫秒）");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时（秒），-1 表示不超时");
DEFINE_int32(drain_timeout_ms, 30000,
             "TASK-026：收到停止信号后等待活跃对局结束的上限（毫秒）；"
             "到点把仍未结束的对局标为 ABORTED（不写对局结果）");

namespace {

std::atomic<bool> g_stopping{false};

/// TASK-026：推进线程的退出标志，**与 g_stopping 分开**。
///
/// 为什么不能共用：排空期间对局必须继续推进（等待就是对局在往前走），
/// 而 g_stopping 在信号处理器里立刻置位。共用的话 ticker 会在收到信号的那一瞬间
/// 退出，于是"等待活跃对局结束"等到的是一个冻结的对局——必然走到超时截断。
/// 首轮实测就是这个结果：15 秒上限全部用满，对局被误标 ABORTED。
std::atomic<bool> g_ticker_stop{false};

void HandleSignal(int /*sig*/) {
    g_stopping.store(true);
}

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
    rgbt::common::SetServiceName("room");

    const std::string env_prefix = FLAGS_env_prefix;

    // --- 对局结果存储（MySQL）---
    rgbt::common::MysqlOptions mysql_options;
    mysql_options.host = FLAGS_mysql_host;
    mysql_options.port = FLAGS_mysql_port;
    mysql_options.user = FLAGS_mysql_user;
    mysql_options.password = FLAGS_mysql_password;
    mysql_options.database = FLAGS_mysql_database;
    mysql_options.connect_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.read_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);
    mysql_options.write_timeout_seconds = static_cast<std::uint32_t>(FLAGS_mysql_timeout_seconds);

    // 构造时不因连接失败而失败：MySQL 抖动不应导致房间进程反复重启。
    // 真正的不可用会在结果写入时以「房间停在 FINISHING 并重试」的形式体现。
    auto mysql_connection =
        std::make_unique<rgbt::common::MysqlConnection>(env_prefix, mysql_options);
    auto result_writer =
        std::make_unique<rgbt::room::MysqlMatchResultWriter>(mysql_connection.get());
    // 快照写入器（TASK-013）。它与 result_writer 共用一条 MySQL 连接，
    // 但**失败策略相反**：快照写失败只记日志、不重试，下一个周期覆盖。
    auto snapshot_writer =
        std::make_unique<rgbt::room::MysqlRoomSnapshotWriter>(mysql_connection.get());

    // 快照读取器（TASK-014）。与写入器共用同一条连接，但语义不同：
    // 读取是启动路径上的前置条件，失败必须让调用方明确知道。
    auto snapshot_reader =
        std::make_unique<rgbt::room::MysqlRoomSnapshotReader>(mysql_connection.get());

    rgbt::room::RoomManager manager(result_writer.get(), snapshot_writer.get(),
                                    snapshot_reader.get());
    rgbt::room::RoomServiceImpl service(&manager);

    // 恢复必须在**开始接受请求之前**完成：否则一个刚连上来的查询会看到
    // "房间不存在"，而几十毫秒后同样的查询又能成功，客户端无从判断哪个是真的。
    const rgbt::room::RestoreReport restore = manager.Restore(NowMs());
    std::printf("启动恢复：扫描 %zu 个未结束房间，恢复 %zu 个，标记 ABORTED %zu 个%s\n",
                restore.scanned, restore.restored, restore.rejected,
                restore.load_failed ? "（快照读取失败，本次未恢复任何房间）" : "");
    std::fflush(stdout);

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

    if (server.Start(FLAGS_port, &options) != 0) {
        rgbt::common::LogError("service_start_failed", {{"port", std::to_string(FLAGS_port)},
                                                        {"hint", "端口可能已被占用"}});
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    // 忽略 SIGPIPE：对端断开时由 brpc 处理错误，不应终止进程。
    std::signal(SIGPIPE, SIG_IGN);
    // --- 房间推进线程 ---
    const std::int64_t tick_interval_ms = FLAGS_tick_interval_ms > 0 ? FLAGS_tick_interval_ms : 50;
    std::thread ticker([&manager, tick_interval_ms]() {
        // TASK-026：用 g_ticker_stop 而不是 g_stopping，这样排空期间对局照常推进。
        while (!g_ticker_stop.load()) {
            manager.Tick(NowMs());
            ::usleep(static_cast<useconds_t>(tick_interval_ms) * 1000);
        }
    });

    // 依赖状态在启动时探测一次并打印，便于排障；不因不可用而拒绝启动。
    const bool mysql_ok = result_writer->IsHealthy();

    std::printf("Room/Battle 服务已启动\n");
    std::printf("  版本: %s\n", rgbt::common::kVersionString);
    std::printf("  监听: %s:%d\n", "0.0.0.0", FLAGS_port);
    std::printf("  MySQL(对局结果): %s:%d/%s (%s)\n", FLAGS_mysql_host.c_str(), FLAGS_mysql_port,
                FLAGS_mysql_database.c_str(),
                mysql_ok ? "可用" : "当前不可用，对局结果将停留在 FINISHING 并持续重试");
    std::printf("  推进间隔: %lld ms，帧长: %lld ms\n", static_cast<long long>(tick_interval_ms),
                static_cast<long long>(rgbt::room::kFrameIntervalMs));
    std::printf("  进程号: %d\n", static_cast<int>(::getpid()));
    std::fflush(stdout);

    while (!g_stopping.load()) {
        ::usleep(100 * 1000);
    }

    std::printf("收到停止信号，开始优雅退出\n");
    std::fflush(stdout);

    // --- TASK-026：排空 ---
    //
    // 顺序是「先停止接收新房间 -> 等活跃对局打完 -> 超时才丢弃」：
    //   1. BeginShutdown() 之后 CreateRoom 一律返回 shutting_down，Match 会把玩家
    //      退回队列，不会有新对局在这段时间里开始；
    //   2. **推进线程必须继续跑**：对局的推进与结果落库都在 Tick 里完成，
    //      先 join 它就等于立刻冻结所有对局；
    //   3. 等待条件同时包含 FINISHING 的房间：只等"打完"会在对局刚结束时立刻退出，
    //      结果还停在内存里（这正是 TASK-008/014 那条已知限制的形状）。
    //      落库失败（MySQL 不可用）时会被超时兜住，而这些房间的 `finishing` 快照
    //      仍在库里，下次启动由 Restore 重新纳入落库重试——所以超时不会丢结果。
    const std::int64_t drain_started_ms = NowMs();
    manager.BeginShutdown();
    const std::int64_t drain_timeout_ms = FLAGS_drain_timeout_ms > 0 ? FLAGS_drain_timeout_ms : 0;
    const std::int64_t drain_deadline_ms = drain_started_ms + drain_timeout_ms;
    rgbt::common::LogInfo("drain_started",
                          {{"unfinished", std::to_string(manager.UnfinishedGameCount())},
                           {"pending_results", std::to_string(manager.PendingResultCount())},
                           {"timeout_ms", std::to_string(drain_timeout_ms)}});

    while ((manager.UnfinishedGameCount() > 0 || manager.PendingResultCount() > 0) &&
           NowMs() < drain_deadline_ms) {
        ::usleep(50 * 1000);
    }

    const std::size_t unfinished = manager.UnfinishedGameCount();
    if (unfinished > 0) {
        const std::size_t aborted = manager.AbortUnfinishedGames(NowMs());
        rgbt::common::LogWarn("drain_timeout_abort",
                              {{"aborted", std::to_string(aborted)},
                               {"waited_ms", std::to_string(NowMs() - drain_started_ms)}});
        std::printf("排空超时：把 %zu 个未结束的对局标为 ABORTED（不写对局结果）\n", aborted);
    } else {
        rgbt::common::LogInfo("drain_finished",
                              {{"waited_ms", std::to_string(NowMs() - drain_started_ms)},
                               {"pending_results", std::to_string(manager.PendingResultCount())}});
        std::printf("排空完成：等待 %lld ms，活跃对局已全部结束\n",
                    static_cast<long long>(NowMs() - drain_started_ms));
    }
    std::fflush(stdout);

    // 排空判据满足之后**再推一次 Tick**：对局进入 FINISHED 之后，终态快照是在
    // 下一次 Tick 才写的（ShouldSnapshot 按"阶段变了"判断）。少了这一次，进程会带着
    // 一条 state=playing 的旧快照退出——库里那一局看起来还在打（首轮实测就是这个：
    // match_results 已有真实胜者，rooms.state 仍是 playing）。
    manager.Tick(NowMs());

    // 排空做完了，才让推进线程退出。
    g_ticker_stop.store(true);
    // 先停推进线程再停 brpc：反过来的话，正在处理的请求可能访问到已停止的管理器。
    if (ticker.joinable()) {
        ticker.join();
    }
    server.Stop(0);
    server.Join();

    std::printf("已优雅退出\n");
    std::fflush(stdout);
    return 0;
}
