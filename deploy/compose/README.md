# 本地开发依赖（Redis / MySQL）

本目录提供本地开发和集成环境所需的 Redis 与 MySQL，通过 Docker Compose 启动。

> 状态：TASK-003 交付。当前只包含 Redis 和 MySQL，**不包含** Kafka、etcd 和任何
> C++ 服务，这些按各自任务引入。

## 1. 前置依赖

| 依赖 | 要求 | 检查方式 |
|---|---|---|
| Docker Desktop | 已启动，WSL Integration 生效 | `docker info` 有输出 |
| Docker Compose | v2 及以上（本机 v5.1.3） | `docker compose version` |
| 仓库根 `.env` | 存在 | `ls .env` |
| 端口 | `REDIS_PORT`、`MYSQL_PORT` 未被占用 | `ss -ltnp \| grep -E ':(6379\|3306)'` |

首次启动需要联网拉取镜像（Redis 约 40 MB，MySQL 约 200 MB）。

## 2. 一条命令启动

在**仓库根目录**执行：

```bash
cp .env.example .env          # 仅首次需要
bash scripts/verify-deps.sh --keep
```

或者直接用 compose：

```bash
docker compose --env-file .env -f deploy/compose/docker-compose.yml up -d
```

> **为什么必须写 `--env-file .env`**：compose 默认从 compose 文件所在目录
> （`deploy/compose/`）读取 `.env`，而本项目的 `.env` 按 `docs/06-operations.md`
> 固定放在仓库根目录。不传这个参数会因缺少变量而报错退出——这是刻意的，
> 目的是避免用空密码/默认值悄悄启动一个错误的环境。

## 3. 健康检查

```bash
docker compose --env-file .env -f deploy/compose/docker-compose.yml ps
```

`STATUS` 列出现 `healthy` 表示就绪。容器名固定为 `rgbt-redis` 和 `rgbt-mysql`。

手动单独探测：

```bash
docker exec rgbt-redis redis-cli ping                     # 期望 PONG
docker exec rgbt-mysql mysqladmin ping -h 127.0.0.1 -uroot -p"$MYSQL_ROOT_PASSWORD"
```

## 4. 日志查看

```bash
# 全部服务，实时跟随
docker compose --env-file .env -f deploy/compose/docker-compose.yml logs -f

# 只看 MySQL 最近 50 行
docker compose --env-file .env -f deploy/compose/docker-compose.yml logs --tail=50 mysql
```

## 5. 连接方式

### 从 WSL 宿主机连接（使用 `.env` 中的 HOST/PORT）

```bash
# Redis
redis-cli -h 127.0.0.1 -p "${REDIS_PORT:-6379}" ping

# MySQL（业务账号）
mysql -h 127.0.0.1 -P "${MYSQL_PORT:-3306}" -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" -D "$MYSQL_DATABASE"
```

### 从其他容器连接（使用服务名，端口用容器内端口）

| 目标 | 地址 |
|---|---|
| Redis | `redis:6379` |
| MySQL | `mysql:3306` |

注意：容器内端口固定为 6379 / 3306，`REDIS_PORT` 和 `MYSQL_PORT` 只影响宿主机映射。

## 6. 停止与清理

```bash
# 停止并移除容器与网络，数据卷保留（推荐日常使用）
docker compose --env-file .env -f deploy/compose/docker-compose.yml down

# 等价写法
bash scripts/verify-deps.sh --down-only
```

> ⚠️ **危险命令**：`down -v` 会删除数据卷，**所有数据库数据立即永久丢失**。
> 仅在明确需要重建环境时使用，且执行前先做备份（见第 7 节）。

```bash
# 会删除数据！
docker compose --env-file .env -f deploy/compose/docker-compose.yml down -v
```

## 7. 数据卷备份与恢复

数据卷为具名卷 `redis-data`、`mysql-data`，由 Docker 管理。**卷被创建不等于
恢复流程被验证过**（`docs/06-operations.md` 第 6 节），因此恢复步骤必须实际演练。

### 备份 MySQL

```bash
docker exec rgbt-mysql sh -c \
  'exec mysqldump -uroot -p"$MYSQL_ROOT_PASSWORD" --databases "$MYSQL_DATABASE"' \
  > "backup-$(date +%Y%m%d-%H%M%S).sql"
```

### 恢复 MySQL

```bash
docker exec -i rgbt-mysql sh -c \
  'exec mysql -uroot -p"$MYSQL_ROOT_PASSWORD"' < backup-YYYYmmdd-HHMMSS.sql
```

### 备份 Redis

Redis 本轮使用默认 RDB 快照（`--save 60 1`），快照文件在卷内：

```bash
docker exec rgbt-redis redis-cli save
docker cp rgbt-redis:/data/dump.rdb ./redis-dump-$(date +%Y%m%d-%H%M%S).rdb
```

### 恢复演练要求

每次演练需记录：恢复点、恢复耗时、数据差异。`docs/06-operations.md` 要求
Redis 数据以“可重建缓存”为前提时，也必须验证重建流程。

## 8. 数据库初始化脚本

`migrations/*.sql` 会以只读方式挂载到容器的 `/docker-entrypoint-initdb.d`。

- 脚本**只在数据目录为空时执行一次**（即首次启动或删除卷之后）。
- 修改 `migrations/` 下的脚本**不会**影响已有数据卷。
- 需要变更已有环境的结构时，新增迁移脚本并手动应用，不要依赖重跑。

## 9. 常见问题

### 启动时报“缺少 xxx，请先执行 cp .env.example .env”

没有传 `--env-file .env`，或者仓库根目录没有 `.env`。按第 2 节执行。

### 端口被占用

```bash
ss -ltnp | grep -E ':(6379|3306)'
```

改 `.env` 中的 `REDIS_PORT` / `MYSQL_PORT` 后重新启动，或先停掉占用端口的进程。

### 改过 `MYSQL_PASSWORD` 后连不上

MySQL 的账号密码只在**首次初始化**时创建。已有数据卷不会因为改 `.env` 而更新
密码。处理方式二选一：

1. 改回原来的密码；
2. 修改数据库中的密码：
   ```bash
   docker exec rgbt-mysql mysql -uroot -p"$MYSQL_ROOT_PASSWORD" \
     -e "ALTER USER '$MYSQL_USER'@'%' IDENTIFIED BY '新密码';"
   ```
3. 确认不需要保留数据时，`down -v` 后重建。

### MySQL 一直不 healthy

MySQL 首次初始化通常需要 30 秒以上。若超过 3 分钟仍未 healthy：

```bash
docker compose --env-file .env -f deploy/compose/docker-compose.yml logs --tail=100 mysql
```

常见原因是数据卷被旧版本写坏，此时只能 `down -v` 重建。

### `docker info` 无响应

先确认 **Docker Desktop 已启动**。2026-09-15 曾因此产生一次误判，参见
`docs/devlog.md`。

## 10. 使用 `--env-file` 之外的推荐做法

如果不想每次都写长命令，可在 shell 中加一个别名（不进入仓库）：

```bash
alias rgbt-deps='docker compose --env-file .env -f deploy/compose/docker-compose.yml'
```

之后 `rgbt-deps up -d`、`rgbt-deps ps`、`rgbt-deps down` 即可。
