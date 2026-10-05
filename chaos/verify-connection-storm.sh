#!/usr/bin/env bash
#
# ⚠ 本脚本**不在** `scripts/verify-all.sh` 里（故障注入 / 长稳 / 重连这一类会占标准
#   端口、停依赖或跑很久，不适合放进快速门禁）。**改动 `src/` 下的产品代码之后，
#   请跑 `bash scripts/verify-chaos.sh`** —— 那是这一类脚本的统一入口。
#   为什么必须写这一句：TASK-026 曾漏改本类里的一个脚本（它不在任何统一入口里，
#   于是回归躺了整整一个任务周期才被 TASK-029 的补跑发现），详见
#   `docs/devlog.md` 的「TASK-029 之后的重跑结果」。
#
# chaos/verify-connection-storm.sh - TASK-025：连接风暴与限流边界
#
# 本脚本要回答 Phase 4 退出标准里的那一条：**瞬时大量连接建立时的限流与拒绝行为，
# 已有连接是否不受影响**。当前系统没有任何针对连接数的显式限流，所以本任务很可能
# 得出"没有限流，边界是 fd 上限"这个结论——**那本身就是必须记录的实测事实**，
# 不是缺陷（见任务单的「非范围」）。
#
# 三个方向都要有证据，缺一条就不算测过：
#   1. **新连接**：风暴期间新连接的失败率与**失败方式**（超时 / 连接被重置 /
#      HTTP 503 / 进程直接死掉）。来源：`bench/loadgen.py` 的结果 JSON
#      （`counters` / `exceptions` / `http_status`）。
#   2. **已有连接**：风暴前建立的基线 SSE 连接，在风暴期间是否**仍然活着并且仍然
#      收到推送**。来源：`chaos/storm_baseline.py` 的按秒输出与结果 JSON。
#      这是"已有连接不受影响"的判据，也是本任务最重要的断言。
#   3. **资源**：Gateway 的 fd 数、线程数、RSS 在风暴前后的变化（`/proc/<pid>` +
#      `/metrics` 的 `rgbt_sse_connections`）。
#
# 关于 fd 上限（任务单点名要测的那一条）
# -------------------------------------
# 任务单假设"不提高 ulimit 时才是默认部署下的真实边界"，但**本机实测默认 soft 是
# 10240**，已经高于 `scripts/bench.sh` 设的 8192——也就是说，在这台机器上"不提高
# ulimit"并不会更低。因此本脚本不去猜，而是**显式构造两个上限**做对照：
#
#   low  ：服务端 soft fd = 256  -> 预期先失败，用来定位"从哪里开始失败"
#   high ：服务端 soft fd = 8192 -> 预期全部成功，用来证明失败确实来自这个上限
#
# 两个上限都写在下面的 `run_config` 调用里，报告里会标明是**构造的**边界，
# 不是本机的默认值。服务端与客户端的上限分开设：客户端（loadgen / 探针）自己
# 提到 8192，否则"服务端没 fd 了"与"客户端没 fd 了"会混在一起，测不出结论。
#
# 基线身份与风暴身份必须错开：基线用 `bench-00990` 起，风暴用 `bench-00000` 起。
# 复用同一批 `player_id` 会让"同一玩家重复入队"变成夹具假象（TASK-022 踩过）。
#
# 用法：
#   bash chaos/verify-connection-storm.sh
#   bash chaos/verify-connection-storm.sh --keep       # 保留服务与日志
#   bash chaos/verify-connection-storm.sh --players 200 --duration 15
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

storm_players=400
storm_duration=20
storm_drain=20
baseline_connections=10
baseline_index_start=990
baseline_duration=45
bench_accounts=1000
client_fd_limit=8192

keep=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keep) keep=1; shift ;;
    --players) storm_players="${2:-400}"; shift 2 ;;
    --duration) storm_duration="${2:-20}"; shift 2 ;;
    -h | --help) sed -n '3,50p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
baseline_pid=""
sampler_pid=""

# ---------------------------------------------------------------------------
# 启停（清理必须显式恢复退出码：验收脚本的退出码就是门禁本身）
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
  kill_pid "$sampler_pid"; sampler_pid=""
  kill_pid "$baseline_pid"; baseline_pid=""
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

# start_services <服务端 soft fd 上限>
#
# 上限只对**服务进程**生效：在子 shell 里 `ulimit -n` 之后再 exec，
# 父脚本（以及它启动的 loadgen / 探针）不受影响，两边才能分开归因。
start_services() {
  local soft_limit="$1"
  # 上一轮的 pid 文件必须先清掉：否则下面的等待循环会立刻通过，
  # 拿到的是已经不存在的旧 pid。
  rm -f "$CHAOS_LOG_DIR"/*.pid
  (
    ulimit -n "$soft_limit" 2>/dev/null || true
    "build/$preset/bin/rgbt_room" -port "$room_port" -env_prefix dev \
      -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
      -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
      -mysql_database "$MYSQL_DATABASE" >"$CHAOS_LOG_DIR/room.log" 2>&1 &
    echo $! >"$CHAOS_LOG_DIR/room.pid"
    "build/$preset/bin/rgbt_match" -match_port "$match_port" \
      -match_timeout_seconds 30 -match_result_ttl_seconds 60 -env_prefix dev \
      -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" \
      -room_host 127.0.0.1 -room_port "$room_port" \
      >"$CHAOS_LOG_DIR/match.log" 2>&1 &
    echo $! >"$CHAOS_LOG_DIR/match.pid"
    "build/$preset/bin/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
      -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" -redis_timeout_ms 500 \
      -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
      -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
      -mysql_database "$MYSQL_DATABASE" \
      -match_host 127.0.0.1 -match_port "$match_port" \
      -room_host 127.0.0.1 -room_port "$room_port" \
      -match_timeout_ms 500 -room_timeout_ms 500 \
      -stream_poll_interval_ms 100 -stream_heartbeat_interval_ms 15000 \
      -enable_bench_accounts >"$CHAOS_LOG_DIR/gateway.log" 2>&1 &
    echo $! >"$CHAOS_LOG_DIR/gateway.pid"
    wait
  ) &
  services_shell_pid=$!
  # 等三个子进程把 pid 文件写出来（它们由子 shell 启动，父脚本拿不到 $!）。
  local i
  for i in $(seq 1 60); do
    [ -s "$CHAOS_LOG_DIR/gateway.pid" ] && [ -s "$CHAOS_LOG_DIR/match.pid" ] &&
      [ -s "$CHAOS_LOG_DIR/room.pid" ] && break
    sleep 0.1
  done
  room_pid="$(cat "$CHAOS_LOG_DIR/room.pid" 2>/dev/null)"
  match_pid="$(cat "$CHAOS_LOG_DIR/match.pid" 2>/dev/null)"
  gateway_pid="$(cat "$CHAOS_LOG_DIR/gateway.pid" 2>/dev/null)"

  wait_http "$room_port" 30 || { chaos_fail "Room 未就绪"; tail -5 "$CHAOS_LOG_DIR/room.log" | sed 's/^/     /'; return 1; }
  wait_http "$match_port" 30 || { chaos_fail "Match 未就绪"; tail -5 "$CHAOS_LOG_DIR/match.log" | sed 's/^/     /'; return 1; }
  wait_http "$gateway_port" 30 || { chaos_fail "Gateway 未就绪"; tail -5 "$CHAOS_LOG_DIR/gateway.log" | sed 's/^/     /'; return 1; }
  return 0
}

# ---------------------------------------------------------------------------
# 观测辅助
# ---------------------------------------------------------------------------
fd_count() { # <pid>
  ls "/proc/$1/fd" 2>/dev/null | wc -l | tr -d ' '
}

proc_field() { # <pid> <字段名>
  awk -v key="$2" '$1 == key { print $2 }' "/proc/$1/status" 2>/dev/null
}

metric_value() { # <端口> <指标名>
  curl -s --max-time 4 "http://127.0.0.1:$1/metrics" 2>/dev/null |
    python3 -c '
import sys
name = sys.argv[1]
value = "MISSING"
for line in sys.stdin:
    if line.startswith(name + " ") or line.startswith(name + "{"):
        parts = line.split()
        if len(parts) >= 2:
            value = parts[-1]
print(value)
' "$2"
}

snapshot_resources() { # <标签>
  local tag="$1"
  local fds threads rss sse
  if [ -n "$gateway_pid" ] && [ -r "/proc/$gateway_pid/status" ]; then
    fds="$(fd_count "$gateway_pid")"
    threads="$(proc_field "$gateway_pid" Threads:)"
    rss="$(proc_field "$gateway_pid" VmRSS:)"
  else
    fds="-"; threads="-"; rss="-"
  fi
  sse="$(metric_value "$gateway_port" rgbt_sse_connections)"
  printf '%s\t%s\t%s\t%s\t%s\n' "$tag" "${fds:-?}" "${threads:-?}" "${rss:-?}" "${sse:-?}" \
    >>"$CHAOS_LOG_DIR/resources.tsv"
  chaos_info "资源 $tag：Gateway fd=$fds 线程=$threads RSS=${rss}kB SSE连接=$sse"
}

# 风暴期间每 0.5 s 采一次，用来给出"峰值 fd"——"从哪里开始失败"需要一个数字。
start_sampler() {
  : >"$CHAOS_LOG_DIR/sampler.tsv"
  (
    local deadline=$(( $(now_ms) + (storm_duration + storm_drain + 10) * 1000 ))
    while [ "$(now_ms)" -lt "$deadline" ]; do
      if [ -n "$gateway_pid" ] && [ -r "/proc/$gateway_pid/status" ]; then
        printf '%s\t%s\t%s\t%s\n' "$(now_ms)" "$(fd_count "$gateway_pid")" \
          "$(proc_field "$gateway_pid" Threads:)" "$(proc_field "$gateway_pid" VmRSS:)" \
          >>"$CHAOS_LOG_DIR/sampler.tsv"
      fi
      sleep 0.5
    done
  ) &
  sampler_pid=$!
}

max_sampled() { # <列号>
  awk -v col="$1" 'NR == 1 || $col + 0 > max { max = $col + 0 } END { print max + 0 }' \
    "$CHAOS_LOG_DIR/sampler.tsv" 2>/dev/null
}

# ---------------------------------------------------------------------------
# 数据准备
# ---------------------------------------------------------------------------
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
  docker exec -i rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "SELECT COUNT(*) FROM players WHERE account LIKE 'bench-%';" 2>/dev/null | tr -d '\r'
}

clean_state() {
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "DELETE FROM rooms; DELETE FROM match_results;" >/dev/null 2>&1
}

cleanup_bench_state() {
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "DELETE FROM players WHERE account LIKE 'bench-%'; DELETE FROM rooms; DELETE FROM match_results;" \
    >/dev/null 2>&1
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
}

# 把 loadgen 的 JSON 打印成一行行可读的"失败方式"。
summarize_storm() { # <json 路径>
  python3 - "$1" <<'PY'
import json, sys

try:
    payload = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception as exc:
    print("storm_json_unreadable=%s" % exc)
    raise SystemExit(0)

counters = payload.get("counters", {})
print("storm_players=%s wall=%.1fs" % (payload.get("config", {}).get("players"),
                                       payload.get("wall_seconds", 0.0)))
for key in ("login_ok", "login_failed", "paired", "join_failed", "stream_opened",
            "stream_failed", "attacks_ok", "attacks_failed", "games_finished",
            "games_aborted", "recycled"):
    if key in counters:
        print("  %s=%s" % (key, counters[key]))

status = payload.get("http_status", {})
bad = {k: v for k, v in status.items() if not k.endswith(":200") and not k.endswith(":0")}
print("  非 200 响应：%s" % (json.dumps(bad, ensure_ascii=False) if bad else "无"))
zero = {k: v for k, v in status.items() if k.endswith(":0")}
if zero:
    print("  无响应（连接层失败，status=0）：%s" % json.dumps(zero, ensure_ascii=False))
exc = payload.get("exceptions", {})
print("  异常：%s" % (json.dumps(exc, ensure_ascii=False) if exc else "无"))
PY
}

# 基线在给定风暴窗口内的事件数与存活性。
summarize_baseline() { # <json 路径> <风暴开始 epoch 秒> <风暴结束 epoch 秒>
  python3 - "$1" "$2" "$3" <<'PY'
import json, sys

path, start, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
try:
    payload = json.load(open(path, encoding="utf-8"))
except Exception as exc:
    print("baseline_json_unreadable=%s" % exc)
    raise SystemExit(0)

by_second = {int(k): v for k, v in payload.get("events_by_second", {}).items()}
window = [v for k, v in by_second.items() if start <= k <= end]
quiet = [k for k, v in sorted(by_second.items()) if start <= k <= end and v == 0]
print("baseline_opened=%s still_connected=%s events_total=%s"
      % (payload.get("stream_opened"), payload.get("still_connected"),
         payload.get("events_total")))
print("baseline_storm_window_seconds=%d events_in_window=%d quiet_seconds=%d"
      % (len(window), sum(window), len(quiet)))
print("baseline_closed_during_window=%s"
      % json.dumps(payload.get("closed_during_window", {}), ensure_ascii=False))
print("baseline_end_reasons=%s" % json.dumps(payload.get("end_reasons", {}), ensure_ascii=False))
PY
}

# ---------------------------------------------------------------------------
# 0. 前置检查
# ---------------------------------------------------------------------------
chaos_step "0. 前置检查"

command -v docker >/dev/null 2>&1 || { echo "x  缺少 docker" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "x  Docker 服务端不可达，请先启动 Docker Desktop" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "x  缺少 python3" >&2; exit 1; }

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container healthy" ||
    chaos_fail "$container 不健康：本脚本需要真实的 Redis 与 MySQL"
done
if chaos_has_failures; then
  chaos_print_failures || true
  echo "请先执行：bash scripts/dev-up.sh"
  exit 1
fi

for port in "$gateway_port" "$match_port" "$room_port"; do
  port_listening "$port" && chaos_fail "端口 $port 已被占用，先执行 bash scripts/dev-down.sh"
done
if chaos_has_failures; then chaos_print_failures || true; exit 1; fi
chaos_ok "3 个服务端口均空闲"

host_soft="$(ulimit -Sn)"
host_hard="$(ulimit -Hn)"
chaos_info "本机 fd 上限：soft=$host_soft hard=$host_hard（本脚本会显式构造服务端上限做对照）"

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

chaos_step "0b. 准备合成身份"
have="$(provision_bench_accounts "$bench_accounts")"
if [ "${have:-0}" -ge "$bench_accounts" ]; then
  chaos_ok "合成身份就绪：bench-* 共 ${have} 个（基线用 $baseline_index_start 起，风暴用 0 起）"
else
  chaos_fail "合成身份不足：bench-* 只有 ${have} 个（需要 $bench_accounts）"
  chaos_print_failures || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 单个配置：基线连接 + 风暴
# ---------------------------------------------------------------------------
run_config() { # <配置名> <服务端 soft fd 上限>
  local name="$1"
  local soft_limit="$2"
  local tag="${name}"
  local storm_json="$CHAOS_LOG_DIR/storm-$tag.json"
  local baseline_json="$CHAOS_LOG_DIR/baseline-$tag.json"
  local baseline_log="$CHAOS_LOG_DIR/baseline-$tag.log"

  chaos_step "配置 $tag：服务端 soft fd = $soft_limit，基线 $baseline_connections 条，风暴 $storm_players 条"

  stop_all
  clean_state
  rm -f "$CHAOS_LOG_DIR/resources.tsv"

  if ! start_services "$soft_limit"; then
    chaos_fail "配置 $tag：服务启动失败"
    return 1
  fi
  chaos_ok "配置 $tag：三个服务已启动（Gateway pid $gateway_pid）"
  snapshot_resources "$tag-before-baseline"

  # 基线探针：客户端自己有足够 fd（上限只压服务端）。
  (
    ulimit -n "$client_fd_limit" 2>/dev/null || true
    exec python3 chaos/storm_baseline.py --gateway "127.0.0.1:$gateway_port" \
      --connections "$baseline_connections" --index-start "$baseline_index_start" \
      --duration "$baseline_duration" --out "$baseline_json"
  ) >"$baseline_log" 2>&1 &
  baseline_pid=$!

  local i ready=0
  for i in $(seq 1 120); do
    grep -q "baseline ready" "$baseline_log" 2>/dev/null && { ready=1; break; }
    kill -0 "$baseline_pid" 2>/dev/null || break
    sleep 0.5
  done
  if [ "$ready" -ne 1 ]; then
    chaos_fail "配置 $tag：基线连接未在 60 秒内建立"
    tail -5 "$baseline_log" | sed 's/^/     /'
    return 1
  fi
  chaos_ok "配置 $tag：$(grep -m1 'baseline ready' "$baseline_log")"
  snapshot_resources "$tag-after-baseline"

  # 风暴：负载源是 TASK-022 交付的 loadgen，客户端自己提 fd 上限。
  start_sampler
  local storm_start storm_end
  storm_start="$(date +%s)"
  chaos_info "配置 $tag：风暴开始（$storm_players 条新连接，loadgen --duration $storm_duration）"
  (
    ulimit -n "$client_fd_limit" 2>/dev/null || true
    exec python3 bench/loadgen.py --players "$storm_players" --bench-accounts \
      --gateway "127.0.0.1:$gateway_port" --duration "$storm_duration" \
      --drain-seconds "$storm_drain" --out "$storm_json" --label "storm-$tag"
  ) >"$CHAOS_LOG_DIR/loadgen-$tag.log" 2>&1
  local storm_rc=$?
  storm_end="$(date +%s)"
  chaos_info "配置 $tag：风暴结束（loadgen 退出码 $storm_rc，墙钟 $(( storm_end - storm_start )) s）"
  snapshot_resources "$tag-after-storm"

  local peak_fd peak_threads peak_rss
  peak_fd="$(max_sampled 2)"
  peak_threads="$(max_sampled 3)"
  peak_rss="$(max_sampled 4)"
  record_time "配置 $tag：风暴窗口" "观测" "$(( (storm_end - storm_start) * 1000 ))" \
    "新连接 $storm_players 条"

  # 风暴把进程打死了？这是任务单明确列的失败场景。
  if kill -0 "$gateway_pid" 2>/dev/null; then
    chaos_ok "配置 $tag：Gateway 在风暴后仍然存活"
  else
    chaos_fail "配置 $tag：Gateway 在风暴中退出（失败场景）"
    tail -20 "$CHAOS_LOG_DIR/gateway.log" | sed 's/^/     gateway: /'
  fi
  if [ -n "$room_pid" ] && kill -0 "$room_pid" 2>/dev/null && [ -n "$match_pid" ] &&
     kill -0 "$match_pid" 2>/dev/null; then
    chaos_ok "配置 $tag：Match / Room 也存活"
  else
    chaos_fail "配置 $tag：Match 或 Room 退出了"
  fi

  # 等基线探针跑完它自己的窗口（风暴在其中，之后还有一段安静期）。
  local waited=0
  while kill -0 "$baseline_pid" 2>/dev/null && [ "$waited" -lt 90 ]; do
    sleep 1
    waited=$((waited + 1))
  done
  kill_pid "$baseline_pid"
  baseline_pid=""
  kill_pid "$sampler_pid"
  sampler_pid=""

  chaos_info "配置 $tag：峰值采样 fd=$peak_fd 线程=$peak_threads RSS=${peak_rss}kB"

  echo
  echo "--- 配置 $tag：风暴的失败方式（loadgen）---"
  summarize_storm "$storm_json" | sed 's/^/   /'
  echo "--- 配置 $tag：基线连接是否受影响 ---"
  summarize_baseline "$baseline_json" "$storm_start" "$storm_end" | sed 's/^/   /'

  # --- 断言 ---
  local opened still window_events quiet
  opened="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('stream_opened',-1))" "$baseline_json" 2>/dev/null)"
  still="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('still_connected',-1))" "$baseline_json" 2>/dev/null)"
  window_events="$(python3 - "$baseline_json" "$storm_start" "$storm_end" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
by_second = {int(k): v for k, v in payload.get("events_by_second", {}).items()}
print(sum(v for k, v in by_second.items() if int(sys.argv[2]) <= k <= int(sys.argv[3])))
PY
)"
  quiet="$(python3 - "$baseline_json" "$storm_start" "$storm_end" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
by_second = {int(k): v for k, v in payload.get("events_by_second", {}).items()}
print(sum(1 for k, v in by_second.items() if int(sys.argv[2]) <= k <= int(sys.argv[3]) and v == 0))
PY
)"

  if [ "${opened:-0}" = "$baseline_connections" ]; then
    chaos_ok "配置 $tag：风暴前基线 $baseline_connections 条 SSE 全部建立"
  else
    chaos_fail "配置 $tag：基线只建立了 ${opened:-?}/$baseline_connections 条"
  fi
  if [ "${still:-0}" = "${opened:-0}" ] && [ "${opened:-0}" != "-1" ]; then
    chaos_ok "配置 $tag：窗口结束时基线 ${still} 条**全部仍然连着**（没有被风暴打断）"
  else
    chaos_fail "配置 $tag：基线连接被中断（结束仍连着 ${still:-?}/${opened:-?}）"
  fi
  # 「已有连接是否受影响」按两个层次分别判定，因为它们的含义不同：
  #   * 连接被**断开**（TCP 层）：任何配置下都是真实缺陷 -> 硬断言（上一段）。
  #   * 连接还在但**收不到推送**：说明 Gateway 自己也没 fd 去向 Room 取状态了。
  #     这在"服务端 fd 被故意压到 256"的 low 配置里是**预期的边界现象**，
  #     属于要记录的实测事实；在按 benchmark 配置（high）里则是缺陷 -> 硬断言。
  #     把两者混成一个断言，会让"故意压小上限"变成脚本失败，掩盖真正的结论。
  if [ "${window_events:-0}" -gt 0 ] && [ "${quiet:-99}" -le 2 ]; then
    chaos_ok "配置 $tag：风暴期间基线仍在收推送（窗口内事件 ${window_events} 次，静默秒数 ${quiet}）"
    record_time "配置 $tag：基线在风暴期间的事件" "观测" "$window_events" \
      "已有连接仍在收到推送（静默 ${quiet} 秒）"
  elif [ "$tag" = "high" ]; then
    chaos_fail "配置 $tag：风暴期间基线没有持续收到推送（事件 ${window_events:-?}，静默 ${quiet:-?} 秒）——已有连接受影响"
  else
    chaos_info "配置 $tag：风暴期间基线收推送不连续（事件 ${window_events:-?}，静默 ${quiet:-?} 秒）"
    chaos_info "配置 $tag：连接没有被断开，但 fd 耗尽后 Gateway 也取不到房间状态，推送会停——这是 low 配置要记录的边界现象"
    record_time "配置 $tag：基线在风暴期间的事件" "观测" "${window_events:-0}" \
      "fd 耗尽后已有连接的推送被打断（静默 ${quiet:-?} 秒）"
  fi

  # 风暴自身的成败：high 配置不允许出现连接层失败；low 配置只记录（那是边界本身）。
  local failures_count
  # 只统计**连接层**失败：登录 / 进房 / 开流失败 + 完全没有响应的请求（status=0）。
  # 刻意**不算** `attacks_failed` 与 `games_aborted`：那是业务层拒绝（例如对局已结束
  # 时的输入），把它们算成"风暴失败"会让一次正常的风暴看起来失败。
  # 首次运行时正是这么误判的（42 次 input 400 被当成连接失败）。
  failures_count="$(python3 - "$storm_json" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
counters = payload.get("counters", {})
total = sum(counters.get(key, 0) for key in ("login_failed", "join_failed", "stream_failed"))
status = payload.get("http_status", {})
total += sum(v for k, v in status.items() if k.endswith(":0"))
print(total)
PY
)"
  local opened_streams
  opened_streams="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('counters',{}).get('stream_opened',0))" "$storm_json" 2>/dev/null)"
  if [ "$tag" = "high" ]; then
    if [ "${failures_count:-1}" = "0" ] && [ "${opened_streams:-0}" -ge "$storm_players" ]; then
      chaos_ok "配置 $tag：$storm_players 条新连接全部成功（流 ${opened_streams} 条），无连接层失败"
    else
      chaos_fail "配置 $tag：提高 fd 上限后仍有连接层失败（失败计数 ${failures_count:-?}，成功流 ${opened_streams:-?}/$storm_players）"
    fi
  else
    if [ "${failures_count:-0}" -gt 0 ]; then
      chaos_ok "配置 $tag：构造的 $soft_limit fd 上限确实触发了连接层失败（${failures_count} 次）——这一组对照成立"
    else
      chaos_fail "配置 $tag：构造的 $soft_limit fd 上限没有触发任何连接失败，本对照不成立（结论不可用）"
    fi
    chaos_info "配置 $tag：新连接失败计数 ${failures_count:-?}，成功建立流 ${opened_streams:-?}/$storm_players（**这是要定位的边界，不是脚本失败**）"
    record_time "配置 $tag：新连接失败计数" "观测" "${failures_count:-0}" \
      "服务端 fd 上限 $soft_limit 下风暴的连接层失败总数"
  fi

  # 错误要从日志里能定位（本机实测：brpc 在 fd 耗尽时会打 Too many open files）。
  local emfile
  emfile="$(grep -c -i -e 'too many open files' -e 'EMFILE' "$CHAOS_LOG_DIR/gateway.log" 2>/dev/null)"
  if [ "${emfile:-0}" -gt 0 ]; then
    chaos_info "配置 $tag：Gateway 日志里出现 ${emfile} 次 'too many open files'（fd 耗尽的直接证据）"
    grep -i -m1 -e 'too many open files' -e 'EMFILE' "$CHAOS_LOG_DIR/gateway.log" | sed 's/^/     /'
  else
    chaos_info "配置 $tag：Gateway 日志里没有 'too many open files'（失败可能来自客户端 fd 或超时）"
  fi

  printf '%s\t%s\t%s\t%s\t%s\n' "$tag-peak-sampled" "${peak_fd:-0}" "${peak_threads:-0}" \
    "${peak_rss:-0}" "$(metric_value "$gateway_port" rgbt_sse_connections)" \
    >>"$CHAOS_LOG_DIR/resources.tsv"

  stop_all
  return 0
}

# ---------------------------------------------------------------------------
# 1/2. 两个对照配置
# ---------------------------------------------------------------------------
run_config "low" 256 || true
run_config "high" "$client_fd_limit" || true

# ---------------------------------------------------------------------------
# 3. 收尾
# ---------------------------------------------------------------------------
chaos_step "3. 收尾"

if [ -s "$CHAOS_LOG_DIR/resources.tsv" ]; then
  echo
  echo "===== Gateway 资源变化（本机实测） ====="
  printf '%-26s %-8s %-8s %-10s %s\n' "采样点" "fd" "线程" "RSS(kB)" "SSE连接"
  while IFS=$'\t' read -r tag fds threads rss sse; do
    printf '%-26s %-8s %-8s %-10s %s\n' "$tag" "$fds" "$threads" "$rss" "$sse"
  done <"$CHAOS_LOG_DIR/resources.tsv"
fi

chaos_print_time_table

if [ "$keep" -eq 0 ]; then
  cleanup_bench_state
  chaos_ok "已清理合成身份与压测状态（只删 bench-* 与 rooms/match_results）"
fi

echo
if chaos_has_failures; then
  chaos_print_failures
  echo
  echo "日志：$CHAOS_LOG_DIR（gateway.log / loadgen-*.log / baseline-*.log）"
  exit 1
fi

echo "验收通过：连接风暴的失败方式与基线连接存活性都有实测证据，见上面的表。"
echo "  low  配置（服务端 soft fd=256）：用于定位「从哪里开始失败」；基线连接未被断开，"
echo "                                 是否仍在收推送见上面的记录"
echo "  high 配置（服务端 soft fd=$client_fd_limit）：同样规模的风暴不产生连接层失败，"
echo "                                 且基线连接全程仍在收推送（已有连接不受影响）"
exit 0