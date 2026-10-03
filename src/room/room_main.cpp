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

namespace {

std::atomic<bool> g_stopping{false};

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
        while (!g_stopping.load()) {
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
