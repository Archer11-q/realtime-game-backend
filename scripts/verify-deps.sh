#!/usr/bin/env bash
#
# verify-deps.sh - 本地依赖环境（Redis / MySQL）验收脚本
#
# 覆盖 TASK-003 的三条验收标准：
#   1. 一条命令启动依赖
#   2. 重启后数据卷保留
#   3. 连接配置和停止方式有文档（本脚本只验证可连通，文档见 deploy/compose/README.md）
#
# 用法：
#   bash scripts/verify-deps.sh              # 启动、验证、重启验证、然后停止
#   bash scripts/verify-deps.sh --keep       # 验证结束后保持容器运行
#   bash scripts/verify-deps.sh --down-only  # 只停止并移除容器和网络
#
# 前置条件：
#   * Docker Desktop 已启动，`docker info` 能返回服务端版本。
#   * deploy/compose/.env 存在；缺失时本脚本会用同目录的 .env.example 生成一份。
#
# 说明：.env 与 docker-compose.yml 同目录，compose 会自动发现，因此本脚本
# 不需要传 --env-file。
#
# 退出码：0 全部通过；非 0 表示失败，失败原因会打印在末尾。
# 本脚本不会执行 `down -v`，因此不会删除数据卷。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

compose_dir="deploy/compose"
compose_file="$compose_dir/docker-compose.yml"
env_file="$compose_dir/.env"
env_example="$compose_dir/.env.example"
keep_running=0
down_only=0

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --down-only) down_only=1 ;;
    -h | --help)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "未知参数: $arg（可用：--keep / --down-only / --help）" >&2
      exit 2
      ;;
  esac
done

failures=()

fail() {
  failures+=("$1")
  echo "x  $1"
}

ok() { echo "v  $1"; }

compose() {
  docker compose -f "$compose_file" "$@"
}

echo "工作目录: $repo_root"
echo

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="

if ! command -v docker >/dev/null 2>&1; then
  echo "未找到 docker 命令。" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "Docker 服务端不可达。请先启动 Docker Desktop，然后重试。" >&2
  exit 1
fi
ok "Docker 可用: $(docker version --format '{{.Server.Version}}' 2>/dev/null)"

if ! [ -f "$env_file" ]; then
  if ! [ -f "$env_example" ]; then
    echo "未找到 $env_example，无法生成配置。" >&2
    exit 1
  fi
  echo "未找到 $env_file，将从 $env_example 生成（.env 已被 git 忽略）。"
  cp "$env_example" "$env_file"
  ok "已生成 $env_file"
else
  ok "$env_file 已存在"
fi

# shellcheck disable=SC1090
set -a
. "./$env_file"
set +a

: "${REDIS_PORT:?缺少 REDIS_PORT}"
: "${MYSQL_PORT:?缺少 MYSQL_PORT}"
: "${MYSQL_DATABASE:?缺少 MYSQL_DATABASE}"
: "${MYSQL_USER:?缺少 MYSQL_USER}"
: "${MYSQL_PASSWORD:?缺少 MYSQL_PASSWORD}"
ok "配置读取完成（Redis:${REDIS_PORT} MySQL:${MYSQL_PORT} 库:${MYSQL_DATABASE}）"
echo

if [ "$down_only" -eq 1 ]; then
  echo "===== 仅停止并移除容器 ====="
  compose down && ok "已停止（数据卷保留）"
  exit 0
fi

# ---------- 1. 一条命令启动 ----------
echo "===== 1. 启动依赖 ====="
echo "命令: docker compose -f $compose_file up -d"
if compose up -d; then
  ok "启动命令执行成功"
else
  fail "启动命令失败"
  echo
  echo "===== 验收失败 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  exit 1
fi
echo

# ---------- 2. 等待健康 ----------
echo "===== 2. 等待健康检查 ====="
wait_healthy() {
  local service="$1" timeout_s="$2" waited=0 state
  while [ "$waited" -lt "$timeout_s" ]; do
    state="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' \
      "$(compose ps -q "$service" 2>/dev/null)" 2>/dev/null)"
    case "$state" in
      healthy | running) return 0 ;;
    esac
    sleep 3
    waited=$((waited + 3))
  done
  echo "   $service 在 ${timeout_s}s 内未变为 healthy，最后状态: ${state:-unknown}"
  return 1
}

if wait_healthy redis 60; then ok "redis 健康"; else fail "redis 未健康"; fi
if wait_healthy mysql 180; then ok "mysql 健康"; else fail "mysql 未健康"; fi
echo

# ---------- 3. 连通性 ----------
echo "===== 3. 连通性验证 ====="

if docker exec rgbt-redis redis-cli ping 2>/dev/null | grep -q PONG; then
  ok "Redis PING 返回 PONG"
else
  fail "Redis PING 无响应"
fi

mysql_query() {
  docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" \
    -D "$MYSQL_DATABASE" -N -B -e "$1" 2>/dev/null
}

if version="$(mysql_query 'SELECT VERSION();')" && [ -n "$version" ]; then
  ok "MySQL 业务账号连接成功，版本 $version"
else
  fail "MySQL 业务账号连接失败"
fi

if count="$(mysql_query 'SELECT COUNT(*) FROM schema_migrations;')"; then
  ok "迁移表可访问，已应用迁移数: $count"
else
  fail "schema_migrations 表不可访问（初始化脚本可能未执行）"
fi
echo

# ---------- 4. 重启后数据保留 ----------
echo "===== 4. 重启后数据保留验证 ====="

# Redis：写一个测试键，重启后确认仍存在。
if docker exec rgbt-redis redis-cli set rgbt:verify:persist ok >/dev/null 2>&1; then
  ok "Redis 已写入测试键 rgbt:verify:persist"
else
  fail "Redis 写入测试键失败"
fi

# MySQL：建一个不进入迁移体系的临时表并写入。
if mysql_query 'CREATE TABLE IF NOT EXISTS _verify_persist (k VARCHAR(32) PRIMARY KEY, v VARCHAR(32));' >/dev/null 2>&1; then
  mysql_query "INSERT INTO _verify_persist (k, v) VALUES ('persist', 'ok') ON DUPLICATE KEY UPDATE v = 'ok';" >/dev/null 2>&1
  ok "MySQL 已写入测试数据"
else
  fail "MySQL 写入测试数据失败"
fi

echo "重启容器中（不删除卷）..."
compose restart >/dev/null 2>&1

if wait_healthy redis 60; then ok "redis 重启后健康"; else fail "redis 重启后未健康"; fi
if wait_healthy mysql 180; then ok "mysql 重启后健康"; else fail "mysql 重启后未健康"; fi

if [ "$(docker exec rgbt-redis redis-cli get rgbt:verify:persist 2>/dev/null)" = "ok" ]; then
  ok "Redis 数据在重启后保留"
else
  fail "Redis 数据在重启后丢失"
fi

if [ "$(mysql_query "SELECT v FROM _verify_persist WHERE k = 'persist';")" = "ok" ]; then
  ok "MySQL 数据在重启后保留"
else
  fail "MySQL 数据在重启后丢失"
fi
echo

# ---------- 5. 清理测试数据 ----------
echo "===== 5. 清理测试数据 ====="
docker exec rgbt-redis redis-cli del rgbt:verify:persist >/dev/null 2>&1 &&
  ok "Redis 测试键已删除" || fail "Redis 测试键删除失败"
mysql_query 'DROP TABLE IF EXISTS _verify_persist;' >/dev/null 2>&1 &&
  ok "MySQL 测试表已删除" || fail "MySQL 测试表删除失败"
echo

# ---------- 6. 停或留 ----------
echo "===== 6. 收尾 ====="
if [ "$keep_running" -eq 1 ]; then
  ok "按 --keep 要求保持容器运行"
  echo "   查看状态: docker compose -f $compose_file ps"
  echo "   停止:     bash scripts/verify-deps.sh --down-only"
else
  if compose down >/dev/null 2>&1; then
    ok "容器已停止并移除，数据卷保留（下次启动数据仍在）"
  else
    fail "停止容器失败"
  fi
fi
echo

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 验收失败 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  echo
  echo "排查提示："
  echo "  1. 端口占用:  ss -ltnp | grep -E ':(6379|3306)'"
  echo "  2. 容器日志:  docker compose -f $compose_file logs --tail=50"
  echo "  3. 容器状态:  docker compose -f $compose_file ps"
  exit 1
fi

echo "===== 验收通过：Redis 与 MySQL 可启动、可连通、重启后数据保留 ====="
