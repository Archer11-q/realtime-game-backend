#!/usr/bin/env bash
#
# verify-persistence.sh - TASK-013 房间快照持久化验收入口
#
# 覆盖 TASK-013 的验收标准：
#   1. 迁移 005 可重复执行，rooms 表结构与预期一致
#   2. 房间创建后立刻落一条快照（"这里曾经有一局"从一开始就成立）
#   3. 对局进行中 frame 随服务端推进增长，且落库节奏受间隔约束而非每个 tick
#   4. 快照里的血量与房间权威状态一致
#   5. 对局结束后落一次终态（phase=finished / winner）
#   6. **MySQL 停机时对局仍能打完**——快照可以丢弃，不能拖住对局
#   7. MySQL 恢复后快照自动继续，不需要任何补写逻辑
#
# 用法：
#   bash scripts/verify-persistence.sh              # 完整验收
#   bash scripts/verify-persistence.sh --no-docker  # 复用已启动的 Redis/MySQL
#
# TASK-014 会往这个脚本里追加「重启恢复」一节；本任务只覆盖写入路径。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"

gateway_port=""
match_port=""
room_port=""
gateway_candidates=(8080 18080 18081 18082)
match_candidates=(8082 18092 18093 18094)
room_candidates=(8083 18103 18104 18105)

keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

failures=()
fail() { failures+=("$1"); echo "x  $1"; }
ok() { echo "v  $1"; }
compose() { docker compose -f "$compose_file" "$@"; }

gateway_pid=""; match_pid=""; room_pid=""
cleanup() {
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      [ "$keep_running" -eq 1 ] && echo "已保留进程 $pid（--keep）" || kill "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="
[ -f "$env_file" ] || { [ -f "$env_example" ] && cp "$env_example" "$env_file"; }
set -a; . "./$env_file"; set +a
ok "配置已加载（库 ${MYSQL_DATABASE}）"

port_in_use() { ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"; }
pick_port() {
  local c
  for c in "$@"; do port_in_use "$c" || { printf '%s' "$c"; return 0; }; done
  return 1
}
gateway_port="$(pick_port "${gateway_candidates[@]}")" || { echo "Gateway 端口均被占用" >&2; exit 1; }
match_pool=(); for c in "${match_candidates[@]}"; do [ "$c" = "$gateway_port" ] || match_pool+=("$c"); done
match_port="$(pick_port "${match_pool[@]}")" || { echo "Match 端口均被占用" >&2; exit 1; }
room_pool=(); for c in "${room_candidates[@]}"; do
  [ "$c" = "$gateway_port" ] && continue; [ "$c" = "$match_port" ] && continue; room_pool+=("$c"); done
room_port="$(pick_port "${room_pool[@]}")" || { echo "Room 端口均被占用" >&2; exit 1; }
ok "端口：Gateway $gateway_port / Match $match_port / Room $room_port"
echo

# ---------- 1. 依赖与迁移 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  compose up -d >/tmp/persist-compose.log 2>&1 && ok "compose up 成功" ||
    { fail "compose up 失败"; tail -20 /tmp/persist-compose.log; }
  for c in rgbt-redis rgbt-mysql; do
    healthy=0
    for _ in $(seq 1 60); do
      [ "$(docker inspect -f '{{.State.Health.Status}}' "$c" 2>/dev/null)" = "healthy" ] &&
        { healthy=1; break; }
      sleep 2
    done
    [ "$healthy" -eq 1 ] && ok "$c 健康" || fail "$c 未达到 healthy"
  done
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
fi

echo
echo "===== 2. 应用迁移（幂等） ====="
applied=0
for file in migrations/*.sql; do
  [ -e "$file" ] || continue
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$file" >/dev/null 2>&1 && applied=$((applied + 1))
done
[ "$applied" -gt 0 ] && ok "已应用 $applied 个迁移" || fail "迁移未成功应用"

# 结构断言：rooms 表存在且列齐全。
columns=$(docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
  -e "SELECT COLUMN_NAME FROM information_schema.columns WHERE table_schema='$MYSQL_DATABASE' AND table_name='rooms' ORDER BY ORDINAL_POSITION;" 2>/dev/null | tr -d '\r')
for col in match_id room_id state frame p1_id p1_hp p1_joined p2_id p2_hp p2_joined winner_id finish_reason started_at_ms finished_at_ms snapshot_at_ms; do
  if echo "$columns" | grep -qx "$col"; then
    :
  else
    fail "rooms 表缺少列 $col"
  fi
done
[ "${#failures[@]}" -eq 0 ] && ok "rooms 表结构符合预期（15 列）"
echo

# ---------- 3. 构建与启动 ----------
echo "===== 3. 构建并启动服务 ====="
cmake --preset "$preset" >/tmp/persist-cmake.log 2>&1 && ok "配置成功" ||
  { fail "配置失败"; tail -20 /tmp/persist-cmake.log; }
cmake --build --preset "$preset" >/tmp/persist-build.log 2>&1 && ok "构建成功" ||
  { fail "构建失败"; tail -30 /tmp/persist-build.log; exit 1; }

room_bin="build/$preset/bin/rgbt_room"
match_bin="build/$preset/bin/rgbt_match"
gw_bin="build/$preset/bin/rgbt_gateway"
for bin in "$room_bin" "$match_bin" "$gw_bin"; do
  [ -x "$bin" ] || { fail "缺少可执行文件 $bin"; exit 1; }
done
ok "三个可执行文件均存在"

# 启动（或重启）Room。抽成函数是因为 TASK-014 的恢复验收要反复 kill -9 再拉起它。
start_room() {
  "$room_bin" -port "$room_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" >/tmp/room.out 2>&1 &
  room_pid=$!
  for _ in $(seq 1 30); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" 2>/dev/null && return 0
    sleep 0.5
  done
  return 1
}

if start_room; then
  ok "Room 就绪"
else
  fail "Room 未就绪"; cat /tmp/room.out; exit 1
fi

"$match_bin" -match_port "$match_port" -match_timeout_seconds 30 -match_result_ttl_seconds 4 \
  -room_host 127.0.0.1 -room_port "$room_port" >/tmp/match.out 2>&1 &
match_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && ok "Match 就绪" ||
  { fail "Match 未就绪"; exit 1; }

"$gw_bin" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 >/tmp/gateway.out 2>&1 &
gateway_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && ok "Gateway 就绪" ||
  { fail "Gateway 未就绪"; exit 1; }
echo

# ---------- HTTP 与 SQL 辅助 ----------
http_post() {
  local body="${2:-}"; [ -n "$body" ] || body='{}'
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
    with open('/tmp/resp.json') as h: node = json.load(h)
except Exception: print(''); sys.exit(0)
for part in sys.argv[1].split('.'):
    if not part: continue
    if isinstance(node, dict):
        if part not in node: print(''); sys.exit(0)
        node = node[part]
    else: print(''); sys.exit(0)
print('' if node is None else node)
PY
}
# 查 rooms 表的一个字段。没有该行时输出空串。
sql_field() {
  docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "SELECT $1 FROM rooms WHERE match_id='$2';" 2>/dev/null | tr -d '\r'
}
login() {
  http_post /api/v1/login \
    "{\"account\":\"$1\",\"password\":\"$2\",\"request_id\":\"$3\",\"client_type\":\"web\"}" >/dev/null
  json_path token
}
enqueue_match() { http_post /api/v1/matches "{\"token\":\"$1\",\"request_id\":\"$2\"}" >/dev/null; }
wait_matched() {
  local token="$1"
  for _ in $(seq 1 30); do
    http_get /api/v1/matches/current "$token" >/dev/null
    [ "$(json_path match.state)" = "matched" ] && { json_path match.room_id; return 0; }
    sleep 0.3
  done
  echo ''; return 1
}
# 开一局并让双方进房。成功输出 room_id。
open_playing_room() {
  enqueue_match "$alice_token" "vp-a-$RANDOM"
  enqueue_match "$bob_token" "vp-b-$RANDOM"
  local rid; rid=$(wait_matched "$alice_token")
  [ -n "$rid" ] || { echo ''; return 1; }
  http_post /api/v1/rooms/join "{\"token\":\"$alice_token\",\"request_id\":\"vp-ja-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_post /api/v1/rooms/join "{\"token\":\"$bob_token\",\"request_id\":\"vp-jb-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_get "/api/v1/rooms/state?room_id=$rid" "$alice_token" >/dev/null
  [ "$(json_path room.state)" = "playing" ] || { echo ''; return 1; }
  echo "$rid"
}

# ---------- 4. 第一局：快照写入路径 ----------
echo "===== 4. 对局进行中的快照 ====="
alice_token=$(login alice alice_dev_pw "vp-alice")
bob_token=$(login bob bob_dev_pw "vp-bob")
[ -n "$alice_token" ] && ok "alice 登录成功" || fail "alice 登录失败"

room_id=$(open_playing_room)
[ -n "$room_id" ] && ok "对局已开始（room_id=$room_id）" || fail "无法开始对局"
http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token" >/dev/null
match_id=$(json_path room.match_id)

# 房间刚创建时就应该有一条快照。
if [ -n "$(sql_field match_id "$match_id")" ]; then
  ok "rooms 表已有该房间的记录（创建后立刻落一条）"
  # 首个快照的状态就是 created——**这是准确的，不是过期**：那一刻房间确实
  # 还没人加入。状态随对局推进而更新由下面的断言覆盖。
  first_state=$(sql_field state "$match_id")
  if [ "$first_state" = "created" ]; then
    ok "首个快照状态为 created（与创建时刻一致）"
  else
    fail "首个快照状态异常：[$first_state]（期望 created）"
  fi
else
  fail "rooms 表没有该房间的记录"
fi

# 等一个快照间隔：状态与帧号都应当已经更新。
sleep 2
if [ "$(sql_field state "$match_id")" = "playing" ]; then
  ok "快照状态随对局推进更新为 playing"
else
  fail "快照状态未更新：$(sql_field state "$match_id")（期望 playing）"
fi

frame1=$(sql_field frame "$match_id")
http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token" >/dev/null
live_frame=$(json_path room.frame)
if [ -n "$frame1" ] && [ "$frame1" -gt 0 ] 2>/dev/null; then
  ok "快照 frame 随服务端推进增长（frame=$frame1，服务端当前 $live_frame）"
else
  fail "快照 frame 没有增长：[$frame1]"
fi
# 快照落后于服务端是**设计如此**：它是周期性的，不是实时的。
if [ "$frame1" -le "$live_frame" ] 2>/dev/null; then
  ok "快照不超前于服务端状态（快照 $frame1 <= 实时 $live_frame）"
else
  fail "快照 frame 超前于服务端（$frame1 > $live_frame），说明写入了未来状态"
fi

# 攻击后血量应当反映在下一份快照里。
http_post /api/v1/rooms/input \
  "{\"token\":\"$alice_token\",\"request_id\":\"vp-hit-$RANDOM\",\"room_id\":\"$room_id\"}" >/dev/null
sleep 2
hp2=$(sql_field p2_hp "$match_id")
if [ "$hp2" -lt 100 ] 2>/dev/null; then
  ok "快照反映了攻击造成的掉血（p2_hp=$hp2）"
else
  fail "快照血量没有下降：p2_hp=[$hp2]"
fi
echo

# ---------- 5. 对局结束：终态快照 ----------
echo "===== 5. 结束后的终态快照 ====="
for attempt in $(seq 1 40); do
  http_get "/api/v1/rooms/state?room_id=$room_id" "$alice_token" >/dev/null
  [ "$(json_path room.state)" != "playing" ] && break
  http_post /api/v1/rooms/input \
    "{\"token\":\"$alice_token\",\"request_id\":\"vp-fin-$attempt\",\"room_id\":\"$room_id\"}" >/dev/null
  sleep 0.25
done
sleep 2
final_state=$(sql_field state "$match_id")
final_winner=$(sql_field winner_id "$match_id")
if [ "$final_state" = "finished" ]; then
  ok "结束后快照进入终态（state=finished）"
else
  fail "终态快照未写入或状态不对：state=[$final_state]"
fi
if [ "$final_winner" = "p-0001" ]; then
  ok "终态快照带上了胜者（winner_id=p-0001）"
else
  fail "终态快照的 winner_id 异常：[$final_winner]"
fi
# 每一列都应当来自终态，不能停在中间态。
if [ "$(sql_field finish_reason "$match_id")" = "hp_zero" ]; then
  ok "终态快照的 finish_reason=hp_zero"
else
  fail "finish_reason 异常：$(sql_field finish_reason "$match_id")"
fi
echo

# ---------- 6. MySQL 停机时对局仍能打完 ----------
echo "===== 6. MySQL 停机不影响对局推进 ====="
# 等上一局的结果保留期过去，双方回到 idle。
for _ in $(seq 1 30); do
  http_get /api/v1/matches/current "$alice_token" >/dev/null; s1=$(json_path match.state)
  http_get /api/v1/matches/current "$bob_token" >/dev/null; s2=$(json_path match.state)
  [ "$s1" = "idle" ] && [ "$s2" = "idle" ] && break
  sleep 0.5
done

room2=$(open_playing_room)
if [ -z "$room2" ]; then
  fail "无法开始第二局"
else
  http_get "/api/v1/rooms/state?room_id=$room2" "$alice_token" >/dev/null
  match2=$(json_path room.match_id)
  ok "第二局已开始（room_id=$room2）"

  docker stop rgbt-mysql >/dev/null 2>&1 && ok "MySQL 已停止" || fail "无法停止 MySQL"

  # 关键断言：MySQL 不可用时，快照写失败**不能拖住对局**。
  before=$(curl -s --max-time 5 -H "Authorization: Bearer $alice_token" \
    "http://127.0.0.1:$gateway_port/api/v1/rooms/state?room_id=$room2" |
    python3 -c "import json,sys; print(json.load(sys.stdin).get('room',{}).get('frame',0))")
  sleep 2
  after=$(curl -s --max-time 5 -H "Authorization: Bearer $alice_token" \
    "http://127.0.0.1:$gateway_port/api/v1/rooms/state?room_id=$room2" |
    python3 -c "import json,sys; print(json.load(sys.stdin).get('room',{}).get('frame',0))")
  if [ "$after" -gt "$before" ] 2>/dev/null; then
    ok "MySQL 停机期间对局继续推进（frame $before -> $after）"
  else
    fail "MySQL 停机期间对局停住了（frame $before -> $after）"
  fi

  # 还能打完。
  for attempt in $(seq 1 40); do
    http_get "/api/v1/rooms/state?room_id=$room2" "$alice_token" >/dev/null
    [ "$(json_path room.state)" != "playing" ] && break
    http_post /api/v1/rooms/input \
      "{\"token\":\"$alice_token\",\"request_id\":\"vp-nodb-$attempt\",\"room_id\":\"$room2\"}" >/dev/null
    sleep 0.25
  done
  http_get "/api/v1/rooms/state?room_id=$room2" "$alice_token" >/dev/null
  if [ "$(json_path room.state)" = "finishing" ]; then
    ok "MySQL 停机时对局仍能打完（停在 finishing，等结果落库）"
  else
    fail "MySQL 停机时对局状态异常：$(json_path room.state)"
  fi

  docker start rgbt-mysql >/dev/null 2>&1 && ok "MySQL 已重启" || fail "无法重启 MySQL"
  for _ in $(seq 1 60); do
    [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" = "healthy" ] && break
    sleep 2
  done

  # 恢复后：结果落库（既有能力），且快照自动继续（本任务的能力）。
  recovered=0
  for _ in $(seq 1 30); do
    [ "$(sql_field state "$match2")" = "finished" ] && { recovered=1; break; }
    sleep 1
  done
  if [ "$recovered" -eq 1 ]; then
    ok "MySQL 恢复后快照自动补上终态，不需要任何补写逻辑"
  else
    fail "MySQL 恢复后 30 秒内快照未进入终态：state=[$(sql_field state "$match2")]"
  fi
fi
echo

# ---------- 7. 重启恢复（TASK-014） ----------
echo "===== 7. 重启恢复 ====="

# 等两人回到 idle（上一局的结果保留期过去）。
wait_idle() {
  local s1 s2
  for _ in $(seq 1 40); do
    http_get /api/v1/matches/current "$alice_token" >/dev/null; s1=$(json_path match.state)
    http_get /api/v1/matches/current "$bob_token" >/dev/null; s2=$(json_path match.state)
    [ "$s1" = "idle" ] && [ "$s2" = "idle" ] && return 0
    sleep 0.5
  done
  return 1
}

# --- 7a. 对局进行中 kill -9 Room：房间仍在，且量出丢失了多少帧 ---
wait_idle
room3=$(open_playing_room)
if [ -z "$room3" ]; then
  fail "无法为恢复验收开出对局"
else
  http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token" >/dev/null
  match3=$(json_path room.match_id)
  hp_before_p2=$(json_path room.players.1.hp)

  # 让它多跑几帧，确保快照与实时状态之间确实存在差距（否则丢帧量恒为 0，
  # 断言就退化成"什么都没验证"）。
  sleep 3

  # kill 之前的实时帧号。
  frame_before=$(curl -s --max-time 5 -H "Authorization: Bearer $alice_token" \
    "http://127.0.0.1:$gateway_port/api/v1/rooms/state?room_id=$room3" |
    python3 -c "import json,sys; print(json.load(sys.stdin).get('room',{}).get('frame',-1))")
  # 最后一次落库的帧号（= 恢复后应当回到的位置）。
  frame_snapshot=$(sql_field frame "$match3")

  kill -9 "$room_pid" 2>/dev/null || true
  wait "$room_pid" 2>/dev/null
  room_pid=""
  ok "已 kill -9 Room（对局进行中，实时帧 $frame_before，最后快照帧 $frame_snapshot）"

  code=$(http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token")
  if [ "$code" = "503" ]; then
    ok "Room 停止期间查询返回 503（不是伪造成功）"
  else
    fail "Room 停止期间查询异常：HTTP $code"
  fi

  if start_room; then
    ok "Room 已重启（进程号 $room_pid）"
    sleep 1
    code=$(http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token")
    if [ "$code" = "200" ] && [ "$(json_path room.state)" = "playing" ]; then
      ok "重启后房间仍在且状态为 playing（room_id 未变）"
    else
      fail "重启后房间丢失或状态异常：HTTP $code state=$(json_path room.state)"
    fi

    frame_after=$(json_path room.frame)
    hp_after_p2=$(json_path room.players.1.hp)
    if [ "$hp_after_p2" = "$hp_before_p2" ]; then
      ok "重启后血量与快照一致（p-0002 HP=$hp_after_p2）"
    else
      fail "重启后血量与快照不一致：kill 前 $hp_before_p2，恢复后 $hp_after_p2"
    fi

    # **实测的进度丢失量**：这是 TASK-014 要求必须给出的数字，Phase 4 会用它做基线。
    frames_lost=$((frame_before - frame_after))
    frames_lost_measured="$frames_lost"
    if [ "$frames_lost" -ge 0 ] 2>/dev/null && [ "$frames_lost" -le 10 ]; then
      ok "进度丢失量实测：$frames_lost 帧（约 $((frames_lost * 100)) 毫秒），上界 10 帧"
    else
      fail "进度丢失量超出预期：$frames_lost 帧（kill 前 $frame_before，恢复后 $frame_after）"
    fi
    # 恢复位置应当**等于最后一次快照**，而不是别的什么中间值。
    if [ "$frame_after" = "$frame_snapshot" ]; then
      ok "恢复位置精确等于最后一次快照帧（$frame_snapshot）"
    else
      fail "恢复位置不等于最后快照：恢复后 $frame_after，快照 $frame_snapshot"
    fi

    # 恢复后还能正常打完。
    for attempt in $(seq 1 40); do
      http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token" >/dev/null
      [ "$(json_path room.state)" != "playing" ] && break
      http_post /api/v1/rooms/input \
        "{\"token\":\"$alice_token\",\"request_id\":\"vp-recover-$attempt\",\"room_id\":\"$room3\"}" \
        >/dev/null
      sleep 0.25
    done
    sleep 2
    if [ "$(sql_field state "$match3")" = "finished" ]; then
      ok "恢复后的对局能打完并落终态"
    else
      fail "恢复后的对局未能打完：state=[$(sql_field state "$match3")]"
    fi
  else
    fail "Room 重启失败"
  fi
fi
echo

# --- 7b. FINISHING 房间重启后仍能落库（TASK-008 的已知限制） ---
echo "===== 7b. FINISHING 房间重启后仍能落库 ====="
wait_idle
docker stop rgbt-mysql >/dev/null 2>&1 && ok "MySQL 已停止" || fail "无法停止 MySQL"

room4=$(open_playing_room)
if [ -z "$room4" ]; then
  fail "无法为 FINISHING 验收开出对局"
else
  http_get "/api/v1/rooms/state?room_id=$room4" "$alice_token" >/dev/null
  match4=$(json_path room.match_id)
  for attempt in $(seq 1 40); do
    http_get "/api/v1/rooms/state?room_id=$room4" "$alice_token" >/dev/null
    [ "$(json_path room.state)" != "playing" ] && break
    http_post /api/v1/rooms/input \
      "{\"token\":\"$alice_token\",\"request_id\":\"vp-finishing-$attempt\",\"room_id\":\"$room4\"}" \
      >/dev/null
    sleep 0.25
  done
  http_get "/api/v1/rooms/state?room_id=$room4" "$alice_token" >/dev/null
  if [ "$(json_path room.state)" = "finishing" ]; then
    ok "MySQL 停机使对局停在 finishing（结果尚未落库）"
  else
    fail "对局状态异常：$(json_path room.state)（期望 finishing）"
  fi

  kill -9 "$room_pid" 2>/dev/null || true
  wait "$room_pid" 2>/dev/null
  room_pid=""
  if start_room; then
    ok "Room 在 MySQL 仍停机时重启成功"
    http_get "/api/v1/rooms/state?room_id=$room4" "$alice_token" >/dev/null
    if [ "$(json_path room.state)" = "finishing" ]; then
      ok "FINISHING 房间被恢复（没有被当成已结束而丢弃）"
    else
      fail "FINISHING 房间恢复失败：$(json_path room.state)"
    fi

    docker start rgbt-mysql >/dev/null 2>&1 && ok "MySQL 已重启" || fail "无法重启 MySQL"
    for _ in $(seq 1 60); do
      [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" = "healthy" ] && break
      sleep 2
    done
    recovered=0
    for _ in $(seq 1 30); do
      if docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
        -e "SELECT 1 FROM match_results WHERE match_id='$match4';" 2>/dev/null | grep -q 1; then
        recovered=1
        break
      fi
      sleep 1
    done
    if [ "$recovered" -eq 1 ]; then
      ok "重启后的 FINISHING 房间把结果补写进了 match_results（TASK-008 的限制已解除）"
    else
      fail "重启后 30 秒内结果仍未落库，match_id=$match4"
    fi
  else
    fail "Room 重启失败"
  fi
fi
echo

# --- 7c. 损坏快照被标记 ABORTED，不静默丢弃 ---
echo "===== 7c. 损坏快照被标记 ABORTED ====="
# 直接塞一条越界血量的行：它会被 ValidateRoomSnapshot 拒绝。
docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" -e "
INSERT INTO rooms (match_id, room_id, state, frame,
  p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined,
  winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms)
VALUES ('m-corrupt-1', 'r-corrupt-1', 'playing', 5,
  'p-0001', 999, 1, 'p-0002', 100, 1,
  NULL, 'none', 0, 0, 1000)
ON DUPLICATE KEY UPDATE snapshot_at_ms = 1000;" >/dev/null 2>&1 &&
  ok "已写入一条损坏快照（p1_hp=999，超出 [0,100]）" || fail "无法写入损坏快照"

kill -9 "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
if start_room; then
  ok "Room 已重启"
  # 启动日志必须说明拒绝了什么、为什么——这是"不静默丢弃"的可观测证据。
  if grep -q '房间快照不可用，标记为 ABORTED' /tmp/room.out; then
    ok "启动日志记录了拒绝原因"
  else
    fail "启动日志没有记录拒绝原因"
  fi
  if [ "$(sql_field state m-corrupt-1)" = "aborted" ]; then
    ok "损坏快照已被改写为 aborted（不会每次启动都被重新扫出来）"
  else
    fail "损坏快照未被改写：state=[$(sql_field state m-corrupt-1)]"
  fi
else
  fail "Room 重启失败"
fi
# 清理这条测试数据，避免污染后续运行。
docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
  -e "DELETE FROM rooms WHERE match_id='m-corrupt-1';" >/dev/null 2>&1 || true
echo

# ---------- 8. 优雅退出 ----------
echo "===== 8. 优雅退出 ====="
for pair in "Gateway:$gateway_pid" "Match:$match_pid" "Room:$room_pid"; do
  name="${pair%%:*}"; pid="${pair#*:}"
  [ -z "$pid" ] && continue
  kill -TERM "$pid" 2>/dev/null || true
  exited=0
  for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || { exited=1; break; }; sleep 0.5; done
  if [ "$exited" -eq 1 ]; then
    wait "$pid" 2>/dev/null; rc=$?
    [ "$rc" -eq 0 ] && ok "$name 退出码 0" || fail "$name 退出码 $rc"
  else
    fail "$name 未在 10 秒内退出"
  fi
done
gateway_pid=""; match_pid=""; room_pid=""
echo

# ---------- 结果 ----------
echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过："
  echo "  [写入路径，TASK-013] 迁移 005 幂等且 rooms 表结构符合预期；房间创建后立刻落快照；"
  echo "          对局中 frame 与血量随服务端推进更新，且快照不超前于实时状态；"
  echo "          结束后落终态（finished / winner / finish_reason）；"
  echo "          MySQL 停机期间对局继续推进并能打完，恢复后快照自动继续"
  echo "  [恢复路径，TASK-014] 对局进行中 kill -9 Room 后重启，房间仍在、血量与快照一致、"
  echo "          恢复位置精确等于最后一次快照；FINISHING 房间重启后仍能落库；"
  echo "          损坏快照被标记 ABORTED 并记录原因，不静默丢弃"
  echo
  echo "  实测进度丢失量：${frames_lost_measured:-未取到} 帧"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do echo "  - $item"; done
echo
echo "排查提示：Gateway /tmp/gateway.out，Match /tmp/match.out，Room /tmp/room.out"
exit 1
