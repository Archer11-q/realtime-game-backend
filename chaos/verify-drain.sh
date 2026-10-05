#!/usr/bin/env bash
#
# ⚠ 本脚本**不在** `scripts/verify-all.sh` 里（故障注入 / 长稳 / 重连这一类会占标准
#   端口、停依赖或跑很久，不适合放进快速门禁）。**改动 `src/` 下的产品代码之后，
#   请跑 `bash scripts/verify-chaos.sh`** —— 那是这一类脚本的统一入口。
#   为什么必须写这一句：TASK-026 曾漏改本类里的一个脚本（它不在任何统一入口里，
#   于是回归躺了整整一个任务周期才被 TASK-029 的补跑发现），详见
#   `docs/devlog.md` 的「TASK-029 之后的重跑结果」。
#
# chaos/verify-drain.sh - TASK-026：优雅退出与排空
#
# Phase 4 的退出标准要求"优雅退出：收到 SIGTERM 后停止接收新房间、等待活跃对局结束"。
# 本脚本把这句话变成四条可判定的实测：
#
#   1. **能排空完**：对局进行中给 Room 发 SIGTERM -> Room 不立刻退出，等这一局打完、
#      结果**落库**（有真实胜者）之后才退出；退出码 0；日志是 drain_finished。
#   2. **新房间被拒**：排空期间新配对的两个人**拿不到房间**（Room 侧
#      `room_create_rejected reason=shutting_down`），并且客户端看到他们停在 queued
#      ——而不是"排空期间还能开新局"。
#   3. **必须超时截断**：把 Room 的排空上限压到 3 秒、摆一局刚开局的对局，
#      到点必须：未结束的对局落一次**终态快照且标 ABORTED**、`match_results` 里
#      **没有这一局**（不伪造胜负）、进程退出码 0、日志是 drain_timeout_abort。
#   4. **Match 与 Gateway**：
#      * Match：SIGTERM 后新入队返回 503 `shutting_down`，而**读状态仍可用**
#        （已配对的结果仍可领取）；
#      * Gateway：SIGTERM 后新请求被拒（503），已建立的 SSE 收到显式的
#        `stream.closed`（reason=server_shutdown）之后才被关闭——不是被直接切断。
#
# 为什么 3 与 1 必须分开测：它们判定的东西相反。1 证明"该等的等到了"，
# 3 证明"不该等的没有无限等，而且没有编造结果"。只测其中一个都会漏掉一半语义。
#
# 用法：
#   bash chaos/verify-drain.sh
#   bash chaos/verify-drain.sh --keep      # 保留服务与日志，便于排查
#
# 前置条件：Docker 的 rgbt-redis / rgbt-mysql 健康；8080/8082/8083 空闲
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

# 排空上限刻意压小：验证的是语义，不是"默认 30 秒"这个数字。
room_drain_ms=15000
room_drain_short_ms=3000
match_grace_ms=2000
gateway_drain_ms=3000
bench_accounts=20

keep=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keep) keep=1; shift ;;
    -h | --help) sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

env_file="deploy/compose/.env"
[ -f "$env_file" ] || { echo "x  缺少 $env_file，请先执行 bash scripts/dev-up.sh" >&2; exit 1; }
set -a
# shellcheck disable=SC1090
. "./$env_file"
set +a

CHAOS_RUN_DIR="$repo_root/.run"
CHAOS_LOG_DIR="$repo_root/.run/chaos"
mkdir -p "$CHAOS_LOG_DIR"
rm -f "$CHAOS_LOG_DIR"/*.log 2>/dev/null || true
CHAOS_RESP_FILE="$CHAOS_LOG_DIR/last-response.json"

GW_PORT="$gateway_port"
MATCH_PORT="$match_port"

gateway_pid=""
match_pid=""
room_pid=""
stream_pid=""

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
}

cleanup() {
  local rc=$?
  if [ "$keep" -eq 1 ]; then
    echo "（--keep：保留服务进程，便于排查）"
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

# ---------------------------------------------------------------------------
# 启停（本次的重点是"收到 SIGTERM 之后怎么退"，所以每个服务都要能单独停）
# ---------------------------------------------------------------------------
start_room() { # <drain_timeout_ms>
  local drain_ms="$1"
  "build/$preset/bin/rgbt_room" -port "$room_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    -drain_timeout_ms "$drain_ms" >>"$CHAOS_LOG_DIR/room.log" 2>&1 &
  room_pid=$!
  wait_http "$room_port" 30
}

start_match() { # <shutdown_grace_ms>
  local grace_ms="$1"
  "build/$preset/bin/rgbt_match" -match_port "$match_port" \
    -match_timeout_seconds 30 -match_result_ttl_seconds 120 \
    -env_prefix dev -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" \
    -redis_timeout_ms 500 \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -shutdown_grace_ms "$grace_ms" >>"$CHAOS_LOG_DIR/match.log" 2>&1 &
  match_pid=$!
  wait_http "$match_port" 30
}

start_gateway() { # <drain_timeout_ms>
  local drain_ms="$1"
  "build/$preset/bin/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
    -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" -redis_timeout_ms 500 \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    -match_host 127.0.0.1 -match_port "$match_port" \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -match_timeout_ms 500 -room_timeout_ms 500 \
    -stream_poll_interval_ms 100 -stream_heartbeat_interval_ms 15000 \
    -drain_timeout_ms "$drain_ms" -enable_bench_accounts \
    >>"$CHAOS_LOG_DIR/gateway.log" 2>&1 &
  gateway_pid=$!
  wait_http "$gateway_port" 30
}

# 给某个服务发 SIGTERM 并等它退出。
#
# 结果**写进文件**而不是靠变量传回：调用点是 `( ... ) &`，里面又套了一层
# `$( ... )`（命令替换本身就是子 shell），函数里赋的全局变量传不出来。
# 首轮实测就是 `SERVICE_EXIT_CODE: unbound variable`，退出码被记成 `?`。
#
# 为什么不用通用的 kill_pid：它超时后会补 SIGKILL，那会把"排空没做完"这一事实
# 掩盖掉。这里超时是要报失败的。
stop_service_measured() { # <名称> <pid> <结果文件前缀>
  local name="$1" pid="$2" prefix="$3"
  local start_us; start_us="$(now_us)"
  kill -TERM "$pid" 2>/dev/null || true
  local i
  for i in $(seq 1 900); do  # 最多 90 秒：比任何排空上限都长，超了就是缺陷
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  local waited; waited="$(elapsed_ms_from_us "$start_us")"
  if kill -0 "$pid" 2>/dev/null; then
    chaos_fail "$name 在 SIGTERM 后 90 秒仍未退出"
    kill -KILL "$pid" 2>/dev/null || true
  fi
  printf '%s' "$waited" >"$prefix-wait.txt"
}

# 收服务退出码。
#
# **必须在父 shell 里做**：`wait` 只能等自己的子进程，而 stop_service_measured 跑在
# 后台子 shell 里——在那里 `wait "$pid"` 会得到 127（首轮实测的"退出码异常：127"）。
# 所以「杀 + 等它消失」放子 shell，「取退出码」放回父 shell。
reap_service() { # <pid> <结果文件前缀>
  local pid="$1" prefix="$2"
  [ -n "$pid" ] || { printf '%s' "?" >"$prefix-exit.txt"; return 0; }
  wait "$pid" 2>/dev/null
  printf '%s' "$?" >"$prefix-exit.txt"
}

read_result() { # <结果文件前缀> <wait|exit>
  local prefix="$1" kind="$2"
  cat "$prefix-$kind.txt" 2>/dev/null || echo "?"
}

# ---------------------------------------------------------------------------
# 数据与业务辅助
# ---------------------------------------------------------------------------
mysql_exec() {
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

provision_bench_accounts() { # <数量>
  local count="$1"
  local sql="INSERT IGNORE INTO players (player_id, account, display_name, status) VALUES "
  local i=0
  while [ "$i" -lt "$count" ]; do
    [ "$i" -gt 0 ] && sql+=","
    sql+="(CONCAT('p-9', LPAD($i, 5, '0')), CONCAT('bench-', LPAD($i, 5, '0')), CONCAT('Bench ', $i), 'active')"
    i=$((i + 1))
  done
  sql+=";"
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "$sql" >/dev/null 2>&1
  sql_scalar "SELECT COUNT(*) FROM players WHERE account LIKE 'bench-%';"
}

clean_state() {
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  mysql_exec "DELETE FROM rooms; DELETE FROM match_results;" >/dev/null 2>&1
}

cleanup_bench_state() {
  mysql_exec "DELETE FROM players WHERE account LIKE 'bench-%'; DELETE FROM rooms; DELETE FROM match_results;" \
    >/dev/null 2>&1
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
}

# 清空 Match 的队列与配对结果并重启它。
#
# **为什么每个场景都要做**（与 TASK-023/024/025 同一个坑）：结果 TTL 是 120 秒，
# `GetStatus` 会优先返回"最近一次配对结果"。不重置的话，下一段里同一对玩家会直接
# 拿到上一段的房间号，表现成 join 409 room_already_finished——看起来像排空坏了，
# 其实是夹具复用了旧结果。
reset_match_state() { # <shutdown_grace_ms>
  local grace_ms="$1"
  kill_pid "$match_pid"
  match_pid=""
  docker exec rgbt-redis redis-cli DEL "dev:match:queue" >/dev/null 2>&1
  local key
  for key in $(docker exec rgbt-redis redis-cli --scan --pattern 'dev:match:*result*' 2>/dev/null); do
    docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1
  done
  start_match "$grace_ms" || return 1
  # 预热 Gateway -> Match 的 brpc 通道（理由见 warm_match_channel 的注释）。
  warm_match_channel
}

# 预热 Gateway -> Match 的 brpc 通道。
#
# **为什么每次重启 Match 之后都要做**：Match 一重启，Gateway 手上的长连接就死了，
# 而 brpc 不会立刻知道——紧接的第一次入队会拿到 503 `match_unavailable`
# （服务其实是好的）。首轮实测正是被这个卡住：场景 2 与场景 4 的配对直接失败，
# 后面的断言全部失去前提。
warm_match_channel() {
  local i
  login_as alice warm || return 1
  for i in $(seq 1 20); do
    if [ "$(http_get /api/v1/matches/current "$LOGIN_TOKEN")" = "200" ]; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

login_as() { # <账号> [请求 id 后缀]
  local account="$1" suffix="${2:-$RANDOM}"
  local password="${account}_dev_pw"
  case "$account" in bench-*) password="bench_dev_pw" ;; esac
  CHAOS_LAST_STATUS="$(http_post /api/v1/login "" \
    "{\"account\":\"$account\",\"password\":\"$password\",\"client_type\":\"web\",\"request_id\":\"drain-$account-$suffix\"}")"
  LOGIN_TOKEN="$(jq_path token)"
  LOGIN_HTTP="$CHAOS_LAST_STATUS"
  return 0
}

pair_two() { # <tokenA> <tokenB> <标签>
  local token_a="$1" token_b="$2" tag="$3"
  PAIR_MATCH_ID=""; PAIR_ROOM_ID=""
  enqueue_with_retry "$token_a" "drain-$tag-a" || return 1
  enqueue_with_retry "$token_b" "drain-$tag-b" || return 1
  local i code status room_id match_id
  for i in $(seq 1 60); do
    code="$(http_get /api/v1/matches/current "$token_a")"
    status="$(jq_path match.state)"
    room_id="$(jq_path match.room_id)"
    match_id="$(jq_path match.match_id)"
    if [ "$code" = "200" ] && [ "$status" = "matched" ] && [ -n "$room_id" ]; then
      PAIR_ROOM_ID="$room_id"
      PAIR_MATCH_ID="$match_id"
      join_room "$room_id" "$token_a" "j-$tag-a" || return 1
      join_room "$room_id" "$token_b" "j-$tag-b" || return 1
      return 0
    fi
    sleep 0.5
  done
  return 1
}

join_room() { # <room_id> <token> <后缀>
  local code
  code="$(http_post /api/v1/rooms/join "$2" "{\"room_id\":\"$1\",\"request_id\":\"drain-$3\"}")"
  [ "$code" = "200" ] && return 0
  chaos_info "加入房间失败：room=$1 code=$code reason=$(jq_path error.reason)"
  return 1
}

room_snapshot() { # <room_id> <token>
  ROOM_FRAME=""; ROOM_STATE=""
  local code
  code="$(http_get "/api/v1/rooms/state?room_id=$1" "$2")"
  [ "$code" = "200" ] || return 1
  ROOM_FRAME="$(jq_path room.frame)"
  ROOM_STATE="$(jq_path room.state)"
  return 0
}

room_playing() { room_snapshot "$1" "$2" || return 1; [ "$ROOM_STATE" = "playing" ]; }
room_finished() { room_snapshot "$1" "$2" || return 1; [ "$ROOM_STATE" = "finished" ]; }
probe_room_state_ok() { local code; code="$(http_get "/api/v1/rooms/state?room_id=$2" "$1")"; [ "$code" = "200" ]; }
probe_result_ok() { local code; code="$(http_get "/api/v1/matches/current" "$1")"; [ "$code" = "200" ]; }

# 轮流攻击直到对局结束（request_id 必须带 room_id，否则会被幂等丢弃）。
attack_until_finished() { # <room_id> <tokenA> <tokenB> [最多回合]
  local room_id="$1" token_a="$2" token_b="$3" max_rounds="${4:-40}"
  local round tag; tag="${room_id: -8}"
  for round in $(seq 1 "$max_rounds"); do
    if [ $(( round % 2 )) -eq 1 ]; then
      http_post /api/v1/rooms/input "$token_a" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"drain-atk-$tag-a$round\"}" >/dev/null
    else
      http_post /api/v1/rooms/input "$token_b" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"drain-atk-$tag-b$round\"}" >/dev/null
    fi
    if room_snapshot "$room_id" "$token_a"; then
      [ "$ROOM_STATE" = "finished" ] || [ "$ROOM_STATE" = "finishing" ] && {
        ATTACK_RESULT="$ROOM_STATE"; return 0; }
    fi
    sleep 0.15
  done
  ATTACK_RESULT="timeout"
  return 0
}

sse_subscribe() { # <token> <room_id> <文件> [最长秒数]
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

log_has() { grep -qF -- "$2" "$CHAOS_LOG_DIR/$1" 2>/dev/null; }
log_has_since() { # <日志名> <起始行号> <匹配串>
  local file="$CHAOS_LOG_DIR/$1" mark="$2" needle="$3"
  [ -f "$file" ] || return 1
  tail -n "+$(( mark + 1 ))" "$file" 2>/dev/null | grep -qF -- "$needle"
}

# ---------------------------------------------------------------------------
# 0. 前置检查
# ---------------------------------------------------------------------------
chaos_step "0. 前置检查"

command -v docker >/dev/null 2>&1 || { echo "x  缺少 docker" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "x  Docker 服务端不可达" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "x  缺少 python3" >&2; exit 1; }

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container healthy" ||
    chaos_fail "$container 不健康：本脚本需要真实的 Redis 与 MySQL"
done
if chaos_has_failures; then chaos_print_failures || true; exit 1; fi

for port in "$gateway_port" "$match_port" "$room_port"; do
  port_listening "$port" && chaos_fail "端口 $port 已被占用，先执行 bash scripts/dev-down.sh"
done
if chaos_has_failures; then chaos_print_failures || true; exit 1; fi
chaos_ok "3 个服务端口均空闲"

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

have="$(provision_bench_accounts "$bench_accounts")"
[ "${have:-0}" -ge 2 ] && chaos_ok "合成身份就绪（bench-* ${have} 个，用于「排空期间的新配对」）" ||
  { chaos_fail "合成身份不足"; chaos_print_failures || true; exit 1; }

# ---------------------------------------------------------------------------
# 1. Room：能在超时前排空完（等对局打完、结果落库、退出码 0）
# ---------------------------------------------------------------------------
chaos_step "1. Room 排空：等待活跃对局结束（drain_timeout_ms=$room_drain_ms）"

clean_state
start_room "$room_drain_ms" && chaos_ok "Room 已启动（排空上限 ${room_drain_ms} ms）" || chaos_fail "Room 未就绪"
start_match "$match_grace_ms" && chaos_ok "Match 已启动" || chaos_fail "Match 未就绪"
start_gateway "$gateway_drain_ms" && chaos_ok "Gateway 已启动" || chaos_fail "Gateway 未就绪"

login_as alice s1a; alice_token="$LOGIN_TOKEN"
login_as bob s1b; bob_token="$LOGIN_TOKEN"
c1_room=""; c1_match=""
if pair_two "$alice_token" "$bob_token" s1; then
  c1_room="$PAIR_ROOM_ID"; c1_match="$PAIR_MATCH_ID"
  chaos_ok "对局已开始 room=$c1_room match=$c1_match"
else
  chaos_fail "无法开始对局"
fi
[ -n "$c1_room" ] && wait_until room_playing "$c1_room" "$alice_token" >/dev/null &&
  chaos_ok "对局进入 playing（双方已进房）" || chaos_fail "对局未进入 playing"

# --- 发 SIGTERM：此刻对局还在打 ---
room_mark="$(wc -l <"$CHAOS_LOG_DIR/room.log" 2>/dev/null || echo 0)"
chaos_info "对局进行中，给 Room 发 SIGTERM"
(
  stop_service_measured "Room" "$room_pid" "$CHAOS_LOG_DIR/room1"
) &
term_job=$!

# 排空期间：新配对的两个人必须**拿不到房间**（Room 拒绝新房间）。
sleep 1
login_as bench-00000 s1c; n1_token="$LOGIN_TOKEN"
login_as bench-00001 s1d; n2_token="$LOGIN_TOKEN"
new_pair_ok=0
if enqueue_with_retry "$n1_token" "drain-s1-new1" && enqueue_with_retry "$n2_token" "drain-s1-new2"; then
  sleep 3
  http_get /api/v1/matches/current "$n1_token" >/dev/null
  new_state="$(jq_path match.state)"
  if [ "$new_state" != "matched" ]; then
    chaos_ok "排空期间新配对拿不到房间（客户端状态仍是 $new_state）"
    new_pair_ok=1
  else
    chaos_fail "排空期间仍然开出了新房间（状态 matched room=$(jq_path match.room_id)）"
  fi
  if log_has room.log "room_create_rejected"; then
    chaos_ok "Room 日志记录了新房间被拒（reason=shutting_down）"
  else
    chaos_fail "Room 日志没有记录新房间被拒"
  fi
else
  chaos_fail "排空期间入队失败（本应能入队，只是配不到房间）"
fi
[ "$new_pair_ok" -eq 0 ] && chaos_info "（新配对这条线未通过，继续测「等对局打完」）"

# 继续把这局打完：排空期间**进行中的对局必须照常接受输入**。
if [ -n "$c1_room" ]; then
  attack_until_finished "$c1_room" "$alice_token" "$bob_token" 60
  chaos_info "排空期间对局结束形态：$ATTACK_RESULT"
fi

wait "$term_job" 2>/dev/null
reap_service "$room_pid" "$CHAOS_LOG_DIR/room1"
room1_wait="$(read_result "$CHAOS_LOG_DIR/room1" wait)"
room1_exit="$(read_result "$CHAOS_LOG_DIR/room1" exit)"
room_pid=""

chaos_info "Room 从 SIGTERM 到退出耗时 ${room1_wait} ms，退出码 ${room1_exit}"
record_time "场景 1：Room 排空到对局结束" "恢复" "${room1_wait:-0}" "SIGTERM 到进程退出"

# 判定 1：不是立刻退出（否则等于没有排空语义）。
if [ "${room1_wait:-0}" -ge 500 ] 2>/dev/null; then
  chaos_ok "Room 没有立刻退出（等了 ${room1_wait} ms 让对局打完）"
else
  chaos_fail "Room 几乎立刻退出（${room1_wait} ms），排空语义没有生效"
fi
[ "$room1_exit" = "0" ] && chaos_ok "Room 退出码 0" || chaos_fail "Room 退出码异常：$room1_exit"
log_has room.log "event=drain_finished" && chaos_ok "Room 日志：drain_finished（等到了终局）" ||
  chaos_fail "Room 日志没有 drain_finished"
log_has room.log "event=drain_timeout_abort" &&
  chaos_fail "Room 在本该排空完的场景里走了超时截断" ||
  chaos_ok "Room 没有走超时截断（符合预期）"

# 判定 2：这一局打完了，而且是**真实结果**（不是 ABORTED、不是没落库）。
if [ -n "$c1_match" ]; then
  rows="$(result_row_count "$c1_match")"
  winner="$(sql_scalar "SELECT winner_id FROM match_results WHERE match_id='$c1_match';")"
  state="$(sql_room_field state "$c1_match")"
  chaos_info "落库情况：match_results 行数=$rows winner=[$winner] rooms.state=[$state]"
  if [ "$rows" = "1" ] && [ "$state" = "finished" ]; then
    chaos_ok "排空等到的是**真实结果**：结果已落库且快照进入终态 finished"
    record_time "场景 1：结果落库" "观测" "1" "match_results 有 1 行、rooms.state=finished"
  else
    chaos_fail "排空后结果不完整：rows=$rows state=$state"
  fi
  if [ "$winner" = "p-0001" ] || [ "$winner" = "p-0002" ]; then
    chaos_ok "结果带上了真实胜者（winner=$winner），不是 ABORTED"
  else
    chaos_info "胜者为空（平局或未判定）：winner=[$winner]"
  fi
fi

# ---------------------------------------------------------------------------
# 2. Room：必须超时截断（未结束 -> ABORTED，且不写结果）
# ---------------------------------------------------------------------------
chaos_step "2. Room 排空超时：未结束的对局标 ABORTED 且不写结果（上限 $room_drain_short_ms ms）"

room_mark2="$(wc -l <"$CHAOS_LOG_DIR/room.log" 2>/dev/null || echo 0)"
start_room "$room_drain_short_ms" && chaos_ok "Room 已重启（排空上限压到 ${room_drain_short_ms} ms）" ||
  chaos_fail "Room 未就绪"
reset_match_state "$match_grace_ms" && chaos_ok "Match 已重置（清空队列与旧配对结果）" ||
  chaos_fail "Match 重置失败"

# 摆一局**刚开局**的对局：双方 100 血、不攻击，正常要 60 秒才会超时判胜。
login_as alice s2a; alice2_token="$LOGIN_TOKEN"
login_as bob s2b; bob2_token="$LOGIN_TOKEN"
c2_room=""; c2_match=""
if pair_two "$alice2_token" "$bob2_token" s2; then
  c2_room="$PAIR_ROOM_ID"; c2_match="$PAIR_MATCH_ID"
  chaos_ok "新对局已开始 room=$c2_room match=$c2_match"
else
  chaos_fail "无法开始第二局"
fi
[ -n "$c2_room" ] && wait_until room_playing "$c2_room" "$alice2_token" >/dev/null &&
  chaos_ok "对局进入 playing（未攻击，正常还要跑约 60 秒）" || chaos_fail "第二局未进入 playing"

chaos_info "对局远未结束，给 Room 发 SIGTERM（上限 ${room_drain_short_ms} ms）"
stop_service_measured "Room" "$room_pid" "$CHAOS_LOG_DIR/room2"
reap_service "$room_pid" "$CHAOS_LOG_DIR/room2"
room2_wait="$(read_result "$CHAOS_LOG_DIR/room2" wait)"
room2_exit="$(read_result "$CHAOS_LOG_DIR/room2" exit)"
room_pid=""
chaos_info "Room 退出耗时 ${room2_wait} ms，退出码 ${room2_exit}"
record_time "场景 2：Room 排空超时截断" "恢复" "${room2_wait:-0}" "SIGTERM 到进程退出（超时上限 ${room_drain_short_ms} ms）"

# 判定 1：等到超时上限附近才退出（明显长于"立刻退出"，且不该远超上限）。
if [ "${room2_wait:-0}" -ge "$room_drain_short_ms" ] 2>/dev/null &&
   [ "${room2_wait:-0}" -le $(( room_drain_short_ms + 3000 )) ] 2>/dev/null; then
  chaos_ok "Room 等满了上限才截断（${room2_wait} ms，上限 ${room_drain_short_ms} ms）"
else
  chaos_fail "Room 退出时机不符合「等满上限」：${room2_wait} ms（上限 ${room_drain_short_ms} ms）"
fi
[ "$room2_exit" = "0" ] && chaos_ok "Room 退出码 0" || chaos_fail "Room 退出码异常：$room2_exit"
log_has_since room.log "$room_mark2" "event=drain_timeout_abort" &&
  chaos_ok "Room 日志：drain_timeout_abort（超时兜底生效）" ||
  chaos_fail "Room 日志没有 drain_timeout_abort"

# 判定 2（本任务最严重的一条）：截断**不能伪造胜负**。
if [ -n "$c2_match" ]; then
  state2="$(sql_room_field state "$c2_match")"
  reason2="$(sql_room_field finish_reason "$c2_match")"
  winner2="$(sql_room_field winner_id "$c2_match")"
  rows2="$(result_row_count "$c2_match")"
  chaos_info "截断后：rooms.state=[$state2] finish_reason=[$reason2] winner=[$winner2] match_results 行数=$rows2"
  if [ "$state2" = "aborted" ]; then
    chaos_ok "未结束的对局被标为 ABORTED（不是 PLAYING、也不是 finished）"
  else
    chaos_fail "截断后快照状态不是 aborted：[$state2]"
  fi
  if [ "$reason2" = "aborted" ] && { [ -z "$winner2" ] || [ "$winner2" = "NULL" ]; }; then
    chaos_ok "终态快照的 finish_reason=aborted 且没有胜者（没有伪造胜负）"
  else
    chaos_fail "终态快照伪造了结果：finish_reason=[$reason2] winner=[$winner2]"
  fi
  if [ "$rows2" = "0" ]; then
    chaos_ok "match_results 里**没有**这一局（被截断的对局不产生结果）"
  else
    chaos_fail "被截断的对局写进了 match_results（$rows2 行）——这是最严重的失败"
  fi
  if [ "$(sql_scalar "SELECT COUNT(*) FROM rooms WHERE state IN ('playing','finishing');")" = "0" ]; then
    chaos_ok "库里没有残留的 playing/finishing 房间"
  else
    chaos_fail "库里仍有 playing/finishing 的房间"
  fi
fi

# ---------------------------------------------------------------------------
# 3. Match：拒绝新入队、但仍能读状态
# ---------------------------------------------------------------------------
chaos_step "3. Match 排空：拒绝新入队，已配对的结果仍可领取（宽限 ${match_grace_ms} ms）"

# 先让 alice + bob **真的配成局但不去领取**，这样才能验证"已配对的结果仍可领取"；
# 只用单人排队验证不了这一条（那只是 queued）。
#
# Room 在这里必须先起来：配对成功要经过「Match -> Room/CreateRoom」这一步，
# Room 不在的话两个人只会被退回队列、停在 queued（上一轮实测就是这样，
# 断言因此失去前提）。
start_room "$room_drain_ms" && chaos_ok "Room 已重启（配对需要它）" || chaos_fail "Room 未就绪"
reset_match_state "$match_grace_ms" && chaos_ok "Match 已重置（清空队列与旧配对结果）" ||
  chaos_fail "Match 重置失败"
login_as alice s3a; alice3_token="$LOGIN_TOKEN"
login_as bob s3b; bob3_token="$LOGIN_TOKEN"
enqueue_with_retry "$alice3_token" "drain-s3-keep-a" || chaos_fail "场景 3：alice 入队失败"
enqueue_with_retry "$bob3_token" "drain-s3-keep-b" || chaos_fail "场景 3：bob 入队失败"
sleep 1
login_as bench-00002 s3c; new3_token="$LOGIN_TOKEN"
http_get /api/v1/matches/current "$alice3_token" >/dev/null
chaos_info "SIGTERM 前 alice 的匹配状态：$(jq_path match.state)（应为 matched）"
if [ "$(jq_path match.state)" != "matched" ]; then
  chaos_fail "场景 3 前提不成立：alice 没有配成局（state=$(jq_path match.state)）"
fi

match_mark="$(wc -l <"$CHAOS_LOG_DIR/match.log" 2>/dev/null || echo 0)"
chaos_info "给 Match 发 SIGTERM（宽限 ${match_grace_ms} ms）"
(
  stop_service_measured "Match" "$match_pid" "$CHAOS_LOG_DIR/match1"
) &
match_term_job=$!

sleep 0.4
unset CHAOS_LAST_STATUS
reject_code="$(http_post /api/v1/matches "$new3_token" "{\"request_id\":\"drain-s3-new\"}")"
reject_reason="$(jq_path error.reason)"
if [ "$reject_code" = "503" ] && [ "$reject_reason" = "shutting_down" ]; then
  chaos_ok "排空期间新入队被拒：HTTP 503 shutting_down"
else
  chaos_fail "排空期间新入队未被拒：HTTP $reject_code reason=[$reject_reason]"
fi
unset CHAOS_LAST_STATUS
read_code="$(http_get /api/v1/matches/current "$alice3_token")"
read_state="$(jq_path match.state)"
read_room="$(jq_path match.room_id)"
if [ "$read_code" = "200" ] && [ "$read_state" = "matched" ] && [ -n "$read_room" ]; then
  chaos_ok "排空期间**已配对的结果仍可领取**（HTTP 200，state=matched room=$read_room）"
else
  chaos_fail "排空期间已配对的结果取不到：HTTP $read_code state=[$read_state] room=[$read_room]"
fi
log_has_since match.log "$match_mark" "match_enqueue_rejected" &&
  chaos_ok "Match 日志记录了入队被拒（reason=shutting_down）" ||
  chaos_fail "Match 日志没有 match_enqueue_rejected"

wait "$match_term_job" 2>/dev/null
reap_service "$match_pid" "$CHAOS_LOG_DIR/match1"
match_wait="$(read_result "$CHAOS_LOG_DIR/match1" wait)"
match_exit="$(read_result "$CHAOS_LOG_DIR/match1" exit)"
match_pid=""
chaos_info "Match 从 SIGTERM 到退出耗时 ${match_wait} ms，退出码 ${match_exit}"
record_time "场景 3：Match 排空（宽限期）" "恢复" "${match_wait:-0}" "SIGTERM 到进程退出"
[ "$match_exit" = "0" ] && chaos_ok "Match 退出码 0" || chaos_fail "Match 退出码异常：$match_exit"
log_has_since match.log "$match_mark" "event=drain_started" &&
  chaos_ok "Match 日志：drain_started" || chaos_fail "Match 日志没有 drain_started"
if [ "${match_wait:-0}" -ge "$match_grace_ms" ] 2>/dev/null; then
  chaos_ok "Match 等满了宽限期才退出（${match_wait} ms >= ${match_grace_ms} ms）"
else
  chaos_fail "Match 没有等满宽限期：${match_wait} ms（期望 >= ${match_grace_ms} ms）"
fi

# ---------------------------------------------------------------------------
# 4. Gateway：新请求被拒，已建立的 SSE 收到显式关闭事件
# ---------------------------------------------------------------------------
chaos_step "4. Gateway 排空：新请求被拒、已建立 SSE 收到 stream.closed（上限 $gateway_drain_ms ms）"

# Match 在场景 3 已被停掉，这里重置后重新拉起（供登录与读路径使用）。
match_pid=""
reset_match_state "$match_grace_ms" && chaos_ok "Match 已重启（供登录与读路径使用）" || chaos_fail "Match 未就绪"

login_as alice s4a; alice4_token="$LOGIN_TOKEN"
login_as bob s4b; bob4_token="$LOGIN_TOKEN"
c4_room=""
if pair_two "$alice4_token" "$bob4_token" s4; then
  c4_room="$PAIR_ROOM_ID"
  chaos_ok "对局已开始 room=$c4_room"
else
  chaos_fail "场景 4：无法开始对局"
fi

if [ -n "$c4_room" ]; then
  wait_until room_playing "$c4_room" "$alice4_token" >/dev/null || chaos_fail "场景 4：对局未进入 playing"
  sse_subscribe "$alice4_token" "$c4_room" "$CHAOS_LOG_DIR/stream-drain.log" 60
  if sse_wait_event "$CHAOS_LOG_DIR/stream-drain.log" 'event: session.ready' 40; then
    chaos_ok "SSE 订阅已建立（session.ready）"
  else
    chaos_fail "SSE 订阅未建立"
  fi

  gateway_mark="$(wc -l <"$CHAOS_LOG_DIR/gateway.log" 2>/dev/null || echo 0)"
  chaos_info "对局还在打，给 Gateway 发 SIGTERM（排空上限 ${gateway_drain_ms} ms）"
  gateway_stop_pid="$gateway_pid"
  (
    stop_service_measured "Gateway" "$gateway_pid" "$CHAOS_LOG_DIR/gateway1"
  ) &
  gateway_term_job=$!
  gateway_pid=""

  # 排空期间新请求必须被拒。
  sleep 0.5
  unset CHAOS_LAST_STATUS
  gw_code="$(http_post /api/v1/matches "$alice4_token" "{\"request_id\":\"drain-s4-new\"}")"
  gw_reason="$(jq_path error.reason)"
  if [ "$gw_code" = "503" ] && [ "$gw_reason" = "shutting_down" ]; then
    chaos_ok "Gateway 排空期间新请求被拒：HTTP 503 shutting_down"
  else
    chaos_fail "Gateway 排空期间新请求未被拒：HTTP $gw_code reason=[$gw_reason]"
  fi

  wait "$gateway_term_job" 2>/dev/null
  reap_service "$gateway_stop_pid" "$CHAOS_LOG_DIR/gateway1"
  gateway_wait="$(read_result "$CHAOS_LOG_DIR/gateway1" wait)"
  gateway_exit="$(read_result "$CHAOS_LOG_DIR/gateway1" exit)"
  chaos_info "Gateway 从 SIGTERM 到退出耗时 ${gateway_wait} ms，退出码 ${gateway_exit}"
  record_time "场景 4：Gateway 排空" "恢复" "${gateway_wait:-0}" "SIGTERM 到进程退出"
  [ "$gateway_exit" = "0" ] && chaos_ok "Gateway 退出码 0" || chaos_fail "Gateway 退出码异常：$gateway_exit"

  # 已建立的 SSE 必须**被告知**（显式 stream.closed），而不是被直接切断。
  if grep -qF "stream.closed" "$CHAOS_LOG_DIR/stream-drain.log" 2>/dev/null; then
    chaos_ok "SSE 客户端收到显式的 stream.closed"
    grep -qF "server_shutdown" "$CHAOS_LOG_DIR/stream-drain.log" &&
      chaos_ok "关闭事件带上了原因 server_shutdown（客户端能区分「服务退出」与「网络故障」）" ||
      chaos_fail "stream.closed 没有带上 server_shutdown 原因"
  else
    chaos_fail "SSE 客户端没有收到 stream.closed（连接被直接切断了）"
    tail -5 "$CHAOS_LOG_DIR/stream-drain.log" | sed 's/^/     stream: /'
  fi
  if log_has_since gateway.log "$gateway_mark" "drain_timeout_close" ||
     log_has_since gateway.log "$gateway_mark" "drain_finished"; then
    chaos_ok "Gateway 日志记录了排空的收尾（drain_timeout_close / drain_finished）"
  else
    chaos_fail "Gateway 日志没有排空收尾记录"
  fi
fi

# ---------------------------------------------------------------------------
# 5. 收尾
# ---------------------------------------------------------------------------
chaos_step "5. 收尾"

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container 仍健康" || chaos_fail "$container 不健康"
done

chaos_print_time_table

if [ "$keep" -eq 0 ]; then
  cleanup_bench_state
  chaos_ok "已清理合成身份与压测状态（只删 bench-* 与 rooms/match_results）"
fi

echo
if chaos_has_failures; then
  chaos_print_failures
  echo
  echo "日志：$CHAOS_LOG_DIR（room.log / match.log / gateway.log / stream-drain.log）"
  exit 1
fi

echo "验收通过：排空语义的四条路径全部实测。"
echo "  1 Room 能排空完：等对局打完、结果落库（真实胜者）、退出码 0、drain_finished"
echo "  2 Room 排空超时：等满上限后标 ABORTED、match_results 无该局（不伪造胜负）"
echo "  3 Match 排空    ：新入队 503 shutting_down，读状态仍 200（已配对结果仍可领取）"
echo "  4 Gateway 排空  ：新请求 503，已建立 SSE 收到 stream.closed(server_shutdown)"
exit 0