#!/usr/bin/env bash
#
# chaos/verify-soak.sh - TASK-027：长稳运行：内存与 FD 稳定性
#
# Phase 4 退出标准的最后一条是"持续连接、断连、重连，观察内存与 FD 是否稳定"。
# 在此之前所有验收都是**短时**的（verify-all 约 4 分钟、容量压测每档 60 秒），
# 内存增长趋势与 fd 泄漏从来没有被观察过——这类问题只在长时间运行后暴露。
#
# 本脚本做四件事：
#   1. 用 `chaos/soak_churn.py` 把 N 个玩家放进房间并各持一条 SSE，
#      **周期性断开其中一部分再重连**（只连着不动测不出订阅泄漏）；
#   2. 每 10 秒记录三个服务的 RSS / 线程数 / fd 数，以及 Gateway 的
#      SSE 连接数与房间数；
#   3. 压载结束后进入**静默期**继续采样：所有客户端都走了，SSE 连接数必须回落；
#   4. 出首尾对比 + 趋势，并按任务单列的失败场景判定（不预设 SLO 阈值——
#      阈值与 SLO 的调整另立任务，这一轮只要可复现的曲线）。
#
# 时长：**默认 30 分钟，上限 1 小时**（2026-10-04 项目所有者裁决）。
# 超过上限**报错退出**，不静默截断——静默截断会让"跑了 30 分钟"与"跑了 3 分钟"
# 看起来一样。
#
# 用法：
#   bash chaos/verify-soak.sh                                  # 30 分钟 / 50 玩家
#   bash chaos/verify-soak.sh --duration 1800 --players 50     # 显式（= 验收命令）
#   bash chaos/verify-soak.sh --duration 300 --players 20      # 冒烟用
#
# 前置条件：Docker 的 rgbt-redis / rgbt-mysql 健康；8080/8082/8083 空闲。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

# shellcheck source=chaos/lib.sh
source chaos/lib.sh

preset="brpc-debug"
gateway_port=8080
match_port=8082
room_port=8083

duration=1800
max_duration=3600
players=50
churn_every=60
churn_fraction=0.2
sample_interval=10
settle_seconds=60

keep=0
while [ $# -gt 0 ]; do
  case "$1" in
    --duration) duration="${2:-}"; shift 2 ;;
    --players) players="${2:-}"; shift 2 ;;
    --churn-every) churn_every="${2:-}"; shift 2 ;;
    --churn-fraction) churn_fraction="${2:-}"; shift 2 ;;
    --interval) sample_interval="${2:-}"; shift 2 ;;
    --settle) settle_seconds="${2:-}"; shift 2 ;;
    --keep) keep=1; shift ;;
    -h | --help) sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

# 时长与规模校验：越界要**明确报错**，不静默截断。
case "$duration" in
  '' | *[!0-9]*) echo "x  --duration 必须是非负整数秒：[$duration]" >&2; exit 2 ;;
esac
if [ "$duration" -le 0 ]; then
  echo "x  --duration 必须大于 0：[$duration]" >&2
  exit 2
fi
if [ "$duration" -gt "$max_duration" ]; then
  echo "x  --duration=$duration 超过上限 $max_duration 秒（1 小时）。" >&2
  echo "  本机 11 GiB，同时跑三个服务与监控栈；更长的窗口请另立任务并说明资源来源。" >&2
  exit 2
fi
case "$players" in
  '' | *[!0-9]*) echo "x  --players 必须是非负整数：[$players]" >&2; exit 2 ;;
esac
if [ "$players" -lt 2 ] || [ $(( players % 2 )) -ne 0 ]; then
  echo "x  --players 必须是 >= 2 的偶数（两人一局，配对需要偶数）：[$players]" >&2
  exit 2
fi

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
soak_pid=""

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
  kill_pid "$soak_pid"; soak_pid=""
  kill_pid "$room_pid"
  kill_pid "$match_pid"
  kill_pid "$gateway_pid"
  gateway_pid=""; match_pid=""; room_pid=""
}

cleanup() {
  local rc=$?
  if [ "$keep" -eq 1 ]; then
    echo "（--keep：保留服务进程与采样，便于排查）"
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

start_services() {
  "build/$preset/bin/rgbt_room" -port "$room_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    >"$CHAOS_LOG_DIR/room.log" 2>&1 &
  room_pid=$!
  wait_http "$room_port" 30 || { chaos_fail "Room 未就绪"; return 1; }

  # `-match_result_ttl_seconds 3`（而不是默认/其它脚本用的 60）：对局打完后玩家要
  # **重新配对**才能继续持有连接，而 Match 会优先返回"最近一次配对结果"，在 TTL 内
  # 那个房间已经结束、玩家只会被判为 stale 而拿不到新房间。TTL 太长时实测 SSE
  # 连接数会从 50 掉到 20 左右并卡在那里（第一轮 30 分钟跑到 171 秒就发现了），
  # 那就不再是"固定负载"。TTL 是产品配置、不是本任务要测的东西，压载里调小它
  # 只是为了让负载真的稳定。
  "build/$preset/bin/rgbt_match" -match_port "$match_port" \
    -match_timeout_seconds 30 -match_result_ttl_seconds 3 -env_prefix dev \
    -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" -redis_timeout_ms 500 \
    -room_host 127.0.0.1 -room_port "$room_port" \
    >"$CHAOS_LOG_DIR/match.log" 2>&1 &
  match_pid=$!
  wait_http "$match_port" 30 || { chaos_fail "Match 未就绪"; return 1; }

  # 默认心跳（15 秒）**故意不改**：静默期"连接数多久回落"本来就是被测事实之一。
  "build/$preset/bin/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
    -redis_host 127.0.0.1 -redis_port "${REDIS_PORT:-6379}" -redis_timeout_ms 500 \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" -mysql_timeout_seconds 3 \
    -match_host 127.0.0.1 -match_port "$match_port" \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -match_timeout_ms 500 -room_timeout_ms 500 \
    -stream_poll_interval_ms 100 \
    -enable_bench_accounts >"$CHAOS_LOG_DIR/gateway.log" 2>&1 &
  gateway_pid=$!
  wait_http "$gateway_port" 30 || { chaos_fail "Gateway 未就绪"; return 1; }
  return 0
}

# ---------------------------------------------------------------------------
# 采样
# ---------------------------------------------------------------------------
fd_count() { [ -d "/proc/$1/fd" ] && ls "/proc/$1/fd" 2>/dev/null | wc -l | tr -d ' ' || echo "-"; }

# TASK-029：压载端（探针进程）自己持有的 fd 数。
#
# 为什么要采它：区分实验的核心就是"服务端订阅数增长时，客户端 socket 数有没有同步
# 增长"。只服务端涨 = Gateway 没回收订阅；两边同涨 = 压载端泄漏了 socket
# （那样服务端保留订阅是**正确**的）。没有这一列，两种可能无法区分。
# 探针自报的"当前连着几条流"（读它日志里最后一条 `connected=N`）。
#
# 为什么要这一列：`sse` 是**服务端**认为还活着的订阅数，探针的 connected 是
# **客户端**认为还连着的流数。两者本该接近；长期对不上就说明有一侧在空转
# （TASK-029 实测到：客户端 50 条、服务端 0 条——服务端关闭了订阅但客户端收不到
# FIN，压载已经没在压任何东西，而旧判定照样报"通过"）。
probe_connected() {
  if [ -f "$soak_log" ]; then
    local value
    value="$(grep -oE '(^| )connected=[0-9]+' "$soak_log" 2>/dev/null | tail -n 1 | cut -d= -f2)"
    [ -n "$value" ] && { echo "$value"; return; }
  fi
  echo "0"
}

client_fd_count() {
  if [ -n "$soak_pid" ] && [ -d "/proc/$soak_pid/fd" ]; then
    fd_count "$soak_pid"
  else
    echo "-"
  fi
}
proc_field() { # <pid> <字段名>
  awk -v key="$2" '$1 == key { print $2 }' "/proc/$1/status" 2>/dev/null
}

# 读一个（可能带标签、可能多条的）指标的**总和**。
metric_sum() { # <端口> <指标名>
  curl -s --max-time 4 "http://127.0.0.1:$1/metrics" 2>/dev/null |
    python3 -c '
import sys
name = sys.argv[1]
total = 0.0
seen = False
for line in sys.stdin:
    if line.startswith(name + " ") or line.startswith(name + "{"):
        parts = line.split()
        if len(parts) >= 2:
            try:
                total += float(parts[-1])
                seen = True
            except ValueError:
                pass
print(int(total) if seen else "MISSING")
' "$2"
}

samples_tsv="$CHAOS_LOG_DIR/soak-samples.tsv"

sample_once() { # <阶段标记>
  local phase="$1"
  local row
  row="$(now_ms)"
  row+=$'\t'"$phase"
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    row+=$'\t'"$(fd_count "$pid")"
    row+=$'\t'"$(proc_field "$pid" Threads:)"
    row+=$'\t'"$(proc_field "$pid" VmRSS:)"
  done
  row+=$'\t'"$(metric_sum "$gateway_port" rgbt_sse_connections)"
  row+=$'\t'"$(metric_sum "$room_port" rgbt_rooms)"
  row+=$'\t'"$(client_fd_count)"
  # TASK-029：订阅生命周期（skipped 累计 / 最老订阅年龄）——定位泄漏用。
  row+=$'\t'"$(metric_sum "$gateway_port" rgbt_sse_subscriptions_skipped_no_write_total)"
  row+=$'\t'"$(metric_sum "$gateway_port" rgbt_sse_oldest_subscription_age_ms)"
  row+=$'\t'"$(probe_connected)"
  printf '%s\n' "$row" >>"$samples_tsv"
  printf '%s\n' "$row"
}

print_samples_header() {
  printf '%-9s %-8s %s\n' "阶段" "时间" "Gateway(fd/线程/RSSkB) | Match | Room | SSE连接 | 房间数 | 客户端fd | 跳过写 | 最老订阅ms | 探针connected"
}

print_sample_row() { # 与 sample_once 的输出字段一一对应
  local ts="$1" phase="$2"
  shift 2
  local elapsed_s=$(( ($(now_ms) - soak_started_ms) / 1000 ))
  printf '%-9s %-8s gw=%s/%s/%s | match=%s/%s/%s | room=%s/%s/%s | sse=%s rooms=%s client_fd=%s skipped=%s oldest_ms=%s connected=%s\n' \
    "$phase" "${elapsed_s}s" "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}" "${11}" "${12}" "${13}" "${14}" "${15}"
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

# ---------------------------------------------------------------------------
# 0. 前置检查
# ---------------------------------------------------------------------------
chaos_step "0. 前置检查"

echo "计划：压载 ${duration} 秒（$(( duration / 60 )) 分钟）/ ${players} 玩家 / 每 ${churn_every} 秒断开 $(( ${players} * 100 * 100 / 100 )) 的 ${churn_fraction} 比例"

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

have="$(provision_bench_accounts $(( players + 8 )))"
[ "${have:-0}" -ge "$players" ] && chaos_ok "合成身份就绪：bench-* ${have} 个" ||
  { chaos_fail "合成身份不足：${have:-0} < $players"; chaos_print_failures || true; exit 1; }

# ---------------------------------------------------------------------------
# 1. 起服务、开始压载与采样
# ---------------------------------------------------------------------------
chaos_step "1. 起服务并开始长稳压载"

clean_state
start_services || { chaos_print_failures || true; exit 1; }
chaos_ok "三个服务已启动（Room $room_pid / Match $match_pid / Gateway $gateway_pid）"

: >"$samples_tsv"
soak_started_ms="$(now_ms)"

soak_json="$CHAOS_LOG_DIR/soak.json"
soak_log="$CHAOS_LOG_DIR/soak-churn.log"
python3 chaos/soak_churn.py --gateway "127.0.0.1:$gateway_port" --players "$players" \
  --duration "$duration" --churn-every "$churn_every" --churn-fraction "$churn_fraction" \
  --out "$soak_json" >"$soak_log" 2>&1 &
soak_pid=$!

# 等压载真正建立（否则前面几分钟的采样是空转，曲线会被"还没连上"污染）。
ready=0
for _ in $(seq 1 240); do
  grep -q "soak ready" "$soak_log" 2>/dev/null && { ready=1; break; }
  kill -0 "$soak_pid" 2>/dev/null || break
  sleep 0.5
done
if [ "$ready" -eq 1 ]; then
  chaos_ok "压载已建立：$(grep -m1 'soak ready' "$soak_log")"
else
  chaos_fail "压载未在 120 秒内建立"
  tail -5 "$soak_log" | sed 's/^/     /'
fi
record_time "本次长稳时长" "观测" "$(( duration * 1000 ))" "压载窗口（秒 -> 毫秒）"

print_samples_header
sample_once "soak" | { read -r ts phase rest; print_sample_row "$ts" "$phase" $rest; }

# 采样循环：每 sample_interval 秒一次，直到压载结束。
while kill -0 "$soak_pid" 2>/dev/null; do
  sleep "$sample_interval"
  kill -0 "$soak_pid" 2>/dev/null || break
  sample_once "soak" | { read -r ts phase rest; print_sample_row "$ts" "$phase" $rest; }
done

# 收压载进程（正常应已自行退出）。
if kill -0 "$soak_pid" 2>/dev/null; then
  kill_pid "$soak_pid"
fi
soak_pid=""
soak_ended_ms="$(now_ms)"
chaos_info "压载结束：$(grep -m1 'soak done' "$soak_log" 2>/dev/null || echo '未看到 soak done')"

# 进程还活着吗？（任务单的失败场景之一：OOM 被杀）
for pair in "Gateway:$gateway_pid" "Match:$match_pid" "Room:$room_pid"; do
  name="${pair%%:*}"; pid="${pair#*:}"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    chaos_ok "$name 在压载结束后仍然存活（未被 OOM 杀掉）"
  else
    chaos_fail "$name 在压载期间退出（OOM 或崩溃）——采样保留在 $samples_tsv"
  fi
done

# ---------------------------------------------------------------------------
# 2. 静默期：所有客户端都走了，连接数必须回落
# ---------------------------------------------------------------------------
chaos_step "2. 静默期：客户端全部离开后，SSE 连接数必须回落"

settle_started_ms="$(now_ms)"
sse_zero_ms=""
while [ $(( ($(now_ms) - settle_started_ms) / 1000 )) -lt "$settle_seconds" ]; do
  sleep "$sample_interval"
  sample_once "settle" | { read -r ts phase rest; print_sample_row "$ts" "$phase" $rest; }
  current_sse="$(metric_sum "$gateway_port" rgbt_sse_connections)"
  if [ "$current_sse" = "0" ] && [ -z "$sse_zero_ms" ]; then
    sse_zero_ms=$(( $(now_ms) - soak_ended_ms ))
    chaos_ok "SSE 连接数回落到 0（客户端离开后 ${sse_zero_ms} ms）"
  fi
done

# ---------------------------------------------------------------------------
# 3. 分析：首尾对比 + 趋势
# ---------------------------------------------------------------------------
chaos_step "3. 分析与判定"

analysis_out="$CHAOS_LOG_DIR/soak-analysis.txt"
python3 - "$samples_tsv" "$players" "$churn_fraction" "$duration" >"$analysis_out" 2>&1 <<'PY'
import sys

path, players_s, fraction_s, duration_s = sys.argv[1:5]
players = int(players_s)
fraction = float(fraction_s)
duration = int(duration_s)

rows = []
with open(path, encoding="utf-8") as handle:
    for line in handle:
        parts = line.rstrip("\n").split("\t")
        # 一行 14 列：ts + phase + 三个服务各 (fd, 线程, RSS) + SSE 连接数 + 房间数
        #            + 客户端(压载端) fd 数（TASK-029 加的区分实验列）
        #            + 探针自报 connected + 服务端 skipped/oldest（TASK-029）
        #            实际上共 16 列：再加最老订阅年龄与探针 connected
        if len(parts) < 14:
            continue
        rows.append(parts)

if not rows:
    print("VERDICT|fail|一条采样都没有（压载没有真正跑起来）")
    raise SystemExit(0)

def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None

# 字段：ts phase gw_fd gw_thr gw_rss m_fd m_thr m_rss r_fd r_thr r_rss sse rooms
services = [("Gateway", 2), ("Match", 5), ("Room", 8)]

print("=== 采样规模 ===")
print("样本数=%d，压载窗口=%d 秒，玩家=%d，每周期断开比例=%.2f" % (len(rows), duration, players, fraction))
print()

def trend(label, idx):
    """打印某个指标的时间序列摘要，并返回是否"后半段全是新高（单调无回落）"。"""
    values = [num(r[idx]) for r in rows]
    values = [v for v in values if v is not None]
    if len(values) < 4:
        print("%-28s 采样不足" % label)
        return False, 0.0
    first, last, lo, hi = values[0], values[-1], min(values), max(values)
    half = len(values) // 2
    running = values[0]
    new_max_in_second_half = 0
    for v in values[half:]:
        if v > running:
            running = v
            new_max_in_second_half += 1
    second_half_len = len(values) - half
    monotonic = new_max_in_second_half == second_half_len
    delta = last - first
    hours = max(duration, 1) / 3600.0
    rate = delta / hours
    print("%-28s 首=%s 末=%s 最小=%s 最大=%s 首尾差=%+.0f 折合每小时=%+.1f 后半段新高=%d/%d %s"
          % (label, first, last, lo, hi, delta, rate, new_max_in_second_half, second_half_len,
             "<= 单调无回落" if monotonic else ""))
    return monotonic, rate

print("=== 首尾对比与趋势 ===")
fd_monotonic = {}
for name, base in services:
    mono, rate = trend("%s fd" % name, base)
    fd_monotonic[name] = (mono, rate)
for name, base in services:
    trend("%s 线程数" % name, base + 1)
for name, base in services:
    trend("%s RSS(kB)" % name, base + 2)
trend("Gateway SSE 连接数", 11)
trend("Gateway 房间数", 12)
print()

# --- 判定 ---
sse_values = [num(r[11]) for r in rows]
sse_values = [v for v in sse_values if v is not None]
sse_max = max(sse_values) if sse_values else 0
# 上界取自探针自己的参数（同时连着 players，瞬时最多多出"正在被替换"的一批），
# 不是拍脑袋的 SLO：只要不随周期累积，这个上界就成立。
sse_bound = players * (1 + fraction) + 4
if sse_max <= sse_bound:
    print("VERDICT|ok|SSE 连接数峰值 %d 未超过上界 %d（%d 玩家 + 每周期替换 %.0f%%）"
          % (sse_max, sse_bound, players, fraction * 100))
else:
    print("VERDICT|fail|SSE 连接数峰值 %d 超过上界 %d：疑似订阅随断连周期累积（未清理）"
          % (sse_max, sse_bound))

# "断连后回落"：静默期最后一条采样必须为 0。
settle_sse = [num(r[11]) for r in rows if r[1] == "settle"]
settle_sse = [v for v in settle_sse if v is not None]
if settle_sse and settle_sse[-1] == 0:
    print("VERDICT|ok|客户端全部离开后 SSE 连接数回落到 0（订阅被清理）")
elif settle_sse:
    print("VERDICT|fail|静默期结束时 SSE 连接数仍为 %d（客户端已全部离开）：订阅没有被清理"
          % settle_sse[-1])
else:
    print("VERDICT|fail|静默期没有采到样本，无法判定连接是否回落")

# TASK-029：压载有效性。客户端说连着，服务端就必须也有订阅。
#
# 这一条是补上的漏洞：修复订阅泄漏之后暴露出第二个缺陷——对局 60 秒结束时服务端
# 关闭订阅，但客户端收不到 FIN，于是 50 个客户端一直"自以为连着"（探针 connected
# 始终 50、客户端 fd 不变），服务端订阅数却是 0。此时压载**已经空转**，
# 而"没有累积 + 静默期回落到 0"这两条判定反而都会通过。
probe_vals = [num(r[16]) for r in rows if len(r) > 16]
probe_vals = [v for v in probe_vals if v is not None]
sse_by_ts = {}
for r in rows:
    if len(r) > 11:
        v = num(r[11])
        if v is not None:
            sse_by_ts[r[0]] = v
if probe_vals:
    soak_pairs = [(r, num(r[16])) for r in rows if len(r) > 16 and r[1] == "soak"]
    soak_pairs = [(r, p) for r, p in soak_pairs if p is not None]
    if soak_pairs:
        mismatched = [(r, p) for r, p in soak_pairs
                      if p >= 10 and sse_by_ts.get(r[0], 0) < p / 2]
        total = len(soak_pairs)
        print("压载有效性：压载期采样 %d 个，其中「客户端说连着、服务端订阅不足其一半」的有 %d 个"
              % (total, len(mismatched)))
        if len(mismatched) > total * 0.3:
            print("VERDICT|fail|压载已退化：%d/%d 个采样里客户端仍持有连接但服务端订阅数不足"
                  "其一半——订阅被关闭而客户端收不到 FIN，此时压载没有在压任何东西"
                  % (len(mismatched), total))
        else:
            print("VERDICT|ok|压载有效：客户端连接数与服务端订阅数全程基本一致")
    else:
        print("INFO|压载期没有可用样本，压载有效性未判定")
else:
    print("INFO|没有采到探针 connected，压载有效性未判定")

# TASK-029：区分实验的判定。服务端订阅数增长到底是谁的问题。
client_fd_values = [num(r[13]) for r in rows if len(r) > 13 and r[13] != "-"]
client_fd_values = [v for v in client_fd_values if v is not None]
if client_fd_values and sse_values:
    half = max(1, len(sse_values) // 2)
    server_growth = sse_values[-1] - sse_values[half - 1] if len(sse_values) >= half else 0
    client_half = max(1, len(client_fd_values) // 2)
    client_growth = (client_fd_values[-1] - client_fd_values[client_half - 1]
                     if len(client_fd_values) >= client_half else 0)
    print("区分实验：后半段服务端订阅数增长 %+.0f，客户端 fd 增长 %+.0f（末值 服务端=%d 客户端=%d）"
          % (server_growth, client_growth, sse_values[-1], client_fd_values[-1]))
    if server_growth > 20 and client_growth < 5:
        print("VERDICT|fail|服务端订阅数增长 %+.0f 而客户端 fd 只增长 %+.0f："
              "**订阅没有被 Gateway 回收**（缺陷在产品侧）" % (server_growth, client_growth))
    elif server_growth > 20 and client_growth >= 5:
        print("VERDICT|fail|客户端 fd 也增长了 %+.0f：**压载端泄漏了 socket**"
              "（缺陷在夹具，服务端保留订阅是正确的）" % client_growth)
    else:
        print("VERDICT|ok|后半段服务端订阅数没有明显增长（%+.0f），区分实验本轮不适用"
              % server_growth)
else:
    print("INFO|本轮没有采到客户端 fd（--settle 期间探针已退出属正常），区分实验无法判定")

for name, (mono, rate) in fd_monotonic.items():
    if mono:
        print("VERDICT|fail|%s 的 fd 数后半段每个采样都是新高（单调增长不回落），"
              "折合 %.1f 个/小时：疑似 fd 泄漏" % (name, rate))
    else:
        print("VERDICT|ok|%s 的 fd 数出现回落（不是单调增长）" % name)

# RSS 只给结论、不判失败（任务单明确"不预设阈值"）：内存增长既可能是泄漏，
# 也可能是分配器/缓存的正常波动，靠 30 分钟的单轮数据分不出来。
# 这里把"是否单调不回落"与折合速率写出来，供下一轮或更长窗口对比。
for name, base in services:
    values = [num(r[base + 2]) for r in rows]
    values = [v for v in values if v is not None]
    if len(values) < 4:
        continue
    half = len(values) // 2
    running = values[0]
    new_max = 0
    for v in values[half:]:
        if v > running:
            running = v
            new_max += 1
    delta = values[-1] - values[0]
    hours = max(duration, 1) / 3600.0
    if new_max == len(values) - half and new_max > 0:
        print("INFO|%s 的 RSS 在窗口内单调不回落：%d -> %d kB（首尾差 %+.0f，"
              "折合 %+.0f kB/小时）。进程存活、fd 未泄漏，因此本轮**不判定为泄漏**；"
              "要区分「启动期增长」与「持续泄漏」需要更长窗口或第二轮对比。"
              % (name, values[0], values[-1], delta, delta / hours))
    else:
        print("INFO|%s 的 RSS 出现过回落：%d -> %d kB（首尾差 %+.0f）"
              % (name, values[0], values[-1], delta))
PY

cat "$analysis_out"

# 把分析里的 VERDICT 翻成脚本的通过与失败（单一真相：判定只在分析里写一次）。
while IFS='|' read -r tag verdict message; do
  [ "$tag" = "VERDICT" ] || continue
  if [ "$verdict" = "ok" ]; then
    chaos_ok "$message"
  else
    chaos_fail "$message"
  fi
done <"$analysis_out"

# 压载本身是否真的发生了断连/重连（否则长稳退化成"一直连着"）。
# 判定口径只要求"真的断过、也真的重连回来了"，不要求跑满多少个周期——
# 周期数受 --duration/--churn-every 影响，冒烟跑法本来就短。
cycles="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('cycles',0))" "$soak_json" 2>/dev/null || echo 0)"
closed_total="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('closed_total',0))" "$soak_json" 2>/dev/null || echo 0)"
recovered_total="$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print(d.get('reconnected',0)+d.get('requeued',0))" "$soak_json" 2>/dev/null || echo 0)"
expected_cycles=$(( duration / ${churn_every:-60} ))
if [ "${cycles:-0}" -ge 1 ] && [ "${closed_total:-0}" -gt 0 ]; then
  chaos_ok "压载期间真的发生了断连/重连：${cycles} 个周期（期望约 ${expected_cycles}），累计主动断开 ${closed_total} 条 SSE"
  record_time "断连/重连周期数" "观测" "$cycles" "累计断开 ${closed_total} 条（期望约 ${expected_cycles} 个周期）"
else
  chaos_fail "压载期间几乎没有发生断连/重连（周期 ${cycles}、断开 ${closed_total}）——长稳会退化成\"一直连着\""
fi
# 断开之后必须真的重连回来（只断不连会让连接数单调下降，那也不是稳态）。
if [ "${recovered_total:-0}" -ge $(( ${closed_total:-0} - 1 )) ] && [ "${closed_total:-0}" -gt 0 ]; then
  chaos_ok "断开之后都重连回来了：重连+换局 ${recovered_total} 次 >= 断开 ${closed_total} 条"
else
  chaos_fail "断开之后没有全部重连回来：重连+换局 ${recovered_total:-0} 次 < 断开 ${closed_total:-0} 条"
fi

# ---------------------------------------------------------------------------
# 4. 收尾
# ---------------------------------------------------------------------------
chaos_step "4. 收尾"

for container in rgbt-redis rgbt-mysql; do
  dependency_up "$container" && chaos_ok "$container 仍健康" || chaos_fail "$container 不健康"
done

echo
echo "采样明细：$samples_tsv（每 ${sample_interval} 秒一行，含 soak / settle 两个阶段）"
echo "压载摘要：$soak_json"
echo "分析全文：$analysis_out"

if [ "$keep" -eq 0 ]; then
  cleanup_bench_state
  chaos_ok "已清理合成身份与压测状态（只删 bench-* 与 rooms/match_results）"
fi

echo
if chaos_has_failures; then
  chaos_print_failures
  echo
  echo "日志：$CHAOS_LOG_DIR（room.log / match.log / gateway.log / soak-churn.log）"
  exit 1
fi

echo "长稳验收通过：${duration} 秒 / ${players} 玩家，内存与 fd 的时间序列见上面的分析。"
echo "  判定口径：fd 是否单调不回落到新高、SSE 峰值是否随断连周期累积、"
echo "            客户端离开后连接数是否回落到 0、进程是否被 OOM 杀掉。"
exit 0