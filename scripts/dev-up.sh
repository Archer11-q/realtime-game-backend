#!/usr/bin/env bash
#
# dev-up.sh - 一条命令起齐集成环境
#
# 起：Redis 与 MySQL（容器）、三个 C++ 服务（本机进程）、Vue 前端（Vite）。
# 停：bash scripts/dev-down.sh
#
# 用法：
#   bash scripts/dev-up.sh                  # 用默认端口
#   GATEWAY_PORT=9080 bash scripts/dev-up.sh  # 覆盖端口（前端代理会自动跟随）
#   bash scripts/dev-up.sh --no-build        # 跳过编译（已编译过时省时间）
#
# 端口（默认）：Gateway 8080 / Match 8082 / Room 8083 / 前端 5173
#
# 为什么应用服务跑在本机而不是容器里（TASK-011 的决定，见
# docs/adr/0003-scope-reduction.md 与 docs/06-operations.md）：
#   容器化要引入 4 个 Dockerfile，并在容器里用 vcpkg 从源码重建 brpc。
#   那是「现场演示可复现」与「故障注入」的前置条件，属于 Phase 4/5 的题目；
#   Phase 1 的退出标准只要求「能用一条命令启动集成环境」，本脚本即满足。
#   在此之前把应用服务容器化，收益要到两个阶段后才兑现，成本却立刻发生。
#
# 进程管理：所有后台进程写入 .run/*.pid，日志写入 .run/*.log。
# 用 setsid 让每个服务自成一个进程组——否则 npm/vite 这类会 fork 子进程的
# 启动方式会留下孤儿进程继续占用端口（这个坑在 TASK-010 已经踩过一次）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"
run_dir="$repo_root/.run"

# 端口。固定值，便于照抄命令；需要并存多套环境时用环境变量覆盖。
GATEWAY_PORT="${GATEWAY_PORT:-8080}"
MATCH_PORT="${MATCH_PORT:-8082}"
ROOM_PORT="${ROOM_PORT:-8083}"
WEB_PORT="${WEB_PORT:-5173}"

skip_build=0
for arg in "$@"; do
  case "$arg" in
    --no-build) skip_build=1 ;;
    -h | --help)
      sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

die() { echo "x  $1" >&2; exit 1; }
step() { echo; echo "===== $1 ====="; }
ok() { echo "v  $1"; }

# ---------- 0. 前置检查 ----------
step "0. 前置检查"
if [ -d "$HOME/tools/node/bin" ]; then
  export PATH="$HOME/tools/node/bin:$PATH"
fi

for tool in docker cmake cmake ninja node npm curl; do
  command -v "$tool" >/dev/null 2>&1 || die "缺少 $tool"
done
ok "docker / cmake / ninja / node $(node --version) / npm $(npm --version) 均可用"

docker info >/dev/null 2>&1 || die "Docker 服务端不可达，请先启动 Docker Desktop"
ok "Docker 可用"

if [ ! -f "$env_file" ]; then
  [ -f "$env_example" ] || die "缺少 $env_file 与 $env_example"
  cp "$env_example" "$env_file"
  ok "已从 .env.example 生成 $env_file"
fi
set -a
. "./$env_file"
set +a
ok "配置已加载（库 ${MYSQL_DATABASE}，账号 ${MYSQL_USER}）"

# 端口冲突必须显式报错，而不是让服务启动失败后让请求打到别人的进程上。
port_in_use() { ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"; }
for pair in "Gateway:$GATEWAY_PORT" "Match:$MATCH_PORT" "Room:$ROOM_PORT" "前端:$WEB_PORT"; do
  name="${pair%%:*}"; port="${pair#*:}"
  if port_in_use "$port"; then
    echo "x  $name 端口 $port 已被占用："
    ss -ltnp 2>/dev/null | grep -E "[:.]$port\b" | sed 's/^/     /'
    echo "   先执行 bash scripts/dev-down.sh，或用环境变量换一组端口。"
    exit 1
  fi
done
ok "四个端口均空闲（$GATEWAY_PORT / $MATCH_PORT / $ROOM_PORT / $WEB_PORT）"

mkdir -p "$run_dir"

# ---------- 1. 依赖 ----------
step "1. 启动 Redis 与 MySQL"
docker compose -f "$compose_file" up -d || die "docker compose up 失败"
for pair in "rgbt-redis:Redis" "rgbt-mysql:MySQL"; do
  container="${pair%%:*}"; name="${pair#*:}"
  healthy=0
  for _ in $(seq 1 60); do
    if [ "$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)" = "healthy" ]; then
      healthy=1
      break
    fi
    sleep 2
  done
  [ "$healthy" -eq 1 ] || die "$name（$container）未达到 healthy"
  ok "$name 健康"
done

# 迁移必须应用：登录要读 players，对局结果要写 match_results。
# 脚本幂等，每次运行都重放一遍。
applied=0
for file in migrations/*.sql; do
  [ -e "$file" ] || continue
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$file" >/dev/null 2>&1 && applied=$((applied + 1))
done
[ "$applied" -gt 0 ] || die "迁移脚本未成功应用"
ok "已应用 $applied 个迁移脚本（幂等）"

# 清掉上一轮的会话，避免「明明退出了却还是登录状态」这种难以理解的现象。
docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
ok "已清理上一轮遗留的会话"

# ---------- 2. 编译 ----------
if [ "$skip_build" -eq 1 ]; then
  step "2. 跳过编译（--no-build）"
  for bin in gateway match room; do
    [ -x "build/$preset/bin/rgbt_$bin" ] || die "缺少 build/$preset/bin/rgbt_$bin，不能跳过编译"
  done
  ok "已有可执行文件"
else
  step "2. 编译后端"
  cmake --preset "$preset" >"$run_dir/cmake.log" 2>&1 || {
    tail -20 "$run_dir/cmake.log"; die "CMake 配置失败"; }
  cmake --build --preset "$preset" >"$run_dir/build.log" 2>&1 || {
    tail -30 "$run_dir/build.log"; die "后端编译失败"; }
  ok "后端编译完成"
fi

# ---------- 3. 前端依赖 ----------
step "3. 前端依赖"
if [ -d web/node_modules ]; then
  ok "web/node_modules 已存在，跳过安装"
else
  ( cd web && npm ci --no-fund --no-audit ) >"$run_dir/npm.log" 2>&1 || {
    tail -20 "$run_dir/npm.log"; die "npm ci 失败"; }
  ok "前端依赖安装完成"
fi

# ---------- 4. 启动后端 ----------
step "4. 启动三个后端服务"

# start_service <名字> <端口> <命令...>
#
# setsid 让服务自成进程组，dev-down.sh 才能连它的子进程一起结束。
# 日志与 pid 都落在 .run/，便于排障时直接看。
start_service() {
  local name="$1" port="$2"
  shift 2
  setsid "$@" >"$run_dir/$name.log" 2>&1 &
  echo $! >"$run_dir/$name.pid"
  local pid
  pid="$(cat "$run_dir/$name.pid")"
  local ready=0
  for _ in $(seq 1 40); do
    if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$port/health" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.5
  done
  if [ "$ready" -eq 1 ]; then
    ok "$name 就绪（端口 $port，进程号 $pid）"
  else
    echo "x  $name 未就绪，日志末尾："
    tail -20 "$run_dir/$name.log" | sed 's/^/     /'
    exit 1
  fi
}

start_service room "$ROOM_PORT" \
  "build/$preset/bin/rgbt_room" -port "$ROOM_PORT" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE"

start_service match "$MATCH_PORT" \
  "build/$preset/bin/rgbt_match" -match_port "$MATCH_PORT" \
  -room_host 127.0.0.1 -room_port "$ROOM_PORT"

start_service gateway "$GATEWAY_PORT" \
  "build/$preset/bin/rgbt_gateway" -port "$GATEWAY_PORT" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$MATCH_PORT" \
  -room_host 127.0.0.1 -room_port "$ROOM_PORT"

# ---------- 5. 启动前端 ----------
step "5. 启动前端开发服务器"
# RGBT_GATEWAY_PORT 必须等于上面的 Gateway 端口，否则页面能打开但接口全部 404。
# npm --prefix web 而不是先 cd：保持脚本工作目录，后面用相对路径不会出错。
setsid env RGBT_WEB_PORT="$WEB_PORT" RGBT_GATEWAY_PORT="$GATEWAY_PORT" \
  npm --prefix web run dev >"$run_dir/web.log" 2>&1 &
echo $! >"$run_dir/web.pid"
web_pid="$(cat "$run_dir/web.pid")"

web_ready=0
for _ in $(seq 1 60); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$WEB_PORT/" 2>/dev/null; then
    web_ready=1
    break
  fi
  sleep 0.5
done
if [ "$web_ready" -eq 1 ]; then
  ok "前端就绪（端口 $WEB_PORT，进程号 $web_pid）"
else
  echo "x  前端未就绪，日志末尾："
  tail -20 "$run_dir/web.log" | sed 's/^/     /'
  exit 1
fi

# ---------- 6. 端到端探活 ----------
step "6. 端到端探活（经前端代理）"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$WEB_PORT/")
[ "$code" = "200" ] && ok "GET / -> 200" || echo "x  GET / -> $code"

code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST \
  -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$WEB_PORT/api/v1/login")
[ "$code" = "400" ] && ok "代理 /api 生效（空登录体 -> 400）" ||
  echo "x  代理异常：POST /api/v1/login -> $code（期望 400，说明 /api 没有转到 Gateway）"

cat <<EOF

============================================================
  集成环境已就绪。浏览器打开：

      http://127.0.0.1:$WEB_PORT

  人工验证（两个标签页即可，不需要无痕模式——token 存在标签页自己的
  内存里，两个标签天然互不干扰）：

    1. 标签页 1：点「填 alice」→「登录」
    2. 标签页 2：点「填 bob」 →「登录」
    3. 两个标签页都点「开始匹配」，第二个点了之后两边会自动跳进对战页
    4. 在任一标签页反复点「攻击」，确认：
         - 对手血条下降（每次 10 点）
         - 服务端帧号在增长
         - 「最近推送序号」在变，旁边的「Ns 前」始终是 0s 或 1s
         - 顶栏显示「推送已连接」
    5. 打到一方 HP 归零，确认自动进入结算页、胜负正确、「结果已落库 = 是」

  注意：刷新页面会退出登录（token 不落 localStorage，这是刻意的）。

  端口：Gateway $GATEWAY_PORT / Match $MATCH_PORT / Room $ROOM_PORT / 前端 $WEB_PORT
  日志：$run_dir/{gateway,match,room,web}.log
  停止：bash scripts/dev-down.sh
============================================================
EOF
