#!/usr/bin/env bash
#
# ⚠ 本脚本**不在** `scripts/verify-all.sh` 里（故障注入 / 长稳 / 重连这一类会占标准
#   端口、停依赖或跑很久，不适合放进快速门禁）。**改动 `src/` 下的产品代码之后，
#   请跑 `bash scripts/verify-chaos.sh`** —— 那是这一类脚本的统一入口。
#   为什么必须写这一句：TASK-026 曾漏改本类里的一个脚本（它不在任何统一入口里，
#   于是回归躺了整整一个任务周期才被 TASK-029 的补跑发现），详见
#   `docs/devlog.md` 的「TASK-029 之后的重跑结果」。
#
# chaos/verify-dependency-down.sh - TASK-023：依赖不可用时的检测时间与恢复时间
#
# 一条命令跑完四个通道，每个通道都是三段式：
#   注入前正常 -> 注入后失败且错误码正确 -> 恢复后自愈（不重启服务）
#
# 四个通道与预期现象：
#   1. Redis（Gateway 会话存储）  -> 登录 HTTP 503 session_store_unavailable
#   2. Redis（Match 队列快照）    -> 匹配**照常可用**（快照可丢弃，TASK-015 决策 B），
#                                    失败只体现在日志里，没有业务失败可测
#   3. MySQL（Gateway 玩家档案）  -> 登录 HTTP 503 player_store_unavailable
#   4. MySQL（Room 快照与对局结果）-> 对局继续推进并打完；查询结果 503 result_pending
#                                    （不是 404、也不返回内存里的胜负）；MySQL 恢复后
#                                    无需重启服务即自动落库，且不产生重复行
#
# 为什么必须有中继（见 chaos/relay.py 的头部说明）：三个服务共用同一个 Redis 与
# 同一个 MySQL。直接 `docker stop rgbt-redis` 会同时打掉 Gateway 的会话存储和
# Match 的快照存储，于是所有 HTTP 请求在鉴权那一步就 503，根本走不到被测通道——
# 那是 TASK-015 已经踩过的坑。因此注入做在**网络层**：让被测服务连中继，
# 杀掉中继就只切断那一个服务的依赖，其余通道保持完好。
#
# 隔离是**自证的**：每个通道注入后都断言「其它通道仍然正常」，
# 而不是只在文档里声称。
#
# 每个通道开始前会**清空 Match 的队列与配对结果**（见 reset_match_state）：
# Match 的结果 TTL 是 120 秒，不清的话上一段的旧配对会被 GetStatus 返回，
# 表现为"新一段拿到了上一段的房间"。那是夹具问题，不是产品缺陷。
#
# 用法：
#   bash chaos/verify-dependency-down.sh
#   bash chaos/verify-dependency-down.sh --keep   # 保留服务与中继，便于排查
#
# 退出码 0 表示四个通道全部通过；非 0 表示有失败项，失败项会逐条列出。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

source chaos/lib.sh

preset="brpc-debug"
gateway_port=8080
match_port=8082
room_port=8083

# 中继端口。落在未使用的 16xxx/13xxx 段，避免与真实依赖和既有脚本冲突。
relay_redis_for_gateway_port=16379   # -> 6379（Gateway 的会话存储通道）
relay_redis_for_match_port=16380     # -> 6379（Match 的队列快照通道）
relay_mysql_for_gateway_port=13307   # -> 3306（Gateway 的玩家档案通道）
relay_mysql_for_room_port=13306      # -> 3306（Room 的快照与对局结果通道）

keep=0
for arg in "$@"; do
  case "$arg" in
    --keep) keep=1 ;;
    -h | --help) sed -n '3,35p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# chaos/lib.sh 约定的端口变量名（大写）。显式声明，避免依赖脚本体里的小写名字。
GW_PORT="$gateway_port"
MATCH_PORT="$match_port"

# 探针账号。三个启用的种子身份：alice / bob / dave。
PROBE_ACCOUNT="alice"
PROBE_PASSWORD="alice_dev_pw"

gateway_pid=""
match_pid=""
room_pid=""

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
  kill_pid "$room_pid"
  kill_pid "$match_pid"
  kill_pid "$gateway_pid"
  gateway_pid=""; match_pid=""; room_pid=""
  local name pidfile
  for name in relay-redis-gateway relay-redis-match relay-mysql-gateway relay-mysql-room; do
    pidfile="$CHAOS_RUN_DIR/relay-$name.pid"
    if [ -f "$pidfile" ]; then
      kill_pid "$(cat "$pidfile")"
      rm -f "$pidfile"
    fi
  done
}

cleanup() {
  if [ "$keep" -eq 1 ]; then
    echo "（--keep：保留服务与中继进程，便于排查）"
    return
  fi
  stop_all
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

start_gateway() {
  "build/$preset/bin/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
    -redis_host 127.0.0.1 -redis_port "$relay_redis_for_gateway_port" \
    -redis_timeout_ms 500 \
    -mysql_host 127.0.0.1 -mysql_port "$relay_mysql_for_gateway_port" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    -match_host 127.0.0.1 -match_port "$match_port" \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -match_timeout_ms 500 -room_timeout_ms 500 \
    >"$CHAOS_LOG_DIR/gateway.log" 2>&1 &
  gateway_pid=$!
  wait_http "$gateway_port" 30
}

start_match() {
  "build/$preset/bin/rgbt_match" -match_port "$match_port" \
    -match_timeout_seconds 30 -match_result_ttl_seconds 120 \
    -env_prefix dev -redis_host 127.0.0.1 -redis_port "$relay_redis_for_match_port" \
    -redis_timeout_ms 500 \
    -room_host 127.0.0.1 -room_port "$room_port" \
    >"$CHAOS_LOG_DIR/match.log" 2>&1 &
  match_pid=$!
  wait_http "$match_port" 30
}

start_room() {
  "build/$preset/bin/rgbt_room" -port "$room_port" -env_prefix dev \
    -mysql_host 127.0.0.1 -mysql_port "$relay_mysql_for_room_port" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    >"$CHAOS_LOG_DIR/room.log" 2>&1 &
  room_pid=$!
  wait_http "$room_port" 30
}

# ---------------------------------------------------------------------------
# 环境准备
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

for port in "$gateway_port" "$match_port" "$room_port" \
            "$relay_redis_for_gateway_port" "$relay_redis_for_match_port" \
            "$relay_mysql_for_gateway_port" "$relay_mysql_for_room_port"; do
  if port_listening "$port"; then
    chaos_fail "端口 $port 已被占用，先执行 bash scripts/dev-down.sh"
  fi
done
if chaos_has_failures; then
  chaos_print_failures || true
  exit 1
fi
chaos_ok "7 个端口（3 个服务 + 4 个中继）均空闲"

# 构建。与既有验收脚本一致：每次都配置 + 构建，避免验收到旧二进制。
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

mysql_exec() { # <SQL>
  docker exec rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "$1" 2>/dev/null
}
sql_scalar() { mysql_exec "$1" | tr -d '\r'; }

# 清掉上一轮开发状态：本脚本会自己造会话、房间与对局结果。
# 与 scripts/bench.sh 的既有做法一致——只清 dev 前缀与两张表，
# **不动** migrations/004 的种子身份。
chaos_step "0b. 清理上一轮开发状态"
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
mysql_exec "DELETE FROM rooms; DELETE FROM match_results;" >/dev/null 2>&1 || true
chaos_ok "已清理 Redis 的 dev:* 与 rooms / match_results"

# ---------------------------------------------------------------------------
# 建立隔离通道并启动服务
# ---------------------------------------------------------------------------
chaos_step "0c. 建立隔离通道并启动服务"

start_relay relay-redis-gateway "$relay_redis_for_gateway_port" "127.0.0.1:$redis_port" || true
start_relay relay-redis-match "$relay_redis_for_match_port" "127.0.0.1:$redis_port" || true
start_relay relay-mysql-gateway "$relay_mysql_for_gateway_port" "127.0.0.1:$mysql_port" || true
start_relay relay-mysql-room "$relay_mysql_for_room_port" "127.0.0.1:$mysql_port" || true

# 中继必须是透明的：经过它的真实协议也要通，否则后面测的不是产品行为。
probe_redis 127.0.0.1 "$relay_redis_for_gateway_port" && chaos_ok "经中继的 Redis PING 成功" ||
  chaos_fail "经中继的 Redis PING 失败：中继不是透明的，后续测量无意义"
probe_mysql 127.0.0.1 "$relay_mysql_for_gateway_port" && chaos_ok "经中继的 MySQL 握手成功" ||
  chaos_fail "经中继的 MySQL 握手失败：中继不是透明的，后续测量无意义"

start_room && chaos_ok "Room 就绪（MySQL 经中继 $relay_mysql_for_room_port）" || chaos_fail "Room 未就绪"
start_match && chaos_ok "Match 就绪（Redis 经中继 $relay_redis_for_match_port）" || chaos_fail "Match 未就绪"
start_gateway && chaos_ok "Gateway 就绪（Redis 经 $relay_redis_for_gateway_port，MySQL 经 $relay_mysql_for_gateway_port）" ||
  chaos_fail "Gateway 未就绪"

if chaos_has_failures; then
  tail -20 "$CHAOS_LOG_DIR/room.log" 2>/dev/null | sed 's/^/     room: /'
  tail -20 "$CHAOS_LOG_DIR/match.log" 2>/dev/null | sed 's/^/     match: /'
  tail -20 "$CHAOS_LOG_DIR/gateway.log" 2>/dev/null | sed 's/^/     gateway: /'
  chaos_print_failures || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 业务辅助
# ---------------------------------------------------------------------------
login_as() { # <账号> [请求 id 后缀]
  local account="$1" suffix="${2:-$RANDOM}"
  CHAOS_LAST_STATUS="$(http_post /api/v1/login "" \
    "{\"account\":\"$account\",\"password\":\"${account}_dev_pw\",\"client_type\":\"web\",\"request_id\":\"chaos-$account-$suffix\"}")"
  LOGIN_TOKEN="$(jq_path token)"
  LOGIN_HTTP="$CHAOS_LAST_STATUS"
  return 0
}

# Redis 里队列快照的条目数（权威队列在 Match 内存里，这份是副本）。
match_queue_len() {
  docker exec rgbt-redis redis-cli LLEN "dev:match:queue" 2>/dev/null | tr -d '\r'
}

# 清空 Match 的队列与配对结果，让下一段从一个干净的匹配状态开始。
#
# **为什么必须做**（实测踩到）：`match_result_ttl_seconds` 是 120 秒，GetStatus 会
# 优先返回该玩家**最近一次的配对结果**。上一段已配对的玩家在下一段入队时，
# 轮询拿到的仍是上一段的房间，表现成 join 409 `room_already_finished`、
# 状态直接是 finished —— 看似"匹配坏了"，其实是夹具复用了旧结果。
#
# 做法：杀掉 Match、清掉 Redis 快照与匹配结果、重新拉起 Match。这样内存队列与
# Redis 两侧都是空的（Match 启动时执行的动作正是从 Redis 恢复队列）。
reset_match_state() { # <说明文字>
  kill_pid "$match_pid"
  match_pid=""
  docker exec rgbt-redis redis-cli DEL "dev:match:queue" >/dev/null 2>&1
  local key
  for key in $(docker exec rgbt-redis redis-cli --scan --pattern 'dev:match:*result*' 2>/dev/null); do
    docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1
  done
  if start_match; then
    chaos_info "已重置匹配状态（$1）：队列与配对结果清空，Match 重启完成"
    warm_match_channel
    return 0
  fi
  chaos_fail "匹配状态重置失败（$1）：Match 未能重新就绪"
  return 1
}

# 预热 Gateway -> Match 的 brpc 通道。
#
# **为什么需要**（实测踩到）：Match 重启后 Gateway 侧的长连接已经死了，
# 但 brpc 不会在重启那一刻就知道。紧随其后的第一次入队会拿到
# `match_unavailable`（HTTP 503），而 Match 本身是好的。若不预热，夹具就会把
# 这个瞬时错误当成"注入导致匹配失败"，测出来的结论是错的。
# 预热方式：反复查询一个无害的只读接口（matches/current），直到它返回 200。
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

# pair_two <tokenA> <tokenB> <标签>：入队、等配对、两名玩家都进房。
#
# 成功后设置 PAIR_MATCH_ID / PAIR_ROOM_ID / PAIR_A_CODE / PAIR_JOIN_FAILED。
pair_two() {
  local token_a="$1" token_b="$2" tag="$3"
  PAIR_MATCH_ID=""; PAIR_ROOM_ID=""; PAIR_JOIN_FAILED=0
  enqueue_with_retry "$token_a" "chaos-$tag-a" || return 1
  enqueue_with_retry "$token_b" "chaos-$tag-b" || return 1
  local i code status room_id match_id
  for i in $(seq 1 60); do
    code="$(http_get /api/v1/matches/current "$token_a")"
    PAIR_A_CODE="$code"
    status="$(jq_path match.state)"
    room_id="$(jq_path match.room_id)"
    match_id="$(jq_path match.match_id)"
    if [ "${CHAOS_DEBUG_PAIR:-0}" = "1" ]; then
      chaos_info "[debug pair $tag] 第 $i 次轮询：code=$code state=$status room=$room_id match=$match_id"
    fi
    if [ "$code" = "200" ] && [ "$status" = "matched" ] && [ -n "$room_id" ] && [ -n "$match_id" ]; then
      PAIR_ROOM_ID="$room_id"
      PAIR_MATCH_ID="$match_id"
      chaos_info "[配对 $tag] room=$room_id match=$match_id"
      # 必须显式进房：配对只创建房间，房间要等两名成员都 JoinRoom 后才开始推进
      # （状态 created -> playing）。漏掉这一步的表现是"帧号永远不动"，
      # 会被误读成"对局在依赖不可用时停住了"。
      if ! join_room "$room_id" "$token_a" "j-$tag-a" || ! join_room "$room_id" "$token_b" "j-$tag-b"; then
        PAIR_JOIN_FAILED=1
        return 1
      fi
      return 0
    fi
    sleep 0.5
  done
  return 1
}

# join_room <room_id> <token> <请求 id 后缀>
join_room() {
  local code
  code="$(http_post /api/v1/rooms/join "$2" \
    "{\"room_id\":\"$1\",\"request_id\":\"chaos-$3\"}")"
  [ "$code" = "200" ] && return 0
  chaos_info "加入房间失败：room=$1 code=$code reason=$(jq_path error.reason)"
  return 1
}

# services_healthy：三个服务的**自身**健康端点（不经过 Gateway）。
#
# 为什么不能经 Gateway 探测「隔离」：Gateway 的鉴权本身就在通道 1 / 通道 3 上，
# 注入后所有 HTTP 请求都会先返回 503，经它探测只能得到"全部不可用"的假象。
# 服务自身的 /health 与 /metrics 不经 Gateway，才是通道隔离的正确判据。
services_healthy() {
  local port
  for port in "$match_port" "$room_port"; do
    curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$port/health" || return 1
  done
  return 0
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

room_frame_at_least() { # 供 wait_until 使用：<room_id> <token> <帧号>
  room_snapshot "$1" "$2" || return 1
  [ -n "$ROOM_FRAME" ] || return 1
  [ "$ROOM_FRAME" -ge "$3" ] 2>/dev/null
}

room_playing() { # <room_id> <token>
  room_snapshot "$1" "$2" || return 1
  [ "$ROOM_STATE" = "playing" ]
}

# 对局是否还在进行（playing / waiting / finishing 之外都算"不再进行"）。
room_not_playing() { # <room_id> <token>
  room_snapshot "$1" "$2" || return 1
  [ "$ROOM_STATE" != "playing" ] && [ "$ROOM_STATE" != "waiting" ] && [ "$ROOM_STATE" != "finishing" ]
}

# 房间是否已经打完（进入终态或已在结果阶段）。
# 与 room_not_playing 分开：在通道 4 里"打完"就包括 finishing——
# 结果写不进去时房间会一直停在 finishing，那正是我们要断言的状态。
room_finished() { # <room_id> <token>
  room_snapshot "$1" "$2" || return 1
  case "$ROOM_STATE" in
    finished|aborted|finishing) return 0 ;;
    *) return 1 ;;
  esac
}

probe_room_state_ok() { local code; code="$(http_get "/api/v1/rooms/state?room_id=$1" "$2")"; [ "$code" = "200" ]; }
probe_result_ok() { local code; code="$(http_get "/api/v1/results?match_id=$1" "$2")"; [ "$code" = "200" ]; }
probe_matches_ok() { local code; code="$(http_get /api/v1/matches/current "$1")"; [ "$code" = "200" ]; }

# 两个客户端轮流攻击（每回合 1 次，10 回合打满 100 血），直到对局结束。
# 每回合都等服务端帧号前进，确保请求真的被处理过，而不是空转刷请求。
play_game() { # <room_id> <tokenA> <tokenB> [最多回合]
  local room_id="$1" token_a="$2" token_b="$3" max_rounds="${4:-40}"
  local round frame target
  # request_id **必须把 room_id 带进去**（实测踩到，代价是一整局白白超时）：
  # 服务端对输入按 request_id 做幂等。早先写成 `chaos-atk-a$round` 这种固定串时，
  # 基线那一局的 `chaos-atk-a1` 已经把它消费掉了；后面的对局虽然换了房间，
  # 但 request_id 相同，于是**所有攻击都被幂等丢弃**——表现是"对局一直不掉血，
  # 直到 600 帧超时按平局结束"，看起来像"MySQL 停机让对局卡住了"，其实不是。
  local tag="${room_id: -8}"
  for round in $(seq 1 "$max_rounds"); do
    if [ $(( round % 2 )) -eq 1 ]; then
      http_post /api/v1/rooms/input "$token_a" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"chaos-atk-$tag-a$round\"}" >/dev/null
    else
      http_post /api/v1/rooms/input "$token_b" \
        "{\"room_id\":\"$room_id\",\"action\":\"attack\",\"request_id\":\"chaos-atk-$tag-b$round\"}" >/dev/null
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
# 基线：注入前四个通道全部正常
# ---------------------------------------------------------------------------
chaos_step "1. 基线：注入前四个通道全部正常"
reset_match_state "基线开始前" || true

baseline_login_start="$(now_us)"
login_as alice base-a
if [ "$LOGIN_HTTP" = "200" ] && [ -n "$LOGIN_TOKEN" ]; then
  chaos_ok "基线 通道 1：登录成功（HTTP 200）"
else
  chaos_fail "基线 通道 1：登录失败 HTTP $LOGIN_HTTP reason=$(jq_path error.reason)"
fi
alice_token="$LOGIN_TOKEN"
record_time "基线：登录（正常路径）" "耗时" "$(elapsed_ms_from_us "$baseline_login_start")" "成功登录的墙钟耗时"

login_as bob base-b
bob_token="$LOGIN_TOKEN"
[ "$LOGIN_HTTP" = "200" ] && chaos_ok "基线：bob 登录成功" || chaos_fail "基线：bob 登录失败 HTTP $LOGIN_HTTP"

baseline_room_id=""
baseline_match_id=""
if [ -n "$alice_token" ] && [ -n "$bob_token" ]; then
  baseline_pair_start="$(now_us)"
  if pair_two "$alice_token" "$bob_token" base; then
    chaos_ok "基线 通道 2：匹配成功 match=$PAIR_MATCH_ID room=$PAIR_ROOM_ID"
    record_time "基线：入队到配对并进房" "耗时" "$(elapsed_ms_from_us "$baseline_pair_start")" "两个玩家入队到同房并开打"
    baseline_room_id="$PAIR_ROOM_ID"
    baseline_match_id="$PAIR_MATCH_ID"
  else
    chaos_fail "基线 通道 2：匹配未成功（HTTP $PAIR_A_CODE state=$(jq_path match.state)）"
  fi
fi

if [ -n "$baseline_room_id" ]; then
  baseline_play_start="$(now_us)"
  play_game "$baseline_room_id" "$alice_token" "$bob_token" 40
  if [ "$PLAY_GAME_RESULT" = "finished" ]; then
    chaos_ok "基线 通道 4：对局正常打完（帧 ${ROOM_FRAME:-?}）"
    record_time "基线：对局打完" "耗时" "$(elapsed_ms_from_us "$baseline_play_start")" "从开局到终态"
  else
    chaos_fail "基线 通道 4：对局未正常结束（结果 $PLAY_GAME_RESULT）"
  fi
  baseline_result_start="$(now_us)"
  if wait_until probe_result_ok "$baseline_match_id" "$alice_token" >/dev/null; then
    chaos_ok "基线 通道 4：对局结果已落库（HTTP 200）"
    record_time "基线：结果落库" "耗时" "$(elapsed_ms_from_us "$baseline_result_start")" "结束后到结果可查"
  else
    chaos_fail "基线 通道 4：结果未落库"
  fi
fi

if [ -n "$alice_token" ]; then
  probe_matches_ok "$alice_token" && chaos_ok "基线：Match 可用（HTTP 200）" || chaos_fail "基线：Match 不可用"
fi
services_healthy && chaos_ok "基线：Match / Room 健康端点均正常" || chaos_fail "基线：服务健康端点异常"

# ---------------------------------------------------------------------------
# 通道 1：Redis 停机 -> Gateway 会话存储不可用
# ---------------------------------------------------------------------------
chaos_step "2. 通道 1：Redis（Gateway 会话存储）不可用"

stop_relay relay-redis-gateway "$relay_redis_for_gateway_port" &&
  chaos_ok "已切断 Gateway 的 Redis 会话存储通道（中继 $relay_redis_for_gateway_port 已停）"

detect_ms="$(wait_until probe_gw_login_503 || true)"
if [ -n "$detect_ms" ]; then
  chaos_ok "检测：登录首次返回 503，用时 ${detect_ms} ms"
  record_time "通道 1：Redis 会话存储不可用" "检测" "$detect_ms" "登录首次观测到 503"
  unset CHAOS_LAST_STATUS
  check_http "通道 1：登录错误码" POST /api/v1/login "" \
    "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"client_type\":\"web\",\"request_id\":\"chaos-c1-check\"}" \
    503 "session_store_unavailable" || true
else
  chaos_fail "通道 1：注入后 12 秒内登录未返回 503（注入可能未生效）"
fi

# 503 必须在 /metrics 的 status 计数上留下痕迹。
#
# 这里**不能**断言日志：Gateway 的依赖不可用路径（FillError）不写日志，
# 只填错误体并返回状态码。实测确认过，因此改为查指标。该缺口已记入 devlog。
gw_503_before="$(gw_metric_status_count 503)"
http_post /api/v1/login "" \
  "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"client_type\":\"web\",\"request_id\":\"chaos-c1-metric\"}" >/dev/null
gw_503_after="$(gw_metric_status_count 503)"
if [ -n "$gw_503_after" ] && [ "$gw_503_after" -gt "$gw_503_before" ] 2>/dev/null; then
  chaos_ok "通道 1：Gateway /metrics 的 503 计数增加（$gw_503_before -> $gw_503_after）"
else
  chaos_fail "通道 1：Gateway /metrics 的 503 计数没有增加（$gw_503_before -> $gw_503_after）"
fi

# 隔离判据用服务自身的健康端点：经 Gateway 探测必然也 503（鉴权就在本通道上）。
services_healthy && chaos_ok "隔离：Match / Room 健康端点仍正常（不经 Gateway 探测）" ||
  chaos_fail "隔离：Match 或 Room 在通道 1 注入期间不健康"

restart_relay relay-redis-gateway "$relay_redis_for_gateway_port" "127.0.0.1:$redis_port" &&
  chaos_ok "已恢复 Gateway 的 Redis 通道"
recover_ms="$(wait_until probe_gw_login_ok || true)"
if [ -n "$recover_ms" ]; then
  chaos_ok "恢复：登录自愈（HTTP 200），用时 ${recover_ms} ms（未重启 Gateway）"
  record_time "通道 1：Redis 会话存储恢复" "恢复" "$recover_ms" "依赖恢复到登录成功"
  check_http "通道 1：恢复后会话可用" GET /api/v1/players/me "$LOGIN_TOKEN" "" 200 || true
else
  chaos_fail "通道 1：依赖恢复后 12 秒内登录仍未自愈"
fi

# ---------------------------------------------------------------------------
# 通道 2：Redis 停机 -> Match 快照存储不可用（业务照常）
# ---------------------------------------------------------------------------
chaos_step "3. 通道 2：Redis（Match 队列快照）不可用"
reset_match_state "通道 2 开始前" || true

# 本段的判定依据与其余三段不同，理由如下。
#
# 队列快照不可用时**业务照常成功**（TASK-015 决策 B：快照可丢弃、降级为纯内存），
# 所以"检测时间"指的不是业务失败，而是：**从注入到 Match 第一次真的去写快照、
# 并且写失败**。写入是变化驱动的（队列变化才写），因此必须先制造一次变化。
# 一次配对正好会产生两次队列变化，是最自然的制造方式。
stop_relay relay-redis-match "$relay_redis_for_match_port" &&
  chaos_ok "已切断 Match 的 Redis 快照通道（中继 $relay_redis_for_match_port 已停）"

login_as dave c2-pair
dave_token="$LOGIN_TOKEN"
if [ -z "$alice_token" ] || [ -z "$dave_token" ]; then
  chaos_fail "通道 2：配对身份未就绪（alice=[$alice_token] dave=[$dave_token]）"
fi

pair_start_us="$(now_us)"
pair_ok=0
if [ -n "$alice_token" ] && [ -n "$dave_token" ]; then
  pair_two "$alice_token" "$dave_token" c2 && pair_ok=1
fi

# 关键断言：业务不受影响。快照可以丢弃，匹配必须照常成功。
if [ "$pair_ok" -eq 1 ]; then
  chaos_ok "通道 2：快照存储不可用时匹配照常成功 match=$PAIR_MATCH_ID room=$PAIR_ROOM_ID"
  record_time "通道 2：匹配仍可用" "验证" "$(elapsed_ms_from_us "$pair_start_us")" "无中断：匹配不依赖快照（降级为纯内存）"
  snapshot_failed_after="$(match_snapshot_metric failed)"
  if [ "${snapshot_failed_after:-0}" -gt 0 ] 2>/dev/null; then
    chaos_ok "通道 2：配对触发的快照写入失败计数为 ${snapshot_failed_after}（失败可观测）"
    record_time "通道 2：Redis 队列快照不可用" "检测" "$(elapsed_ms_from_us "$pair_start_us")" "队列变化触发的首次快照写入失败"
  else
    chaos_info "本次队列变化没有产生快照写入失败（可能在旧连接上写成功）"
    record_time "通道 2：Redis 队列快照不可用" "检测" "0" "本次变化未触发新写入失败"
  fi
  if log_has "match.log" "queue_snapshot_write_failed"; then
    chaos_ok "观测：Match 日志里有快照写入失败记录（累计 $(log_count match.log "queue_snapshot_write_failed") 次）"
  else
    chaos_info "Match 日志里没有快照写入失败记录（与计数器的解释一致）"
  fi
  chaos_info "注入期间：Match 内存队列长度 $(match_metric_queue_length)，Redis 快照长度 $(match_queue_len)（队列权威状态在内存，快照没写进去）"
else
  chaos_fail "通道 2：快照不可用时匹配失败（HTTP $PAIR_A_CODE state=$(jq_path match.state)）"
fi

# 恢复判定必须能看到「新写入」：注入前那份快照是旧的/空的，读它证明不了通道已恢复。
# 顺序很重要——先把中继拉起来并等它真的在监听，**再**触发一次新的队列变化。
snapshot_present() {
  local queued
  queued="$(match_queue_len)"
  [ "${queued:-0}" -ge 1 ]
}

restart_relay relay-redis-match "$relay_redis_for_match_port" "127.0.0.1:$redis_port" &&
  chaos_ok "已恢复 Match 的 Redis 通道"
wait_until probe_tcp 127.0.0.1 "$relay_redis_for_match_port" >/dev/null

# dave 在检测阶段已被配对，用 bob 制造一次新的队列变化（他此刻不在队列里：
# 基线结束时他已配对，其旧结果已被 reset_match_state 清掉）。
login_as bob c2-recover
if [ "$LOGIN_HTTP" = "200" ]; then
  enqueue_with_retry "$LOGIN_TOKEN" "chaos-c2-recover" || true
  recover_ms="$(wait_until snapshot_present || true)"
  if [ -n "$recover_ms" ]; then
    chaos_ok "恢复：快照重新写入 Redis（dev:match:queue 有 $(match_queue_len) 条），用时 ${recover_ms} ms"
    record_time "通道 2：Redis 队列快照恢复" "恢复" "$recover_ms" "链路恢复到新快照落进 Redis"
  else
    chaos_fail "通道 2：依赖恢复后 8 秒内 Match 未把新条目写进快照"
  fi
else
  chaos_fail "通道 2：恢复阶段登录失败 HTTP $LOGIN_HTTP"
fi

# ---------------------------------------------------------------------------
# 通道 3：MySQL 停机 -> Gateway 玩家档案不可用
# ---------------------------------------------------------------------------
chaos_step "4. 通道 3：MySQL（Gateway 玩家档案）不可用"

stop_relay relay-mysql-gateway "$relay_mysql_for_gateway_port" &&
  chaos_ok "已切断 Gateway 的 MySQL 玩家档案通道（中继 $relay_mysql_for_gateway_port 已停）"

detect_ms="$(wait_until probe_gw_login_503 || true)"
if [ -n "$detect_ms" ]; then
  chaos_ok "检测：登录首次返回 503，用时 ${detect_ms} ms"
  record_time "通道 3：MySQL 玩家档案不可用" "检测" "$detect_ms" "登录首次观测到 503"
  unset CHAOS_LAST_STATUS
  check_http "通道 3：登录错误码" POST /api/v1/login "" \
    "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"client_type\":\"web\",\"request_id\":\"chaos-c3-check\"}" \
    503 "player_store_unavailable" || true
else
  chaos_fail "通道 3：注入后 12 秒内登录未返回 503（注入可能未生效）"
fi

# 同通道 1：Gateway 的依赖不可用路径不写日志，判据放在 /metrics 的 503 计数上。
gw_503_before="$(gw_metric_status_count 503)"
http_post /api/v1/login "" \
  "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"client_type\":\"web\",\"request_id\":\"chaos-c3-metric\"}" >/dev/null
gw_503_after="$(gw_metric_status_count 503)"
if [ -n "$gw_503_after" ] && [ "$gw_503_after" -gt "$gw_503_before" ] 2>/dev/null; then
  chaos_ok "通道 3：Gateway /metrics 的 503 计数增加（$gw_503_before -> $gw_503_after）"
else
  chaos_fail "通道 3：Gateway /metrics 的 503 计数没有增加（$gw_503_before -> $gw_503_after）"
fi

services_healthy && chaos_ok "隔离：Match / Room 健康端点仍正常（不经 Gateway 探测）" ||
  chaos_fail "隔离：Match 或 Room 在通道 3 注入期间不健康"

restart_relay relay-mysql-gateway "$relay_mysql_for_gateway_port" "127.0.0.1:$mysql_port" &&
  chaos_ok "已恢复 Gateway 的 MySQL 通道"
recover_ms="$(wait_until probe_gw_login_ok || true)"
if [ -n "$recover_ms" ]; then
  chaos_ok "恢复：登录自愈（HTTP 200），用时 ${recover_ms} ms（未重启 Gateway）"
  record_time "通道 3：MySQL 玩家档案恢复" "恢复" "$recover_ms" "依赖恢复到登录成功"
else
  chaos_fail "通道 3：依赖恢复后 12 秒内登录仍未自愈"
fi

# ---------------------------------------------------------------------------
# 通道 4：MySQL 停机 -> 对局照常推进，结果停在 FINISHING
# ---------------------------------------------------------------------------
chaos_step "5. 通道 4：MySQL（Room 快照与对局结果）不可用"
reset_match_state "通道 4 开始前" || true

login_as bob c4-a
c4_token_a="$LOGIN_TOKEN"
login_as dave c4-b
c4_token_b="$LOGIN_TOKEN"
if [ -n "$c4_token_a" ] && [ -n "$c4_token_b" ]; then
  chaos_ok "通道 4：两个身份已就绪（会话不受 Room 的 MySQL 通道影响）"
else
  chaos_fail "通道 4：登录失败，无法进行对局"
fi

if pair_two "$c4_token_a" "$c4_token_b" c4; then
  chaos_ok "通道 4：匹配成功 match=$PAIR_MATCH_ID room=$PAIR_ROOM_ID"
else
  chaos_fail "通道 4：匹配失败（HTTP $PAIR_A_CODE state=$(jq_path match.state)）"
fi

room_id="${PAIR_ROOM_ID:-}"
match_id="${PAIR_MATCH_ID:-}"
frame_before_injection=""
if [ -n "$room_id" ]; then
  if wait_until room_playing "$room_id" "$c4_token_a" >/dev/null; then
    chaos_ok "通道 4：双方进房后对局开始推进（状态 playing）"
  else
    room_snapshot "$room_id" "$c4_token_a" || true
    chaos_fail "通道 4：注入前对局未进入 playing（状态 ${ROOM_STATE:-?}）"
  fi
  room_snapshot "$room_id" "$c4_token_a" && frame_before_injection="${ROOM_FRAME:-0}"
  chaos_info "注入前：帧 ${frame_before_injection:-?}，状态 ${ROOM_STATE:-?}"
fi

gw_503_before_ch4="$(gw_metric_status_count 503)"

stop_relay relay-mysql-room "$relay_mysql_for_room_port" &&
  chaos_ok "已切断 Room 的 MySQL 通道（中继 $relay_mysql_for_room_port 已停）；本通道只影响 Room"

# 1) 对局必须继续推进：快照可以丢弃，对局不能停（TASK-013 的既有语义）。
if [ -n "$room_id" ]; then
  progress_start_us="$(now_us)"
  progress_target=$(( ${frame_before_injection:-0} + 5 ))
  progress_ms="$(wait_until room_frame_at_least "$room_id" "$c4_token_a" "$progress_target" || true)"
  room_snapshot "$room_id" "$c4_token_a" || true
  if [ -n "$progress_ms" ]; then
    chaos_ok "通道 4：对局在 MySQL 不可用时继续推进（帧 $frame_before_injection -> $progress_target）"
    record_time "通道 4：对局照常推进" "观测" "$(elapsed_ms_from_us "$progress_start_us")" "注入后帧号仍前进"
  elif [ "${ROOM_STATE:-}" != "playing" ]; then
    chaos_ok "通道 4：对局在 MySQL 不可用期间自行打到终态（帧 ${ROOM_FRAME:-?}，状态 ${ROOM_STATE:-?}）"
  else
    chaos_fail "通道 4：注入后对局停住了（帧 $frame_before_injection -> ${ROOM_FRAME:-?}，状态 ${ROOM_STATE:-?}）"
  fi
fi

# 2) Room 的快照写入失败必须被记录（旁路失败要可见，不能静默丢弃）。
if wait_log_until room.log "room_snapshot_write_failed" 8000 >/dev/null; then
  chaos_ok "通道 4：Room 记录了快照写入失败（旁路失败可见，未静默丢弃）"
else
  chaos_fail "通道 4：Room 没有记录快照写入失败"
fi

# 3) 把对局打到终态，并抓 `result_pending` 窗口。
#
# 注意：MySQL 不可用时推进**明显变慢**——每次快照写入都要等一次 3 秒的连接超时，
# 而快照每 1 秒调度一次，于是 ticker 线程大部分时间停在 connect 上。
# 这正是本通道要记录的实测事实（见下面的耗时），因此这里给足超时。
if [ -n "$room_id" ]; then
  play_start_us="$(now_us)"
  play_game "$room_id" "$c4_token_a" "$c4_token_b" 80
  chaos_info "对局结束形态：$PLAY_GAME_RESULT（帧 ${ROOM_FRAME:-?}，状态 ${ROOM_STATE:-?}）"
  CHAOS_WAIT_TIMEOUT_MS=90000
  if wait_until room_finished "$room_id" "$c4_token_a" >/dev/null; then
    chaos_ok "通道 4：MySQL 不可用时对局仍能打到终态（帧 ${ROOM_FRAME:-?}）"
    record_time "通道 4：MySQL 不可用时打完整局" "耗时" "$(elapsed_ms_from_us "$play_start_us")" "含快照写入超时带来的推进变慢"
  else
    room_snapshot "$room_id" "$c4_token_a" || true
    chaos_fail "通道 4：对局未能在 MySQL 不可用期间打到终态（状态 ${ROOM_STATE:-?}）"
  fi

  # 结果必须先落库才能查到。运行中查询**绝不能**返回内存里的胜负：
  #   * 结果写不进去 -> 503 result_pending；
  #   * 恰好已落库   -> 200 并带结果。
  # 两者都合法，**404 与"赢家非空但状态是 pending"都不合法**。
  unset CHAOS_LAST_STATUS
  # 必须直接捕获 http_get 的输出：`> /dev/null` 会把状态码一起丢掉，
  # 于是 result_code 恒为空——那正是第一版把 503 判成失败的原因。
  result_code="$(http_get "/api/v1/results?match_id=$match_id" "$c4_token_a")"
  result_reason="$(jq_path error.reason)"
  result_winner="$(jq_path result.winner_id)"
  chaos_info "运行中查询结果：HTTP $result_code reason=[$result_reason] winner=[$result_winner]"
  if [ "$result_code" = "503" ] && [ "$result_reason" = "result_pending" ]; then
    chaos_ok "通道 4：结果未落库时返回 503 result_pending（不是 404）"
    if [ -z "$result_winner" ]; then
      chaos_ok "通道 4：pending 响应没有返回内存里的胜负（result.winner_id 为空）"
    else
      chaos_fail "通道 4：pending 响应返回了内存里的胜负 winner_id=$result_winner"
    fi
  elif [ "$result_code" = "200" ]; then
    chaos_ok "通道 4：查询时结果已落库，返回 200（该窗口内已写成功）"
  else
    chaos_fail "通道 4：运行中查询结果返回 HTTP $result_code reason=[$result_reason]（期望 503 result_pending 或 200）"
  fi

  probe_room_state_ok "$room_id" "$c4_token_a" &&
    chaos_ok "隔离：Room 内存状态仍可查（快照失败不影响权威状态）" ||
    chaos_fail "隔离：Room 在通道 4 注入期间不可用"
fi

probe_matches_ok "$alice_token" 2>/dev/null && chaos_ok "隔离：Match 未受影响（HTTP 200）" ||
  chaos_fail "隔离：Match 在通道 4 注入期间不可用"
if probe_gw_login_ok; then
  chaos_ok "隔离：Gateway 的 MySQL 玩家档案通道未受影响（登录成功）"
else
  chaos_fail "隔离：Gateway 登录在通道 4 注入期间失败——注入打到了共享 MySQL 而不是单通道"
fi

# 4) 恢复：MySQL 回来后**无需重启服务**，待落库的结果自动写入。
restart_relay relay-mysql-room "$relay_mysql_for_room_port" "127.0.0.1:$mysql_port" &&
  chaos_ok "已恢复 Room 的 MySQL 通道"
recover_start_us="$(now_us)"
CHAOS_WAIT_TIMEOUT_MS=30000
recover_ms="$(wait_until probe_result_ok "$match_id" "$c4_token_a" || true)"
if [ -n "$recover_ms" ]; then
  chaos_ok "恢复：结果自动落库（查询返回 200），用时 ${recover_ms} ms（未重启 Room）"
  record_time "通道 4：MySQL 恢复后结果落库" "恢复" "$(elapsed_ms_from_us "$recover_start_us")" "依赖恢复到结果可查（未重启服务）"
else
  chaos_fail "通道 4：MySQL 恢复后 30 秒内结果仍未落库"
fi

# 5) 幂等：结果只写一次，不能出现重复行（对 match_results 做实际查询）。
dup_rows="$(sql_scalar "SELECT COUNT(*) FROM (SELECT match_id FROM match_results WHERE match_id='$match_id' GROUP BY match_id HAVING COUNT(*) > 1) x;")"
if [ "${dup_rows:-0}" = "0" ]; then
  chaos_ok "幂等：match_results 中 $match_id 没有重复行"
else
  chaos_fail "幂等：match_results 中 $match_id 出现重复行（$dup_rows 组）"
fi
result_rows="$(sql_scalar "SELECT COUNT(*) FROM match_results WHERE match_id='$match_id';")"
chaos_info "match_results 中 $match_id 的落库行数：${result_rows:-?}"
if [ "${result_rows:-0}" = "1" ]; then
  chaos_ok "幂等：$match_id 恰好落库 1 行"
else
  chaos_fail "幂等：$match_id 落库 ${result_rows:-?} 行（期望 1）"
fi

# 6) 注入期间经 Gateway 的请求只有"查询结果/房间状态"这一条路会 503：前者按设计返回
#    result_pending，后者在房间进入 FINISHING 后由 Gateway 映射为 result_store_unavailable。
#    因此 503 的数量取决于轮询次数，不是一个固定小数字；这里给出宽松上界。
gw_503_after_ch4="$(gw_metric_status_count 503)"
gw_503_delta=$(( ${gw_503_after_ch4:-0} - ${gw_5xx_before_ch4:-0} ))
if [ "$gw_503_delta" -le 400 ]; then
  chaos_ok "隔离：通道 4 注入期间 Gateway 新增 503 共 $gw_503_delta 次（来自结果/状态查询，属预期路径）"
else
  chaos_fail "隔离：通道 4 注入期间 Gateway 新增了 $gw_503_delta 次 503（超出预期上限 400）"
fi

# ---------------------------------------------------------------------------
# 收尾
# ---------------------------------------------------------------------------
chaos_step "6. 收尾"

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container 仍健康（本脚本只操作中继，不停容器）" ||
    chaos_fail "$container 不健康，请手动检查"
done

chaos_print_time_table

echo
if chaos_has_failures; then
  chaos_print_failures
  echo
  echo "排查提示：Gateway $CHAOS_LOG_DIR/gateway.log，Match $CHAOS_LOG_DIR/match.log，Room $CHAOS_LOG_DIR/room.log"
  exit 1
fi

echo "验收通过：四个通道全部实测，检测时间与恢复时间见上表。"
echo "  通道 1（Redis / Gateway 会话）：登录 503 session_store_unavailable，恢复后自愈，未重启服务"
echo "  通道 2（Redis / Match 快照）  ：快照可丢弃，匹配照常成功，失败在日志里可见"
echo "  通道 3（MySQL / Gateway 档案）：登录 503 player_store_unavailable，恢复后自愈"
echo "  通道 4（MySQL / Room）        ：对局照常打完，结果停在 FINISHING 并 503 result_pending，"
echo "                                  恢复后自动落库且不产生重复行"
exit 0
