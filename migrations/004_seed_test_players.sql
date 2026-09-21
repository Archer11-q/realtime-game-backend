-- 004_seed_test_players.sql
--
-- ⚠️ 仅用于开发环境
--
-- 本文件插入固定的测试玩家，使登录链路可以在真实数据库上验证。
-- 它**不是**生产数据，也不代表项目的真实用户规模。
--
-- 依据 docs/07-open-decisions.md 的 D-002：
--   * 第一版不实现注册系统。
--   * 账号密码**不存入数据库**，密码仍保留在代码中作为测试身份
--     （见 src/gateway/player_directory.cpp）。因此本表只有档案，没有密码列。
--
-- 与代码的对应关系（账号 / player_id / 状态必须一致，否则登录会失败）：
--   alice / p-0001 / active
--   bob   / p-0002 / active
--   carol / p-0003 / disabled   —— 刻意保留一个禁用账号，用于覆盖失败路径
--
-- 幂等：重复执行只更新展示名与状态，不产生重复行，也不会覆盖 player_id。

INSERT INTO players (player_id, account, display_name, status)
VALUES ('p-0001', 'alice', 'Alice', 'active'),
       ('p-0002', 'bob', 'Bob', 'active'),
       ('p-0003', 'carol', 'Carol', 'disabled')
ON DUPLICATE KEY UPDATE display_name = VALUES(display_name), status = VALUES(status);

INSERT INTO schema_migrations (version, description)
VALUES ('004', 'seed test players for development')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
