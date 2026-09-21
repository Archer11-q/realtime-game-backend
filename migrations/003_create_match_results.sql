-- 003_create_match_results.sql
--
-- 用途：建立对局结果表。
--
-- 范围说明：
--   * 表的所有者是 Settlement 服务（正式归属）。Phase 1 只建表，**不写入**；
--     Settlement Worker 在 Phase 5 落地后才写入，见 docs/02-roadmap.md。
--   * match_id 既是主键，也是结算的幂等业务键，依据 docs/05-api-and-data.md
--     第 5 节「业务结算使用 match_id 作为唯一业务键」。
--   * 不使用本表保存高频帧日志（依据第 4 节约束）。
--
-- 约定：幂等，可在已有数据卷上安全重跑。

CREATE TABLE IF NOT EXISTS match_results (
    match_id     VARCHAR(64) NOT NULL COMMENT '对局唯一标识，同时是结算幂等业务键',
    room_id      VARCHAR(64) NULL COMMENT '来源房间标识，可空',
    winner_id    VARCHAR(64) NULL COMMENT '胜者 player_id；平局为 NULL',
    player_count INT         NOT NULL COMMENT '参战玩家数',
    started_at   TIMESTAMP   NULL COMMENT '开局时间，可空',
    finished_at  TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '结束时间',
    PRIMARY KEY (match_id)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_0900_ai_ci
  COMMENT = '对局结果，match_id 为幂等业务键';

INSERT INTO schema_migrations (version, description)
VALUES ('003', 'create match_results table')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
