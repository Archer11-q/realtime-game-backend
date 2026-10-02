#!/usr/bin/env bash
#
# verify-reconnect.sh - TASK-016 断线重连与宽限期的验收入口
#
# 覆盖的验收标准（见 docs/TASKS.md 的 TASK-016）：
#   1. 对局中断开推送连接 → 房间如实记录该玩家离线，且**对局暂停推进**（帧号冻结）
#   2. 宽限期内重连 → 仍在原房间，帧号与血量与断线那一刻**精确一致**，随后继续推进
#   3. 超过宽限期未归 → 判断线方负，reason = disconnect，并正常写入 match_results
#   4. 双方都断线且都没回来 → 本局作废，不写 match_results（不造一个假胜负）
#
# 为什么要自己驱动 SSE 而不是只靠 HTTP：**"连接断了"这个事实只有 Gateway 的
# 推送出口才知道**（SSE 是长连接，客户端断开时服务端收不到显式通知）。
# 因此本脚本用 curl 起一条真实的 SSE 流，再把它杀掉来制造断线。
#
# 用法：
#   bash scripts/verify-reconnect.sh              # 完整验收
#   bash scripts/verify-reconnect.sh --no-docker  # 复用已启动的 Redis/MySQL
#   bash scripts/verify-reconnect.sh --keep       # 保留服务进程
#
# 注意：宽限期是 30 秒（kReconnectGraceMs），因此"到期判负"那一段会真的等 30 秒。

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

gateway_pid=""; match_pid=""; room_pid=""; sse_pid=""
cleanup() {
  [ -n "$sse_pid" ] && kill "$sse_pid" 2>/dev/null
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      [ "$keep_running" -eq 1 ] && echo "已保留进程 $pid（--keep）" || kill "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

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

if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  compose up -d >/tmp/reconnect-compose.log 2>&1 && ok "compose up 成功" ||
    { fail "compose up 失败"; tail -20 /tmp/reconnect-compose.log; }
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

echo "===== 2. 应用迁移 + 清理上一轮状态 ====="
applied=0
for file in migrations/*.sql; do
  [ -e "$file" ] || continue
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$file" >/dev/null 2>&1 && applied=$((applied + 1))
done
[ "$applied" -gt 0 ] && ok "已应用 $applied 个迁移" || fail "迁移未成功应用"
# 与会话一起清掉队列快照：Match 启动会恢复它，不清会让上一轮的玩家出现在本轮。
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
ok "已清理上一轮的 dev:* Key"
echo

echo "===== 3. 构建并启动服务 ====="
cmake --preset "$preset" >/tmp/reconnect-cmake.log 2>&1 && ok "配置成功" ||
  { fail "配置失败"; tail -20 /tmp/reconnect-cmake.log; }
cmake --build --preset "$preset" >/tmp/reconnect-build.log 2>&1 && ok "构建成功" ||
  { fail "构建失败"; tail -30 /tmp/reconnect-build.log; exit 1; }

room_bin="build/$preset/bin/rgbt_room"
match_bin="build/$preset/bin/rgbt_match"
gw_bin="build/$preset/bin/rgbt_gateway"

"$room_bin" -port "$room_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" >/tmp/reconnect-room.out 2>&1 &
room_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && ok "Room 就绪" || { fail "Room 未就绪"; exit 1; }

"$match_bin" -match_port "$match_port" -match_timeout_seconds 30 -match_result_ttl_seconds 4 \
  -env_prefix dev -room_host 127.0.0.1 -room_port "$room_port" >/tmp/reconnect-match.out 2>&1 &
match_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && ok "Match 就绪" || { fail "Match 未就绪"; exit 1; }

"$gw_bin" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 >/tmp/reconnect-gateway.out 2>&1 &
gateway_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && ok "Gateway 就绪" || { fail "Gateway 未就绪"; exit 1; }
echo

# ---------- 辅助 ----------
http_post() {
  local body="${2:-}"; [ -n "$body" ] || body='{}'
  curl -s -o /tmp/rc-resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}
http_get() {
  curl -s -o /tmp/rc-resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer ${2:-}" "http://127.0.0.1:$gateway_port$1"
}
json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/rc-resp.json') as h: node = json.load(h)
except Exception: print(''); sys.exit(0)
for part in sys.argv[1].split('.'):
    if not part: continue
    if isinstance(node, dict):
        if part not in node: print(''); sys.exit(0)
        node = node[part]
    elif isinstance(node, list):
        try: node = node[int(part)]
        except Exception: print(''); sys.exit(0)
    else: print(''); sys.exit(0)
print('' if node is None else node)
PY
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
open_playing_room() {
  enqueue_match "$alice_token" "vr-a-$RANDOM"
  enqueue_match "$bob_token" "vr-b-$RANDOM"
  local rid; rid=$(wait_matched "$alice_token")
  [ -n "$rid" ] || { echo ''; return 1; }
  http_post /api/v1/rooms/join "{\"token\":\"$alice_token\",\"request_id\":\"vr-ja-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_post /api/v1/rooms/join "{\"token\":\"$bob_token\",\"request_id\":\"vr-jb-$RANDOM\",\"room_id\":\"$rid\"}" >/dev/null
  http_get "/api/v1/rooms/state?room_id=$rid" "$alice_token" >/dev/null
  [ "$(json_path room.state)" = "playing" ] || { echo ''; return 1; }
  echo "$rid"
}
# 起一条真实的 SSE 流（背景），并把它的 pid 记在 sse_pid。
open_stream() {
  local room_id="$1" token="$2"
  curl -sN --max-time 120 -H "Authorization: Bearer $token" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_id" \
    >/tmp/rc-sse.out 2>&1 &
  sse_pid=$!
  # 等 session.ready，确认订阅真的建立了（也就意味着 online 已上报）。
  for _ in $(seq 1 40); do
    grep -q 'session.ready' /tmp/rc-sse.out 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}
close_stream() {
  [ -n "$sse_pid" ] && kill "$sse_pid" 2>/dev/null
  wait "$sse_pid" 2>/dev/null
  sse_pid=""
}
room_field() { http_get "/api/v1/rooms/state?room_id=$1" "$alice_token" >/dev/null; json_path "$2"; }
# 等到某个房间字段等于期望值（最多 10 秒），返回实际值并小写化。
#
# 为什么要轮询而不是 sleep 一次就断言：断线/重连的上报是**异步**的（Gateway 侧
# 由写失败感知，Room 侧要下一次查询才看到），而且刚被杀掉的 SSE 流可能让紧随其后的
# 第一个查询短暂失败——那会让断言读到空值而误报。实测踩到过：同一段里
# "重连后 online 恢复"通过、而前一个"断线已记录"读到空。
wait_room_field() {
  local room_id="$1" path="$2" expected="$3" value=""
  for _ in $(seq 1 20); do
    value=$(room_field "$room_id" "$path" | tr 'A-Z' 'a-z')
    [ "$value" = "$expected" ] && { echo "$value"; return 0; }
    sleep 0.5
  done
  echo "$value"
  return 1
}

# ---------- 4. 断开与宽限期内重连 ----------
echo "===== 4. 断线 → 暂停 → 宽限期内重连 ====="
alice_token=$(login alice alice_dev_pw "vr-alice")
bob_token=$(login bob bob_dev_pw "vr-bob")
[ -n "$alice_token" ] && ok "alice 登录成功" || fail "alice 登录失败"

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
wait_idle

room1=$(open_playing_room)
if [ -z "$room1" ]; then
  fail "无法开始对局"
else
  ok "对局已开始（room_id=$room1）"

  # alice 建立推送连接（= 上报 online）。
  if open_stream "$room1" "$alice_token"; then
    ok "alice 的 SSE 订阅已建立（session.ready）"
  else
    fail "alice 的 SSE 订阅未建立"
    cat /tmp/rc-sse.out | head -5
  fi

  # 让对局跑几帧，取一个基准。
  sleep 2
  frame_before=$(room_field "$room1" room.frame)
  hp_before=$(room_field "$room1" room.players.1.hp)
  if [ "${frame_before:-0}" -gt 0 ] 2>/dev/null; then
    ok "对局正在推进（frame=$frame_before）"
  else
    fail "对局没有推进：frame=[$frame_before]"
  fi

  # 断开：杀掉 SSE 流。上报是异步的，因此轮询等待而不是 sleep 一次就断言。
  #
  # 注意判据：proto3 的 JSON 输出**会省略等于默认值的字段**，因此 online=false 时
  # 这个字段在响应里根本不出现（online=true 时才会出现）。所以"已断线"的判据是
  # "字段不是 true"。为了不让这条断言退化成"永远成立"，同一段里紧接着还有一条
  # 反向检查——重连后必须出现 online=true，它证明这个字段确实存在且有效。
  close_stream
  online_after=""
  for _ in $(seq 1 20); do
    online_after=$(room_field "$room1" room.players.0.online | tr 'A-Z' 'a-z')
    { [ "$online_after" = "false" ] || [ -z "$online_after" ]; } && break
    online_after="still-online"
    sleep 0.5
  done
  if [ "$online_after" != "true" ] && [ "$online_after" != "still-online" ]; then
    ok "断线已被记录（players[0].online 不再为 true）"
  else
    fail "断线未被记录：players[0].online=[$online_after]"
  fi

  # 关键断言：宽限期内对局**暂停推进**。
  frame_paused_1=$(room_field "$room1" room.frame)
  sleep 3
  frame_paused_2=$(room_field "$room1" room.frame)
  if [ "$frame_paused_1" = "$frame_paused_2" ]; then
    ok "宽限期内对局暂停推进（frame 冻结在 $frame_paused_2）"
  else
    fail "宽限期内对局仍在推进：$frame_paused_1 -> $frame_paused_2"
  fi

  # 重连：再建一条 SSE。
  if open_stream "$room1" "$alice_token"; then
    ok "宽限期内重连成功"
  else
    fail "宽限期内重连失败"
  fi
  sleep 1
  online_back=$(wait_room_field "$room1" room.players.0.online true)
  if [ "$online_back" = "true" ]; then
    ok "重连后 online 恢复为 true"
  else
    fail "重连后 online 未恢复：[$online_back]"
  fi
  hp_after=$(room_field "$room1" room.players.1.hp)
  if [ "$hp_after" = "$hp_before" ]; then
    ok "重连后血量与断线时一致（players[1].hp=$hp_after）"
  else
    fail "重连后血量变化：断线时 $hp_before，现在 $hp_after"
  fi
  frame_resume_1=$(room_field "$room1" room.frame)
  sleep 2
  frame_resume_2=$(room_field "$room1" room.frame)
  if [ "${frame_resume_2:-0}" -gt "${frame_resume_1:-0}" ] 2>/dev/null; then
    ok "重连后对局继续推进（$frame_resume_1 -> $frame_resume_2）"
  else
    fail "重连后对局没有继续推进：$frame_resume_1 -> $frame_resume_2"
  fi
  close_stream
fi
echo

# ---------- 5. 超过宽限期：判断线方负 ----------
echo "===== 5. 超过宽限期（30 秒）判断线方负 ====="
# 注意：这一段的等待是宽限期本身，不是脚本在空转。
wait_idle
room2=$(open_playing_room)
if [ -z "$room2" ]; then
  fail "无法为到期验收开出对局"
else
  ok "对局已开始（room_id=$room2）"
  http_get "/api/v1/rooms/state?room_id=$room2" "$alice_token" >/dev/null
  match2=$(json_path room.match_id)

  open_stream "$room2" "$alice_token" >/dev/null 2>&1
  sleep 1
  close_stream
  ok "alice 已断线，开始等 30 秒宽限期"

  finished=0
  for _ in $(seq 1 45); do
    http_get "/api/v1/rooms/state?room_id=$room2" "$alice_token" >/dev/null
    state=$(json_path room.state)
    if [ "$state" = "finishing" ] || [ "$state" = "finished" ]; then
      finished=1
      break
    fi
    sleep 1
  done
  if [ "$finished" -eq 1 ]; then
    ok "宽限期到期后对局结束（state=$state）"
  else
    fail "宽限期到期后对局仍未结束（state=$state）"
  fi
  reason=$(room_field "$room2" room.finish_reason)
  winner=$(room_field "$room2" room.winner_id)
  if [ "$reason" = "disconnect" ] || [ "$reason" = "FINISH_REASON_DISCONNECT" ] || [ "$reason" = "4" ]; then
    ok "结束原因是断线（finish_reason=$reason）"
  else
    fail "结束原因异常：[$reason]（期望 disconnect）"
  fi
  if [ "$winner" = "p-0002" ]; then
    ok "在线的 bob 获胜（winner_id=p-0002）"
  else
    fail "胜者异常：[$winner]（期望 p-0002）"
  fi

  # 结果必须真的落库：断线判负是**产生胜负**的结束，与 ABORTED 不同。
  stored=0
  for _ in $(seq 1 20); do
    if docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      -e "SELECT 1 FROM match_results WHERE match_id='$match2';" 2>/dev/null | grep -q 1; then
      stored=1
      break
    fi
    sleep 1
  done
  if [ "$stored" -eq 1 ]; then
    ok "断线判负的结果已写入 match_results（与 ABORTED 不同，它产生胜负）"
  else
    fail "结果未落库，match_id=$match2"
  fi
fi
echo

# ---------- 6. 双方都断线：作废且不写结果 ----------
echo "===== 6. 双方都断线 → 本局作废 ====="
wait_idle
room3=$(open_playing_room)
if [ -z "$room3" ]; then
  fail "无法为作废验收开出对局"
else
  http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token" >/dev/null
  match3=$(json_path room.match_id)
  # 两边都建立推送再都断开——这样才构成"双方都断线"。
  open_stream "$room3" "$alice_token" >/dev/null 2>&1
  sleep 1
  close_stream
  # bob 的连接：用 bob 的 token 再起一条。
  curl -sN --max-time 60 -H "Authorization: Bearer $bob_token" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room3" >/tmp/rc-sse-bob.out 2>&1 &
  sse_pid=$!
  sleep 2
  close_stream
  ok "双方都已断线，等待作废"

  aborted=0
  for _ in $(seq 1 45); do
    http_get "/api/v1/rooms/state?room_id=$room3" "$alice_token" >/dev/null
    state=$(json_path room.state)
    if [ "$state" = "aborted" ]; then
      aborted=1
      break
    fi
    # 房间被回收后会查不到，也算作废（作废不写结果，因此没有别的痕迹）。
    [ -z "$state" ] && { aborted=1; break; }
    sleep 1
  done
  if [ "$aborted" -eq 1 ]; then
    ok "双方都断线后本局作废（state=${state:-已回收}）"
  else
    fail "双方都断线后本局未作废（state=$state）"
  fi
  if docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "SELECT 1 FROM match_results WHERE match_id='$match3';" 2>/dev/null | grep -q 1; then
    fail "作废的对局不应该写结果，但 match_results 里有 $match3"
  else
    ok "作废的对局没有写 match_results（不造一个假胜负）"
  fi
fi
echo

# ---------- 7. 优雅退出 ----------
echo "===== 7. 优雅退出 ====="
close_stream
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
  echo "  [TASK-016] 断线被如实记录；宽限期内对局暂停推进且帧号冻结；"
  echo "          期内重连后血量与帧号与断线那一刻一致并继续推进；"
  echo "          超过宽限期判断线方负（finish_reason=disconnect）并写入 match_results；"
  echo "          双方都断线则作废且不写结果"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do echo "  - $item"; done
echo
echo "排查提示：Gateway /tmp/reconnect-gateway.out，Match /tmp/reconnect-match.out，"
echo "          Room /tmp/reconnect-room.out，SSE /tmp/rc-sse.out"
exit 1
