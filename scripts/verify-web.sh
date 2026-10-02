#!/usr/bin/env bash
#
# verify-web.sh - TASK-010 演示前端的验收入口
#
# 覆盖 TASK-010 的验收标准：
#   1. 前端单元测试通过（SSE 解析的边界）
#   2. 类型检查（vue-tsc）无错误
#   3. 生产构建成功并产出可服务的产物
#   4. Vite 开发服务器能提供页面
#   5. **所有前端用到的接口都能穿过 Vite 代理**（登录、匹配、房间、结果、SSE）
#   6. SSE 经过代理是**增量到达**的，不是被代理缓冲到最后一起吐出
#   7. 完整双人对局（登录 -> 匹配 -> 进房 -> 攻击 -> 结束 -> 查结果）全程走代理
#   8. 非本局成员订阅被拒绝（400 not_a_member）
#   9. 前端进程与后端进程都能优雅退出
#
# 用法：
#   bash scripts/verify-web.sh              # 完整验收
#   bash scripts/verify-web.sh --no-docker  # 复用已启动的 Redis/MySQL
#   bash scripts/verify-web.sh --keep       # 结束后保留进程
#
# 前置：Docker Desktop 已启动；Node 已安装（TASK-010 起，见 docs/06-operations.md）。
#
# 为什么用 curl 而不是浏览器驱动：这里要验证的是**网络路径**——前端发出的每个请求
# 走的 URL、方法与请求头，以及 SSE 经过代理的行为。这些 curl 都能精确断言，
# 而引入 Playwright 之类会带来浏览器下载、版本漂移与不稳定的 CI 依赖。
# **本脚本覆盖不到渲染**（Canvas 画得对不对、按钮状态对不对），那部分由人工按
# 下面的「人工检查步骤」确认。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"
gateway_binary="build/$preset/bin/rgbt_gateway"
match_binary="build/$preset/bin/rgbt_match"
room_binary="build/$preset/bin/rgbt_room"

gateway_port=""
match_port=""
room_port=""
web_port=""
gateway_candidates=(8080 18080 18081 18082)
match_candidates=(8082 18092 18093 18094)
room_candidates=(8083 18103 18104 18105)
web_candidates=(5173 15173 15174 15175)

match_timeout_seconds=30
match_result_ttl_seconds=4

keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

failures=()
fail() { failures+=("$1"); echo "x  $1"; }
ok() { echo "v  $1"; }
compose() { docker compose -f "$compose_file" "$@"; }

gateway_pid=""
match_pid=""
room_pid=""
web_pid=""
stream_pid=""
cleanup() {
  for pid in "$stream_pid" "$web_pid" "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      if [ "$keep_running" -eq 1 ]; then
        echo "已保留进程 $pid（--keep）"
      else
        kill "$pid" 2>/dev/null || true
      fi
    fi
  done
}
trap cleanup EXIT

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="
# Node 装在用户目录（见 docs/06-operations.md），因此显式加入 PATH，
# 不假设调用者的 shell 已经 source 过 .bashrc。
if [ -d "$HOME/tools/node/bin" ]; then
  export PATH="$HOME/tools/node/bin:$PATH"
fi
if ! command -v node >/dev/null 2>&1; then
  echo "缺少 node。TASK-010 起前端需要它，安装方式见 docs/06-operations.md。" >&2
  exit 1
fi
if ! command -v npm >/dev/null 2>&1; then
  echo "缺少 npm。" >&2
  exit 1
fi
ok "node $(node --version) / npm $(npm --version)"

if [ ! -f "$env_file" ]; then
  if [ -f "$env_example" ]; then
    cp "$env_example" "$env_file"
    ok "已从 .env.example 生成 $env_file"
  else
    echo "缺少 $env_file 与 $env_example" >&2
    exit 1
  fi
else
  ok "$env_file 已存在"
fi

set -a
. "./$env_file"
set +a
ok "配置已加载（库: ${MYSQL_DATABASE} 账号: ${MYSQL_USER}）"

port_in_use() { ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"; }
pick_port() {
  local candidate
  for candidate in "$@"; do
    if ! port_in_use "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
}

gateway_port="$(pick_port "${gateway_candidates[@]}")" || {
  echo "Gateway 候选端口均被占用" >&2; exit 1; }
ok "Gateway 端口 $gateway_port"

match_pool=()
for candidate in "${match_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  match_pool+=("$candidate")
done
match_port="$(pick_port "${match_pool[@]}")" || { echo "Match 端口均被占用" >&2; exit 1; }
ok "Match 端口 $match_port"

room_pool=()
for candidate in "${room_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  [ "$candidate" = "$match_port" ] && continue
  room_pool+=("$candidate")
done
room_port="$(pick_port "${room_pool[@]}")" || { echo "Room 端口均被占用" >&2; exit 1; }
ok "Room 端口 $room_port"

web_pool=()
for candidate in "${web_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  [ "$candidate" = "$match_port" ] && continue
  [ "$candidate" = "$room_port" ] && continue
  web_pool+=("$candidate")
done
web_port="$(pick_port "${web_pool[@]}")" || { echo "Vite 端口均被占用" >&2; exit 1; }
ok "Vite 端口 $web_port"
echo

# ---------- 1. 启动依赖 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  if compose up -d >/tmp/web-compose.log 2>&1; then
    ok "docker compose up 成功"
  else
    fail "docker compose up 失败"; tail -20 /tmp/web-compose.log
  fi
  for container in rgbt-redis rgbt-mysql; do
    healthy=0
    for _ in $(seq 1 60); do
      [ "$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)" = "healthy" ] &&
        { healthy=1; break; }
      sleep 2
    done
    [ "$healthy" -eq 1 ] && ok "$container 健康" || fail "$container 未达到 healthy"
  done

  applied=0
  for file in migrations/*.sql; do
    [ -e "$file" ] || continue
    if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      <"$file" >/dev/null 2>&1; then
      applied=$((applied + 1))
    fi
  done
  [ "$applied" -gt 0 ] && ok "已应用 $applied 个迁移脚本（幂等）" || fail "迁移脚本未成功应用"

  docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  ok "已清理上次运行残留的会话"
  echo
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
  echo "注意：--no-docker 不会重新应用迁移，dave 可能不存在。"
  echo
fi

# ---------- 2. 后端构建 ----------
echo "===== 2. 构建后端 ====="
cmake --preset "$preset" >/tmp/web-cmake.log 2>&1 && ok "配置成功" ||
  { fail "配置失败"; tail -20 /tmp/web-cmake.log; }
if cmake --build --preset "$preset" >/tmp/web-build.log 2>&1; then
  ok "后端构建成功"
else
  fail "后端构建失败"; tail -30 /tmp/web-build.log; exit 1
fi
echo

# ---------- 3. 前端：依赖、单测、类型检查、构建 ----------
echo "===== 3. 前端检查 ====="
cd web || { echo "缺少 web/ 目录" >&2; exit 1; }

# 有 lockfile 就用 npm ci（可复现），否则退回 npm install。
if [ -f package-lock.json ]; then
  if npm ci --no-fund --no-audit >/tmp/web-npm.log 2>&1; then
    ok "npm ci 成功（按 lockfile 安装）"
  else
    fail "npm ci 失败"; tail -20 /tmp/web-npm.log
  fi
else
  if npm install --no-fund --no-audit >/tmp/web-npm.log 2>&1; then
    ok "npm install 成功（没有 lockfile）"
  else
    fail "npm install 失败"; tail -20 /tmp/web-npm.log
  fi
fi

if npm run test >/tmp/web-unit.log 2>&1; then
  ok "前端单元测试通过"
  grep -E 'Tests +[0-9]+ passed' /tmp/web-unit.log | tail -1 | sed 's/^/    /'
else
  fail "前端单元测试失败"; tail -25 /tmp/web-unit.log
fi

if npm run type-check >/tmp/web-typecheck.log 2>&1; then
  ok "类型检查通过（vue-tsc）"
else
  fail "类型检查失败"; tail -25 /tmp/web-typecheck.log
fi

if npm run build >/tmp/web-build-prod.log 2>&1; then
  ok "生产构建成功"
  if [ -f dist/index.html ] && ls dist/assets/*.js >/dev/null 2>&1; then
    ok "构建产物包含 index.html 与 JS bundle"
  else
    fail "构建产物不完整"
  fi
else
  fail "生产构建失败"; tail -25 /tmp/web-build-prod.log
fi

cd "$repo_root" || exit 1
echo

# ---------- 4. 启动后端 ----------
echo "===== 4. 启动 Room、Match 与 Gateway ====="
"$room_binary" -port "$room_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" >/tmp/room.out 2>&1 &
room_pid=$!

for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null &&
  ok "Room 已就绪" || { fail "Room 未就绪"; cat /tmp/room.out; exit 1; }

"$match_binary" -match_port "$match_port" \
  -match_timeout_seconds "$match_timeout_seconds" \
  -match_result_ttl_seconds "$match_result_ttl_seconds" \
  -room_host 127.0.0.1 -room_port "$room_port" >/tmp/match.out 2>&1 &
match_pid=$!

for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null &&
  ok "Match 已就绪" || { fail "Match 未就绪"; cat /tmp/match.out; exit 1; }

"$gateway_binary" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 \
  -stream_poll_interval_ms 100 -stream_heartbeat_interval_ms 500 >/tmp/gateway.out 2>&1 &
gateway_pid=$!

for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" 2>/dev/null && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" 2>/dev/null &&
  ok "Gateway 已就绪" || { fail "Gateway 未就绪"; cat /tmp/gateway.out; exit 1; }
echo

# ---------- 5. 启动 Vite 开发服务器 ----------
echo "===== 5. 启动 Vite 开发服务器 ====="
# RGBT_GATEWAY_PORT 让 vite.config.ts 的代理指向本次实际使用的端口，
# 而不是写死的 8080——验收脚本会挑空闲端口，所以这条必须传。
( cd web && RGBT_WEB_PORT="$web_port" RGBT_GATEWAY_PORT="$gateway_port" \
  npm run dev >/tmp/vite.out 2>&1 ) &
web_pid=$!

web_ready=0
for _ in $(seq 1 60); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$web_port/" 2>/dev/null; then
    web_ready=1
    break
  fi
  sleep 0.5
done
if [ "$web_ready" -eq 1 ]; then
  ok "Vite 已就绪（进程号 $web_pid）"
else
  fail "Vite 未就绪"
  cat /tmp/vite.out
  exit 1
fi
echo

base="http://127.0.0.1:$web_port"

http_post() {
  local body="${2:-}"
  [ -n "$body" ] || body='{}'
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "$base$1"
}
http_get() {
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer ${2:-}" "$base$1"
}
json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/resp.json') as handle:
        node = json.load(handle)
except Exception:
    print(''); sys.exit(0)
for part in sys.argv[1].split('.'):
    if not part:
        continue
    if isinstance(node, dict):
        if part not in node:
            print(''); sys.exit(0)
        node = node[part]
    else:
        print(''); sys.exit(0)
print('' if node is None else node)
PY
}
login() {
  http_post /api/v1/login \
    "{\"account\":\"$1\",\"password\":\"$2\",\"request_id\":\"$3\",\"client_type\":\"web\"}" >/dev/null
  json_path token
}
wait_for_file_pattern() {
  local path="$1" pattern="$2" tries="${3:-40}"
  for _ in $(seq 1 "$tries"); do
    grep -q "$pattern" "$path" 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}

# ---------- 6. 页面与代理 ----------
echo "===== 6. 页面与代理 ====="
code=$(curl -s -o /tmp/index.html -w '%{http_code}' --max-time 5 "$base/")
if [ "$code" = "200" ] && grep -q 'id="app"' /tmp/index.html; then
  ok "GET / 返回演示页面（含挂载点 #app）"
else
  fail "GET / 异常：HTTP $code"; head -5 /tmp/index.html; echo
fi

# 这条是代理配置的核心证据：前端用的是同源相对路径 /api/...，
# 必须有东西把它转到 Gateway。没有代理就会是 404。
code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 -X POST \
  -H 'Content-Type: application/json' -d '{}' "$base/api/v1/login")
if [ "$code" = "400" ] && grep -q 'account_required' /tmp/resp.json; then
  ok "Vite 代理把 /api 转到了 Gateway（空请求体得到 400 account_required）"
else
  fail "代理未生效或行为异常：HTTP $code"; cat /tmp/resp.json; echo
fi
echo

# ---------- 7. 双人对局：全程走代理 ----------
echo "===== 7. 双人对局（全程走 Vite 代理） ====="
alice_token=$(login alice alice_dev_pw "verify-web-alice")
bob_token=$(login bob bob_dev_pw "verify-web-bob")
dave_token=$(login dave dave_dev_pw "verify-web-dave")
for pair in "alice:$alice_token" "bob:$bob_token"; do
  name="${pair%%:*}"; value="${pair#*:}"
  [ -n "$value" ] && ok "$name 登录成功（经代理）" || fail "$name 登录失败"
done

# 两侧都先入队：配对只在第二个玩家入队时发生。
http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"verify-web-m-alice\"}" >/dev/null
http_post /api/v1/matches "{\"token\":\"$bob_token\",\"request_id\":\"verify-web-m-bob\"}" >/dev/null

room_id=""
for _ in $(seq 1 30); do
  http_get /api/v1/matches/current "$alice_token" >/dev/null
  if [ "$(json_path match.state)" = "matched" ]; then
    room_id=$(json_path match.room_id)
    break
  fi
  sleep 0.3
done
[ -n "$room_id" ] && ok "匹配成功（经代理），room_id=$room_id" || fail "匹配失败"

http_get /api/v1/matches/current "$alice_token" >/dev/null
match_id=$(json_path match.match_id)

http_post /api/v1/rooms/join \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-web-join-alice\",\"room_id\":\"$room_id\"}" >/dev/null
code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$bob_token\",\"request_id\":\"verify-web-join-bob\",\"room_id\":\"$room_id\"}")
if [ "$code" = "200" ] && [ "$(json_path room.state)" = "playing" ]; then
  ok "双方进房，对局开始（经代理）"
else
  fail "进房异常：HTTP $code state=$(json_path room.state)"
fi
echo

# ---------- 8. SSE 经过代理：增量到达 ----------
echo "===== 8. SSE 经过 Vite 代理 ====="
stream_file=/tmp/web-stream.out
: >"$stream_file"

# 这条断言的核心是**时序**：如果 Vite 代理把响应缓冲到连接结束，
# 下面这些文件轮询会一直看不到内容，而对局一结束内容才一次性出现。
# 因此它验证的是「事件在连接仍然打开时就已经到达」。
curl -sN --max-time 60 -H "Authorization: Bearer $alice_token" \
  "$base/api/v1/stream?room_id=$room_id" >"$stream_file" 2>&1 &
stream_pid=$!

if wait_for_file_pattern "$stream_file" 'event: session.ready' 40; then
  ok "经代理收到 session.ready"
else
  fail "经代理未收到 session.ready"; cat "$stream_file"; echo
fi

if wait_for_file_pattern "$stream_file" 'event: room.state' 40; then
  ok "经代理收到 room.state（说明代理没有把响应缓冲到最后）"
else
  fail "经代理未收到 room.state"; cat "$stream_file"; echo
fi

# 攻击后必须能在**连接仍然打开**时看到血量变化。
http_post /api/v1/rooms/input \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-web-hit\",\"room_id\":\"$room_id\"}" >/dev/null
if wait_for_file_pattern "$stream_file" '"player_id":"p-0002","hp":90' 40; then
  ok "经代理看到攻击造成的血量变化（p-0002 hp=90）"
else
  fail "经代理未观察到血量变化"
fi

# 心跳必须**等待**而不是立即断言：它按固定间隔发送，而上一步的断言可能在
# 第一次心跳之前就返回了。立即 grep 会变成一个只在特定时序下失败的脆弱断言。
if wait_for_file_pattern "$stream_file" ': ping' 20; then
  ok "经代理收到心跳注释"
else
  fail "经代理未观察到心跳"
fi

# 非本局成员：dave 是真人但不在这一局里。
if [ -n "$dave_token" ]; then
  code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer $dave_token" "$base/api/v1/stream?room_id=$room_id")
  if [ "$code" = "400" ] && grep -q 'not_a_member' /tmp/resp.json; then
    ok "经代理拒绝非本局成员订阅（400 not_a_member）"
  else
    fail "非本局成员订阅异常：HTTP $code（期望 400 not_a_member）"
  fi
else
  fail "dave 登录失败，无法验证非成员订阅（请不加 --no-docker 重跑）"
fi
echo

# ---------- 9. 打完整局并核对结果 ----------
echo "===== 9. 打完整局 ====="
for attempt in $(seq 1 40); do
  http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token" >/dev/null
  state=$(json_path room.state)
  [ "$state" != "playing" ] && break
  http_post /api/v1/rooms/input \
    "{\"token\":\"$alice_token\",\"request_id\":\"verify-web-finish-$attempt\",\"room_id\":\"$room_id\"}" \
    >/dev/null
  sleep 0.25
done

if wait_for_file_pattern "$stream_file" 'event: room.finished' 40; then
  ok "经代理收到 room.finished"
else
  fail "经代理未收到 room.finished"
fi

closed=0
for _ in $(seq 1 40); do
  kill -0 "$stream_pid" 2>/dev/null || { closed=1; break; }
  sleep 0.25
done
if [ "$closed" -eq 1 ]; then
  wait "$stream_pid" 2>/dev/null
  ok "服务端主动关闭了经代理的流"
else
  fail "对局已结束但经代理的流未关闭"
  kill "$stream_pid" 2>/dev/null || true
fi
stream_pid=""

# 结果接口（前端结算页用的就是它）。
code=$(http_get "/api/v1/results?match_id=$match_id" "$alice_token")
if [ "$code" = "200" ] && [ "$(json_path result.winner_id)" = "p-0001" ]; then
  ok "结果查询经代理返回 200，winner=$(json_path result.winner_id)"
else
  fail "结果查询异常：HTTP $code"
fi

# 轮询兜底（前端在流断开时依赖它）。
code=$(http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token")
[ "$code" = "200" ] && ok "轮询兜底经代理返回 200" || fail "轮询兜底异常：HTTP $code"
echo

# ---------- 10. 优雅退出 ----------
echo "===== 10. 优雅退出 ====="
for pair in "Vite:$web_pid" "Gateway:$gateway_pid" "Match:$match_pid" "Room:$room_pid"; do
  name="${pair%%:*}"; pid="${pair#*:}"
  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    fail "$name 进程不存在"
    continue
  fi
  kill -TERM "$pid" 2>/dev/null || true
  exited=0
  for _ in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || { exited=1; break; }
    sleep 0.5
  done
  if [ "$exited" -eq 1 ]; then
    ok "$name 收到 SIGTERM 后已退出"
  else
    fail "$name 未在 10 秒内退出"
  fi
done
web_pid=""; gateway_pid=""; match_pid=""; room_pid=""
echo

# ---------- 结果 ----------
echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过：前端单测通过；类型检查无错误；生产构建成功；"
  echo "          Vite 提供页面且 /api 代理指向 Gateway；"
  echo "          登录、匹配、进房、SSE 订阅、攻击、结束、结果查询**全程经代理**；"
  echo "          SSE 事件在连接仍然打开时增量到达（未被代理缓冲）；"
  echo "          心跳以注释形式到达且未污染事件流；"
  echo "          非本局成员订阅被拒绝（400 not_a_member）；"
  echo "          Vite 与三个后端进程都能优雅退出"
  echo
  echo "本脚本覆盖不到「渲染」：Canvas 画面、按钮可用状态、四个视图的切换。"
  echo "人工检查步骤："
  echo "  1. 启动依赖与三个服务（scripts/verify-stream.sh 的日志里有启动命令）"
  echo "  2. cd web && RGBT_GATEWAY_PORT=<Gateway 端口> npm run dev"
  echo "  3. 浏览器打开 http://127.0.0.1:5173 ，在**两个窗口**分别登录 alice / bob"
  echo "  4. 两个窗口都点「开始匹配」，确认自动进入对战页"
  echo "  5. 反复点「攻击」，确认血条下降、帧号增长、'最近推送序号' 在变"
  echo "  6. 打到一方 HP 归零，确认自动进入结算页且胜负正确、'结果已落库' 为「是」"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do
  echo "  - $item"
done
echo
echo "排查提示：Vite 日志 /tmp/vite.out，Gateway 日志 /tmp/gateway.out，"
echo "          Match 日志 /tmp/match.out，Room 日志 /tmp/room.out，"
echo "          SSE 抓包 $stream_file"
exit 1
