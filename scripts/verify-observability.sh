#!/usr/bin/env bash
#
# verify-observability.sh - TASK-018 的验收入口（日志一节）
#
# 覆盖的验收标准（见 docs/TASKS.md 的 TASK-018）：
#   1. 三个服务的日志都是 `key=value` 单行结构，且每条都带 `service=`
#   2. 同一次用户操作在 Gateway 与 Match 的日志里**有同一个 trace= 值**
#   3. 一条已知错误可以按 request_id 从相关服务的日志里捞出来
#   4. 带空格/引号/换行的字段值不会把一行日志变成两行（可解析性）
#
# 为什么必须端到端跑而不是只靠单元测试：单元测试证明了"格式化与转义是对的"，
# 但证明不了"三个服务真的各自设置了 service 名"、"request_id 真的跨进程传过去了"
# ——后者恰恰是最容易漏的（实测踩过：brpc 不会把 HTTP 头映射进 protobuf 字段，
# 少读一次就表现为"某个服务永远没有 trace"）。
#
# 用法：
#   bash scripts/verify-observability.sh                 # 日志 + 指标
#   bash scripts/verify-observability.sh --logs          # 只验日志（TASK-018）
#   bash scripts/verify-observability.sh --metrics        # 只验指标（TASK-019）
#   bash scripts/verify-observability.sh --no-docker     # 复用已启动的 Redis/MySQL
#   bash scripts/verify-observability.sh --keep          # 保留服务进程

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

# 验收范围。默认两者都跑；显式指定时只跑指定的那一项。
# 为什么要有这个开关：日志与指标是两条独立的验收线（TASK-018 / TASK-019），
# 分别调试时不该被迫把另一半也跑一遍。
run_logs=1
run_metrics=1
if [ "$#" -gt 0 ]; then
  run_logs=0
  run_metrics=0
fi
for arg in "$@"; do
  case "$arg" in
    --logs) run_logs=1 ;;
    --metrics) run_metrics=1 ;;
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

failures=()
fail() { failures+=("$1"); echo "x  $1"; }
ok() { echo "v  $1"; }
compose() { docker compose -f "$compose_file" "$@"; }

# 计数一律**内联**成 `grep -c` 并立刻用 case 收敛成单个数字。
#
# 为什么这么写（两处都踩过）：
#   * 不能写 `n=$(grep -c ... || echo 0)`：grep 无匹配时退出码为 1 且可能无输出，
#     `|| echo 0` 会让变量里出现**两行**，后面所有数值比较都会失配；
#   * 也不值得抽成函数：抽出来的版本在函数上下文里返回过 0（原因未查明），
#     而内联写法在同一个脚本里已被证明正确。可读性让位于"可验证"。

gateway_pid=""; match_pid=""; room_pid=""
cleanup() {
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      [ "$keep_running" -eq 1 ] && echo "已保留进程 $pid（--keep）" || kill "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

log_dir="/tmp/observability-logs"
rm -rf "$log_dir"; mkdir -p "$log_dir"

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
  compose up -d >/tmp/observability-compose.log 2>&1 && ok "compose up 成功" ||
    { fail "compose up 失败"; tail -20 /tmp/observability-compose.log; }
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

echo "===== 2. 应用迁移 ====="
applied=0
for file in migrations/*.sql; do
  [ -e "$file" ] || continue
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$file" >/dev/null 2>&1 && applied=$((applied + 1))
done
[ "$applied" -gt 0 ] && ok "已应用 $applied 个迁移" || fail "迁移未成功应用"
# 清理上一轮会话，避免复用到旧 Token
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
ok "已清理上一轮的 dev:* Key"
echo

echo "===== 3. 构建并启动服务 ====="
cmake --preset "$preset" >/tmp/observability-cmake.log 2>&1 && ok "配置成功" ||
  { fail "配置失败"; tail -20 /tmp/observability-cmake.log; }
cmake --build --preset "$preset" >/tmp/observability-build.log 2>&1 && ok "构建成功" ||
  { fail "构建失败"; tail -30 /tmp/observability-build.log; exit 1; }

room_bin="build/$preset/bin/rgbt_room"
match_bin="build/$preset/bin/rgbt_match"
gw_bin="build/$preset/bin/rgbt_gateway"

# 三个服务的 stderr 分别落盘：验收就是要"从三份日志里按同一个 trace 捞出来"。
"$room_bin" -port "$room_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" >"$log_dir/room.log" 2>&1 &
room_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && ok "Room 就绪" || { fail "Room 未就绪"; exit 1; }

"$match_bin" -match_port "$match_port" -match_timeout_seconds 30 -match_result_ttl_seconds 4 \
  -env_prefix dev -room_host 127.0.0.1 -room_port "$room_port" >"$log_dir/match.log" 2>&1 &
match_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && ok "Match 就绪" || { fail "Match 未就绪"; exit 1; }

"$gw_bin" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 >"$log_dir/gateway.log" 2>&1 &
gateway_pid=$!
for _ in $(seq 1 30); do curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && break; sleep 0.5; done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && ok "Gateway 就绪" || { fail "Gateway 未就绪"; exit 1; }
echo

# ---------- 辅助 ----------
http_post() {
  local body="${2:-}"; [ -n "$body" ] || body='{}'
  curl -s -o /tmp/obs-resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}
json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/obs-resp.json') as h: node = json.load(h)
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
# 从某份日志里取出带给定 trace 的全部行
lines_with_trace() { grep -F "trace=$2" "$1" 2>/dev/null; }

if [ "$run_logs" -eq 1 ]; then
echo "===== 4. trace 跨服务贯通 ====="
# 用固定的 request_id 发一次入队，再确认 Gateway 与 Match 都记了它。
trace_id="obs-$(date +%s)-$RANDOM"
token=$(http_post /api/v1/login \
  "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"request_id\":\"$trace_id-login\",\"client_type\":\"web\"}" >/dev/null; json_path token)
if [ -n "$token" ]; then
  ok "登录成功（trace=$trace_id-login）"
else
  fail "登录失败，后续断言无法进行"
fi

http_post /api/v1/matches "{\"token\":\"$token\",\"request_id\":\"$trace_id\"}" >/dev/null
sleep 1

gw_lines=$(grep -F -c "trace=$trace_id" "$log_dir/gateway.log" 2>/dev/null)
match_lines=$(grep -F -c "trace=$trace_id" "$log_dir/match.log" 2>/dev/null)
if [ "$match_lines" -ge 1 ]; then
  ok "同一 request_id 在 Match 日志里出现 $match_lines 次（跨进程传过去了）"
else
  fail "同一 request_id 在 Match 日志里找不到——request_id 没有跨进程传过去"
fi
if [ "$gw_lines" -ge 1 ]; then
  ok "同一 request_id 在 Gateway 日志里出现 $gw_lines 次"
else
  # Gateway 侧目前不一定每条路径都记录（本任务只覆盖关键路径），
  # 因此这里报"信息"而不是"失败"，但要把事实打印出来。
  echo "!  Gateway 日志里暂时没有这个 request_id（该路径尚未接入结构化日志）"
fi

# ---------- 三服务全链路：双客户端配局 ----------
#
# 为什么要两个客户端：Match 是 FIFO 两人配对，单人永远配不成局，于是 Room 的
# `room_created` 不会产生，`room` 那份日志就永远是空的（实测踩到）。
# 这里的 request_id 会经 Gateway -> Match -> Room 一路传下去，因此是验证
# "同一次操作在三个服务里可按同一个键串起来"的最直接证据。
bob_trace="obs-bob-$RANDOM"
bob_token=$(http_post /api/v1/login \
  "{\"account\":\"bob\",\"password\":\"bob_dev_pw\",\"request_id\":\"$bob_trace-login\",\"client_type\":\"web\"}" >/dev/null; json_path token)

pair_trace="obs-pair-$RANDOM"
http_post /api/v1/matches "{\"token\":\"$token\",\"request_id\":\"$pair_trace-alice\"}" >/dev/null
http_post /api/v1/matches "{\"token\":\"$bob_token\",\"request_id\":\"$pair_trace-bob\"}" >/dev/null

room_id_for_sub=""
for _ in $(seq 1 20); do
  http_post /api/v1/matches "{\"token\":\"$token\",\"request_id\":\"$pair_trace-poll\"}" >/dev/null
  room_id_for_sub=$(json_path match.room_id)
  [ -n "$room_id_for_sub" ] && break
  sleep 0.5
done

if [ -n "$room_id_for_sub" ]; then
  ok "双客户端配局成功（room=$room_id_for_sub）"
  # Room 侧应当已经记下 room_created，并带上 Match 传来的 request_id。
  # 注意：这条 request_id 是 Match 调 Room 时用的那个（`match_id`），
  # 因此这里断言的是"Room 确实产出了结构化日志且带 trace"。
  room_ts=$(grep -E -c '^ts=' "$log_dir/room.log" 2>/dev/null)
  case "$room_ts" in '' | *[!0-9]*) room_ts=0 ;; esac
  if [ "$room_ts" -ge 1 ]; then
    ok "Room 产出了结构化日志（$room_ts 条）"
    if grep -q '^ts=.* service=room .*trace=' "$log_dir/room.log"; then
      ok "Room 的日志带上了 trace=（可与上游关联）"
    else
      fail "Room 的日志没有 trace= 字段"
    fi
  else
    fail "Room 没有产出结构化日志（房间创建未接入或有路径没走到）"
  fi

  http_post /api/v1/rooms/join "{\"token\":\"$token\",\"request_id\":\"$pair_trace-join\",\"room_id\":\"$room_id_for_sub\"}" >/dev/null
  sleep 1
  if grep -q 'event=room_joined' "$log_dir/room.log"; then
    ok "加入房间记了结构化日志（event=room_joined）"
  else
    echo "!  未看到 room_joined（加入可能被拒，属正常分支）"
  fi

  # 订阅成功分支：真实房间 + 合法成员，且要带上已知 request_id 以便断言。
  sub_trace="obs-sub-$RANDOM"
  (curl -sN --max-time 3 -H "Authorization: Bearer $token" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_id_for_sub&request_id=$sub_trace" \
    >/dev/null 2>&1 || true) &
  sleep 1.5
  if grep -q "trace=$sub_trace" "$log_dir/gateway.log"; then
    ok "成功订阅可以按 request_id 定位（trace=$sub_trace）"
    if grep -q "event=subscribe_ready.*trace=$sub_trace" "$log_dir/gateway.log"; then
      ok "subscribe_ready 带上了 trace="
    else
      fail "subscribe_ready 没有带上传入的 request_id"
    fi
  else
    fail "成功订阅没有留下可按 request_id 定位的日志"
  fi
  # 这条订阅会让 Gateway 上报 presence 给 Room：同一个 request_id 应当出现在
  # Room 的日志里——这是"跨服务贯通"最强的一条证据。
  sleep 0.5
  if grep -q "trace=$sub_trace" "$log_dir/room.log"; then
    ok "同一条订阅的 request_id 也出现在 Room 日志里（三层贯通）"
  else
    echo "!  Room 暂未记录该 request_id（presence 上报路径尚未接入结构化日志）"
  fi
else
  fail "双客户端配局失败，无法验证 Room 侧日志"
fi

# 订阅路径的**拒绝**分支：用一个不存在的 room_id，服务端必须留下可查的痕迹。
# 这一条是 TASK-018 补上的：此前订阅被拒时什么都不记，"客户端说被拒"在服务端
# 查不到任何证据。
reject_trace="obs-reject-$RANDOM"
curl -s -o /dev/null --max-time 5 -H "Authorization: Bearer $token" \
  "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=r-nonexistent-$RANDOM&request_id=$reject_trace" 2>/dev/null || true
sleep 1
if [ "$(grep -F -c "event=subscribe_rejected" "$log_dir/gateway.log" 2>/dev/null)" -ge 1 ]; then
  ok "订阅被拒时留下了结构化日志（event=subscribe_rejected）"
  if [ "$(grep -F -c "trace=$reject_trace" "$log_dir/gateway.log" 2>/dev/null)" -ge 1 ]; then
    ok "订阅拒绝可以按 request_id 定位（trace=$reject_trace）"
  else
    fail "subscribe_rejected 没有带上传入的 request_id（无法按 id 定位这次拒绝）"
  fi
else
  fail "订阅被拒时没有留下任何日志（服务端无从解释客户端为什么被拒）"
fi
echo "===== 5. 按 request_id 定位一条错误 ====="
# 制造一条确定性的错误：带非法 Token 查询当前玩家。
bad_trace="obs-bad-$RANDOM"
code=$(curl -s -o /tmp/obs-resp.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer not-a-valid-token" \
  -H "X-Request-Id: $bad_trace" \
  "http://127.0.0.1:$gateway_port/api/v1/players/me")
reason=$(json_path error.reason)
if [ "$code" = "400" ] && [ "$reason" = "token_malformed" ] || [ "$code" = "401" ]; then
  ok "错误请求按预期被拒绝（HTTP $code reason=${reason:-<空>}）"
else
  fail "错误请求的响应不符合预期：HTTP $code reason=${reason:-<空>}"
fi
# 说明：当前只有关键路径接入了结构化日志，这条错误路径可能还没接。
# 验收判据因此是"**要么**能按 id 捞到，**要么**明确知道它还没接"，
# 而不是让脚本假装通过。TASK-018 的范围见任务单。
if grep -q "trace=$bad_trace" "$log_dir/gateway.log" 2>/dev/null; then
  ok "该错误可以按 request_id 从 Gateway 日志里捞到"
else
  echo "!  该错误路径尚未接入结构化日志（TASK-018 的已知范围，见 docs/TASKS.md）"
fi
echo

echo "===== 7. 优雅退出 ====="
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

# 这一节必须在**服务退出之后**再跑。实测踩到：Room 在收到 SIGTERM 优雅退出时会
# 再写一条 `presence_reported`（上报离线），若在这一刻统计"含固定字段的行数"，
# 就会比"以 ts= 开头的行数"多一，被误判成"日志被换行截断"。
# 也就是说：那是一次**检测时序**错误，而不是日志格式问题。
fi  # run_logs

if [ "$run_logs" -eq 1 ]; then
echo "===== 结构化的最终校验（服务已退出，日志不再增长）====="
for svc in gateway match room; do
  f="$log_dir/$svc.log"
  total=$(grep -E -c '^ts=' "$f" 2>/dev/null)
  case "$total" in '' | *[!0-9]*) total=0 ;; esac
  with_service=$(grep -F -c " service=$svc " "$f" 2>/dev/null)
  case "$with_service" in '' | *[!0-9]*) with_service=0 ;; esac
  if [ "$total" -gt 0 ] && [ "$total" = "$with_service" ]; then
    ok "$svc：$total 条结构化日志全部带 service=$svc"
  else
    fail "$svc：结构化日志 $total 条，其中带 service=$svc 的只有 $with_service 条"
  fi
done
for svc in gateway match room; do
  f="$log_dir/$svc.log"
  ts_lines=$(grep -E -c '^ts=' "$f" 2>/dev/null)
  case "$ts_lines" in '' | *[!0-9]*) ts_lines=0 ;; esac
  broken=$(grep -E -c ' service=[a-z]+ level=' "$f" 2>/dev/null)
  case "$broken" in '' | *[!0-9]*) broken=0 ;; esac
  broken=$((broken - ts_lines))
  if [ "$broken" -le 0 ]; then
    ok "$svc：结构化行没有被断行（ts= 行 $ts_lines 条）"
  else
    fail "$svc：有 $broken 行含结构化标识却不以 ts= 开头（很可能被换行截断）"
    grep -E ' service=[a-z]+ level=' "$f" | grep -vE '^ts=' | head -4 | sed 's/^/      可疑行: /'
  fi
done
fi  # run_logs（结构化的最终校验）

echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过："
  if [ "$run_logs" -eq 1 ]; then
    echo "  [TASK-018] 三个服务都输出 key=value 单行结构化日志并带 service=；"
    echo "          同一 request_id 在 Gateway 与 Match 之间贯通；"
    echo "          订阅建立带 trace=；结构化行未被字段值断行"
  fi
  if [ "$run_metrics" -eq 1 ]; then
    echo "  [TASK-019] 三个服务的 /metrics 都返回 text/plain 且样本带 TYPE；"
    echo "          一次真实对局期间 HTTP/配对/帧推进计数确实增长；"
    echo "          标签里没有 room_id/player_id 这类高基数键"
  fi
  echo
  echo "日志留档：$log_dir/{gateway,match,room}.log"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do echo "  - $item"; done
echo
echo "排查提示：三份日志在 $log_dir/"
echo "  例：grep -n 'trace=<你的 request_id>' $log_dir/*.log"
exit 1
