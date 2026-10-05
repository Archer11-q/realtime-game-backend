#!/usr/bin/env bash
#
# ⚠ 本脚本**不在** `scripts/verify-all.sh` 里（故障注入 / 长稳 / 重连这一类会占标准
#   端口、停依赖或跑很久，不适合放进快速门禁）。**改动 `src/` 下的产品代码之后，
#   请跑 `bash scripts/verify-chaos.sh`** —— 那是这一类脚本的统一入口。
#   为什么必须写这一句：TASK-026 曾漏改本类里的一个脚本（它不在任何统一入口里，
#   于是回归躺了整整一个任务周期才被 TASK-029 的补跑发现），详见
#   `docs/devlog.md` 的「TASK-029 之后的重跑结果」。
#
# chaos/verify-process-crash.sh - TASK-024：进程崩溃与恢复边界（kill -9）
#
# 本脚本只做**进程崩溃**这一类故障：对 Room / Match / Gateway 各发一次 kill -9，
# 实测「恢复时间」与「数据丢失边界」，并把「丢失了什么」**逐项列出**——而不是
# 只报一句「恢复成功」。Phase 4 的退出标准要的是「每个场景都有实测的检测时间、
# 恢复时间和数据丢失边界」，因此下面的每个场景都给出数字或明确的「无丢失」。
#
# 与 TASK-023 的关系
# -----------------
# TASK-023 交付了 chaos/lib.sh（注入原语 + 检测/恢复时间测量 + 断言汇总）与
# chaos/verify-dependency-down.sh（依赖不可用）。本脚本**复用 lib.sh 的测量原语**，
# 只新增「进程崩溃」这一族场景，不重复实现计时与断言。
#
# 为什么只用一条中继（而不是像 TASK-023 那样四条）
# ----------------------------------------------
# 崩溃注入本身不需要切断依赖：三个服务直连真实的 Redis / MySQL 即可。
# 唯一例外是第 2b 节「重启时存储不可用」——要测那条边界，必须**只**切断 Room 的
# MySQL，所以只有 Room 走 chaos/relay.py（13306 -> 3306）。
# 直接 `docker stop rgbt-mysql` 会同时打掉 Gateway 的玩家档案通道（TASK-015 与
# TASK-023 都踩过这个坑），那样测到的就不是本场景了。
#
# 时间口径（沿用 TASK-023 的「检测/恢复」两个量，但锚点必须说清楚）
# ----------------------------------------------------------------
#   检测时间 = 崩溃时刻 -> 首次观测到预期失败（503 / 连接断开）
#   恢复时间 = **发起重启（或依赖恢复）** -> 业务重新可用。含进程重启本身，
#              也含客户端侧那一段（brpc 长连接重建 + 首次成功调用）。
#   为什么不从崩溃时刻起算：崩溃到夹具发起重启之间是**夹具在观测**
#   （等端口释放、探测失败、读最后一次快照）。这段由脚本自己计时并打印
#   （本机实测约 0.2 s），量不大，但它的长短取决于夹具怎么写，
#   不该混进「服务多久恢复」里。因此脚本把两段分开打印，并把
#   「崩溃 -> 可用（含夹具观测）」作为参考值一并给出，不隐藏。
#   时刻一律用 bash 的 EPOCHREALTIME（lib.sh 的 now_us）；
#   本机 `date +%s%3N` 不是真实毫秒（TASK-023 实测），因此不用 date。
#
# 四个场景与各自的判据
# -------------------
#   1. Room：对局进行中崩溃
#      - 重启后房间是否仍在（room_id 不变、状态仍是 playing）
#      - 回退了多少帧（对照 TASK-014 的「上界 10 帧」）与丢失窗口（毫秒）
#      - 恢复点是否**精确等于**最后一次快照（读 Room 启动日志的 room_restored）
#      - 血量是否与快照一致、恢复后能否继续推进并打完整局
#      - match_results 是否恰好 1 行（崩溃+重启不能破坏幂等）
#   2. Room：FINISHING（结果待落库）崩溃
#      - 重启后是否被重新纳入落库重试、结果是否落库（TASK-008 已知限制的正解）
#   2b. 已知边界：**重启时 MySQL 不可用** -> 一个房间都不恢复（如实 load_failed）
#   3. Match：排队中崩溃
#      - 3a 停机时间在超时内 -> 排队状态是否从 Redis 快照恢复
#      - 3b 停机时间超过超时 -> 该条目按 TASK-015 语义不再恢复（实测语义，不是缺陷）
#   4. Gateway：崩溃
#      - SSE 订阅是否全部丢失（订阅表在进程内存里，客户端必须重连）
#      - 会话是否不受影响（在 Redis）、对局是否不受影响（Room 独立推进）
#      - 实测「崩溃 -> 重新订阅成功」的时间
#
# 用法：
#   bash chaos/verify-process-crash.sh
#   bash chaos/verify-process-crash.sh --keep    # 保留服务与中继，便于排查
#
# 退出码 0 表示四个场景全部通过；非 0 表示有失败项，失败项会逐条列出。
# 前置条件：Docker 的 rgbt-redis / rgbt-mysql 健康，且 4 个端口空闲
# （先执行 bash scripts/dev-down.sh）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

# shellcheck source=chaos/lib.sh
source chaos/lib.sh

preset="brpc-debug"
gateway_port=8080
match_port=8082
room_port=8083

# 只给 Room 的 MySQL 通道建中继（见头部说明）。
relay_mysql_for_room_port=13306

keep=0
for arg in "$@"; do
  case "$arg" in
    --keep) keep=1 ;;
    -h | --help) sed -n '3,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$arg" >&2; exit 2 ;;
  esac
done

env_file="deploy/compose/.env"
[ -f "$env_file" ] || { echo "x  缺少 $env_file，请先执行 bash scripts/dev-up.sh" >&2; exit 1; }
set -a
# shellcheck disable=SC1090
. "./$env_file"
set +a

redis_port="${REDIS_PORT:-6379}"
mysql_port="${MYSQL_PORT:-3306}"

CHAOS_RUN_DIR="$repo_root/.run"
CHAOS_LOG_DIR="$repo_root/.run/chaos"
mkdir -p "$CHAOS_LOG_DIR"
rm -f "$CHAOS_LOG_DIR"/*.log 2>/dev/null || true
CHAOS_RESP_FILE="$CHAOS_LOG_DIR/last-response.json"

# chaos/lib.sh 约定的端口变量名（大写）。
GW_PORT="$gateway_port"
MATCH_PORT="$match_port"

PROBE_ACCOUNT="alice"
PROBE_PASSWORD="alice_dev_pw"

gateway_pid=""
match_pid=""
room_pid=""
stream_pid=""

# ---------------------------------------------------------------------------
# 数据丢失边界汇总（本脚本特有的输出）
# ---------------------------------------------------------------------------
# 任务单要求每个场景记录「丢失了什么，逐项列出」。计时用 lib.sh 的 record_time，
# 丢失项用这张表；两者分开是因为它们衡量的东西不同（毫秒 vs 条目/帧）。
CHAOS_LOSSES=()
record_loss() { # <场景> <丢失项> <说明>
  CHAOS_LOSSES+=("$1|$2|$3")
}

print_loss_table() {
  echo
  echo "===== 数据丢失边界汇总（本机实测） ====="
  printf '%-40s %-28s %s\n' "场景" "丢失了什么" "说明"
  printf '%-40s %-28s %s\n' "----------------------------------------" \
    "----------------------------" "----"
  local row scene item note
  for row in "${CHAOS_LOSSES[@]}"; do
    IFS='|' read -r scene item note <<<"$row"
    printf '%-40s %-28s %s\n' "$scene" "$item" "$note"
  done
}

wall_clock() { date -u '+%H:%M:%S'; }

# ---------------------------------------------------------------------------
# 启停
# ---------------------------------------------------------------------------
kill_pid() {
  local pid="$1"
  [ -n "$pid" ] || return 0
  kill -0 "$pid" 2>/dev/null || return 0
  kill -TERM "$pid" 2>/dev/null || true
  local i
  for i in $(seq 1 40); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null || true
}

stop_all() {
  [ -n "$stream_pid" ] && kill_pid "$stream_pid"
  stream_pid=""
  kill_pid "$room_pid"
  kill_pid "$match_pid"
  kill_pid "$gateway_pid"
  gateway_pid=""; match_pid=""; room_pid=""
  # pidfile 命名必须与 start_relay 一致：它写的是 `relay-$name.pid`，而本脚本传进去的
  # name 本身已经带 `relay-` 前缀，所以文件名是 `relay-relay-mysql-room.pid`。
  # 本轮实测踩到：早先这里写成 `relay-mysql-room.pid`（少一层前缀），于是 pidfile
  # 永远找不到、中继永远不被回收。它在同一条 WSL 会话里会一直占着 13306，
  # 让**同一会话内的下一轮**验收卡在前置检查上；会话结束（SIGHUP）后它才会死，
  # 所以单次运行看不出来。教训：清理逻辑的命名要和创建逻辑对齐，并且要用
  # "同一会话连跑两轮"来验证。
  local name pidfile
  for name in relay-mysql-room; do
    pidfile="$CHAOS_RUN_DIR/relay-$name.pid"
    if [ -f "$pidfile" ]; then
      kill_pid "$(cat "$pidfile")"
      rm -f "$pidfile"
    fi
  done
  # 兜底：确认中继端口真的释放了。释放不了说明还有别的进程占着，
  # 那不是"退出时顺便清理一下"的问题，必须让下一轮的前置检查说出来。
  wait_port_released "$relay_mysql_for_room_port" 5 || true
}

cleanup() {
  # 显式保存并恢复退出码，不依赖 EXIT trap 的退出码语义。
  #
  # 起因（如实记录）：本轮一度观察到「前置检查失败 -> 打印失败清单 -> 退出码却是 0」，
  # 于是做了 7 组受控实验（外部进程占端口、子 shell 启动占端口、残留 pidfile、
  # pidfile 指向活进程、显式恢复退出码等），**全部实测返回 1，无法复现**。
  # 因此这里既不下「门禁已损坏」的结论，也不留着这个不确定性：
  # 验收脚本的退出码本身就是门禁，必须自己说了算。
  local rc=$?
  if [ "$keep" -eq 1 ]; then
    echo "（--keep：保留服务与中继进程，便于排查）"
    exit "$rc"
  fi
  stop_all
  exit "$rc"
}
trap cleanup EXIT

wait_http() { # <端口> [超时秒数]
  local port="$1"
  local wait_seconds="${2:-30}"
  local deadline=$(( $(now_ms) + wait_seconds * 1000 ))
  while [ "$(now_ms)" -lt "$deadline" ]; do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$port/health" && return 0
    sleep 0.5
  done
  return 1
}

# 日志一律**追加**写：本脚本会反复重启进程，追加 + 行号标记（log_mark /
# log_has_since）才能区分「这次重启前就有」与「这次重启后才出现」。
start_gateway() {
  "build/$preset/bin/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
    -redis_host 127.0.0.1 -redis_port "$redis_port" -redis_timeout_ms 500 \
    -mysql_host 127.0.0.1 -mysql_port "$mysql_port" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    -match_host 127.0.0.1 -match_port "$match_port" \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -match_timeout_ms 500 -room_timeout_ms 500 \
    -stream_poll_interval_ms 100 -stream_heartbeat_interval_ms 500 \
    >>"$CHAOS_LOG_DIR/gateway.log" 2>&1 &
  gateway_pid=$!
  wait_http "$gateway_port" 30
}

start_match() { # [排队超时秒数]
  local match_timeout_seconds="${1:-30}"
  "build/$preset/bin/rgbt_match" -match_port "$match_port" \
    -match_timeout_seconds "$match_timeout_seconds" -match_result_ttl_seconds 120 \
    -env_prefix dev -redis_host 127.0.0.1 -redis_port "$redis_port" \
    -redis_timeout_ms 500 \
    -room_host 127.0.0.1 -room_port "$room_port" \
    >>"$CHAOS_LOG_DIR/match.log" 2>&1 &
  match_pid=$!
  wait_http "$match_port" 30
}

# Room 的 MySQL 走中继：第 2b 节要单独切断它。
start_room() {
  "build/$preset/bin/rgbt_room" -port "$room_port" -env_prefix dev \
    -mysql_host 127.0.0.1 -mysql_port "$relay_mysql_for_room_port" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    >>"$CHAOS_LOG_DIR/room.log" 2>&1 &
  room_pid=$!
  wait_http "$room_port" 30
}

# ---------------------------------------------------------------------------
# 查询辅助
# ---------------------------------------------------------------------------
mysql_exec() { # <SQL>
  docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "$1" 2>/dev/null
}

sql_scalar() { mysql_exec "$1" | tr -d '\r'; }

sql_room_field() { # <列名> <match_id>
  sql_scalar "SELECT $1 FROM rooms WHERE match_id='$2';"
}

result_row_count() { # <match_id>
  local n
  n="$(sql_scalar "SELECT COUNT(*) FROM match_results WHERE match_id='$1';")"
  echo "${n:-0}"
}

result_dup_groups() { # <match_id>
  local n
  n="$(sql_scalar "SELECT COUNT(*) FROM (SELECT match_id FROM match_results WHERE match_id='$1' GROUP BY match_id HAVING COUNT(*)>1) x;")"
  echo "${n:-0}"
}

# 从 CHAOS_RESP_FILE 里按 player_id 取血量。房间快照里 players 的顺序不保证与
# rooms 表的 p1/p2 一致，因此按 id 对齐而不是按下标对齐。
room_hp_of() { # <player_id>
  python3 - "$1" "$CHAOS_RESP_FILE" <<'PY'
import json, sys
want = sys.argv[1]
try:
    room = json.load(open(sys.argv[2], encoding='utf-8')).get('room', {})
except Exception:
    print('')
    raise SystemExit(0)
for player in room.get('players', []):
    if player.get('player_id') == want:
        print(player.get('hp', ''))
        raise SystemExit(0)
print('')
PY
}

# 读某个服务 /metrics 上某个指标的最后一段数值。
metric_value() { # <端口> <指标名>
  curl -s --max-time 4 "http://127.0.0.1:$1/metrics" 2>/dev/null |
    python3 -c '
import sys
name = sys.argv[1]
value = ""
for line in sys.stdin:
    if line.startswith(name + " ") or line.startswith(name + "{"):
        parts = line.split()
        if len(parts) >= 2:
            value = parts[-1]
print(value)
' "$2"
}

# ---------------------------------------------------------------------------
# 业务辅助（与 chaos/verify-dependency-down.sh 同构。两个脚本各自独立：它们依赖
# 具体端口与账号，属场景层，不进只放原语的 lib.sh）
# ---------------------------------------------------------------------------
login_as() { # <账号> [请求 id 后缀]
  local account="$1" suffix="${2:-$RANDOM}"
  CHAOS_LAST_STATUS="$(http_post /api/v1/login "" \
    "{\"account\":\"$account\",\"password\":\"${account}_dev_pw\",\"client_type\":\"web\",\"request_id\":\"crash-$account-$suffix\"}")"
  LOGIN_TOKEN="$(jq_path token)"
  LOGIN_HTTP="$CHAOS_LAST_STATUS"
  return 0
}

match_queue_len() {
  docker exec rgbt-redis redis-cli LLEN "dev:match:queue" 2>/dev/null | tr -d '\r'
}

queue_snapshot_present() {
  local n="${1:-$(match_queue_len)}"
  [ "${n:-0}" -ge 1 ] 2>/dev/null
}

# 清空 Match 的队列与配对结果并重启它。理由与 TASK-023 相同：结果 TTL 是 120 秒，
# GetStatus 优先返回「最近一次配对结果」，不清就会出现「新一段拿到上一段的房间」。
reset_match_state() { # <说明文字>
  kill_pid "$match_pid"
  match_pid=""
  docker exec rgbt-redis redis-cli DEL "dev:match:queue" >/dev/null 2>&1
  local key
  for key in $(docker exec rgbt-redis redis-cli --scan --pattern 'dev:match:*result*' 2>/dev/null); do
    docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1
  done
  if start_match 30; then
    chaos_info "已重置匹配状态（$1）：队列与配对结果清空，Match 重启完成"
    warm_match_channel
    return 0
  fi
  chaos_fail "匹配状态重置失败（$1）：Match 未能重新就绪"
  return 1
}

# 预热 Gateway -> Match 的 brpc 通道：Match 重启后 Gateway 手上的长连接已死，
# 但 brpc 不会立刻知道，紧接的第一次调用会拿到 match_unavailable（服务其实是好的）。
warm_match_channel() {
  local i
  login_as alice warm
  if [ "$LOGIN_HTTP" != "200" ]; then
    chaos_fail "预热 Gateway->Match 通道失败：登录返回 HTTP $LOGIN_HTTP"
    return 1
  fi
  for i in $(seq 1 20); do
    if [ "$(http_get /api/v1/matches/current "$LOGIN_TOKEN")" = "200" ]; then
      return 0
    fi
    sleep 0.25
  done
  chaos_fail "预热 Gateway->Match 通道失败：预热后仍返回 503 match_unavailable"
  return 1
}

# 预热 Gateway -> Room 的 brpc 通道并等到房间状态可查（Room 重启后同理）。
warm_room_channel() { # <token> <room_id> [超时毫秒]
  local timeout_ms="${3:-12000}"
  CHAOS_WAIT_TIMEOUT_MS="$timeout_ms" wait_until probe_room_state_ok "$1" "$2" >/dev/null
}

pair_two() { # <tokenA> <tokenB> <标签>
  local token_a="$1" token_b="$2" tag="$3"
  PAIR_MATCH_ID=""; PAIR_ROOM_ID=""; PAIR_A_CODE=""
  enqueue_with_retry "$token_a" "crash-$tag-a" || return 1
  enqueue_with_retry "$token_b" "crash-$tag-b" || return 1
  local i code status room_id match_id
  for i in $(seq 1 60); do
    code="$(http_get /api/v1/matches/current "$token_a")"
    PAIR_A_CODE="$code"
    status="$(jq_path match.state)"
    room_id="$(jq_path match.room_id)"
    match_id="$(jq_path match.match_id)"
    if [ "$code" = "200" ] && [ "$status" = "matched" ] && [ -n "$room_id" ] && [ -n "$match_id" ]; then
      PAIR_ROOM_ID="$room_id"
      PAIR_MATCH_ID="$match_id"
      chaos_info "[配对 $tag] room=$room_id match=$match_id"
      if ! join_room "$room_id" "$token_a" "j-$tag-a" || ! join_room "$room_id" "$token_b" "j-$tag-b"; then
        return 1
      fi
      return 0
    fi
    sleep 0.5
  done
  return 1
}

join_room() { # <room_id> <token> <请求 id 后缀>
  local code
  code="$(http_post /api/v1/rooms/join "$2" \
    "{\"room_id\":\"$1\",\"request_id\":\"crash-$3\"}")"
  [ "$code" = "200" ] && return 0
  chaos_info "加入房间失败：room=$1 code=$code reason=$(jq_path error.reason)"
  return 1
}

room_snapshot() { # <room_id> <token>  -> 设置 ROOM_FRAME / ROOM_STATE
  ROOM_FRAME=""; ROOM_STATE=""
  local code
  code="$(http_get "/api/v1/rooms/state?room_id=$1" "$2")"
  [ "$code" = "200" ] || return 1
  ROOM_FRAME="$(jq_path room.frame)"
  ROOM_STATE="$(jq_path room.state)"
  return 0
}

room_frame_at_least() { # <room_id> <token> <帧号>
  room_snapshot "$1" "$2" || return 1
  [ -n "$ROOM_FRAME" ] || return 1
  [ "$ROOM_FRAME" -ge "$3" ] 2>/dev/null
}

room_playing() { # <room_id> <token>
  room_snapshot "$1" "$2" || return 1
  [ "$ROOM_STATE" = "playing" ]
}

room_finished() { # <room_id> <token>
  room_snapshot "$1" "$2" || return 1
  case "$ROOM_STATE" in
    finished|aborted|finishing) return 0 ;;
    *) return 1 ;;
  esac
}

probe_room_state_ok() { local code; code="$(http_get "/api/v1/rooms/state?room_id=$2" "$1")"; [ "$code" = "200" ]; }
probe_room_state_fails() { local code; code="$(http_get "/api/v1/rooms/state?room_id=$2" "$1")"; [ "$code" != "200" ]; }
probe_result_ok() { local code; code="$(http_get "/api/v1/results?match_id=$2" "$1")"; [ "$code" = "200" ]; }
probe_match_queued() { # <token>
  local code
  code="$(http_get /api/v1/matches/current "$1")"
  [ "$code" = "200" ] && [ "$(jq_path match.state)" = "queued" ]
}
probe_gw_down() {
  [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
    "http://127.0.0.1:$GW_PORT/health" 2>/dev/null)" != "200" ]
}
probe_process_gone() { ! kill -0 "$1" 2>/dev/null; }

# 从**指定行号之后**的 Room 日志里取恢复点帧号（TASK-018 结构化的
# `event=room_restored ... room=<id> frame=N phase=<p>`；frame 在 room 之后）。
room_restored_frame_since() { # <日志起始行号> <room_id>
  local mark="$1" room="$2"
  tail -n "+$(( mark + 1 ))" "$CHAOS_LOG_DIR/room.log" 2>/dev/null |
    grep -E "event=room_restored .*room=$room( |$)" | tail -1 |
    grep -oE ' frame=[0-9]+' | tail -1 | cut -d= -f2
}

# SSE 订阅：把长连接落到文件，便于断言事件与「连接是否断开」。
sse_subscribe() { # <token> <room_id> <输出文件> [最长秒数]
  local token="$1" room_id="$2" file="$3" max_time="${4:-60}"
  : >"$file"
  curl -sN --max-time "$max_time" -H "Authorization: Bearer $token" \
    "http://127.0.0.1:$GW_PORT/api/v1/stream?room_id=$room_id" >"$file" 2>&1 &
  stream_pid=$!
}

sse_wait_event() { # <文件> <模式> [尝试次数]
  local file="$1" pattern="$2" tries="${3:-40}" i
  for i in $(seq 1 "$tries"); do
    grep -q "$pattern" "$file" 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}

# 两个客户端轮流攻击直到对局结束（request_id 必须带 room_id，否则会被幂等丢弃）。
play_game() { # <room_id> <tokenA> <tokenB> [最多回合]
  local room_id="$1" token_a="$2" token_b="$3" max_rounds="${4:-40}"
  local round frame target tag
  tag="${room_id: -8}"
  for round in $(seq 1 "$max_rounds"); do
    if [ $(( round % 2 )) -eq 1 ]; then
      http_post /api/v1/rooms/input "$token_a" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"crash-atk-$tag-a$round\"}" >/dev/null
    else
      http_post /api/v1/rooms/input "$token_b" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"crash-atk-$tag-b$round\"}" >/dev/null
    fi
    if room_snapshot "$room_id" "$token_a"; then
      [ "$ROOM_STATE" = "finished" ] && { PLAY_GAME_RESULT="finished"; return 0; }
      frame="$ROOM_FRAME"
      target=$(( frame + 2 ))
      wait_until room_frame_at_least "$room_id" "$token_a" "$target" >/dev/null
    fi
  done
  PLAY_GAME_RESULT="timeout"
  return 0
}

# ---------------------------------------------------------------------------
# 0. 前置检查
# ---------------------------------------------------------------------------
chaos_step "0. 前置检查"

command -v docker >/dev/null 2>&1 || { echo "x  缺少 docker" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "x  Docker 服务端不可达，请先启动 Docker Desktop" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "x  缺少 python3（中继与 JSON 解析需要）" >&2; exit 1; }

for container in rgbt-redis rgbt-mysql; do
  if dependency_up "$container"; then
    chaos_ok "$container healthy"
  else
    chaos_fail "$container 不健康：本脚本需要真实的 Redis 与 MySQL 在跑"
  fi
done
if chaos_has_failures; then
  chaos_print_failures || true
  echo "请先执行：bash scripts/dev-up.sh（它会起 Redis/MySQL、应用迁移并构建）"
  exit 1
fi

for port in "$gateway_port" "$match_port" "$room_port" "$relay_mysql_for_room_port"; do
  if port_listening "$port"; then
    chaos_fail "端口 $port 已被占用，先执行 bash scripts/dev-down.sh"
  fi
done
if chaos_has_failures; then
  chaos_print_failures || true
  exit 1
fi
chaos_ok "4 个端口（3 个服务 + 1 个中继）均空闲"

if cmake --preset "$preset" >"$CHAOS_LOG_DIR/cmake-configure.log" 2>&1; then
  chaos_ok "cmake 配置成功"
else
  chaos_fail "cmake 配置失败，见 $CHAOS_LOG_DIR/cmake-configure.log"
  chaos_print_failures || true
  exit 1
fi
if cmake --build --preset "$preset" >"$CHAOS_LOG_DIR/cmake-build.log" 2>&1; then
  chaos_ok "构建成功"
else
  chaos_fail "构建失败，见 $CHAOS_LOG_DIR/cmake-build.log"
  chaos_print_failures || true
  exit 1
fi
for bin in rgbt_gateway rgbt_match rgbt_room; do
  [ -x "build/$preset/bin/$bin" ] || chaos_fail "缺少可执行文件 build/$preset/bin/$bin"
done
if chaos_has_failures; then chaos_print_failures || true; exit 1; fi

# 清掉上一轮开发状态（只清 dev 前缀与两张表，不动 migrations/004 的种子身份）。
chaos_step "0b. 清理上一轮开发状态"
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
mysql_exec "DELETE FROM rooms; DELETE FROM match_results;" >/dev/null 2>&1 || true
chaos_ok "已清理 Redis 的 dev:* 与 rooms / match_results"

# ---------------------------------------------------------------------------
# 0c. 中继与启动
# ---------------------------------------------------------------------------
chaos_step "0c. 建立 Room-MySQL 中继并启动三个服务"

start_relay relay-mysql-room "$relay_mysql_for_room_port" "127.0.0.1:$mysql_port" || true
probe_mysql 127.0.0.1 "$relay_mysql_for_room_port" && chaos_ok "经中继的 MySQL 握手成功" ||
  chaos_fail "经中继的 MySQL 握手失败：中继不是透明的，第 2b 节无意义"

start_room && chaos_ok "Room 就绪（MySQL 经中继 $relay_mysql_for_room_port）" || chaos_fail "Room 未就绪"
start_match 30 && chaos_ok "Match 就绪" || chaos_fail "Match 未就绪"
start_gateway && chaos_ok "Gateway 就绪" || chaos_fail "Gateway 未就绪"

if chaos_has_failures; then
  tail -20 "$CHAOS_LOG_DIR/room.log" 2>/dev/null | sed 's/^/     room: /'
  tail -20 "$CHAOS_LOG_DIR/match.log" 2>/dev/null | sed 's/^/     match: /'
  tail -20 "$CHAOS_LOG_DIR/gateway.log" 2>/dev/null | sed 's/^/     gateway: /'
  chaos_print_failures || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. Room：对局进行中崩溃
# ---------------------------------------------------------------------------
chaos_step "1. Room：对局进行中 kill -9"

reset_match_state "场景 1 开始前" || true

login_as alice c1-a
alice_token="$LOGIN_TOKEN"
login_as bob c1-b
bob_token="$LOGIN_TOKEN"
if [ "$LOGIN_HTTP" != "200" ] || [ -z "$alice_token" ] || [ -z "$bob_token" ]; then
  chaos_fail "场景 1：登录失败（alice=[$alice_token] bob=[$bob_token]）"
fi

c1_room_id=""
c1_match_id=""
if [ -n "$alice_token" ] && [ -n "$bob_token" ]; then
  if pair_two "$alice_token" "$bob_token" c1; then
    c1_room_id="$PAIR_ROOM_ID"
    c1_match_id="$PAIR_MATCH_ID"
    chaos_ok "场景 1：配对成功 room=$c1_room_id match=$c1_match_id"
  else
    chaos_fail "场景 1：配对失败（HTTP $PAIR_A_CODE state=$(jq_path match.state)）"
  fi
fi

if [ -n "$c1_room_id" ]; then
  if wait_until room_playing "$c1_room_id" "$alice_token" >/dev/null; then
    chaos_ok "场景 1：双方进房，对局进入 playing"
  else
    chaos_fail "场景 1：对局未进入 playing（状态 ${ROOM_STATE:-?}）"
  fi

  # 让它多跑几帧，确保「最后一次快照」与「崩溃时的实时状态」之间确实有差距，
  # 否则丢帧量恒为 0，断言就退化成什么都没验证。
  sleep 3

  room_snapshot "$c1_room_id" "$alice_token" || true
  frame_before="${ROOM_FRAME:-0}"

  crash_clock="$(wall_clock)"
  crash_ms="$(now_ms)"
  crash_us="$(now_us)"

  chaos_info "崩溃前状态：room=$c1_room_id 状态=${ROOM_STATE:-?} 实时帧=$frame_before"

  kill -9 "$room_pid" 2>/dev/null || true
  wait "$room_pid" 2>/dev/null
  room_pid=""
  wait_port_released "$room_port" 10
  chaos_ok "已 kill -9 Room（崩溃时刻 $crash_clock）"

  # 时间一律锚定在**崩溃时刻**，而不是「探针启动时刻」：wait_until 只返回它自己的
  # 轮询耗时，直接拿它当结果会把「崩溃 -> 探针启动」这一段漏掉，数字偏小。
  detect_probe_ms="$(wait_until probe_room_state_fails "$alice_token" "$c1_room_id" || true)"
  if [ -n "$detect_probe_ms" ]; then
    detect_ms="$(elapsed_ms_from_us "$crash_us")"
    chaos_ok "检测：崩溃后查询首次失败，用时 ${detect_ms} ms"
    record_time "场景 1：Room 崩溃（对局中）" "检测" "$detect_ms" "kill -9 到查询首次失败"
    check_http "场景 1：崩溃期间房间查询的错误码" GET \
      "/api/v1/rooms/state?room_id=$c1_room_id" "$alice_token" "" 503 "room_unavailable" || true
  else
    chaos_fail "场景 1：崩溃后 12 秒内房间查询未失败（进程可能没真的死）"
  fi

  # 崩溃后读快照：进程已死，不会再写，这一行就是**恢复点**。
  #
  # 只发**一次** MySQL 查询（CONCAT_WS 一次取六个字段）：夹具这类观测动作
  # 合计约 0.2 s（脚本会把实测值打印出来）。它本来就不计入恢复时间
  # （恢复时间从发起重启起算），少发几次只是让"夹具观测"这一段更短、更好解释。
  frame_snapshot=""; snapshot_at_ms=""; p1_id=""; p1_hp_snap=""; p2_id=""; p2_hp_snap=""
  snap_row="$(sql_scalar "SELECT CONCAT_WS('|', frame, snapshot_at_ms, p1_id, p1_hp, p2_id, p2_hp) FROM rooms WHERE match_id='$c1_match_id';")"
  IFS='|' read -r frame_snapshot snapshot_at_ms p1_id p1_hp_snap p2_id p2_hp_snap <<<"$snap_row"

  restart_mark_room="$(log_mark room.log)"
  fixture_ms="$(elapsed_ms_from_us "$crash_us")"
  restart_us="$(now_us)"
  if start_room; then
    restart_ms=$(( ($(now_us) - restart_us) / 1000 ))
    chaos_ok "Room 已重启（进程号 $room_pid，重启进程自身用时 ${restart_ms} ms）"
  else
    restart_ms=""
    chaos_fail "场景 1：Room 未能重新就绪（崩溃后进程必须能再起来）"
  fi

  recover_probe_ms="$(wait_until probe_room_state_ok "$alice_token" "$c1_room_id" || true)"
  recover_clock="$(wall_clock)"
  if [ -n "$recover_probe_ms" ]; then
    recover_ms="$(elapsed_ms_from_us "$restart_us")"
    total_ms="$(elapsed_ms_from_us "$crash_us")"
    chaos_info "场景 1：夹具在崩溃后花在观测上的时间 ${fixture_ms} ms（等端口释放 + 探测失败 + 一次快照查询，不计入恢复时间）"
    chaos_ok "恢复：房间状态重新可查（HTTP 200），发起重启到可用 ${recover_ms} ms（其中重启进程 ${restart_ms:-?} ms，重启后客户端侧重建 $(( recover_ms - ${restart_ms:-0} )) ms，恢复时刻 $recover_clock）"
    chaos_info "场景 1：把夹具观测也算进去，崩溃到可用共 ${total_ms} ms（参考值）"
    record_time "场景 1：Room 崩溃（对局中）" "恢复" "$recover_ms" "发起重启到房间状态可查（含重启进程 ${restart_ms:-?} ms）"
  else
    chaos_fail "场景 1：重启后 12 秒内房间状态仍不可查"
  fi

  # --- 恢复出的状态是否与快照一致 ---
  if room_snapshot "$c1_room_id" "$alice_token"; then
    frame_after="${ROOM_FRAME:-0}"
    if [ "$ROOM_STATE" = "playing" ]; then
      chaos_ok "场景 1：房间仍在且状态为 playing（room_id 未变）"
    else
      chaos_fail "场景 1：恢复后状态异常：${ROOM_STATE:-?}（期望 playing）"
    fi

    restored_frame="$(room_restored_frame_since "$restart_mark_room" "$c1_room_id")"
    if [ -n "$restored_frame" ] && [ "$restored_frame" = "$frame_snapshot" ]; then
      chaos_ok "场景 1：恢复点精确等于最后一次快照帧（$frame_snapshot）"
    else
      chaos_fail "场景 1：恢复点不等于最后快照：日志恢复点 [${restored_frame:-未记录}]，快照 [$frame_snapshot]"
    fi

    hp_after_p1="$(room_hp_of "$p1_id")"
    hp_after_p2="$(room_hp_of "$p2_id")"
    if [ "$hp_after_p1" = "$p1_hp_snap" ] && [ "$hp_after_p2" = "$p2_hp_snap" ]; then
      chaos_ok "场景 1：双方血量与快照一致（$p1_id=$hp_after_p1，$p2_id=$hp_after_p2）"
    else
      chaos_fail "场景 1：血量与快照不一致：快照($p1_id=$p1_hp_snap,$p2_id=$p2_hp_snap) 恢复后($p1_id=$hp_after_p1,$p2_id=$hp_after_p2)"
    fi

    # 回退量：用「崩溃前实时帧 - 恢复点」度量（下界，因为 kill 之前还会再推进几帧）。
    frames_lost=$(( frame_before - ${frame_snapshot:-0} ))
    frames_lost_measured="$frames_lost"
    if [ "$frames_lost" -ge -2 ] 2>/dev/null && [ "$frames_lost" -le 10 ]; then
      chaos_ok "场景 1：回退帧数实测 $frames_lost 帧（上界 10 帧 = 一个快照间隔）"
      record_loss "场景 1：Room 对局中崩溃" "自最后快照之后推进的帧" \
        "实测 $frames_lost 帧（上界 10 帧，约 1 秒进度）"
    else
      chaos_fail "场景 1：回退帧数超出预期：$frames_lost 帧（崩溃前实时 $frame_before，恢复点 $frame_snapshot）"
    fi

    # 丢失窗口：恢复点快照的写入时刻到崩溃时刻。它比「帧数」更直接，
    # 不受采样时机影响。
    if [ -n "$snapshot_at_ms" ] && [ "${snapshot_at_ms:-0}" -gt 0 ] 2>/dev/null; then
      loss_window_ms=$(( crash_ms - snapshot_at_ms ))
      if [ "$loss_window_ms" -ge 0 ] && [ "$loss_window_ms" -le 1200 ]; then
        chaos_ok "场景 1：丢失窗口实测 ${loss_window_ms} ms（快照间隔 1000 ms + 推进调用间隔 50 ms + 采样余量）"
        record_time "场景 1：丢失窗口（快照 -> 崩溃）" "观测" "$loss_window_ms" "未落盘进度的墙钟跨度"
      else
        chaos_fail "场景 1：丢失窗口异常：${loss_window_ms} ms（快照 $snapshot_at_ms，崩溃 $crash_ms）"
      fi
    else
      chaos_fail "场景 1：读不到快照写入时刻（snapshot_at_ms）"
    fi

    chaos_ok "场景 1：未丢失项——房间、成员、match_id、双方血量均由快照恢复"
    record_loss "场景 1：Room 对局中崩溃" "（无其它丢失项）" \
      "房间/成员/血量/match_id 均从最近快照恢复"

    # 恢复后必须继续推进（否则「恢复成功」可能是假象）。
    frame_at_recovery="$frame_after"
    sleep 1
    if room_snapshot "$c1_room_id" "$alice_token"; then
      if [ "${ROOM_FRAME:-0}" -gt "$frame_at_recovery" ] 2>/dev/null; then
        chaos_ok "场景 1：恢复后继续推进（帧 $frame_at_recovery -> $ROOM_FRAME）"
      else
        chaos_fail "场景 1：恢复后帧号没有前进（$frame_at_recovery -> ${ROOM_FRAME:-?}）"
      fi
    else
      chaos_fail "场景 1：恢复后无法查询房间状态"
    fi

    # --- 打完整局：崩溃+重启不能破坏幂等 ---
    play_game "$c1_room_id" "$alice_token" "$bob_token" 80
    CHAOS_WAIT_TIMEOUT_MS=60000
    if wait_until room_finished "$c1_room_id" "$alice_token" >/dev/null; then
      chaos_ok "场景 1：恢复后的对局能打到终态（帧 ${ROOM_FRAME:-?}，状态 ${ROOM_STATE:-?}）"
    else
      chaos_fail "场景 1：恢复后的对局未能打到终态（状态 ${ROOM_STATE:-?}）"
    fi

    if wait_until probe_result_ok "$alice_token" "$c1_match_id" >/dev/null; then
      chaos_ok "场景 1：对局结果已落库（HTTP 200）"
    else
      chaos_fail "场景 1：崩溃重启后的对局结果未落库"
    fi

    rows="$(result_row_count "$c1_match_id")"
    dups="$(result_dup_groups "$c1_match_id")"
    if [ "$rows" = "1" ] && [ "$dups" = "0" ]; then
      chaos_ok "场景 1：幂等未被破坏——match_results 恰好 1 行、无重复分组"
    else
      chaos_fail "场景 1：match_results 行数/重复异常：rows=$rows dup_groups=$dups（期望 1 / 0）"
    fi
  else
    chaos_fail "场景 1：恢复后房间查询失败（房间丢失？room_id=$c1_room_id）"
  fi
fi

# ---------------------------------------------------------------------------
# 2. Room：FINISHING（结果待落库）崩溃
# ---------------------------------------------------------------------------
chaos_step "2. Room：FINISHING 房间崩溃（结果只在内存）"

login_as alice c2
alice_token="$LOGIN_TOKEN"
[ -n "$alice_token" ] && chaos_ok "场景 2：alice 已登录" ||
  chaos_fail "场景 2：alice 登录失败，无法查询结果"

# 为什么用**注入一条合法 finishing 行**的方式构造，而不是「停 MySQL 再打完一局」：
# 结果写入的重试间隔是 1 秒，MySQL 正常时 FINISHING 只是一个约 1 秒的窗口，
# 外部脚本抓不稳；而 MySQL 不可用时连 finishing 快照本身也写不进库，
# 崩的是 phase=playing 的旧快照——那是另一条边界（见 2b）。
# 注入的这行就是「进程在结果落库前被杀」时库里的样子，与
# scripts/verify-persistence.sh 第 7b 节的做法一致。
c2_match="m-crash-finishing"
c2_room="r-crash-finishing"
c2inject_now="$(now_ms)"
mysql_exec "DELETE FROM rooms WHERE match_id='$c2_match';
            DELETE FROM match_results WHERE match_id='$c2_match';
            INSERT INTO rooms (match_id, room_id, state, frame,
              p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined,
              winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms)
            VALUES ('$c2_match', '$c2_room', 'finishing', 600,
              'p-0001', 100, 1, 'p-0002', 0, 1,
              'p-0001', 'hp_zero', $(( c2inject_now - 60000 )), $c2inject_now, $c2inject_now);" \
  >/dev/null 2>&1
if [ "$(sql_room_field state "$c2_match")" = "finishing" ]; then
  chaos_ok "场景 2：已注入合法的 finishing 快照（模拟「结果只在内存里」的崩溃现场）"
else
  chaos_fail "场景 2：无法注入 finishing 快照"
fi
if [ "$(result_row_count "$c2_match")" = "0" ]; then
  chaos_ok "场景 2：前置成立——match_results 里还没有这一局"
else
  chaos_fail "场景 2：前置不成立——match_results 已有该局行"
fi

c2_crash_clock="$(wall_clock)"
c2_crash_us="$(now_us)"
kill -9 "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
wait_port_released "$room_port" 10
chaos_ok "场景 2：已 kill -9 Room（崩溃时刻 $c2_crash_clock，结果尚未落库）"

c2_restart_mark="$(log_mark room.log)"
c2_restart_us="$(now_us)"
if start_room; then
  c2_restart_ms=$(( ($(now_us) - c2_restart_us) / 1000 ))
  chaos_ok "场景 2：Room 已重启（重启进程自身用时 ${c2_restart_ms} ms）"
else
  c2_restart_ms=""
  chaos_fail "场景 2：Room 重启失败"
fi

c2_recover_probe_ms="$(CHAOS_WAIT_TIMEOUT_MS=30000 wait_until probe_result_ok "$alice_token" "$c2_match" || true)"
c2_recover_clock="$(wall_clock)"
if [ -n "$c2_recover_probe_ms" ]; then
  c2_recover_ms="$(elapsed_ms_from_us "$c2_restart_us")"
  c2_total_ms="$(elapsed_ms_from_us "$c2_crash_us")"
  chaos_ok "场景 2：结果自动落库并可查（HTTP 200），发起重启到可用 ${c2_recover_ms} ms（其中重启进程 ${c2_restart_ms:-?} ms，重启后客户端侧重建 $(( c2_recover_ms - ${c2_restart_ms:-0} )) ms，恢复时刻 $c2_recover_clock）"
  chaos_info "场景 2：把夹具观测也算进去，崩溃到可用共 ${c2_total_ms} ms（参考值）"
  record_time "场景 2：Room FINISHING 崩溃" "恢复" "$c2_recover_ms" "发起重启到结果可查（含重启进程 ${c2_restart_ms:-?} ms）"
else
  chaos_fail "场景 2：重启后 30 秒内结果仍未落库（TASK-008 的已知限制回归了）"
fi

c2_restored_frame="$(room_restored_frame_since "$c2_restart_mark" "$c2_room")"
if [ "$c2_restored_frame" = "600" ]; then
  chaos_ok "场景 2：FINISHING 房间被恢复（恢复点 frame=600，没有被当成已结束而丢弃）"
else
  chaos_fail "场景 2：FINISHING 房间未被恢复：日志恢复点 [${c2_restored_frame:-未记录}]（期望 600）"
fi

c2_rows="$(result_row_count "$c2_match")"
c2_dups="$(result_dup_groups "$c2_match")"
c2_winner="$(sql_scalar "SELECT winner_id FROM match_results WHERE match_id='$c2_match';")"
if [ "$c2_rows" = "1" ] && [ "$c2_dups" = "0" ]; then
  chaos_ok "场景 2：幂等——match_results 恰好 1 行、无重复分组"
else
  chaos_fail "场景 2：match_results 行数/重复异常：rows=$c2_rows dup_groups=$c2_dups（期望 1 / 0）"
fi
if [ "$c2_winner" = "p-0001" ]; then
  chaos_ok "场景 2：补写的结果带上了正确的胜者（winner_id=p-0001）"
else
  chaos_fail "场景 2：补写结果的 winner_id 异常：[$c2_winner]"
fi
if [ "$(sql_room_field state "$c2_match")" = "finished" ]; then
  chaos_ok "场景 2：快照已随落库推进到终态（state=finished）"
else
  chaos_fail "场景 2：快照未进入终态：state=[$(sql_room_field state "$c2_match")]"
fi
chaos_ok "场景 2：未丢失项——待落库的对局结果在重启后被补写"
record_loss "场景 2：Room FINISHING 崩溃" "（无丢失项）" \
  "结果由重启后的落库重试补写，未产生重复行"

# --- 2b. 已知边界：重启时存储不可用 ---
#
# 这一节不是「产品缺陷」，而是**必须写出来的恢复边界**：RoomManager::Restore 在
# 快照读取失败时一个房间都不恢复（「空手启动比以为恢复了其实没有安全」）。
# 没有实测数据时，这条边界很容易被误读成「结果丢了」——实际是「这次启动没有恢复，
# 存储恢复后再启动一次就能恢复」。两者必须区分开。
chaos_step "2b. 已知边界：Room 重启时 MySQL 不可用"

c2b_match="m-crash-boundary"
c2b_room="r-crash-boundary"
c2b_now="$(now_ms)"
mysql_exec "DELETE FROM rooms WHERE match_id='$c2b_match';
            DELETE FROM match_results WHERE match_id='$c2b_match';
            INSERT INTO rooms (match_id, room_id, state, frame,
              p1_id, p1_hp, p1_joined, p2_id, p2_hp, p2_joined,
              winner_id, finish_reason, started_at_ms, finished_at_ms, snapshot_at_ms)
            VALUES ('$c2b_match', '$c2b_room', 'finishing', 600,
              'p-0001', 100, 1, 'p-0002', 0, 1,
              'p-0001', 'hp_zero', $(( c2b_now - 60000 )), $c2b_now, $c2b_now);" \
  >/dev/null 2>&1
chaos_ok "场景 2b：已注入另一条 finishing 快照（$c2b_match）"

stop_relay relay-mysql-room "$relay_mysql_for_room_port" &&
  chaos_ok "场景 2b：已只切断 Room 的 MySQL 通道（中继 $relay_mysql_for_room_port 已停）" ||
  chaos_fail "场景 2b：无法切断 Room 的 MySQL 通道"

c2b_mark="$(log_mark room.log)"
kill -9 "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
wait_port_released "$room_port" 10
if start_room; then
  chaos_ok "场景 2b：Room 在 MySQL 不可用时仍然启动（不因依赖不可用而拒绝启动）"
else
  chaos_fail "场景 2b：Room 在 MySQL 不可用时未能启动"
fi

if log_has_since room.log "$c2b_mark" "restore_load_failed" ||
   log_has_since room.log "$c2b_mark" "本次未恢复任何房间"; then
  chaos_ok "场景 2b：启动日志如实报告快照读取失败（load_failed），不假装恢复了房间"
else
  chaos_fail "场景 2b：启动日志没有报告快照读取失败（可能悄悄恢复了空集）"
fi

# 存储不可用时**不能**返回 404：那等于声称「没有这条结果」。实测应当是
# 503 result_store_unavailable。第一次调用可能撞上「刚重启的 brpc 长连接已死」，
# 因此重试到 reason 不再是 room_unavailable 为止——那是夹具噪声，不是被测行为。
c2b_code=""
c2b_reason=""
for _ in $(seq 1 24); do
  c2b_code="$(http_get "/api/v1/results?match_id=$c2b_match" "$alice_token")"
  c2b_reason="$(jq_path error.reason)"
  [ "$c2b_reason" != "room_unavailable" ] && break
  sleep 0.25
done
if [ "$c2b_code" = "503" ] && [ "$c2b_reason" = "result_store_unavailable" ]; then
  chaos_ok "场景 2b：存储不可用时返回 503 result_store_unavailable（不假装 404「没有这条结果」）"
else
  chaos_fail "场景 2b：预期 503 result_store_unavailable，实际 HTTP $c2b_code reason=[$c2b_reason]"
fi
chaos_info "场景 2b：这一局在 rooms 表里仍是 [$(sql_room_field state "$c2b_match")]，match_results 行数 $(result_row_count "$c2b_match")"
record_loss "场景 2b：Room 重启时存储不可用" "本次启动的全部房间" \
  "load_failed：一个都不恢复（不假装恢复）；读取成功后再启动一次即可恢复"

# 存储恢复 + 再启动一次：必须能恢复并落库，才算「边界」而不是「数据丢了」。
restart_relay relay-mysql-room "$relay_mysql_for_room_port" "127.0.0.1:$mysql_port" &&
  chaos_ok "场景 2b：已恢复 Room 的 MySQL 通道" || chaos_fail "场景 2b：无法恢复中继"
wait_until probe_mysql 127.0.0.1 "$relay_mysql_for_room_port" >/dev/null
c2b_dep_us="$(now_us)"

c2b_mark2="$(log_mark room.log)"
kill -9 "$room_pid" 2>/dev/null || true
wait "$room_pid" 2>/dev/null
room_pid=""
wait_port_released "$room_port" 10
c2b_restart_us="$(now_us)"
if start_room; then
  c2b_restart_ms=$(( ($(now_us) - c2b_restart_us) / 1000 ))
  chaos_ok "场景 2b：存储恢复后再次重启 Room 成功（重启进程自身用时 ${c2b_restart_ms} ms）"
else
  c2b_restart_ms=""
  chaos_fail "场景 2b：存储恢复后 Room 重启失败"
fi
c2b_recover_probe_ms="$(CHAOS_WAIT_TIMEOUT_MS=30000 wait_until probe_result_ok "$alice_token" "$c2b_match" || true)"
c2b_recovered_frame="$(room_restored_frame_since "$c2b_mark2" "$c2b_room")"
if [ -n "$c2b_recover_probe_ms" ]; then
  c2b_recover_ms="$(elapsed_ms_from_us "$c2b_dep_us")"
  chaos_ok "场景 2b：存储恢复后结果落库并可查（HTTP 200），依赖恢复到可用共 ${c2b_recover_ms} ms（其中再次重启进程 ${c2b_restart_ms:-?} ms）"
  record_time "场景 2b：存储恢复后再重启" "恢复" "$c2b_recover_ms" "中继恢复到结果可查（含再次重启进程）"
else
  chaos_fail "场景 2b：存储恢复后再重启，结果仍未落库（数据真的丢了？）"
fi
if [ "$c2b_recovered_frame" = "600" ]; then
  chaos_ok "场景 2b：被恢复的正是那条 finishing 快照（恢复点 frame=600）"
else
  chaos_fail "场景 2b：恢复点异常：[${c2b_recovered_frame:-未记录}]（期望 600）"
fi
c2b_rows="$(result_row_count "$c2b_match")"
c2b_dups="$(result_dup_groups "$c2b_match")"
if [ "$c2b_rows" = "1" ] && [ "$c2b_dups" = "0" ]; then
  chaos_ok "场景 2b：幂等——match_results 恰好 1 行、无重复分组"
else
  chaos_fail "场景 2b：match_results 行数/重复异常：rows=$c2b_rows dup_groups=$c2b_dups（期望 1 / 0）"
fi

# 清掉两条注入行，避免污染后续运行与其他脚本。
mysql_exec "DELETE FROM rooms WHERE match_id IN ('$c2_match','$c2b_match');
            DELETE FROM match_results WHERE match_id IN ('$c2_match','$c2b_match');" \
  >/dev/null 2>&1 || true
chaos_info "场景 2/2b：已清理注入的测试行"

# ---------------------------------------------------------------------------
# 3. Match：排队中崩溃
# ---------------------------------------------------------------------------
chaos_step "3. Match：排队中 kill -9"

# --- 3a. 停机时间在超时内：排队状态必须回来 ---
reset_match_state "场景 3a 开始前" || true
login_as alice c3a
c3a_token="$LOGIN_TOKEN"
if [ "$LOGIN_HTTP" = "200" ] && [ -n "$c3a_token" ]; then
  chaos_ok "场景 3a：alice 已登录"
else
  chaos_fail "场景 3a：alice 登录失败 HTTP $LOGIN_HTTP"
fi

if enqueue_with_retry "$c3a_token" "crash-c3a-enqueue"; then
  chaos_ok "场景 3a：alice 已入队（单人入队不会配对，会停在 queued）"
else
  chaos_fail "场景 3a：入队失败"
fi
if wait_until queue_snapshot_present >/dev/null; then
  chaos_ok "场景 3a：Redis 队列快照已有 $(match_queue_len) 条（dev:match:queue）"
else
  chaos_fail "场景 3a：Redis 队列快照为空，重启无从恢复（前置不成立）"
fi

c3a_mark="$(log_mark match.log)"
c3a_clock="$(wall_clock)"
c3a_crash_us="$(now_us)"
kill -9 "$match_pid" 2>/dev/null || true
wait "$match_pid" 2>/dev/null
match_pid=""
wait_port_released "$match_port" 10
chaos_ok "场景 3a：已 kill -9 Match（崩溃时刻 $c3a_clock，alice 仍在队列里）"

c3a_restart_us="$(now_us)"
if start_match 30; then
  c3a_restart_ms=$(( ($(now_us) - c3a_restart_us) / 1000 ))
  chaos_ok "场景 3a：Match 已重启（排队超时仍为 30 秒，重启进程自身用时 ${c3a_restart_ms} ms）"
else
  c3a_restart_ms=""
  chaos_fail "场景 3a：Match 重启失败"
fi
warm_match_channel
c3a_recover_probe_ms="$(wait_until probe_match_queued "$LOGIN_TOKEN" || true)"
c3a_recover_clock="$(wall_clock)"
if [ -n "$c3a_recover_probe_ms" ]; then
  c3a_recover_ms="$(elapsed_ms_from_us "$c3a_restart_us")"
  c3a_total_ms="$(elapsed_ms_from_us "$c3a_crash_us")"
  chaos_ok "场景 3a：排队状态已恢复（state=queued），发起重启到可用 ${c3a_recover_ms} ms（其中重启进程 ${c3a_restart_ms:-?} ms，含夹具预热通道，恢复时刻 $c3a_recover_clock）"
  chaos_info "场景 3a：把夹具观测也算进去，崩溃到可用共 ${c3a_total_ms} ms（参考值）"
  record_time "场景 3a：Match 崩溃（排队中）" "恢复" "$c3a_recover_ms" "发起重启到排队状态可查（含重启进程 ${c3a_restart_ms:-?} ms）"
else
  chaos_fail "场景 3a：重启后 12 秒内排队状态未恢复（state=$(jq_path match.state)）"
fi
if log_has_since match.log "$c3a_mark" "重建排队 1 人"; then
  chaos_ok "场景 3a：启动日志报告重建了 1 名排队玩家"
else
  chaos_fail "场景 3a：启动日志没有报告重建排队玩家"
fi
chaos_ok "场景 3a：未丢失项——排队状态由 Redis 快照恢复"
record_loss "场景 3a：Match 排队中崩溃（停机在超时内）" "（无丢失项）" \
  "排队状态从 Redis 快照恢复，queued_at 仍是原入队时刻"

# --- 3b. 停机时间超过超时：按语义不再恢复（实测，不是缺陷） ---
chaos_step "3b. Match 超时语义：停机时间算进排队超时"

kill_pid "$match_pid"
match_pid=""
docker exec rgbt-redis redis-cli DEL "dev:match:queue" >/dev/null 2>&1
for key in $(docker exec rgbt-redis redis-cli --scan --pattern 'dev:match:*result*' 2>/dev/null); do
  docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1
done
if start_match 3; then
  chaos_ok "场景 3b：Match 已重启，排队超时设为 3 秒"
else
  chaos_fail "场景 3b：Match 重启失败"
fi
warm_match_channel
login_as bob c3b
c3b_token="$LOGIN_TOKEN"
if [ "$LOGIN_HTTP" = "200" ] && enqueue_with_retry "$c3b_token" "crash-c3b-enqueue"; then
  chaos_ok "场景 3b：bob 已入队（排队超时 3 秒）"
else
  chaos_fail "场景 3b：bob 入队失败"
fi
wait_until queue_snapshot_present >/dev/null || chaos_fail "场景 3b：队列快照未写入"

c3b_mark="$(log_mark match.log)"
c3b_clock="$(wall_clock)"
kill -9 "$match_pid" 2>/dev/null || true
wait "$match_pid" 2>/dev/null
match_pid=""
wait_port_released "$match_port" 10
c3b_downtime_start_us="$(now_us)"
chaos_info "场景 3b：已 kill -9 Match（$c3b_clock），故意让停机时间超过 3 秒超时"
sleep 5
if start_match 3; then
  chaos_ok "场景 3b：Match 已重启（停机时长超过排队超时）"
else
  chaos_fail "场景 3b：Match 重启失败"
fi
c3b_downtime_ms="$(elapsed_ms_from_us "$c3b_downtime_start_us")"
record_time "场景 3b：停机时长" "观测" "$c3b_downtime_ms" "超过 3 秒排队超时"
warm_match_channel

login_as bob c3b-verify
c3b_state_code="$(http_get /api/v1/matches/current "$LOGIN_TOKEN")"
c3b_state="$(jq_path match.state)"
if [ "$c3b_state_code" = "200" ] && [ "$c3b_state" != "queued" ]; then
  chaos_ok "场景 3b：该条目按超时语义不再恢复（state=$c3b_state），没有假装还在排队"
else
  chaos_fail "场景 3b：预期不再是 queued，实际 HTTP $c3b_state_code state=[$c3b_state]"
fi
if log_has_since match.log "$c3b_mark" "已超时未恢复 1 人"; then
  chaos_ok "场景 3b：启动日志如实报告「已超时未恢复 1 人」（停机时间被算进超时）"
else
  chaos_fail "场景 3b：启动日志没有报告超时未恢复"
fi
record_loss "场景 3b：Match 停机超过排队超时" "该排队条目" \
  "按 TASK-015 语义不再恢复（停机时间算进超时），玩家需重新入队"

# 回到默认配置，供场景 4 使用。
reset_match_state "场景 4 开始前" || true

# ---------------------------------------------------------------------------
# 4. Gateway：崩溃
# ---------------------------------------------------------------------------
chaos_step "4. Gateway：kill -9（SSE 订阅丢失，会话与对局不受影响）"

login_as alice c4-a
alice_token="$LOGIN_TOKEN"
login_as bob c4-b
bob_token="$LOGIN_TOKEN"
c4_room_id=""
c4_match_id=""
if [ -n "$alice_token" ] && [ -n "$bob_token" ] && pair_two "$alice_token" "$bob_token" c4; then
  c4_room_id="$PAIR_ROOM_ID"
  c4_match_id="$PAIR_MATCH_ID"
  chaos_ok "场景 4：配对成功 room=$c4_room_id match=$c4_match_id"
else
  chaos_fail "场景 4：配对失败（HTTP $PAIR_A_CODE state=$(jq_path match.state)）"
fi

if [ -n "$c4_room_id" ]; then
  wait_until room_playing "$c4_room_id" "$alice_token" >/dev/null || \
    chaos_fail "场景 4：对局未进入 playing"

  sse_subscribe "$alice_token" "$c4_room_id" "$CHAOS_LOG_DIR/stream1.log" 60
  if sse_wait_event "$CHAOS_LOG_DIR/stream1.log" 'event: session.ready' 40; then
    chaos_ok "场景 4：SSE 订阅成功并收到 session.ready"
  else
    chaos_fail "场景 4：SSE 订阅未收到 session.ready"
    tail -5 "$CHAOS_LOG_DIR/stream1.log" | sed 's/^/     stream: /'
  fi
  sse_before="$(metric_value "$gateway_port" rgbt_sse_connections)"
  if [ "${sse_before:-0}" -ge 1 ] 2>/dev/null; then
    chaos_ok "场景 4：Gateway /metrics 的 SSE 连接数为 $sse_before"
  else
    chaos_fail "场景 4：Gateway /metrics 的 SSE 连接数异常：[$sse_before]"
  fi

  room_snapshot "$c4_room_id" "$alice_token" || true
  frame_before_gw="${ROOM_FRAME:-0}"
  chaos_info "崩溃前状态：room=$c4_room_id 状态=${ROOM_STATE:-?} 帧=$frame_before_gw SSE 订阅=1"

  c4_clock="$(wall_clock)"
  c4_crash_us="$(now_us)"
  kill -9 "$gateway_pid" 2>/dev/null || true
  wait "$gateway_pid" 2>/dev/null
  gateway_pid=""
  wait_port_released "$gateway_port" 10
  chaos_ok "场景 4：已 kill -9 Gateway（崩溃时刻 $c4_clock，SSE 订阅随进程消失）"

  c4_probe_ms="$(wait_until probe_process_gone "$stream_pid" || true)"
  if [ -n "$c4_probe_ms" ]; then
    c4_detect_ms="$(elapsed_ms_from_us "$c4_crash_us")"
    chaos_ok "检测：SSE 长连接在 ${c4_detect_ms} ms 内断开"
    record_time "场景 4：Gateway 崩溃" "检测" "$c4_detect_ms" "kill -9 到 SSE 长连接断开"
  else
    chaos_fail "场景 4：Gateway 崩溃后 SSE 长连接未断开（curl 仍在跑）"
  fi
  c4_http_probe_ms="$(wait_until probe_gw_down || true)"
  if [ -n "$c4_http_probe_ms" ]; then
    c4_http_ms="$(elapsed_ms_from_us "$c4_crash_us")"
    chaos_ok "检测：Gateway 崩溃后 HTTP 请求失败，用时 ${c4_http_ms} ms"
  else
    chaos_fail "场景 4：Gateway 崩溃后 HTTP 仍然可用（进程没死？）"
  fi

  c4_restart_us="$(now_us)"
  if start_gateway; then
    c4_restart_end_us="$(now_us)"
    c4_restart_ms=$(( (c4_restart_end_us - c4_restart_us) / 1000 ))
    chaos_ok "场景 4：Gateway 已重启（进程号 $gateway_pid，重启进程自身用时 ${c4_restart_ms} ms）"
  else
    c4_restart_end_us="$(now_us)"
    c4_restart_ms=""
    chaos_fail "场景 4：Gateway 重启失败"
  fi

  # 会话在 Redis，不受进程崩溃影响：旧 token 必须仍然有效。
  c4_me_code="$(http_get /api/v1/players/me "$alice_token")"
  if [ "$c4_me_code" = "200" ]; then
    chaos_ok "场景 4：会话未受影响——崩溃前签发的 token 重启后仍可用（HTTP 200）"
  else
    chaos_fail "场景 4：旧 token 在重启后失效（HTTP $c4_me_code），会话没有真正落在 Redis"
  fi

  # 重新订阅：客户端必须重连（订阅表只在进程内存里）。
  sse_subscribe "$alice_token" "$c4_room_id" "$CHAOS_LOG_DIR/stream2.log" 45
  if sse_wait_event "$CHAOS_LOG_DIR/stream2.log" 'event: session.ready' 60; then
    c4_recover_ms="$(elapsed_ms_from_us "$c4_crash_us")"
    chaos_ok "恢复：客户端重新订阅成功（收到 session.ready），崩溃到可用共 ${c4_recover_ms} ms（其中重启进程 ${c4_restart_ms:-?} ms，其余为客户端重新订阅）"
    record_time "场景 4：Gateway 崩溃" "恢复" "$c4_recover_ms" "kill -9 到重新订阅成功（含重启进程）"
  else
    chaos_fail "场景 4：Gateway 重启后重新订阅未能收到 session.ready"
    tail -5 "$CHAOS_LOG_DIR/stream2.log" | sed 's/^/     stream2: /'
  fi
  sse_after="$(metric_value "$gateway_port" rgbt_sse_connections)"
  if [ "${sse_after:-0}" -ge 1 ] 2>/dev/null; then
    chaos_ok "场景 4：重新订阅后 SSE 连接数为 $sse_after（新进程重新建立）"
  else
    chaos_fail "场景 4：重新订阅后 SSE 连接数异常：[$sse_after]"
  fi

  # 对局不受影响：Room 独立推进，Gateway 停机期间的帧照样前进。
  if room_snapshot "$c4_room_id" "$alice_token"; then
    if [ "${ROOM_FRAME:-0}" -gt "$frame_before_gw" ] 2>/dev/null; then
      chaos_ok "场景 4：对局未受影响——Gateway 停机期间帧继续前进（$frame_before_gw -> $ROOM_FRAME）"
    else
      chaos_fail "场景 4：对局帧号没有前进（$frame_before_gw -> ${ROOM_FRAME:-?}）"
    fi
  else
    chaos_fail "场景 4：Gateway 重启后无法查询房间状态"
  fi

  record_loss "场景 4：Gateway 崩溃" "全部 SSE 订阅" \
    "订阅表在进程内存里，客户端必须重新订阅（Phase 2 已记的已知限制）"
  record_loss "场景 4：Gateway 崩溃" "（无其它丢失项）" \
    "会话在 Redis、房间与对局在 Room，均不受 Gateway 崩溃影响"
fi

# ---------------------------------------------------------------------------
# 5. 收尾
# ---------------------------------------------------------------------------
chaos_step "5. 收尾"

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container 仍健康" || chaos_fail "$container 不健康，请手动检查"
done

chaos_print_time_table
print_loss_table

echo
if chaos_has_failures; then
  chaos_print_failures
  echo
  echo "排查提示：Gateway $CHAOS_LOG_DIR/gateway.log，Match $CHAOS_LOG_DIR/match.log，"
  echo "          Room $CHAOS_LOG_DIR/room.log，SSE 抓包 $CHAOS_LOG_DIR/stream1.log / stream2.log"
  exit 1
fi

echo "验收通过：三个服务的进程崩溃场景全部实测，恢复时间与丢失边界见上两张表。"
echo "  Room（对局中）   ：房间仍在、恢复点等于最后快照、回退帧数在 10 帧内；重启后能打完且无重复行"
echo "  Room（FINISHING）：结果在重启后被补写（TASK-008 的已知限制已解除）；"
echo "                     重启时存储不可用则如实 load_failed、一个都不恢复（已知边界）"
echo "  Match（排队中）  ：停机在超时内 -> 排队状态从 Redis 快照恢复；"
echo "                     停机超过超时 -> 该条目按语义不再恢复（不是缺陷）"
echo "  Gateway          ：SSE 订阅随进程丢失（客户端必须重连）；会话与对局不受影响"
exit 0