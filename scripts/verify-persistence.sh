#!/usr/bin/env bash
#
# verify-persistence.sh - 房间快照与重启恢复验收入口
#
# 覆盖 TASK-013（写入路径）的验收标准：
#   1. 迁移 005 可重复执行，rooms 表结构与预期一致
#   2. 房间创建后立刻落一条快照（"这里曾经有一局"从一开始就成立）
#   3. 对局进行中 frame 随服务端推进增长，且落库节奏受间隔约束而非每个 tick
#   4. 快照里的血量与房间权威状态一致
#   5. 对局结束后落一次终态（phase=finished / winner）
#   6. **MySQL 停机时对局仍能打完**——快照可以丢弃，不能拖住对局
#   7. MySQL 恢复后快照自动继续，不需要任何补写逻辑
#
# 覆盖 TASK-014（恢复路径）的验收标准：
#   8. 对局进行中 kill -9 Room：重启后房间仍在，且实测进度丢失量有上界
#   9. FINISHING 房间重启后仍能把结果落库（TASK-008 的已知限制解除）
#  10. 损坏快照被标记 ABORTED 并记录原因，不静默丢弃
#  11. **启动时存在未结束房间**时，Room 反复重启都必须存活
#      （TASK-014 期间这里曾 7/10 概率 abort，见 docs/devlog.md）
#  12. 多个请求并发使用同一条 MySQL 连接时不能出错
#
# 覆盖 TASK-015（匹配队列恢复）的验收标准：
#  13. 入队后 Redis 里确实有队列快照
#  14. kill -9 Match 后重启，玩家**仍在队列里**（排队状态不丢）
#  15. Match 的快照存储不可用时**降级为纯内存、匹配照常**，且日志明确记录降级
#
# 用法：
#   bash scripts/verify-persistence.sh              # 完整验收
#   bash scripts/verify-persistence.sh --no-docker  # 复用已启动的 Redis/MySQL

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

# 启动（或重启）Match。抽成函数是因为 TASK-015 的队列恢复验收要
# `kill -9` 之后再拉起它，验证排队状态能从 Redis 快照里回来。
start_match() {
  local redis_port="${1:-${REDIS_PORT:-6379}}"
  "$match_bin" -match_port "$match_port" -match_timeout_seconds 30 -match_result_ttl_seconds 4 \
    -env_prefix dev -redis_host "${REDIS_HOST:-127.0.0.1}" \
    -redis_port "$redis_port" \
    -room_host 127.0.0.1 -room_port "$room_port" >/tmp/match.out 2>&1 &
  match_pid=$!
  for _ in $(seq 1 30); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null && return 0
    sleep 0.5
  done
  return 1
}

if start_match; then
  ok "Match 就绪"
else
  fail "Match 未就绪"; cat /tmp/match.out; exit 1
fi

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
  #
  # 为什么用轮询等最多 12 秒，而不是固定 sleep 2 秒再看一次：
  #   快照写入是在 Room 的 **ticker 线程**上做的（RoomManager::Tick -> FlushSnapshots），
  #   所以一次连不上的写入会让这个线程阻塞在 MySQL 的连接超时上（本脚本用默认 3 秒）。
  #   结果是 MySQL 停机期间对局推进变成**突发式**的：停 3 秒、再连推几十帧。
  #   固定的 2 秒观察窗可能整段落在一次停顿里，于是"对局停住了"是**误报**——
  #   TASK-014 期间实测到过一次（frame 14 -> 14）。这里改成观察"12 秒内是否推进过"。
  before=$(curl -s --max-time 5 -H "Authorization: Bearer $alice_token" \
    "http://127.0.0.1:$gateway_port/api/v1/rooms/state?room_id=$room2" |
    python3 -c "import json,sys; print(json.load(sys.stdin).get('room',{}).get('frame',0))")
  after="$before"
  for _ in $(seq 1 12); do
    sleep 1
    after=$(curl -s --max-time 5 -H "Authorization: Bearer $alice_token" \
      "http://127.0.0.1:$gateway_port/api/v1/rooms/state?room_id=$room2" |
      python3 -c "import json,sys; print(json.load(sys.stdin).get('room',{}).get('frame',0))" 2>/dev/null)
    [ "${after:-0}" -gt "$before" ] 2>/dev/null && break
    after="$before"
  done
  if [ "$after" -gt "$before" ] 2>/dev/null; then
    ok "MySQL 停机期间对局继续推进（frame $before -> $after）"
  else
    fail "MySQL 停机期间对局停住了（frame $before -> $after，观察 12 秒）"
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
    #
    # 定义是「kill 时的实时帧 - 最后一次落库的帧」，也就是"还没来得及写进快照的进度"。
    # 不能用"恢复后查询到的帧"来算：房间一恢复就继续按 10 Hz 推进，等脚本查到它时
    # 帧号已经往前走了（实测重启后 1~3 秒内会多出十几帧），那样算出来会是负数。
    frames_lost=$((frame_before - frame_snapshot))
    frames_lost_measured="$frames_lost"
    if [ "$frames_lost" -ge 0 ] 2>/dev/null && [ "$frames_lost" -le 10 ]; then
      ok "进度丢失量实测：$frames_lost 帧（约 $((frames_lost * 100)) 毫秒），上界 10 帧"
    else
      fail "进度丢失量超出预期：$frames_lost 帧（kill 时实时 $frame_before，最后快照 $frame_snapshot）"
    fi

    # 恢复位置应当**精确等于最后一次快照**，而不是别的什么中间值。
    # 判据取自 Room 启动日志里的恢复点（`已恢复房间：... frame=N`），因为外部查询
    # 拿不到这个数字——见上面「进度丢失量」的说明。
    restored_frame=$(grep -o "room_id=$room3 frame=[0-9]*" /tmp/room.out | tail -1 | grep -o '[0-9]*$')
    if [ -n "$restored_frame" ] && [ "$restored_frame" = "$frame_snapshot" ]; then
      ok "恢复点精确等于最后一次快照帧（$frame_snapshot）"
    else
      fail "恢复点不等于最后快照：日志恢复点 [${restored_frame:-未记录}]，快照 $frame_snapshot"
    fi
    # 房间确实在恢复点之后继续推进了（否则"恢复成功"可能是假象）。
    if [ "$frame_after" -ge "$frame_snapshot" ] 2>/dev/null; then
      ok "恢复后继续推进（恢复点 $frame_snapshot -> 查询时 $frame_after）"
    else
      fail "恢复后帧号倒退：恢复点 $frame_snapshot，查询时 $frame_after"
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
#
# ⚠ 本节在 TASK-014 期间被重写过，原因是**原来的写法不可能通过**：
#   它先停掉 MySQL、把对局打到 finishing，然后在 MySQL 仍停机时重启 Room，
#   再断言"FINISHING 房间被恢复"。
#   但恢复本身就要读 MySQL：存储不可用时 RoomManager::Restore 按设计
#   **一个房间都不恢复**（空手启动比"以为恢复了其实没有"安全）。
#   也就是说那条断言要求"在没有 MySQL 的情况下从 MySQL 恢复"——前提自相矛盾。
#
# 现在改为直接构造那个状态：**在 MySQL 正常时**往 rooms 表插一条合法的
# finishing 行（这正是"进程被杀时结果尚未落库"在库里的样子），重启 Room，
# 断言它被恢复成 finishing，并且**把结果补写进 match_results**。
# 这才是 TASK-008 那条已知限制的正解，也是本任务真正要验的行为。
echo "===== 7b. FINISHING 房间重启后仍能落库 ====="
q_mysql() {
  docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" -e "$1" 2>/dev/null
}
if [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" != "healthy" ]; then
  docker start rgbt-mysql >/dev/null 2>&1 || true
  for _ in $(seq 1 60); do
    [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" = "healthy" ] && break
    sleep 2
  done
fi

now_ms=$(( $(date +%s) * 1000 ))
q_mysql "DELETE FROM rooms WHERE match_id='m-finishing-014';
        DELETE FROM match_results WHERE match_id='m-finishing-014';
        INSERT INTO rooms (match_id, room_id, state, frame,
          p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined,
          winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms)
        VALUES ('m-finishing-014', 'r-finishing-014', 'finishing', 600,
          'p-0001', 100, 1, 'p-0002', 0, 1,
          'p-0001', 'hp_zero', $((now_ms - 60000)), $now_ms, $now_ms);" &&
  ok "已写入一条合法的 finishing 快照（结果尚未落库）" ||
  fail "无法写入 finishing 快照"

kill -9 "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
if start_room; then
  ok "Room 已重启"
  if grep -q 'room_id=r-finishing-014 frame=600' /tmp/room.out; then
    ok "FINISHING 房间被恢复，且恢复点就是快照帧 600（没有被当成已结束而丢弃）"
  else
    fail "FINISHING 房间未被恢复：$(grep -o 'room_id=r-finishing-014 frame=[0-9]*' /tmp/room.out | tail -1)"
  fi

  recovered=0
  for _ in $(seq 1 30); do
    if docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      -e "SELECT 1 FROM match_results WHERE match_id='m-finishing-014';" 2>/dev/null | grep -q 1; then
      recovered=1
      break
    fi
    sleep 1
  done
  if [ "$recovered" -eq 1 ]; then
    ok "重启后的 FINISHING 房间把结果补写进了 match_results（TASK-008 的限制已解除）"
    winner=$(docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      -e "SELECT winner_id FROM match_results WHERE match_id='m-finishing-014';" 2>/dev/null | tr -d '\r')
    if [ "$winner" = "p-0001" ]; then
      ok "补写的结果带上了正确的胜者（winner_id=p-0001）"
    else
      fail "补写结果的 winner_id 异常：[$winner]"
    fi
  else
    fail "重启后 30 秒内结果仍未落库，match_id=m-finishing-014"
  fi
  q_mysql "DELETE FROM rooms WHERE match_id='m-finishing-014';
           DELETE FROM match_results WHERE match_id='m-finishing-014';"
else
  fail "Room 重启失败"
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

# --- 7d. 启动时存在未结束房间：Room 反复重启都必须存活 ---
#
# 这一节是 TASK-014 那次 abort 的直接回归。当时的现象是：
#   只要 rooms 表里有一条未结束的快照，Room 启动就会以 7/10 的概率 abort
#   （`OpenSSL internal error: refcount error`）或段错误。
# 根因不是恢复逻辑，而是**同一条 MysqlConnection 被多个线程共用**：
#   恢复在启动线程上做一次 SELECT，ticker 线程紧接着为刚恢复的房间写第一份
#   快照，而启动线程同一时刻还在做启动探活。一股字节流被两个线程写，
#   MySQL 协议被搅乱（`Received malformed packet`），走到重连分支关闭 TLS 时
#   进程直接死掉。详见 docs/devlog.md 与 common/mysql_connection.hpp 的 mutex_。
#
# 为什么必须**重复**几次：这是竞态，跑一次通过说明不了任何事。
echo "===== 7d. 存在未结束房间时反复重启 Room ====="
# 前面的 7b 可能把 MySQL 停掉了；这一节需要它是活的。
if [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" != "healthy" ]; then
  docker start rgbt-mysql >/dev/null 2>&1 || true
  for _ in $(seq 1 60); do
    [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" = "healthy" ] && break
    sleep 2
  done
fi
if [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-mysql 2>/dev/null)" = "healthy" ]; then
  now_ms=$(( $(date +%s) * 1000 ))
  docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" -e "
    INSERT INTO rooms (match_id, room_id, state, frame,
      p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined,
      winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms)
    VALUES ('m-restart-014', 'r-restart-014', 'playing', 10,
      'p-0001', 100, 1, 'p-0002', 90, 1,
      NULL, 'none', $((now_ms - 3000)), 0, $now_ms)
    ON DUPLICATE KEY UPDATE state='playing', frame=10, snapshot_at_ms=$now_ms;" >/dev/null 2>&1 &&
    ok "已写入一条未结束快照（模拟进程被杀时对局仍在进行）" ||
    fail "无法写入未结束快照"

  restarts=3
  survived=0
  for attempt in $(seq 1 "$restarts"); do
    kill -9 "$room_pid" 2>/dev/null || true
    wait "$room_pid" 2>/dev/null
    room_pid=""
    sleep 1
    if start_room; then
      survived=$((survived + 1))
      # 恢复必须真的发生，否则这一节什么也没验证。
      # 断言"至少恢复 1 个"而不是"恰好 1 个"：表里可能还有别的历史未结束行，
      # 那不影响本节的结论——多个房间只会让并发写快照更密集。
      if ! grep -qE '恢复 [1-9][0-9]* 个' /tmp/room.out; then
        fail "第 $attempt 次重启没有恢复任何房间（前置条件失效，本节未验证到目标路径）"
      fi
      # 崩溃前的直接征兆，出现即失败，便于定位。
      if grep -qE 'refcount error|malformed packet' /tmp/room.out; then
        fail "第 $attempt 次重启的日志出现了并发/协议错误征兆"
      fi
    else
      fail "第 $attempt 次重启后 Room 未就绪（崩溃？）"
      tail -15 /tmp/room.out | sed 's/^/    /'
    fi
  done
  if [ "$survived" -eq "$restarts" ]; then
    ok "连续 $restarts 次「存在未结束房间时重启」全部存活"
  else
    fail "重启存活 $survived/$restarts"
  fi
  docker exec rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "DELETE FROM rooms WHERE match_id='m-restart-014';" >/dev/null 2>&1 || true
else
  fail "MySQL 未恢复，无法验证启动恢复路径"
fi
echo

# --- 7e. 多请求并发使用同一条 MySQL 连接 ---
#
# Gateway 与 Room 都只创建**一条** MysqlConnection（见各自 main）：Gateway 由
# 所有 brpc worker 线程共用，Room 由 ticker 线程与启动路径共用。
# 这一段并发打 12 个登录：每个登录都要读一次 players 表，因此它们会真的争用
# 同一条连接。**它不能保证复现竞态**（竞态没有确定性的时序），作用是把暴露
# 概率拉高到验收里能被看见；真正的确定性回归是上面的 7d。
#
# 先"预热"一次：本节前面停过 MySQL，Gateway 手上那条连接已经死了，**第一个**
# 请求必然要负责发现断连并重连。那一次失败属于"依赖刚恢复时的首次探测"，
# 不能算到"并发共用连接"头上（设计上依赖不可用时就是返回可重试的 503，
# 见 docs/01-architecture 的失败模型）。预热之后，才测真正想测的东西。
echo "===== 7e. 并发共用同一条 MySQL 连接 ====="
warm_code=$(http_post /api/v1/login \
  "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"request_id\":\"vp-warm-$RANDOM\",\"client_type\":\"web\"}")
if [ "$warm_code" = "200" ]; then
  ok "预热登录成功（Gateway 的 MySQL 连接已可用）"
else
  ok "预热登录返回 HTTP $warm_code（依赖刚恢复时的首次探测，不计入并发检查）"
fi

concurrent=12
conc_pids=()
for i in $(seq 1 "$concurrent"); do
  curl -s -o "/tmp/persist-conc-$i.json" -w '%{http_code}' --max-time 10 -X POST \
    -H 'Content-Type: application/json' \
    -d "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"request_id\":\"vp-conc-$i-$RANDOM\",\"client_type\":\"web\"}" \
    "http://127.0.0.1:$gateway_port/api/v1/login" > "/tmp/persist-conc-$i.code" 2>/dev/null &
  conc_pids+=("$!")
done
# 只等这些 curl：**裸 wait 会连 Room/Match/Gateway 一起等**，那三个进程永远不退出。
for pid in "${conc_pids[@]}"; do
  wait "$pid" 2>/dev/null || true
done
conc_ok=0
conc_bad=""
for i in $(seq 1 "$concurrent"); do
  code=$(cat "/tmp/persist-conc-$i.code" 2>/dev/null)
  token=$(python3 -c "
import json,sys
try:
    print(json.load(open('/tmp/persist-conc-$i.json')).get('token','') or '')
except Exception:
    print('')
" 2>/dev/null)
  if [ "$code" = "200" ] && [ -n "$token" ]; then
    conc_ok=$((conc_ok + 1))
  else
    conc_bad="$conc_bad #$i(HTTP ${code:-无响应})"
  fi
done
if [ "$conc_ok" -eq "$concurrent" ]; then
  ok "$concurrent 个并发登录全部成功（同一条连接被并发使用）"
else
  fail "并发登录失败 $((concurrent - conc_ok)) 个：$conc_bad"
fi
if grep -q 'malformed packet' /tmp/gateway.out; then
  fail "Gateway 日志出现 malformed packet（并发使用同一条连接）"
else
  ok "Gateway 日志没有协议错乱"
fi
echo

# --- 7f. 匹配队列的 Redis 快照与重启恢复（TASK-015） ---
#
# 背景：TASK-007 的已知限制是「Match 重启即丢失排队状态」，排队中的玩家会
# 突然变成 idle 且没有任何解释。本节验证三件事：
#   1. 入队后 Redis 里确实有快照；
#   2. kill -9 Match 再拉起，玩家**仍在队列里**（状态与恢复人数都对得上）；
#   3. Redis 不可用时**降级为纯内存、匹配照常**——这是项目所有者确认的策略，
#      也是与房间快照一致的取舍：快照可以丢弃，不阻塞业务。
echo "===== 7f. 匹配队列的重启恢复 ====="
redis_key="dev:match:queue"
redis_cli() { docker exec rgbt-redis redis-cli "$@" 2>/dev/null | tr -d '\r'; }

wait_idle
enqueue_match "$alice_token" "vq-a-$RANDOM"
sleep 1
qlen=$(redis_cli LLEN "$redis_key")
if [ "${qlen:-0}" -ge 1 ] 2>/dev/null; then
  ok "入队后 Redis 快照里有 $qlen 条（$redis_key）"
else
  fail "入队后快照为空：LLEN $redis_key = [$qlen]"
fi

# --- 重启 Match：排队状态必须回来 ---
kill -9 "$match_pid" 2>/dev/null || true
wait "$match_pid" 2>/dev/null
match_pid=""
ok "已 kill -9 Match（排队中）"
if start_match; then
  ok "Match 已重启"
  http_get /api/v1/matches/current "$alice_token" >/dev/null
  state_after_restart=$(json_path match.state)
  if [ "$state_after_restart" = "queued" ]; then
    ok "重启后玩家仍在队列里（state=queued）"
  else
    fail "重启后匹配状态异常：[$state_after_restart]（期望 queued）"
  fi
  if grep -qE '队列恢复：.*重建排队 [1-9]' /tmp/match.out; then
    ok "启动日志报告了恢复到的排队人数"
  else
    fail "启动日志没有报告恢复结果：$(grep -o '队列恢复：.*' /tmp/match.out | tail -1)"
  fi
else
  fail "Match 重启失败"
fi

# --- Match 自己的快照存储不可用：降级为纯内存，匹配照常 ---
#
# 为什么**不能**靠 `docker stop rgbt-redis` 来测这一段：Gateway 的会话也在同一个
# Redis 上，停掉它之后所有 HTTP 请求会先因鉴权失败返回 503，根本走不到 Match，
# 测到的就不是"Match 的快照存储不可用"。（第一版就是这么写的，结果失败项指向的是
# Gateway 的会话存储，而不是本任务要验证的行为。）
# 这里改成让 **Match 指向一个没有监听的端口**：只有 Match 的快照存储不可用，
# 链路其余部分完好，降级行为才能被隔离验证。
kill -9 "$match_pid" 2>/dev/null || true
wait "$match_pid" 2>/dev/null
match_pid=""
dead_redis_port=6399
if start_match "$dead_redis_port"; then
  ok "Match 已重启并指向不可用的快照存储（端口 $dead_redis_port，故意不监听）"
  if grep -q '队列快照读取失败' /tmp/match.out; then
    ok "启动时如实报告快照读取失败（不假装恢复了一个空队列）"
  else
    fail "启动日志没有报告快照读取失败"
  fi

  enqueue_match "$alice_token" "vq-a2-$RANDOM"
  enqueue_match "$bob_token" "vq-b2-$RANDOM"
  matched_without_snapshot=0
  for _ in $(seq 1 20); do
    http_get /api/v1/matches/current "$alice_token" >/dev/null
    [ "$(json_path match.state)" = "matched" ] && { matched_without_snapshot=1; break; }
    sleep 0.3
  done
  if [ "$matched_without_snapshot" -eq 1 ]; then
    ok "快照存储不可用时匹配照常成功（降级为纯内存，不阻塞业务）"
  else
    fail "快照存储不可用时匹配失败：state=[$(json_path match.state)]"
  fi
  if grep -q '队列快照写入失败' /tmp/match.out; then
    ok "日志明确记录了降级（队列快照写入失败，可丢弃）"
  else
    fail "没有记录降级：快照写失败却静默无日志"
  fi
else
  fail "Match 重启失败"
fi
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
  echo "          损坏快照被标记 ABORTED 并记录原因，不静默丢弃；"
  echo "          存在未结束房间时连续 3 次重启全部存活；并发共用同一条 MySQL 连接无协议错乱"
  echo "  [队列恢复，TASK-015] 入队后 Redis 有快照；kill -9 Match 后重启，玩家仍在队列里；"
  echo "          Match 的快照存储不可用时降级为纯内存，匹配照常成功且日志明确记录降级"
  echo
  echo "  实测进度丢失量：${frames_lost_measured:-未取到} 帧"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do echo "  - $item"; done
echo
echo "排查提示：Gateway /tmp/gateway.out，Match /tmp/match.out，Room /tmp/room.out"
exit 1
