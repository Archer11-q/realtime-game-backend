#!/usr/bin/env bash
#
# verify-room.sh - TASK-008 房间与对局结果验收入口
#
# 覆盖 TASK-008 的验收标准：
#   1. 两个账号登录；双方匹配后拿到相同的 match_id 与 room_id
#   2. 双方加入房间：第一人 waiting，第二人 playing
#   3. 提交攻击后对手 HP 按规则下降
#   4. 一方 HP 归零 -> 对局结束，winner_id 是攻击方
#   5. 结果落库后可查询，且 match_results 中该 match_id 只有一行（幂等）
#   6. 对局结束后再提交输入 -> 409 room_already_finished
#   7. Room 停机 -> 查询返回 503；且匹配不会产生半成品配对（玩家留在队列）
#   8. MySQL 停机后结束对局 -> 结果查询返回 503 result_pending（不是 404，也不是假成功）
#   9. 三个进程收到 SIGTERM 后都能优雅退出
#
# 用法：
#   bash scripts/verify-room.sh                # 完整验收（含启停依赖）
#   bash scripts/verify-room.sh --no-docker    # 复用已启动的 Redis/MySQL
#   bash scripts/verify-room.sh --keep         # 结束后保留进程
#
# 前置：Docker Desktop 已启动；deploy/compose/.env 存在（缺失时由本脚本生成）。
# 依赖：python3（用于解析嵌套 JSON——players 是数组，sed 无法可靠处理）。
#
# 为什么需要 MySQL：登录要读 players 表（ADR-0002），对局结果要写 match_results。
#
# 为了让「等待加入超时」等路径在几秒内可验证，本脚本给服务传了**测试配置**而不是
# 默认值：Match 的结果保留时长设为 4 秒（默认 120 秒），Room 的推进间隔保持 50 ms。
# 默认值见 src/match/match_main.cpp 与 src/room/room_main.cpp。

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

# 端口。候选列表都以文档中的约定值为首选；被占用时依次尝试其它端口。
# 必须显式预检：端口冲突时进程启动失败，而请求会打到占用端口的那个服务上，
# 表现为难以定位的 404，而不是「端口冲突」这种可定位的错误。
gateway_port=""
match_port=""
room_port=""
gateway_candidates=(8080 18080 18081 18082)
match_candidates=(8082 18092 18093 18094)
room_candidates=(8083 18103 18104 18105)

# 测试配置：让匹配状态的 TTL 足够短，使同两个账号可以在一次验收里跑三局。
match_timeout_seconds=30
match_result_ttl_seconds=4

keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
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
cleanup() {
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
  echo "缺少 python3：本脚本需要它解析嵌套 JSON（players 是数组）" >&2
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

# 必须先加载 .env 再读取 MYSQL_*，否则未绑定变量会直接让脚本以退出码 1 中止。
set -a
. "./$env_file"
set +a
ok "配置已加载（库: ${MYSQL_DATABASE} 账号: ${MYSQL_USER}）"

port_in_use() {
  ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"
}
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
  echo "Gateway 候选端口均被占用：${gateway_candidates[*]}" >&2
  exit 1
}
ok "Gateway 选用空闲端口 $gateway_port"

# 三个端口必须互不相同，因此每选一个就从候选里剔除已占用的。
match_pool=()
for candidate in "${match_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  match_pool+=("$candidate")
done
match_port="$(pick_port "${match_pool[@]}")" || {
  echo "Match 候选端口均被占用" >&2
  exit 1
}
ok "Match 选用空闲端口 $match_port"

room_pool=()
for candidate in "${room_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  [ "$candidate" = "$match_port" ] && continue
  room_pool+=("$candidate")
done
room_port="$(pick_port "${room_pool[@]}")" || {
  echo "Room 候选端口均被占用" >&2
  exit 1
}
ok "Room 选用空闲端口 $room_port"
echo

# ---------- 1. 启动依赖 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  if compose up -d >/tmp/room-compose.log 2>&1; then
    ok "docker compose up 成功"
  else
    fail "docker compose up 失败"
    tail -20 /tmp/room-compose.log
  fi

  for pair in "rgbt-redis:redis" "rgbt-mysql:mysql"; do
    container="${pair%%:*}"
    service="${pair#*:}"
    healthy=0
    for _ in $(seq 1 60); do
      status=$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)
      if [ "$status" = "healthy" ]; then
        healthy=1
        break
      fi
      sleep 2
    done
    if [ "$healthy" -eq 1 ]; then
      ok "$container 健康"
    else
      fail "$container 未达到 healthy（service=$service）"
    fi
  done

  # 对局结果要写 match_results，因此迁移必须已应用。
  applied=0
  for file in migrations/*.sql; do
    [ -e "$file" ] || continue
    if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      <"$file" >/dev/null 2>&1; then
      applied=$((applied + 1))
    fi
  done
  if [ "$applied" -gt 0 ]; then
    ok "已应用 $applied 个迁移脚本（幂等，可重复执行）"
  else
    fail "迁移脚本未成功应用"
  fi

  # 清理上一次运行留下的会话与幂等映射，保证结果可复现。
  stale=$(docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null | wc -l)
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  ok "已清理上次运行残留的 $stale 个 dev:gateway:* Key"
  echo
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
  echo
fi

# ---------- 2. 构建 ----------
echo "===== 2. 构建 ====="
if cmake --preset "$preset" >/tmp/room-cmake.log 2>&1; then
  ok "配置成功"
else
  fail "配置失败"
  tail -20 /tmp/room-cmake.log
fi
if cmake --build --preset "$preset" >/tmp/room-build.log 2>&1; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 /tmp/room-build.log
  exit 1
fi
for bin in "$gateway_binary" "$match_binary" "$room_binary"; do
  if [ -x "$bin" ]; then
    ok "可执行文件存在: $bin"
  else
    fail "缺少可执行文件: $bin"
    exit 1
  fi
done
echo

# ---------- 3. 单元测试 ----------
echo "===== 3. 单元测试 ====="
if ctest --test-dir "build/$preset" --output-on-failure >/tmp/room-ctest.log 2>&1; then
  ok "单元测试全部通过"
  grep -E 'tests passed|tests failed' /tmp/room-ctest.log | tail -2
else
  fail "单元测试失败"
  tail -25 /tmp/room-ctest.log
fi
echo

# ---------- 4. 启动服务 ----------
echo "===== 4. 启动 Room、Match 与 Gateway ====="

start_room() {
  "$room_binary" -port "$room_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" >/tmp/room.out 2>&1 &
  room_pid=$!
  local ready=0
  for _ in $(seq 1 30); do
    if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.5
  done
  [ "$ready" -eq 1 ]
}

if start_room; then
  ok "Room 已就绪（进程号 $room_pid）"
else
  fail "Room 未就绪"
  cat /tmp/room.out
  exit 1
fi

"$match_binary" -match_port "$match_port" \
  -match_timeout_seconds "$match_timeout_seconds" \
  -match_result_ttl_seconds "$match_result_ttl_seconds" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -room_timeout_ms 500 >/tmp/match.out 2>&1 &
match_pid=$!
echo "Match 进程号: $match_pid"

match_ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null; then
    match_ready=1
    break
  fi
  sleep 0.5
done
if [ "$match_ready" -eq 1 ]; then
  ok "Match 已就绪"
else
  fail "Match 未就绪"
  cat /tmp/match.out
  exit 1
fi

"$gateway_binary" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 >/tmp/gateway.out 2>&1 &
gateway_pid=$!
echo "Gateway 进程号: $gateway_pid"

gateway_ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" 2>/dev/null; then
    gateway_ready=1
    break
  fi
  sleep 0.5
done
if [ "$gateway_ready" -eq 1 ]; then
  ok "Gateway 已就绪"
else
  fail "Gateway 未就绪"
  cat /tmp/gateway.out
  exit 1
fi
echo

# ---------- HTTP 与 JSON 辅助 ----------
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

# 取一个嵌套字段。用法：json_path room.state / json_path result.winner_id
#
# 为什么用 python3 而不是 sed：players 是数组，每项是一个对象，
# sed 的正则在「多个同名字段」上不可靠，会把第一个玩家的 hp 当成第二个的。
json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/resp.json') as handle:
        node = json.load(handle)
except Exception:
    print('')
    sys.exit(0)
for part in sys.argv[1].split('.'):
    if not part:
        continue
    if isinstance(node, list):
        try:
            node = node[int(part)]
        except Exception:
            print(''); sys.exit(0)
    elif isinstance(node, dict):
        if part not in node:
            print(''); sys.exit(0)
        node = node[part]
    else:
        print(''); sys.exit(0)
print('' if node is None else node)
PY
}

# 取指定玩家在房间快照里的血量。用法：room_hp_of p-0002
room_hp_of() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/resp.json') as handle:
        data = json.load(handle)
except Exception:
    print('')
    sys.exit(0)
room = data.get('room') or {}
for player in room.get('players') or []:
    if player.get('player_id') == sys.argv[1]:
        print(player.get('hp', ''))
        break
else:
    print('')
PY
}

login() {
  http_post /api/v1/login \
    "{\"account\":\"$1\",\"password\":\"$2\",\"request_id\":\"$3\",\"client_type\":\"web\"}" \
    >/dev/null
  json_path token
}

match_state() { http_get /api/v1/matches/current "$1" >/dev/null; json_path match.state; }
match_room_id() { http_get /api/v1/matches/current "$1" >/dev/null; json_path match.room_id; }
match_match_id() { http_get /api/v1/matches/current "$1" >/dev/null; json_path match.match_id; }

# 进入匹配（幂等，重复调用返回当前状态）。
enqueue_match() {
  http_post /api/v1/matches "{\"token\":\"$1\",\"request_id\":\"$2\"}" >/dev/null
}

# 轮询直到该玩家变为 matched。成功时输出 match_id。
#
# 注意：**必须先把两个玩家都入队再轮询**。配对只在第二个玩家入队时发生，
# 先等第一个玩家变成 matched 会一直等不到——那不是系统的问题，是调用顺序错了。
wait_matched() {
  local token="$1" tries="${2:-30}"
  local state
  for _ in $(seq 1 "$tries"); do
    state="$(match_state "$token")"
    if [ "$state" = "matched" ]; then
      match_match_id "$token"
      return 0
    fi
    sleep 0.3
  done
  echo ''
  return 1
}

# 等待某个 match 的比赛结果可查询。成功返回 0。
wait_for_result() {
  local token="$1" match_id="$2" tries="${3:-20}"
  local code
  for _ in $(seq 1 "$tries"); do
    code=$(http_get "/api/v1/results?match_id=$match_id" "$token")
    if [ "$code" = "200" ]; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

# 等待两个玩家的匹配状态回到 idle（结果 TTL 过期）。
wait_until_idle() {
  local first="$1" second="$2" tries="${3:-20}"
  for _ in $(seq 1 "$tries"); do
    if [ "$(match_state "$first")" = "idle" ] && [ "$(match_state "$second")" = "idle" ]; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

# ---------- 5. 登录 ----------
echo "===== 5. 登录测试账号 ====="
# alice 与 bob 参与正常对局；carol 在种子数据里是 disabled，用于覆盖失败路径；
# dave 是第三个启用身份，在第 7 节用于验证「非本局成员无法加入」。
alice_token=$(login alice alice_dev_pw "verify-room-alice")
bob_token=$(login bob bob_dev_pw "verify-room-bob")
for pair in "alice:$alice_token" "bob:$bob_token"; do
  name="${pair%%:*}"
  value="${pair#*:}"
  if [ -n "$value" ]; then
    ok "$name 登录成功"
  else
    fail "$name 登录失败"
  fi
done
echo

# ---------- 6. 第一局：完整对局 ----------
echo "===== 6. 匹配并完成一局 ====="
# 两侧都要先入队：配对只在第二个玩家入队的那一刻发生。
enqueue_match "$alice_token" "verify-room-m1-alice"
enqueue_match "$bob_token" "verify-room-m1-bob"
alice_match=$(wait_matched "$alice_token") || fail "alice 未能匹配成功"
bob_match=$(wait_matched "$bob_token") || fail "bob 未能匹配成功"

if [ -n "$alice_match" ] && [ "$alice_match" = "$bob_match" ]; then
  ok "双方 match_id 一致（$alice_match）"
else
  fail "match_id 不一致：alice=[$alice_match] bob=[$bob_match]"
fi

alice_room=$(match_room_id "$alice_token")
bob_room=$(match_room_id "$bob_token")
if [ -n "$alice_room" ] && [ "$alice_room" = "$bob_room" ]; then
  ok "双方 room_id 一致（$alice_room）"
else
  fail "room_id 不一致：alice=[$alice_room] bob=[$bob_room]"
fi

# Match 创建房间时以 match_id 为幂等键，因此房间的 match_id 必须与匹配结果一致。
http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token" >/dev/null
if [ "$(json_path room.match_id)" = "$alice_match" ]; then
  ok "房间的 match_id 与匹配结果一致（幂等键生效）"
else
  fail "房间 match_id 不匹配：$(json_path room.match_id) != $alice_match"
fi
echo

# ---------- 7. 加入房间 ----------
echo "===== 7. 加入房间 ====="
code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-join-alice\",\"room_id\":\"$alice_room\"}")
state=$(json_path room.state)
if [ "$code" = "200" ] && [ "$state" = "waiting" ]; then
  ok "alice 进房后 state=waiting"
else
  fail "alice 进房异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$bob_token\",\"request_id\":\"verify-room-join-bob\",\"room_id\":\"$alice_room\"}")
state=$(json_path room.state)
if [ "$code" = "200" ] && [ "$state" = "playing" ]; then
  ok "bob 进房后 state=playing（双方到齐，对局开始）"
else
  fail "bob 进房异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

# 重复加入必须幂等。
code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-join-alice-again\",\"room_id\":\"$alice_room\"}")
if [ "$code" = "200" ] && [ "$(json_path room.state)" = "playing" ]; then
  ok "重复加入幂等（仍为 200 playing）"
else
  fail "重复加入异常：HTTP $code"
fi

# 第三个启用身份（dave）是**真人但不是这一局的人**：房间还有空位也进不去。
# 这条不变量此前因为「种子数据只有两个可用身份」而无法在端到端覆盖，
# 2026-10-02 补上 dave/p-0004 后改为真正验证。
dave_token=$(login dave dave_dev_pw "verify-room-dave")
code=$(http_post /api/v1/rooms/join \
  "{\"token\":\"$dave_token\",\"request_id\":\"verify-room-join-dave\",\"room_id\":\"$alice_room\"}")
if [ "$code" = "400" ] && grep -q 'not_a_member' /tmp/resp.json; then
  ok "非本局成员无法加入（dave 是真人但不在这一局，400 not_a_member）"
else
  fail "非本局成员加入异常：HTTP $code（期望 400 not_a_member）"
  cat /tmp/resp.json; echo
fi

# 缺少 Token 这一类输入错误。
code=$(http_post /api/v1/rooms/join "{\"request_id\":\"verify-room-join-notoken\",\"room_id\":\"$alice_room\"}")
if [ "$code" = "400" ] && grep -q 'token_required' /tmp/resp.json; then
  ok "缺少 Token 时拒绝加入（400 token_required）"
else
  fail "缺少 Token 的加入请求响应异常：HTTP $code（期望 400）"
  cat /tmp/resp.json; echo
fi
echo

# ---------- 8. 攻击与血量 ----------
echo "===== 8. 提交攻击，验证血量下降 ====="
code=$(http_post /api/v1/rooms/input \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-hit-1\",\"room_id\":\"$alice_room\"}")
if [ "$code" = "200" ]; then
  ok "alice 提交攻击被接受"
else
  fail "alice 提交攻击失败：HTTP $code"
  cat /tmp/resp.json; echo
fi

# 等至少一帧结算。只提交过一次攻击，因此无论过了多少帧，对手都只掉一次血。
sleep 0.4
http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token" >/dev/null
bob_hp=$(room_hp_of p-0002)
alice_hp=$(room_hp_of p-0001)
if [ "$bob_hp" = "90" ] && [ "$alice_hp" = "100" ]; then
  ok "攻击生效：bob HP 100 -> 90，alice 未受伤"
else
  fail "血量不符合预期：alice=$alice_hp bob=$bob_hp（期望 100 / 90）"
fi

# 再确认一次：不提交新输入，血量不会继续下降（输入不跨帧累积）。
sleep 0.4
http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token" >/dev/null
if [ "$(room_hp_of p-0002)" = "90" ]; then
  ok "不提交输入则血量不再下降（输入不跨帧累积）"
else
  fail "血量在无输入时继续变化：bob HP=$(room_hp_of p-0002)"
fi
echo

# ---------- 9. 打到结束 ----------
echo "===== 9. 打到一方 HP 归零 ====="
# 循环提交攻击，每次间隔大于一帧（100 ms），直到房间离开 playing。
# 用循环而不是固定 9 次：若某两次攻击恰好落在同一帧，其中一次会被合并，
# 固定次数就会打得不够。这里以「状态是否已结束」为退出条件，不依赖精确次数。
finished=0
for attempt in $(seq 1 40); do
  http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token" >/dev/null
  state=$(json_path room.state)
  if [ "$state" != "playing" ]; then
    finished=1
    break
  fi
  http_post /api/v1/rooms/input \
    "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-hit-$attempt\",\"room_id\":\"$alice_room\"}" \
    >/dev/null
  sleep 0.25
done

if [ "$finished" -eq 1 ]; then
  ok "对局已结束（state=$state，共提交 $attempt 次攻击）"
else
  fail "对局在 40 次攻击内未结束"
fi

http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token" >/dev/null
winner=$(json_path room.winner_id)
reason=$(json_path room.finish_reason)
if [ "$winner" = "p-0001" ]; then
  ok "winner_id=p-0001（持续攻击方获胜），finish_reason=$reason"
else
  fail "winner_id 不符合预期：[$winner]，finish_reason=$reason"
fi

# 结束后再提交输入必须被拒绝。
code=$(http_post /api/v1/rooms/input \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-hit-after\",\"room_id\":\"$alice_room\"}")
if [ "$code" = "409" ] && grep -q 'room_already_finished' /tmp/resp.json; then
  ok "对局结束后提交输入返回 409 room_already_finished"
else
  fail "结束后提交输入异常：HTTP $code（期望 409）"
  cat /tmp/resp.json; echo
fi
echo

# ---------- 10. 结果落库与幂等 ----------
echo "===== 10. 对局结果落库 ====="
if wait_for_result "$alice_token" "$alice_match" 20; then
  ok "对局结果可查询（HTTP 200）"
  result_winner=$(json_path result.winner_id)
  result_count=$(json_path result.player_count)
  if [ "$result_winner" = "p-0001" ] && [ "$result_count" = "2" ]; then
    ok "结果内容正确：winner=$result_winner player_count=$result_count"
  else
    fail "结果内容异常：winner=[$result_winner] player_count=[$result_count]"
    cat /tmp/resp.json; echo
  fi
else
  fail "对局结果在 10 秒内不可查询"
  cat /tmp/resp.json; echo
fi

# 幂等：match_results 里这个 match_id 只能有一行。
row_count=$(docker exec -i rgbt-mysql mysql -N -B \
  -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
  -e "SELECT COUNT(*) FROM match_results WHERE match_id='$alice_match';" 2>/dev/null | tr -d '\r')
if [ "$row_count" = "1" ]; then
  ok "match_results 中该 match_id 只有 1 行（幂等写入）"
else
  fail "match_results 行数异常：[$row_count]（期望 1）"
fi

# 再查几次，确认不会因为重复查询产生第二行。
for _ in 1 2 3; do
  http_get "/api/v1/results?match_id=$alice_match" "$alice_token" >/dev/null
done
row_count=$(docker exec -i rgbt-mysql mysql -N -B \
  -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
  -e "SELECT COUNT(*) FROM match_results WHERE match_id='$alice_match';" 2>/dev/null | tr -d '\r')
if [ "$row_count" = "1" ]; then
  ok "重复查询后行数仍为 1"
else
  fail "重复查询后行数变为 [$row_count]"
fi

# 不存在的 match_id 必须是 404，而不是 503——「没有结果」与「存储挂了」要分开。
code=$(http_get "/api/v1/results?match_id=m-does-not-exist" "$alice_token")
if [ "$code" = "404" ] && grep -q 'result_not_found' /tmp/resp.json; then
  ok "不存在的 match_id 返回 404 result_not_found"
else
  fail "不存在的 match_id 响应异常：HTTP $code（期望 404）"
  cat /tmp/resp.json; echo
fi
echo

# ---------- 11. Room 停机 ----------
echo "===== 11. Room 停机：查询 503 且不产生半成品配对 ====="
if wait_until_idle "$alice_token" "$bob_token" 20; then
  ok "双方匹配状态已回到 idle（结果保留期已过）"
else
  fail "匹配状态未在 10 秒内回到 idle"
fi

kill "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
ok "Room 进程已停止"

code=$(http_get "/api/v1/rooms/state?room_id=$alice_room" "$alice_token")
if [ "$code" = "503" ] && grep -q 'room_unavailable' /tmp/resp.json; then
  ok "Room 不可用时查询房间返回 503 room_unavailable"
else
  fail "Room 不可用时查询房间响应异常：HTTP $code（期望 503）"
  cat /tmp/resp.json; echo
fi

# 核心断言：Room 不可用时，配对不能产生「已 matched 但没有房间」的半成品状态。
http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-down-alice\"}" >/dev/null
http_post /api/v1/matches "{\"token\":\"$bob_token\",\"request_id\":\"verify-room-down-bob\"}" >/dev/null
sleep 1
alice_state=$(match_state "$alice_token")
bob_state=$(match_state "$bob_token")
if [ "$alice_state" = "queued" ] && [ "$bob_state" = "queued" ]; then
  ok "分配失败时玩家留在队列（alice=$alice_state bob=$bob_state），未产生半成品配对"
else
  fail "分配失败时状态异常：alice=[$alice_state] bob=[$bob_state]（期望 queued）"
fi

# 恢复 Room。
if start_room; then
  ok "Room 已重启（进程号 $room_pid）"
else
  fail "Room 重启失败"
  cat /tmp/room.out
fi

# Room 恢复后，两个人仍在队列里。**不重新入队**，直接轮询即可：
# GetStatus 会顺带重试上一次失败的配对（惰性重试，见 src/match/match_queue.hpp）。
# 这一点很重要——brpc 的 channel 是惰性重连的，Room 刚起来时第一次调用必然失败；
# 若没有这个重试，玩家会一直卡在队列里，直到下一个新玩家入队。
recovered=0
if [ -n "$(wait_matched "$alice_token" 30)" ] && [ -n "$(wait_matched "$bob_token" 30)" ]; then
  recovered=1
fi
if [ "$recovered" -eq 1 ]; then
  ok "Room 恢复后仅靠轮询即重新匹配成功（惰性重试生效，无需重新入队）"
else
  fail "Room 恢复后无法重新匹配"
  # 失败时打印足够定位的信息，而不是只报一句「失败」。
  echo "  诊断：alice state=$(match_state "$alice_token") bob state=$(match_state "$bob_token")"
  echo "  诊断：队列长度由 Match 自身维护，此处看 Match 日志末尾："
  tail -12 /tmp/match.out | sed 's/^/    /'
  echo "  诊断：Room 是否仍在监听：$(
    curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$room_port/health" || echo '无响应'
  )"
fi
echo

# ---------- 12. MySQL 停机：结果待落库 ----------
echo "===== 12. MySQL 停机：结果返回 result_pending 而非假成功 ====="
# 复用第 11 步恢复后配到的这一局。先让双方进房，再停 MySQL，然后打完。
recovery_room=$(match_room_id "$alice_token")
recovery_match=$(match_match_id "$alice_token")
http_post /api/v1/rooms/join \
  "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-nodb-join-alice\",\"room_id\":\"$recovery_room\"}" >/dev/null
http_post /api/v1/rooms/join \
  "{\"token\":\"$bob_token\",\"request_id\":\"verify-room-nodb-join-bob\",\"room_id\":\"$recovery_room\"}" >/dev/null

if docker stop rgbt-mysql >/dev/null 2>&1; then
  ok "MySQL 已停止"
else
  fail "无法停止 MySQL 容器"
fi

finished=0
for attempt in $(seq 1 40); do
  http_get "/api/v1/rooms/state?room_id=$recovery_room" "$alice_token" >/dev/null
  state=$(json_path room.state)
  if [ "$state" != "playing" ] && [ "$state" != "waiting" ] && [ "$state" != "created" ]; then
    finished=1
    break
  fi
  http_post /api/v1/rooms/input \
    "{\"token\":\"$alice_token\",\"request_id\":\"verify-room-nodb-$attempt\",\"room_id\":\"$recovery_room\"}" \
    >/dev/null
  sleep 0.25
done
if [ "$finished" -eq 1 ]; then
  ok "MySQL 停机期间对局正常结束（state=$state）"
else
  fail "MySQL 停机期间对局未结束"
fi

# MySQL 不可用时，结果必须报「还没写好」而不是 404，也不能返回假的胜负。
sleep 1.5
code=$(http_get "/api/v1/results?match_id=$recovery_match" "$alice_token")
if [ "$code" = "503" ] && grep -q 'result_pending' /tmp/resp.json; then
  ok "结果未落库时返回 503 result_pending（不是 404、不是假成功）"
else
  fail "结果未落库时响应异常：HTTP $code（期望 503 result_pending）"
  cat /tmp/resp.json; echo
fi
if grep -q '"result"' /tmp/resp.json; then
  fail "result_pending 时响应体不应包含 result 字段"
else
  ok "result_pending 时未返回任何胜负字段"
fi

# 恢复 MySQL，结果应当在一个重试周期内落库，且**不需要重启 Room**。
if docker start rgbt-mysql >/dev/null 2>&1; then
  ok "MySQL 已重启"
else
  fail "无法重启 MySQL 容器"
fi
for _ in $(seq 1 60); do
  status=$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)
  [ "$status" = "healthy" ] && break
  sleep 2
done

if wait_for_result "$alice_token" "$recovery_match" 30; then
  ok "MySQL 恢复后结果自动落库并可查询（Room 未重启）"
  if [ "$(json_path result.winner_id)" = "p-0001" ]; then
    ok "恢复后结果内容正确：winner=p-0001"
  else
    fail "恢复后结果内容异常：winner=[$(json_path result.winner_id)]"
  fi
else
  fail "MySQL 恢复后 15 秒内仍查不到结果"
fi
echo

# ---------- 13. 优雅退出 ----------
echo "===== 13. SIGTERM 优雅退出 ====="
for pair in "Gateway:$gateway_pid" "Match:$match_pid" "Room:$room_pid"; do
  name="${pair%%:*}"
  pid="${pair#*:}"
  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    fail "$name 进程不存在，无法验证优雅退出"
    continue
  fi
  kill -TERM "$pid" 2>/dev/null || true
  exited=0
  for _ in $(seq 1 20); do
    if ! kill -0 "$pid" 2>/dev/null; then
      exited=1
      break
    fi
    sleep 0.5
  done
  if [ "$exited" -eq 1 ]; then
    wait "$pid" 2>/dev/null
    rc=$?
    if [ "$rc" -eq 0 ]; then
      ok "$name 收到 SIGTERM 后退出码 0"
    else
      fail "$name 退出码 $rc（期望 0）"
    fi
  else
    fail "$name 未在 10 秒内退出"
  fi
done
gateway_pid=""
match_pid=""
room_pid=""
echo

# ---------- 结果 ----------
echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过：匹配后可创建真实房间；双方进房驱动 waiting->playing；"
  echo "          攻击按 10 Hz 帧结算且不跨帧累积；一方 HP 归零后产生胜负；"
  echo "          对局结果同步幂等落库（match_results 仅 1 行）；"
  echo "          非本局成员无法加入（400）；结束后提交输入返回 409；"
  echo "          Room 停机返回 503 且不产生半成品配对；"
  echo "          MySQL 停机时结果返回 503 result_pending 且不返回假胜负，"
  echo "          恢复后无需重启 Room 即自动落库；三个进程均可优雅退出"
  echo
  echo "覆盖说明：「非本局成员无法加入」自 2026-10-02 起已在端到端覆盖"
  echo "          （种子数据新增了第三个启用身份 dave/p-0004）。"
  echo "          与之对应的单元测试仍保留："
  echo "          BattleRoomTest.StrangerCannotJoin"
  echo "          BattleRoomTest.StrangerCannotSubmitInput"
  echo "          GatewayServiceTest.JoinRoomUsesPlayerIdFromSession（player_id 取自会话）"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do
  echo "  - $item"
done
echo
echo "排查提示：Gateway 日志 /tmp/gateway.out，Match 日志 /tmp/match.out，Room 日志 /tmp/room.out"
exit 1
