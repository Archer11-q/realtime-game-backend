-- 005_create_rooms.sql
--
-- 用途：建立房间记录表，保存房间权威状态的**周期性快照**。
--
-- 依据 docs/01-architecture.md 第 5 节「状态归属」：
--   房间权威状态 | Room/Battle | 内存 + 快照 | 从最近快照恢复
-- 本表就是那个"快照"的落点。所有者是 **Room/Battle 服务**。
--
-- 三个需要留意的设计（完整论证见 docs/TASKS.md 的 TASK-013）：
--
--   1. 字段全部离散成列，**不用 JSON 快照列**。
--      schema 本身就是"能恢复什么"的文档；把它藏进一个不透明的 JSON 字段，
--      等于把这份文档删掉，同时立刻引入格式版本管理问题。
--      p1_* / p2_* 两列不是未经验证的假设：`kPlayersPerRoom = 2` 是 TASK-008
--      已由项目所有者确认的对局规则（见 src/room/room_types.hpp）。
--
--   2. 快照是**可丢弃**的：写入失败不重试，下一次写入天然覆盖它。
--      这与 match_results 的性质相反（对局结果不可丢弃，失败必须持续重试）。
--      两张表都由 Room/Battle 拥有，但失败策略完全不同，因此代码里也用
--      两个独立接口（见 src/room/room_snapshot_writer.hpp 的说明）。
--
--   3. 本表**不是**状态变化的审计日志。它只保留每个房间的最新一份快照，
--      因此看不到历史。需要历史时应先有明确消费者，而不是提前建表。
--
-- 约定：幂等，可在已有数据卷上安全重跑。

CREATE TABLE IF NOT EXISTS rooms
(
    match_id       VARCHAR(64) NOT NULL COMMENT '对局唯一标识，同时是快照的幂等键',
    room_id        VARCHAR(64) NOT NULL COMMENT '房间标识',
    state          VARCHAR(16) NOT NULL COMMENT 'created/waiting/playing/finishing/finished/aborted',
    frame          BIGINT      NOT NULL COMMENT '快照时的服务端帧号',

    p1_id          VARCHAR(64) NOT NULL COMMENT '第 1 位玩家 player_id',
    p1_hp          INT         NOT NULL COMMENT '第 1 位玩家血量',
    p1_joined      TINYINT(1)  NOT NULL COMMENT '第 1 位玩家是否已加入房间（不是网络是否连通）',

    p2_id          VARCHAR(64) NOT NULL COMMENT '第 2 位玩家 player_id',
    p2_hp          INT         NOT NULL COMMENT '第 2 位玩家血量',
    p2_joined      TINYINT(1)  NOT NULL COMMENT '第 2 位玩家是否已加入房间',

    -- 胜者。平局与未结束均为 NULL，与 match_results 的约定一致：
    -- 「平局」和「标识为空」在数据库里必须能区分。
    winner_id      VARCHAR(64) NULL COMMENT '胜者 player_id；平局或未结束为 NULL',
    finish_reason  VARCHAR(24) NOT NULL COMMENT 'none/hp_zero/timeout/aborted',

    started_at_ms  BIGINT      NOT NULL COMMENT '开局时间（Unix 毫秒）；未开始为 0',
    finished_at_ms BIGINT      NOT NULL COMMENT '结束时间（Unix 毫秒）；未结束为 0',
    snapshot_at_ms BIGINT      NOT NULL COMMENT '本次快照写入时间（Unix 毫秒）',

    created_at     TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '首次写入时间',
    updated_at     TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        COMMENT '最近一次快照时间（数据库侧）',

    PRIMARY KEY (match_id),
    -- room_id 也唯一：一个房间只对应一局，重复的 room_id 说明房间号生成出了问题。
    UNIQUE KEY uk_rooms_room_id (room_id),
    -- 供 TASK-014 启动恢复时扫描「未结束的房间」，以及事后核对用。
    KEY idx_rooms_state_snapshot (state, snapshot_at_ms)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_0900_ai_ci
  COMMENT = '房间权威状态的周期性快照，match_id 为幂等键';

INSERT INTO schema_migrations (version, description)
VALUES ('005', 'create rooms table for room snapshots')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
