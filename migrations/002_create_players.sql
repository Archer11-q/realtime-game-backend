-- 002_create_players.sql
--
-- 用途：建立玩家档案表。
--
-- 范围说明（依据 docs/07-open-decisions.md 与 docs/adr/0003-scope-reduction.md）：
--   * 本表**不含密码列**。D-002 决定第一版账号密码保留在代码中作为测试数据，
--     不写入数据库，也不实现注册系统。
--   * 表的所有者是 **Gateway**。原定的 Player/State 服务已列为非目标、不实现，
--     因此 docs/adr/0002 中"临时例外"的退出条件已由 docs/adr/0003 取消，
--     该归属自 2026-10-02 起为正式安排。Gateway 对本表只读。
--
-- 约定：
--   * 幂等：重复执行不报错、不产生重复行，便于在已有数据卷上安全重跑。
--   * 账号唯一性由 uk_players_account 唯一约束保证，而不是靠应用层判断。

CREATE TABLE IF NOT EXISTS players (
    player_id    VARCHAR(64)  NOT NULL COMMENT '玩家唯一标识，例如 p-0001',
    account      VARCHAR(64)  NOT NULL COMMENT '登录账号名',
    display_name VARCHAR(64)  NOT NULL COMMENT '展示名',
    status       VARCHAR(16)  NOT NULL DEFAULT 'active' COMMENT '账号状态：active / disabled',
    created_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        COMMENT '更新时间，行变更时自动刷新',
    PRIMARY KEY (player_id),
    UNIQUE KEY uk_players_account (account)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_0900_ai_ci
  COMMENT = '玩家档案，第一版不含密码';

-- 记录迁移本身，保持与 001 相同的幂等写法。
INSERT INTO schema_migrations (version, description)
VALUES ('002', 'create players table')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
