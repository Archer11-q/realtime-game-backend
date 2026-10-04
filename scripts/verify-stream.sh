#!/usr/bin/env bash
#
# verify-stream.sh - TASK-009 SSE 推送验收入口
#
# 覆盖 TASK-009 的验收标准：
#   1. 订阅成功后立刻收到 session.ready（含 player_id 与 room_id）
#   2. 房间帧号前进时收到 room.state，且 sequence 单调递增
#   3. 帧号未变化时不推送（不刷屏）
#   4. 对局结束后收到 room.finished，随后连接被服务端关闭
#   5. 非本局成员不能订阅（400 not_a_member）——否则能偷看别人的血量
#   6. 缺 Token / 缺 room_id 各返回 400
#   7. 轮询接口仍然可用（兜底未被破坏）
#   8. 持有打开的 SSE 连接时，SIGTERM 仍能在 10 秒内优雅退出
#      （长连接不主动关闭的话，brpc 的 Stop 会一直等下去）
#
# 用法：
#   bash scripts/verify-stream.sh              # 完整验收（含启停依赖）
#   bash scripts/verify-stream.sh --no-docker  # 复用已启动的 Redis/MySQL
#   bash scripts/verify-stream.sh --keep       # 结束后保留进程
#
# 前置：Docker Desktop 已启动；deploy/compose/.env 存在（缺失时由本脚本生成）。
# 依赖：python3（解析 SSE 事件里的 JSON 与 sequence）。
#
# 为什么推送是 SSE 而不是 WebSocket：见 docs/adr/0004-sse-instead-of-websocket.md。
# 简单说：brpc 1.16.0 不支持 WebSocket，而官方支持 SSE。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

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
gateway_candidates=(8080 18080 18081 18082)
match_candidates=(8082 18092 18093 18094)
room_candidates=(8083 18103 18104 18105)

match_timeout_seconds=30
match_result_ttl_seconds=4

keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
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
stream_pid=""
cleanup() {
  if [ -n "$stream_pid" ] && kill -0 "$stream_pid" 2>/dev/null; then
    kill "$stream_pid" 2>/dev/null || true
  fi
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
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
if ! command -v python3 >/dev/null 2>&1; then
  echo "缺少 python3：本脚本需要它解析 SSE 事件中的 JSON" >&2
  exit 1
fi
ok "python3 可用"

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
  echo "Gateway 候选端口均被占用：${gateway_candidates[*]}" >&2; exit 1; }
ok "Gateway 选用空闲端口 $gateway_port"

match_pool=()
for candidate in "${match_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  match_pool+=("$candidate")
done
match_port="$(pick_port "${match_pool[@]}")" || { echo "Match 端口均被占用" >&2; exit 1; }
ok "Match 选用空闲端口 $match_port"

room_pool=()
for candidate in "${room_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  [ "$candidate" = "$match_port" ] && continue
  room_pool+=("$candidate")
done
room_port="$(pick_port "${room_pool[@]}")" || { echo "Room 端口均被占用" >&2; exit 1; }
ok "Room 选用空闲端口 $room_port"
echo

# ---------- 1. 启动依赖 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  if compose up -d >/tmp/stream-compose.log 2>&1; then
    ok "docker compose up 成功"
  else
    fail "docker compose up 失败"; tail -20 /tmp/stream-compose.log
  fi
  for pair in "rgbt-redis:redis" "rgbt-mysql:mysql"; do
    container="${pair%%:*}"
    healthy=0
    for _ in $(seq 1 60); do
      [ "$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)" = "healthy" ] &&
        { healthy=1; break; }
      sleep 2
    done
    [ "$healthy" -eq 1 ] && ok "$container 健康" || fail "$container 未达到 healthy"
  done

  # 对局结果要写 match_results，因此迁移必须已应用。
  # 种子数据还提供第三个启用身份 dave，用于验证「非本局成员不能订阅」。
  applied=0
  for file in migrations/*.sql; do
    [ -e "$file" ] || continue
    if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      <"$file" >/dev/null 2>&1; then
      applied=$((applied + 1))
    fi
  done
  [ "$applied" -gt 0 ] && ok "已应用 $applied 个迁移脚本（幂等）" || fail "迁移脚本未成功应用"

  stale=$(docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null | wc -l)
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  ok "已清理上次运行残留的 $stale 个 dev:* Key（会话与队列快照）"
  echo
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
  echo "注意：--no-docker 不会重新应用迁移，因此 dave 可能不存在。"
  echo
fi

# ---------- 2. 构建 ----------
echo "===== 2. 构建 ====="
cmake --preset "$preset" >/tmp/stream-cmake.log 2>&1 && ok "配置成功" ||
  { fail "配置失败"; tail -20 /tmp/stream-cmake.log; }
if cmake --build --preset "$preset" >/tmp/stream-build.log 2>&1; then
  ok "构建成功"
else
  fail "构建失败"; tail -30 /tmp/stream-build.log; exit 1
fi
for bin in "$gateway_binary" "$match_binary" "$room_binary"; do
  [ -x "$bin" ] && ok "可执行文件存在: $bin" || { fail "缺少可执行文件: $bin"; exit 1; }
done
echo

# ---------- 3. 启动服务 ----------
echo "===== 3. 启动 Room、Match 与 Gateway ====="
"$room_binary" -port "$room_port" -env_prefix dev \
  -drain_timeout_ms 1000 \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" >/tmp/room.out 2>&1 &
room_pid=$!

for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null &&
  ok "Room 已就绪（进程号 $room_pid）" || { fail "Room 未就绪"; cat /tmp/room.out; exit 1; }

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
  ok "Match 已就绪（进程号 $match_pid）" || { fail "Match 未就绪"; cat /tmp/match.out; exit 1; }

# 推送间隔设成 100 ms（与 Room 帧长对齐），心跳 500 ms：
# 心跳默认 15 秒，本脚本没有耐心等那么久，因此缩短它是为了**能在脚本内观察到**，
# 不是为了改产品默认值。
"$gateway_binary" -port "$gateway_port" -env_prefix dev \
  -drain_timeout_ms 1000 \
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
  ok "Gateway 已就绪（进程号 $gateway_pid）" || { fail "Gateway 未就绪"; cat /tmp/gateway.out; exit 1; }
echo

# ---------- HTTP 辅助 ----------
http_post() {
  local body="${2:-}"
  [ -n "$body" ] || body='{}'
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}
http_get() {
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer ${2:-}" "http://127.0.0.1:$gateway_port$1"
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

# 从 SSE 抓包文件里取某个事件的 sequence 列表。
stream_sequences() {
  python3 - "$1" "$2" <<'PY'
import re, sys
path, event = sys.argv[1], sys.argv[2]
try:
    text = open(path, encoding='utf-8', errors='replace').read()
except Exception:
    sys.exit(0)
# SSE 事件以空行分隔，每个事件由若干 "event:" / "data:" 行组成。
out = []
for block in text.split('\n\n'):
    name = None
    data = []
    for line in block.split('\n'):
        if line.startswith('event: '):
            name = line[7:].strip()
        elif line.startswith('data: '):
            data.append(line[6:])
    if name != event or not data:
        continue
    m = re.search(r'"sequence":(\d+)', '\n'.join(data))
    if m:
        out.append(m.group(1))
print(' '.join(out))
PY
}

wait_for_file_pattern() {
  local path="$1" pattern="$2" tries="${3:-40}"
  for _ in $(seq 1 "$tries"); do
    grep -q "$pattern" "$path" 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}

# ---------- 4. 登录与进房 ----------
echo "===== 4. 登录并进入一局 ====="
alice_token=$(login alice alice_dev_pw "verify-stream-alice")
bob_token=$(login bob bob_dev_pw "verify-stream-bob")
dave_token=$(login dave dave_dev_pw "verify-stream-dave")
for pair in "alice:$alice_token" "bob:$bob_token"; do
  name="${pair%%:*}"; value="${pair#*:}"
  [ -n "$value" ] && ok "$name 登录成功" || fail "$name 登录失败"
done

enqueue_match() {
  http_post /api/v1/matches "{\"token\":\"$1\",\"request_id\":\"$2\"}" >/dev/null
}
wait_matched() {
  local token="$1"
  for _ in $(seq 1 30); do
    http_get /api/v1/matches/current "$token" >/dev/null
    if [ "$(json_path match.state)" = "matched" ]; then
      json_path match.room_id
      return 0
    fi
    sleep 0.3
  done
  echo ''
  return 1
}

# 等待两个玩家的匹配状态都回到 idle（上一个对局的结果保留期已过）。
# 第 9 节要再开一局，必须先让这两个人能被重新匹配。
wait_until_idle() {
  local first state
  for _ in $(seq 1 30); do
    first=1
    for token in "$@"; do
      http_get /api/v1/matches/current "$token" >/dev/null
      state=$(json_path match.state)
      [ "$state" = "idle" ] || first=0
    done
    [ "$first" -eq 1 ] && return 0
    sleep 0.5
  done
  return 1
}

# 开一局并让双方进房，成功后把 room_id 写到 stdout。
open_playing_room() {
  enqueue_match "$alice_token" "verify-stream-open-alice-$RANDOM"
  enqueue_match "$bob_token" "verify-stream-open-bob-$RANDOM"
  local rid
  rid=$(wait_matched "$alice_token")
  if [ -z "$rid" ]; then
    echo ''
    return 1
  fi
  http_post /api/v1/rooms/join \
    "{\"token\":\"$alice_token\",\"request_id\":\"verify-stream-open-ja-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_post /api/v1/rooms/join \
    "{\"token\":\"$bob_token\",\"request_id\":\"verify-stream-open-jb-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_get "/api/v1/rooms/state?room_id=$rid" "$alice_token" >/dev/null
  if [ "$(json_path room.state)" != "playing" ]; then
    echo ''
    return 1
  fi
  echo "$rid"
}

# 两侧都先入队：配对只在第二个玩家入队时发生。
enqueue_match "$alice_token" "verify-stream-m-alice"
enqueue_match "$bob_token" "verify-stream-m-bob"
room_id=$(wait_matched "$alice_token")
[ -n "$room_id" ] && ok "匹配成功，room_id=$room_id" || fail "匹配失败"

http_get "/api/v1/matches/current" "$alice_token" >/dev/null
alice_match_id=$(json_path match.match_id)

http_post /api/v1/rooms/join \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-stream-join-alice\",\"room_id\":\"$room_id\"}" >/dev/null
code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$bob_token\",\"request_id\":\"verify-stream-join-bob\",\"room_id\":\"$room_id\"}")
if [ "$code" = "200" ] && [ "$(json_path room.state)" = "playing" ]; then
  ok "双方进房，对局开始"
else
  fail "进房异常：HTTP $code state=$(json_path room.state)"
fi
echo

# ---------- 5. 订阅前的失败路径 ----------
echo "===== 5. 订阅的失败路径 ====="
# 无 Token。
code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_id")
if [ "$code" = "400" ] && grep -q 'token_required' /tmp/resp.json; then
  ok "缺 Token 返回 400 token_required"
else
  fail "缺 Token 响应异常：HTTP $code"; cat /tmp/resp.json; echo
fi

# 缺 room_id。
code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $alice_token" "http://127.0.0.1:$gateway_port/api/v1/stream")
if [ "$code" = "400" ] && grep -q 'room_id_required' /tmp/resp.json; then
  ok "缺 room_id 返回 400 room_id_required"
else
  fail "缺 room_id 响应异常：HTTP $code"; cat /tmp/resp.json; echo
fi

# 房间不存在。
code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $alice_token" \
  "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=r-does-not-exist")
if [ "$code" = "404" ]; then
  ok "不存在的房间返回 404"
else
  fail "不存在的房间响应异常：HTTP $code"; cat /tmp/resp.json; echo
fi

# 非本局成员：dave 是真人，但不在这一局里。
if [ -n "$dave_token" ]; then
  code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer $dave_token" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_id")
  if [ "$code" = "400" ] && grep -q 'not_a_member' /tmp/resp.json; then
    ok "非本局成员不能订阅（400 not_a_member，否则能偷看别人血量）"
  else
    fail "非本局成员订阅异常：HTTP $code（期望 400 not_a_member）"; cat /tmp/resp.json; echo
  fi
else
  fail "dave 登录失败，无法验证「非本局成员不能订阅」（请不加 --no-docker 重跑）"
fi
echo

# ---------- 6. 订阅并接收推送 ----------
echo "===== 6. 订阅并接收推送 ====="
stream_file=/tmp/stream.out
: >"$stream_file"

# -N 关闭 curl 的输出缓冲，否则事件会被攒在缓冲区里看不到。
curl -sN --max-time 60 -H "Authorization: Bearer $alice_token" \
  "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_id" \
  >"$stream_file" 2>&1 &
stream_pid=$!

if wait_for_file_pattern "$stream_file" 'event: session.ready' 40; then
  ok "订阅后收到 session.ready"
  grep -q '"player_id":"p-0001"' "$stream_file" &&
    ok "session.ready 带上了订阅者身份" || fail "session.ready 缺少 player_id"
else
  fail "20 秒内未收到 session.ready"; cat "$stream_file"; echo
fi

if wait_for_file_pattern "$stream_file" 'event: room.state' 40; then
  ok "收到 room.state 推送"
else
  fail "未收到 room.state 推送"; cat "$stream_file"; echo
fi

# 打一次攻击，让帧号与血量都发生变化，然后确认推送里有新的帧。
http_post /api/v1/rooms/input \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-stream-hit\",\"room_id\":\"$room_id\"}" >/dev/null
sleep 1.0

states=$(stream_sequences "$stream_file" room.state)
state_count=$(echo "$states" | wc -w)
if [ "$state_count" -ge 2 ]; then
  ok "帧推进产生了多次 room.state 推送（共 $state_count 次）"
else
  fail "room.state 推送次数过少（$state_count 次）"
fi

# sequence 必须单调递增：客户端据此发现丢事件。
if [ "$state_count" -ge 2 ]; then
  monotonic=$(python3 -c "
import sys
seq=[int(x) for x in '$states'.split()]
print('yes' if all(b>a for a,b in zip(seq,seq[1:])) else 'no')
")
  [ "$monotonic" = "yes" ] && ok "sequence 单调递增：$states" ||
    fail "sequence 非单调递增：$states"
fi

# 攻击生效：推送里出现过对手掉血的状态。
if grep -q '"player_id":"p-0002","hp":90' "$stream_file"; then
  ok "推送中能看到攻击导致的血量变化（p-0002 hp=90）"
else
  fail "推送中未观察到血量变化"
fi

# 心跳：以注释行发送，不应被当成事件。
# 用等待而不是立即断言：心跳按固定间隔发送，前面的断言可能在第一次心跳之前返回，
# 立即 grep 会变成一个只在特定时序下失败的脆弱断言。
if wait_for_file_pattern "$stream_file" ': ping' 20; then
  ok "空闲期间收到心跳注释（保持长连接）"
else
  fail "未观察到心跳"
fi

# 关键：心跳是注释，不能混进事件里。
if grep -q 'event: ping' "$stream_file"; then
  fail "心跳被错误地当成事件发送"
else
  ok "心跳以注释形式发送，未污染事件流"
fi
echo

# ---------- 7. 对局结束：推送 finished 并关闭连接 ----------
echo "===== 7. 对局结束 ====="
# 一直攻击直到房间离开 playing。帧长 100 ms，每次攻击间隔大于一帧。
for attempt in $(seq 1 40); do
  http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token" >/dev/null
  state=$(json_path room.state)
  [ "$state" != "playing" ] && break
  http_post /api/v1/rooms/input \
    "{\"token\":\"$alice_token\",\"request_id\":\"verify-stream-finish-$attempt\",\"room_id\":\"$room_id\"}" \
    >/dev/null
  sleep 0.25
done

if wait_for_file_pattern "$stream_file" 'event: room.finished' 40; then
  ok "对局结束后收到 room.finished"
  grep -q '"winner_id":"p-0001"' "$stream_file" &&
    ok "room.finished 带上了胜者" || fail "room.finished 缺少 winner_id"
else
  fail "未收到 room.finished"; tail -5 "$stream_file"; echo
fi

# 服务端应当主动关闭这条流：curl 自己退出，而不是等 --max-time。
closed=0
for _ in $(seq 1 40); do
  if ! kill -0 "$stream_pid" 2>/dev/null; then
    closed=1
    break
  fi
  sleep 0.25
done
if [ "$closed" -eq 1 ]; then
  wait "$stream_pid" 2>/dev/null
  ok "服务端主动关闭了流（curl 自行退出）"
else
  fail "对局已结束但流未关闭——长连接会拖住优雅退出"
  kill "$stream_pid" 2>/dev/null || true
fi
stream_pid=""
echo

# ---------- 8. 轮询兜底仍然可用 ----------
echo "===== 8. 轮询接口未被破坏（兜底） ====="
code=$(http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token")
if [ "$code" = "200" ]; then
  ok "GET /api/v1/rooms/state 仍返回 200"
else
  fail "轮询接口异常：HTTP $code"
fi
if [ -n "$alice_match_id" ]; then
  code=$(http_get "/api/v1/results?match_id=$alice_match_id" "$alice_token")
  if [ "$code" = "200" ]; then
    ok "GET /api/v1/results 仍返回 200（winner=$(json_path result.winner_id)）"
  else
    fail "结果接口异常：HTTP $code"
  fi
fi
echo

# ---------- 9. 持有长连接时的优雅退出 ----------
echo "===== 9. 持有 SSE 长连接时 SIGTERM 优雅退出 ====="
# 这条长连接必须建立在**仍在进行中**的房间上。上一局已经结束，订阅它会立刻
# 收到 room.finished 并被服务端关闭——那样就测不出「长连接是否拖住了退出」。
# 因此先等双方回到 idle，再开一局新的。
if wait_until_idle "$alice_token" "$bob_token"; then
  live_room=$(open_playing_room)
  if [ -n "$live_room" ]; then
    ok "已开出一局仍在进行中的房间（$live_room）"
  else
    fail "无法开出仍在进行中的房间"
  fi
else
  fail "双方未能回到 idle，无法验证长连接下的优雅退出"
  live_room=""
fi

if [ -n "$live_room" ]; then
  : >"$stream_file"
  curl -sN --max-time 30 -H "Authorization: Bearer $alice_token" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$live_room" \
    >"$stream_file" 2>&1 &
  stream_pid=$!
  sleep 1.0

  if kill -0 "$stream_pid" 2>/dev/null; then
    ok "已建立一条保持打开的长连接"
  else
    fail "长连接未能保持打开"; cat "$stream_file"; echo
  fi
fi

if [ -n "$gateway_pid" ] && kill -0 "$gateway_pid" 2>/dev/null; then
  kill -TERM "$gateway_pid" 2>/dev/null || true
  exited=0
  for _ in $(seq 1 20); do
    if ! kill -0 "$gateway_pid" 2>/dev/null; then
      exited=1
      break
    fi
    sleep 0.5
  done
  if [ "$exited" -eq 1 ]; then
    wait "$gateway_pid" 2>/dev/null
    rc=$?
    [ "$rc" -eq 0 ] && ok "Gateway 持有长连接时仍在 10 秒内以退出码 0 退出" ||
      fail "Gateway 退出码 $rc（期望 0）"
  else
    fail "Gateway 未在 10 秒内退出——长连接很可能拖住了 server.Stop()"
  fi
  gateway_pid=""
fi

kill "$stream_pid" 2>/dev/null || true
stream_pid=""
echo

# ---------- 10. 其余进程优雅退出 ----------
echo "===== 10. 其余进程优雅退出 ====="
for pair in "Match:$match_pid" "Room:$room_pid"; do
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
    wait "$pid" 2>/dev/null
    rc=$?
    [ "$rc" -eq 0 ] && ok "$name 收到 SIGTERM 后退出码 0" || fail "$name 退出码 $rc（期望 0）"
  else
    fail "$name 未在 10 秒内退出"
  fi
done
match_pid=""
room_pid=""
echo

# ---------- 结果 ----------
echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过：订阅后收到 session.ready；帧推进产生 room.state 且 sequence 单调递增；"
  echo "          攻击造成的血量变化可见；空闲时收到心跳注释（未污染事件流）；"
  echo "          对局结束后收到 room.finished 且服务端主动关闭连接；"
  echo "          非本局成员被拒绝（400 not_a_member）；缺 Token / 缺 room_id 各返回 400；"
  echo "          轮询接口仍可用；持有长连接时 Gateway 仍能优雅退出"
  echo
  echo "未在端到端覆盖：「没有订阅者时不产生 Gateway→Room 轮询」难以从外部观测，"
  echo "          由单元测试断言："
  echo "          StreamHubTest.NoSubscribersMeansNoRoomPolling"
  echo "          StreamHubTest.MultipleSubscribersShareOnePoll"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do
  echo "  - $item"
done
echo
echo "排查提示：Gateway 日志 /tmp/gateway.out，Match 日志 /tmp/match.out，"
echo "          Room 日志 /tmp/room.out，SSE 抓包 $stream_file"
exit 1
