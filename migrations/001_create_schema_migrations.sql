-- 001_create_schema_migrations.sql
--
-- 用途：建立迁移记录表，验证 MySQL 初始化链路（migrations -> 容器内
--       /docker-entrypoint-initdb.d -> 首次启动自动执行）。
--
-- 约束：
--   * 本文件只允许在空数据目录首次启动时被 MySQL 执行一次。
--   * 使用幂等写法，即使被重复执行也不产生重复记录。
--   * 本轮不创建业务表；业务表结构在 TASK-005 及之后的任务中按需追加。
--
-- 表结构说明：
--   version     迁移版本号，主键，重复执行时更新而非插入。
--   description 迁移内容简述。
--   applied_at  首次应用时间。

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     VARCHAR(64)  NOT NULL,
    description VARCHAR(255) NOT NULL,
    applied_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (version)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_0900_ai_ci;

-- 幂等写入：重复执行只更新时间，不产生第二行。
INSERT INTO schema_migrations (version, description)
VALUES ('001', 'create schema_migrations table')
ON DUPLICATE KEY UPDATE applied_at = applied_at;
