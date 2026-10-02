#!/usr/bin/env bash
#
# dev-down.sh - 停掉 dev-up.sh 起的所有东西
#
# 用法：
#   bash scripts/dev-down.sh              # 停三个服务与前端，保留 Redis/MySQL 容器
#   bash scripts/dev-down.sh --with-docker # 连容器一起停
#
# 为什么不能只 kill pid 文件里的那个号：
#   `npm run dev` 的进程树是 npm -> sh -c vite -> node .../vite，
#   只结束第一层会留下后两层继续占用端口，下次启动直接报端口冲突。
#   这个坑在 TASK-010 已经踩过，因此这里对**整个进程组**发信号，
#   并按端口兜底核对，最后断言端口确实释放了。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
run_dir="$(pwd)/.run"

GATEWAY_PORT="${GATEWAY_PORT:-8080}"
MATCH_PORT="${MATCH_PORT:-8082}"
ROOM_PORT="${ROOM_PORT:-8083}"
WEB_PORT="${WEB_PORT:-5173}"

with_docker=0
for arg in "$@"; do
  case "$arg" in
    --with-docker) with_docker=1 ;;
    -h | --help)
      sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

ok() { echo "v  $1"; }
warn() { echo "!  $1"; }

# 结束一个进程及其整个进程组。
stop_pidfile() {
  local name="$1" pidfile="$run_dir/$1.pid"
  [ -f "$pidfile" ] || return 0
  local pid
  pid="$(cat "$pidfile" 2>/dev/null)"
  if [ -z "$pid" ]; then
    rm -f "$pidfile"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    warn "$name（pid $pid）已不在运行"
    rm -f "$pidfile"
    return 0
  fi

  # 负号 = 整个进程组。setsid 保证了 pid 就是组长。
  kill -TERM -"$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$pid" 2>/dev/null; then
    warn "$name 未在 10 秒内退出，强制结束"
    kill -KILL -"$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
  fi
  ok "$name 已停止"
  rm -f "$pidfile"
}

echo "===== 停止服务 ====="
for name in gateway match room web; do
  stop_pidfile "$name"
done

# 兜底：pid 文件可能因为异常退出而丢失或过期。按**我们自己的端口**找出
# 仍在监听的进程并结束——只针对这几个端口，不误伤其它程序。
echo
echo "===== 端口兜底清理 ====="
for pair in "Gateway:$GATEWAY_PORT" "Match:$MATCH_PORT" "Room:$ROOM_PORT" "前端:$WEB_PORT"; do
  name="${pair%%:*}"; port="${pair#*:}"
  leaked=$(ss -ltnp 2>/dev/null | grep -E "[:.]$port\b" |
    grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u)
  if [ -z "$leaked" ]; then
    ok "$name 端口 $port 空闲"
    continue
  fi
  warn "$name 端口 $port 仍被占用，结束残留进程：$leaked"
  for pid in $leaked; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 1
  for pid in $leaked; do
    kill -KILL "$pid" 2>/dev/null || true
  done
  if ss -ltn 2>/dev/null | grep -qE "[:.]$port\b"; then
    echo "x  $name 端口 $port 仍被占用，请手工检查：ss -ltnp | grep $port"
  else
    ok "$name 端口 $port 已释放"
  fi
done

# 进程名兜底：万一 pid 文件丢了，端口也可能因为服务启动失败而没被占用。
echo
for pat in 'rgbt_gateway' 'rgbt_match' 'rgbt_room'; do
  pids="$(pgrep -f "$pat" 2>/dev/null || true)"
  if [ -n "$pids" ]; then
    warn "发现残留的 $pat：$pids"
    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null || true
  fi
done

if [ -f "$run_dir/web.log" ]; then
  ok "日志保留在 $run_dir/（下次 dev-up.sh 会覆盖）"
fi

if [ "$with_docker" -eq 1 ]; then
  echo
  echo "===== 停止 Redis 与 MySQL ====="
  # 保留数据卷：down 不带 -v，避免把开发数据一起删掉。
  if docker compose -f deploy/compose/docker-compose.yml down >/dev/null 2>&1; then
    ok "容器已停止（数据卷保留）"
  else
    warn "docker compose down 失败，请手工检查"
  fi
else
  echo
  echo "Redis 与 MySQL 容器仍在运行。要一起停：bash scripts/dev-down.sh --with-docker"
fi
