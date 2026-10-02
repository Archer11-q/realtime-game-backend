#include "room_snapshot_reader.hpp"

#include <string>

namespace rgbt::room {

std::optional<RoomPhase> ParseRoomPhase(std::string_view text) {
    if (text == "created") {
        return RoomPhase::kCreated;
    }
    if (text == "waiting") {
        return RoomPhase::kWaiting;
    }
    if (text == "playing") {
        return RoomPhase::kPlaying;
    }
    if (text == "finishing") {
        return RoomPhase::kFinishing;
    }
    if (text == "finished") {
        return RoomPhase::kFinished;
    }
    if (text == "aborted") {
        return RoomPhase::kAborted;
    }
    return std::nullopt;
}

std::optional<FinishReason> ParseFinishReason(std::string_view text) {
    if (text == "none") {
        return FinishReason::kNone;
    }
    if (text == "hp_zero") {
        return FinishReason::kHpZero;
    }
    if (text == "timeout") {
        return FinishReason::kTimeout;
    }
    if (text == "aborted") {
        return FinishReason::kAborted;
    }
    return std::nullopt;
}

std::string ValidateRoomSnapshot(const RoomSnapshotRecord& record) {
    if (record.match_id.empty() || record.match_id.size() > kMaxIdLength) {
        return "match_id 缺失或超长";
    }
    if (record.room_id.empty() || record.room_id.size() > kMaxIdLength) {
        return "room_id 缺失或超长";
    }
    if (record.frame < 0 || record.frame > kMaxFrames) {
        return "frame 超出 [0, " + std::to_string(kMaxFrames) + "]";
    }
    if (record.started_at_ms < 0 || record.finished_at_ms < 0) {
        return "时间戳为负";
    }

    for (std::size_t i = 0; i < kPlayersPerRoom; ++i) {
        const RoomPlayerRecord& player = record.players[i];
        if (player.player_id.empty() || player.player_id.size() > kMaxIdLength) {
            return "第 " + std::to_string(i + 1) + " 位玩家标识缺失或超长";
        }
        if (player.hp < 0 || player.hp > kInitialHp) {
            return "第 " + std::to_string(i + 1) + " 位玩家血量超出 [0, " +
                   std::to_string(kInitialHp) + "]";
        }
    }
    if (record.players[0].player_id == record.players[1].player_id) {
        return "两位玩家的标识相同，同一人占了两个位置";
    }

    // 对局真的打过，就不可能双方都没加入。这条约束不适用于 CREATED / WAITING：
    // 那时 0 人是正常状态，不是损坏。
    const bool played =
        (record.phase == RoomPhase::kPlaying || record.phase == RoomPhase::kFinishing);
    if (played && !record.players[0].joined && !record.players[1].joined) {
        return "对局状态为已开打，但双方都不在房间内";
    }

    if (!record.winner_id.empty() && record.winner_id != record.players[0].player_id &&
        record.winner_id != record.players[1].player_id) {
        return "winner_id 不在场上玩家之中";
    }

    // 已结束却带 kNone 的结束原因，说明两列不是同一次写入的结果。
    const bool finished =
        (record.phase == RoomPhase::kFinished || record.phase == RoomPhase::kAborted);
    if (finished && record.finish_reason == FinishReason::kNone) {
        return "已处于终态但 finish_reason 为 none";
    }

    return {};
}

}  // namespace rgbt::room
