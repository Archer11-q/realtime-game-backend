/// @file room_types.hpp
/// @brief Room/Battle Service 的内部数据类型与对局常量。
///
/// 为什么单独一个文件：BattleRoom、RoomManager、结果写入器三者都要用这些类型，
/// 但它们都不需要 brpc 生成代码。把类型独立出来后，房间逻辑可以在**不链接
/// protobuf 生成代码**的情况下被单元测试，这是本项目一贯的做法
/// （对照 src/match/match_queue.hpp 的注释）。

#ifndef RGBT_ROOM_ROOM_TYPES_HPP
#define RGBT_ROOM_ROOM_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rgbt::room {

// ---------------------------------------------------------------------------
// 对局规则常量（TASK-008 决策 A）
// ---------------------------------------------------------------------------
//
// 这些数值是**服务端权威**的：客户端不参与判定，也不下发这些数值。
// 集中定义在此处，便于后续调整时只改一处，并有测试锁定行为。

/// 一局的人数。与 Match 的 FIFO 两人配对一致（docs/02-roadmap.md Phase 1）。
inline constexpr std::size_t kPlayersPerRoom = 2;

/// HP 初值。
inline constexpr std::int32_t kInitialHp = 100;

/// 每次攻击造成的伤害。
inline constexpr std::int32_t kAttackDamage = 10;

/// 一帧的时长（毫秒）。100 ms 即 10 Hz。
inline constexpr std::int64_t kFrameIntervalMs = 100;

/// 一局的最大帧数。600 帧 = 60 秒，到时按 HP 判定胜负。
inline constexpr std::int64_t kMaxFrames = 600;

/// 单次 Tick 最多推进的帧数。
///
/// 为什么需要上限：进程被挂起或调度延迟后，`now_ms` 可能一次跳过很多帧。
/// 若一次性补完所有帧，会造成 CPU 尖峰，且这段时间内的输入其实已经失去意义。
/// 超过上限的部分直接丢弃，把它当作「这段时间没有对局推进」处理。
inline constexpr std::int64_t kMaxCatchUpFrames = 10;

/// 结果写入失败后的重试间隔（毫秒）。以帧为单位表达，便于与 Tick 对齐。
inline constexpr std::int64_t kResultRetryIntervalMs = 1000;

/// 房间等待玩家加入的超时（毫秒）。超时后房间被标记为 ABORTED。
///
/// 为什么需要：匹配成功但客户端始终不加入时，房间不能被永久占着，
/// 否则就是一次资源泄漏。
inline constexpr std::int64_t kWaitingTimeoutMs = 30 * 1000;

/// 已结束房间的保留时长（毫秒）。客户端靠轮询领取结果，因此结束后必须保留一段
/// 时间；超过后房间被回收，之后查询结果走 MySQL。
inline constexpr std::int64_t kFinishedRetentionMs = 5 * 60 * 1000;

/// 快照环形缓冲保留的帧数。TASK-017 起它有了明确用途与上界说明。
///
/// **用途**：Gateway 建立 SSE 订阅时带 `Last-Event-ID`，Room 用它把这期间的状态
/// 补发出去（`GetRoomSnapshotsSince`）。128 帧 ≈ 12.8 秒。
///
/// **为什么不需要更大**（这是 TASK-017 明确记录的判断，不是省略）：
/// 宽限期内对局**暂停推进**（TASK-016），因此"断线多久就要补多少帧"这个前提
/// 不成立——断线 30 秒期间帧号根本不前进，缺口天然极小。这个窗口真正覆盖的是
/// 另外两类情况：客户端在断开**之前**就已经落后（网络慢、渲染卡顿），
/// 以及同一房间的多条订阅互相追赶。两者都只在秒级落后时才有意义。
///
/// **不再放大它的条件**：只有实测出现"重连后需要补发的缺口超过 128 帧"时才调大，
/// 且必须同时给出实测数字。TASK-017 的实测结论记录在 `docs/devlog.md` 的
/// 「推送连续性与恢复时间汇总」一节。
inline constexpr std::size_t kMaxSnapshotHistory = 128;

/// TASK-016：断线后的宽限期（毫秒）。项目所有者确认取 30 秒。
///
/// 取值的理由：与"等待玩家加入"的超时（kWaitingTimeoutMs）同量级，
/// 足以覆盖刷新页面、切网络、短暂断连；同时对手等待时间仍然可接受。
/// 期内对局**暂停推进**（帧不前进），回来接上继续打；
/// 到期未归则判断线方负（FinishReason::kDisconnect）。
inline constexpr std::int64_t kReconnectGraceMs = 30 * 1000;

/// 房间号与标识的长度上限。超长输入直接判为输入错误。
inline constexpr std::size_t kMaxIdLength = 64;

// ---------------------------------------------------------------------------
// 状态与枚举
// ---------------------------------------------------------------------------

/// 房间生命周期状态。与 api/proto/room.proto 的 RoomState 一一对应。
enum class RoomPhase {
    /// 已创建，尚未有人加入。
    kCreated,
    /// 至少一人加入，等待双方到齐。
    kWaiting,
    /// 双方到齐，tick 正在推进。
    kPlaying,
    /// 已分出胜负，结果尚未写入 MySQL。
    kFinishing,
    /// 结果已写入 MySQL。
    kFinished,
    /// 异常终止，**不写**对局结果。
    kAborted,
};

/// 对局结束原因。与 proto 的 FinishReason 一一对应。
enum class FinishReason {
    kNone,
    kHpZero,
    kTimeout,
    kAborted,
    /// TASK-016：一方在宽限期内没有回来，判其负。**它产生胜负**，会写入 match_results。
    /// 只有"双方都断线且都没回来"才走 kAborted（那一局没有任何一方值得判负）。
    kDisconnect,
};

/// 玩家输入类型。TASK-008 只有攻击一种。
enum class InputKind {
    kAttack,
};

// ---------------------------------------------------------------------------
// 快照与结果
// ---------------------------------------------------------------------------

/// 单个玩家在对局中的权威状态。
struct PlayerSnapshot {
    std::string player_id;
    std::int32_t hp = kInitialHp;

    /// 是否在房间内（已加入且未离开）。**不是**"网络是否连通"，见 online。
    bool connected = true;

    /// TASK-016：推送连接是否在线。断线后进入宽限期。
    bool online = true;
};

/// 房间权威状态快照。
struct RoomSnapshot {
    std::string room_id;
    std::string match_id;
    RoomPhase phase = RoomPhase::kCreated;
    std::int64_t frame = 0;
    std::vector<PlayerSnapshot> players;
    FinishReason finish_reason = FinishReason::kNone;
    /// 胜者 player_id。平局或未结束时为空。
    std::string winner_id;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
};

/// 对局结果。与 MySQL 的 match_results 表一一对应。
struct MatchResultRecord {
    std::string match_id;
    std::string room_id;
    /// 胜者 player_id。平局为空字符串，落库时为 NULL。
    std::string winner_id;
    std::int32_t player_count = 0;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
};

/// 快照间隔（毫秒）。到达间隔才把房间快照写入 MySQL（TASK-013）。
///
/// 取值理由：对战 10 Hz、最长 600 帧（60 秒）。1 秒间隔意味着重启后最多回退
/// 1 秒进度，也就是最多重放或少算一次攻击（每次 10 点血，占满血 10%）。
/// 1 Hz/房间 对 MySQL 的压力可以忽略；更密不划算，更疏则偏差难以解释。
inline constexpr std::int64_t kSnapshotIntervalMs = 1000;

/// 单个玩家的快照记录。与 rooms 表的 p1_* / p2_* 列对应。
struct RoomPlayerRecord {
    std::string player_id;
    std::int32_t hp = kInitialHp;
    /// 是否已加入房间。注意它表达的是"在不在名单里"，不是网络是否连通。
    bool joined = false;
};

/// 房间快照记录。与 MySQL 的 rooms 表一一对应。
///
/// 为什么与 RoomSnapshot 分开而不是复用：RoomSnapshot 用 `std::vector` 存玩家，
/// 而表结构是固定的两组列。分开之后，"写进去的玩家数正好是 kPlayersPerRoom"
/// 这件事由**类型系统**保证，而不是等到插入时才发现少了一个人。
struct RoomSnapshotRecord {
    std::string match_id;
    std::string room_id;
    RoomPhase phase = RoomPhase::kCreated;
    std::int64_t frame = 0;
    std::array<RoomPlayerRecord, kPlayersPerRoom> players{};
    /// 胜者 player_id。平局或未结束为空字符串，落库时为 NULL。
    std::string winner_id;
    FinishReason finish_reason = FinishReason::kNone;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    /// 快照写入时间（Unix 毫秒）。TASK-014 的恢复边界要靠它说明"落后了多少"。
    std::int64_t snapshot_at_ms = 0;
};

/// 把状态转成可读字符串。仅用于日志与验收脚本的可读输出，不参与协议。
[[nodiscard]] inline const char* ToString(RoomPhase phase) {
    switch (phase) {
        case RoomPhase::kCreated:
            return "created";
        case RoomPhase::kWaiting:
            return "waiting";
        case RoomPhase::kPlaying:
            return "playing";
        case RoomPhase::kFinishing:
            return "finishing";
        case RoomPhase::kFinished:
            return "finished";
        case RoomPhase::kAborted:
            return "aborted";
    }
    return "unknown";
}

/// 把结束原因转成可读字符串。
[[nodiscard]] inline const char* ToString(FinishReason reason) {
    switch (reason) {
        case FinishReason::kNone:
            return "none";
        case FinishReason::kHpZero:
            return "hp_zero";
        case FinishReason::kTimeout:
            return "timeout";
        case FinishReason::kAborted:
            return "aborted";
        case FinishReason::kDisconnect:
            return "disconnect";
    }
    return "unknown";
}

}  // namespace rgbt::room

#endif  // RGBT_ROOM_ROOM_TYPES_HPP
