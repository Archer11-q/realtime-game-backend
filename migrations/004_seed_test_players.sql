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
--   dave  / p-0004 / active     —— 第三个启用身份
--
-- 为什么需要 dave（2026-10-02 补上）：
--   有两条不变量需要**三个启用身份**才能在端到端层面覆盖，
--   而此前只有 alice 与 bob 两个：
--     * 匹配：第三个玩家不会被并入一个已配满的局
--     * 房间：非本局成员无法加入（不是这一局的人，即使房间还有空位也进不去）
--   此前这两条只能靠单元测试覆盖，端到端脚本里显式打印为「未覆盖项」。
--   是否新增第三个启用身份在 docs/devlog.md 里记为待项目所有者决定，本文件是结论。
--
-- 幂等：重复执行只更新展示名与状态，不产生重复行，也不会覆盖 player_id。

INSERT INTO players (player_id, account, display_name, status)
VALUES ('p-0001', 'alice', 'Alice', 'active'),
       ('p-0002', 'bob', 'Bob', 'active'),
       ('p-0003', 'carol', 'Carol', 'disabled'),
       ('p-0004', 'dave', 'Dave', 'active')
ON DUPLICATE KEY UPDATE display_name = VALUES(display_name), status = VALUES(status);

INSERT INTO schema_migrations (version, description)
VALUES ('004', 'seed test players for development')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
