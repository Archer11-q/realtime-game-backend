#!/usr/bin/env bash
#
# bench.sh - TASK-022 容量基线与压测编排
#
# 做什么：按连接档位依次起服务、施加负载、采集指标、落原始数据，最后打印汇总。
#
#   bash scripts/bench.sh                          # 默认 1,10,50,100,500,1000
#   bash scripts/bench.sh --levels 1,10,50         # 指定档位
#   bash scripts/bench.sh --level 100              # 只跑一档（可独立运行，任务单要求）
#   bash scripts/bench.sh --duration 30 --keep     # 每档 30 秒，跑完保留服务
#   bash scripts/bench.sh --no-docker              # 依赖已在跑（省掉 compose up）
#
# 为什么每一档都**重启服务**：
#   同一进程连跑多档时，上一档留下的房间、匹配结果、SSE 订阅、连接池都会影响下一档，
#   档位之间就不可比了——而"可复现"是 Phase 3 的退出标准之一。重启的代价是每档多
#   几秒，换来的是"这一档的数据只属于这一档"。开跑前还会清掉 Redis 的 `dev:*` 与
#   MySQL 的 `rooms` 表快照：Room 启动时会从最近快照恢复未结束的房间（TASK-014），
#   不清的话新一档会拿到一个**双方从未加入过**的旧房间，表现为"对局怎么都打不完"。
#   这个坑在 TASK-021 的 verify-trace.sh 里已经踩过一次，这里提前处理。
#
# 为什么强制用 8080/8082/8083 而不是挑空闲端口：
#   Prometheus 的抓取配置（deploy/compose/prometheus/prometheus.yml）写死了这三个
#   端口。换端口会让 `--prometheus` 的采集全部查不到数据，而报告里"面板口径与端点
#   一致"正是要证明的事。因此端口被占用时**直接报错退出**，让用户先停掉 dev-up 起
#   的服务，而不是静默换端口制造一份查不到指标的报告。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"

gateway_port=8080
match_port=8082
room_port=8083

levels="1,10,50,100,500,1000"
duration=60
attack_interval=2.0
poll_interval=0.5
recycle=1
recycle_gap=1.0
keep=0
manage_docker=1
use_prometheus=1
prometheus_port="${PROMETHEUS_PORT:-9090}"
label_suffix=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --levels) levels="${2:-}"; shift 2 ;;
    --level) levels="${2:-}"; shift 2 ;;
    --duration) duration="${2:-}"; shift 2 ;;
    --attack-interval) attack_interval="${2:-}"; shift 2 ;;
    --poll-interval) poll_interval="${2:-}"; shift 2 ;;
    --no-recycle) recycle=0; shift ;;
    --recycle-gap) recycle_gap="${2:-}"; shift 2 ;;
    --keep) keep=1; shift ;;
    --no-docker) manage_docker=0; shift ;;
    --no-prometheus) use_prometheus=0; shift ;;
    --label) label_suffix="${2:-}"; shift 2 ;;
    -h | --help)
      sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "未知参数：$1" >&2
      exit 2
      ;;
  esac
done

run_id="$(date +%Y%m%d-%H%M%S)${label_suffix:+-$label_suffix}"
raw_dir="docs/benchmarks/raw/$run_id"
log_dir="$raw_dir/logs"
mkdir -p "$log_dir"
summary_tsv="$raw_dir/summary.tsv"

failures=()
fail() { failures+=("$1"); echo "x  $1"; }
ok() { echo "v  $1"; }

log() { echo; echo "===== $* ====="; }

gateway_pid=""
match_pid=""
room_pid=""

stop_services() {
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  # 等它们真的退出：SIGTERM 之后还要落库/收订阅，立刻返回会污染下一档。
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    [ -z "$pid" ] && continue
    for _ in $(seq 1 40); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.25
    done
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  done
  gateway_pid=""; match_pid=""; room_pid=""
}

cleanup() {
  [ "$keep" -eq 1 ] || stop_services
}
trap cleanup EXIT

port_in_use() { ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"; }

# 结构化日志取值：`key=value`，值可能带引号。
log_field() {
  local file="$1" event="$2" field="$3"
  grep -F "event=$event" "$file" 2>/dev/null | tail -1 |
    grep -oE " ${field}=(\"[^\"]*\"|[^ ]*)" | sed -E "s/^ ${field}=//; s/^\"//; s/\"$//"
}

metric_value() {
  awk -v key="$2" '
    index($0, key) == 1 {
      n = split($0, parts, " ")
      value = parts[n]
      found = 1
    }
    END { print found ? value : "MISSING" }
  ' "$1"
}

prom_value() {
  curl -s --max-time 5 --data-urlencode "query=$1" "http://127.0.0.1:${prometheus_port}/api/v1/query" \
    2>/dev/null | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    print("MISSING")
    sys.exit(0)
result = data.get("data", {}).get("result", [])
print(result[0]["value"][1] if result else "MISSING")
' 2>/dev/null
}

# ---------------------------------------------------------------------------
log "0. 前置检查"
# ---------------------------------------------------------------------------

# 文件描述符：1000 条 SSE 长连接 + 压测端 socket 会打满默认的 10240。
soft_limit="$(ulimit -Sn)"
hard_limit="$(ulimit -Hn)"
if [ "$hard_limit" -lt 8192 ]; then
  fail "硬上限 ulimit -Hn=$hard_limit 太小，1000 档位不可能成立"
else
  ulimit -n 8192 2>/dev/null || ulimit -n "$hard_limit" 2>/dev/null || true
  ok "文件描述符：soft $soft_limit -> $(ulimit -Sn)（硬上限 $hard_limit）"
fi

[ -f "$env_file" ] || { [ -f "$env_example" ] && cp "$env_example" "$env_file"; }
set -a
# shellcheck disable=SC1090
. "./$env_file"
set +a

# 环境留档。报告里每一条"可复现"的主张都要靠它：没有这份记录，
# 别人无法判断数字是在什么机器、什么编译器、什么构建类型下得到的。
{
  echo "# TASK-022 压测环境快照"
  echo "captured_at=$(date -Is)"
  echo "kernel=$(uname -srmo)"
  echo "distro=$(grep -m1 '^PRETTY_NAME=' /etc/os-release 2>/dev/null | cut -d= -f2- | tr -d '"')"
  echo "cpu_model=$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo)"
  echo "cpu_cores=$(nproc)"
  echo "mem_total_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)"
  echo "mem_available_kb=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)"
  echo "ulimit_n_soft=$(ulimit -Sn)  ulimit_n_hard=$(ulimit -Hn)"
  echo "file_max=$(cat /proc/sys/fs/file-max 2>/dev/null)"
  echo "gcc=$(gcc --version 2>/dev/null | head -1)"
  echo "cmake=$(cmake --version 2>/dev/null | head -1)"
  echo "cmake_preset=$preset  (Debug，未开优化)"
  echo "python=$(python3 --version 2>&1)"
  echo "loadgen=bench/loadgen.py  (asyncio，与三个服务同机)"
  if docker info >/dev/null 2>&1; then
    echo "docker_cpus=$(docker info --format '{{.NCPU}}' 2>/dev/null)"
    echo "docker_mem_bytes=$(docker info --format '{{.MemTotal}}' 2>/dev/null)"
  fi
  echo "prometheus_scrape_interval=5s  (deploy/compose/prometheus/prometheus.yml)"
  echo "gateway_port=$gateway_port  match_port=$match_port  room_port=$room_port"
  echo "note=压测端与被测服务同机、Debug 构建：本报告用于横向比较档位与定位瓶颈，"
  echo "note=不用于宣称绝对性能。"
} >"$log_dir/env.txt"
ok "环境快照已写入 $log_dir/env.txt"

if [ "$manage_docker" -eq 1 ]; then
  if ! docker info >/dev/null 2>&1; then
    fail "Docker 不可达，无法启动 Redis/MySQL"
  fi
fi

# 端口预检：占用就退出，不静默换端口（理由见文件头）。
port_busy=0
for pair in "Gateway:$gateway_port" "Match:$match_port" "Room:$room_port"; do
  name="${pair%%:*}"
  port="${pair##*:}"
  if port_in_use "$port"; then
    fail "$name 端口 $port 已被占用（先执行 bash scripts/dev-down.sh）"
    port_busy=1
  fi
done
[ "$port_busy" -eq 0 ] && ok "端口 $gateway_port/$match_port/$room_port 空闲"

if [ "$use_prometheus" -eq 1 ]; then
  if curl -s -o /dev/null --max-time 3 "http://127.0.0.1:${prometheus_port}/-/ready"; then
    ok "Prometheus 可访问（:${prometheus_port}），将采集服务端指标"
  else
    echo "!  Prometheus 不可达：本次不采集服务端指标（服务端 /metrics 仍会落盘）"
    echo "   需要服务端指标时先执行：bash scripts/observability-up.sh"
    use_prometheus=0
  fi
fi

if [ "${#failures[@]}" -gt 0 ]; then
  echo
  echo "前置检查未通过，退出。"
  exit 1
fi

# ---------------------------------------------------------------------------
log "1. 构建（$preset）"
# ---------------------------------------------------------------------------
cmake --preset "$preset" >"$log_dir/cmake.log" 2>&1 &&
  ok "配置成功" || { fail "配置失败"; tail -20 "$log_dir/cmake.log"; }
cmake --build --preset "$preset" >"$log_dir/build.log" 2>&1
if [ $? -eq 0 ]; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 "$log_dir/build.log"
  exit 1
fi

bin_dir="build/$preset/bin"
for exe in rgbt_gateway rgbt_match rgbt_room; do
  [ -x "$bin_dir/$exe" ] || { fail "缺少可执行文件 $bin_dir/$exe"; exit 1; }
done

# ---------------------------------------------------------------------------
log "2. 依赖（Redis / MySQL）"
# ---------------------------------------------------------------------------
if [ "$manage_docker" -eq 1 ]; then
  docker compose -f "$compose_file" up -d >"$log_dir/compose.log" 2>&1 &&
    ok "compose up 成功" || { fail "compose up 失败"; tail -20 "$log_dir/compose.log"; }
fi
for container in rgbt-redis rgbt-mysql; do
  healthy=0
  for _ in $(seq 1 60); do
    if [ "$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)" = "healthy" ]; then
      healthy=1
      break
    fi
    sleep 2
  done
  [ "$healthy" -eq 1 ] && ok "$container 健康" || fail "$container 未 healthy"
done
[ "${#failures[@]}" -gt 0 ] && exit 1

# 迁移只做一次：表结构在执行期间不变，放进每档循环只是浪费。
applied=0
for migration in migrations/*.sql; do
  [ -e "$migration" ] || continue
  if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$migration" >/dev/null 2>&1; then
    applied=$((applied + 1))
  fi
done
ok "已应用 $applied 个迁移"

# ---------------------------------------------------------------------------
# 每档：清状态 -> 起服务 -> 压测 -> 采集 -> 停服务
# ---------------------------------------------------------------------------

printf 'level\tplayers\twall_s\tlogin_ok\tpaired\tstream_opened\tattacks_ok\tgames_finished\texceptions\thttp_req_per_s\troom_rpc_per_s\tframes_per_s\tsnapshot_per_s\tgw_rss_mb\n' \
  >"$summary_tsv"

# 累计计数器求和。**列名必须叫 `..._total` 而不是 `qps`**：这些是 Prometheus 的
# 累计值，把它当 QPS 写进报告是最容易犯也最难发现的一类错误——数字看起来合理，
# 含义完全错了。速率一律在这里用"累计 / 墙钟时长"现算。
metric_sum() {
  local file="$1" prefix="$2"
  [ -f "$file" ] || { echo 0; return; }
  awk -v p="$prefix" 'index($0, p) == 1 { n = split($0, parts, " "); total += parts[n] } END { printf "%.0f", total + 0 }' "$file"
}

per_second() {
  awk -v total="$1" -v wall="$2" 'BEGIN { if (wall > 0) printf "%.1f", total / wall; else print "0" }'
}

start_services() {
  # `-enable_bench_accounts`：识认合成测试身份 `bench-NNNNN`。
  # 默认关闭，只有本脚本显式打开——理由见 src/gateway/test_credentials.hpp。
  "$bin_dir/rgbt_room" -port "$room_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" >"$log_dir/room.log" 2>&1 &
  room_pid=$!
  for _ in $(seq 1 40); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && break
    sleep 0.5
  done
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" ||
    { fail "Room 未就绪"; return 1; }

  "$bin_dir/rgbt_match" -match_port "$match_port" -match_timeout_seconds 30 \
    -match_result_ttl_seconds 60 -env_prefix dev \
    -room_host 127.0.0.1 -room_port "$room_port" >"$log_dir/match.log" 2>&1 &
  match_pid=$!
  for _ in $(seq 1 40); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && break
    sleep 0.5
  done
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" ||
    { fail "Match 未就绪"; return 1; }

  "$bin_dir/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
    -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
    -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
    -mysql_database "$MYSQL_DATABASE" \
    -match_host 127.0.0.1 -match_port "$match_port" \
    -room_host 127.0.0.1 -room_port "$room_port" \
    -match_timeout_ms 500 -room_timeout_ms 500 \
    -enable_bench_accounts >"$log_dir/gateway.log" 2>&1 &
  gateway_pid=$!
  for _ in $(seq 1 40); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && break
    sleep 0.5
  done
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" ||
    { fail "Gateway 未就绪"; return 1; }
  return 0
}

# 为 N 个机器人准备**互不相同**的玩家档案。
#
# 为什么必须做：内置的启用身份只有 3 个（alice/bob/dave）。用它们跑 1000 连接时
# 1000 个机器人只有 3 个 `player_id`，于是：
#   * 同一玩家的重复入队被幂等判为 `already_queued`（实测 997/1000 次）；
#   * 同一玩家的重复进房是**幂等成功**，把"加入已结束的房间"计成正常对局；
#   * 最终得到"667 次 join 挤进同一个房间"这种自相矛盾的数字。
# 那个数字既不是容量上限也不是缺陷证据，只是夹具的假象。
provision_bench_accounts() {
  local count="$1"
  local sql=""
  sql+="INSERT IGNORE INTO players (player_id, account, display_name, status) VALUES "
  local i=0
  while [ "$i" -lt "$count" ]; do
    [ "$i" -gt 0 ] && sql+=","
    # player_id 用 p-9NNNNN 段，避开内置身份占用的 p-0001..p-0004。
    sql+="(CONCAT('p-9', LPAD($i, 5, '0')), CONCAT('bench-', LPAD($i, 5, '0')), CONCAT('Bench ', $i), 'active')"
    i=$((i + 1))
  done
  sql+=";"
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "$sql" >/dev/null 2>&1
  local have
  have=$(docker exec -i rgbt-mysql mysql -N -B -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "SELECT COUNT(*) FROM players WHERE account LIKE 'bench-%';" 2>/dev/null | tr -d '\r')
  printf '%s' "${have:-0}"
}

reset_state() {
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    -e "DELETE FROM rooms;" >/dev/null 2>&1
}

collect_metrics() {
  local tag="$1"
  for pair in "gateway:$gateway_port" "match:$match_port" "room:$room_port"; do
    name="${pair%%:*}"
    port="${pair##*:}"
    curl -s --max-time 5 "http://127.0.0.1:$port/metrics" >"$raw_dir/$tag.$name.metrics" 2>/dev/null ||
      : >"$raw_dir/$tag.$name.metrics"
  done

  # 进程内存（fault-injection 与容量报告都需要一个真实数字，不能只写"没测"）。
  # 直接从 /proc/<pid>/status 读 VmRSS：不引入采集组件，也不依赖容器。
  {
    printf 'service\trss_kb\tthreads\n'
    for pair in "gateway:$gateway_pid" "match:$match_pid" "room:$room_pid"; do
      name="${pair%%:*}"
      pid="${pair##*:}"
      if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
        rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")
        threads=$(awk '/^Threads:/{print $2}' "/proc/$pid/status")
        printf '%s\t%s\t%s\n' "$name" "${rss:-0}" "${threads:-0}"
      else
        printf '%s\t-\t-\n' "$name"
      fi
    done
  } >"$raw_dir/$tag.rss.tsv"

  # 按端点分组的延迟分位数。**必须分组**：指标层的直方图不带 path 标签
  # （刻意避免高基数），只取 Prometheus 的聚合值回答不了"哪个端点慢"。
  if [ -x "$(command -v python3)" ]; then
    python3 bench/histogram_quantiles.py "$raw_dir/$tag.gateway.metrics" \
      >"$raw_dir/$tag.latency_by_path.txt" 2>/dev/null || : >"$raw_dir/$tag.latency_by_path.txt"
  fi

  if [ "$use_prometheus" -eq 1 ]; then
    for spec in \
      "rgbt_http_requests_total|gw_http_requests_total" \
      "rgbt_http_request_seconds_count|gw_http_request_seconds_count" \
      "rgbt_rpc_calls_total|gw_rpc_calls_total" \
      "rgbt_sse_connections|gw_sse_connections" \
      "rgbt_match_events_total|match_events_total" \
      "rgbt_match_queue_length|match_queue_length" \
      "rgbt_rooms|room_phase_count" \
      "rgbt_room_frames_advanced_total|room_frames_advanced_total" \
      "rgbt_snapshot_write_total|room_snapshot_write_total" \
      "rgbt_result_persist_total|room_result_persist_total"; do
      query="${spec%%|*}"
      out="${spec##*|}"
      curl -s --max-time 5 --data-urlencode "query=sum($query)" \
        "http://127.0.0.1:${prometheus_port}/api/v1/query" >"$raw_dir/$tag.prom.$out.json" 2>/dev/null
    done
    # 延迟分位数从 Prometheus 侧取（服务端直方图），这样报告里的 P95/P99 与
    # Grafana 面板同源，不需要另写一个解析器。
    for q in \
      'histogram_quantile(0.50, sum by (le) (rate(rgbt_http_request_seconds_bucket[1m])))' \
      'histogram_quantile(0.95, sum by (le) (rate(rgbt_http_request_seconds_bucket[1m])))' \
      'histogram_quantile(0.99, sum by (le) (rate(rgbt_http_request_seconds_bucket[1m])))'; do
      name="p$(printf '%s' "$q" | grep -oE '0\.[0-9]+' | head -1 | cut -d. -f2)"
      curl -s --max-time 5 --data-urlencode "query=$q" \
        "http://127.0.0.1:${prometheus_port}/api/v1/query" \
        >"$raw_dir/$tag.prom.latency_$name.json" 2>/dev/null
    done
  fi
}

prom_scalar() {
  python3 -c '
import json, sys
try:
    data = json.load(open(sys.argv[1]))
except Exception:
    print("MISSING"); sys.exit(0)
result = data.get("data", {}).get("result", [])
if not result:
    print("0"); sys.exit(0)
try:
    print(round(float(result[0]["value"][1]), 3))
except Exception:
    print("MISSING")
' "$1" 2>/dev/null || echo "MISSING"
}

IFS=',' read -r -a level_list <<<"$levels"

for level in "${level_list[@]}"; do
  level="$(printf '%s' "$level" | tr -d ' ')"
  [ -n "$level" ] || continue
  log "档位 $level 连接（时长 ${duration}s）"

  reset_state
  start_services || { stop_services; continue; }

  # 开户：为这一档准备 `level` 个独立玩家。做成"至少 level 个"而不是精确相等，
  # 这样多档连续跑时不会反复重建同一批账号。
  have="$(provision_bench_accounts "$level")"
  if [ "${have:-0}" -lt "$level" ]; then
    fail "档位 $level：合成账号只准备了 ${have:-0} 个，不足以让每个机器人有独立身份"
    stop_services
    continue
  fi
  ok "合成账号就绪（bench-* 共 ${have} 个）"

  # 每档用**独立的日志文件名**。第一版所有档位都写同一个 room.log，
  # 于是事后只能看到最后一档的服务端日志——而"每档到底建了几个房间"
  # 恰恰是判断结果是否可信的关键证据。
  run_log_dir="$log_dir/level-$level"
  mkdir -p "$run_log_dir"
  gw_log="$log_dir/gateway.log"
  # 把当前档位启动的日志复制一份归档（start_services 已经写进公共文件名）。
  cp "$log_dir/room.log" "$run_log_dir/room.log" 2>/dev/null || true
  cp "$log_dir/match.log" "$run_log_dir/match.log" 2>/dev/null || true
  cp "$gw_log" "$run_log_dir/gateway.log" 2>/dev/null || true

  # Prometheus 需要一点时间才能抓到这个新进程，并让 rate() 有窗口。
  sleep 5

  result_json="$raw_dir/level-$level.json"
  python3 bench/loadgen.py \
    --players "$level" \
    --bench-accounts \
    --gateway "127.0.0.1:$gateway_port" \
    --duration "$duration" \
    --attack-interval "$attack_interval" \
    --poll-interval "$poll_interval" \
    $( [ "$recycle" -eq 1 ] && printf '%s' "--recycle --recycle-gap $recycle_gap" ) \
    --out "$result_json" --label "players-$level" >"$log_dir/loadgen-$level.log" 2>&1
  loadgen_rc=$?
  tail -1 "$log_dir/loadgen-$level.log"

  # 归档本档的服务端日志（loadgen 跑完才写，因此要在这时复制）。
  cp "$log_dir/room.log" "$run_log_dir/room.log" 2>/dev/null || true
  cp "$log_dir/match.log" "$run_log_dir/match.log" 2>/dev/null || true
  cp "$gw_log" "$run_log_dir/gateway.log" 2>/dev/null || true

  collect_metrics "level-$level"

  # 汇总一行。服务端数字优先取 Prometheus（与面板同源）；查不到时回落到端点抓取。
  read -r players wall login_ok paired stream_opened attacks_ok finished exceptions <<EOF
$(python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("- - - - - - - -"); sys.exit(0)
c = d.get("counters", {})
print(d.get("players", 0),
      d.get("wall_seconds", 0),
      c.get("login_ok", 0),
      c.get("paired", 0),
      c.get("stream_opened", 0),
      c.get("attacks_ok", 0),
      c.get("games_finished", 0),
      sum(d.get("exceptions", {}).values()))
' "$result_json" 2>/dev/null || echo "- - - - - - - -")
EOF

  if [ "$use_prometheus" -eq 1 ]; then
    : # Prometheus 侧读数已落盘（prom.*.json），速率统一从端点文件现算，避免两处口径
  fi
  # 速率一律用"累计 / 墙钟时长"。用 awk 现算而不是存成字段：`wall` 是每次运行的
  # 实测值，硬编码任何除数都会让数字在换档位后悄悄失真。
  http_total="$(metric_sum "$raw_dir/level-$level.gateway.metrics" 'rgbt_http_requests_total{')"
  rpc_total="$(metric_sum "$raw_dir/level-$level.gateway.metrics" 'rgbt_rpc_calls_total{target="room"')"
  frames_total="$(metric_sum "$raw_dir/level-$level.room.metrics" 'rgbt_room_frames_advanced_total')"
  snapshot_total="$(metric_sum "$raw_dir/level-$level.room.metrics" 'rgbt_snapshot_write_total{')"
  gw_rss_mb="$(awk -F'\t' '$1=="gateway"{printf "%.1f", $2/1024; found=1} END{if(!found) print "-"}' \
    "$raw_dir/level-$level.rss.tsv" 2>/dev/null)"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$level" "$players" "$wall" "$login_ok" "$paired" "$stream_opened" "$attacks_ok" \
    "$finished" "$exceptions" \
    "$(per_second "$http_total" "$wall")" \
    "$(per_second "$rpc_total" "$wall")" \
    "$(per_second "$frames_total" "$wall")" \
    "$(per_second "$snapshot_total" "$wall")" \
    "${gw_rss_mb:--}" >>"$summary_tsv"

  if [ "$loadgen_rc" -ne 0 ]; then
    fail "档位 $level：loadgen 退出码 $loadgen_rc（登录失败或订阅完全打不开）"
    echo "  详情：$log_dir/loadgen-$level.log"
  else
    ok "档位 $level 完成（日志：$log_dir/loadgen-$level.log）"
  fi

  # 每档末尾都要停干净：下一档重启，档位之间不共享进程状态。
  stop_services
  sleep 2
done

# ---------------------------------------------------------------------------
log "3. 汇总"
# ---------------------------------------------------------------------------
column -t -s $'\t' "$summary_tsv" 2>/dev/null || cat "$summary_tsv"
echo
echo "原始数据：$raw_dir/"
echo "  summary.tsv              每档的关键计数"
echo "  level-<N>.json           压测端原始结果（含延迟分位数）"
echo "  level-<N>.{gateway,match,room}.metrics   服务端 /metrics 快照"
if [ "$use_prometheus" -eq 1 ]; then
  echo "  level-<N>.prom.*.json    Prometheus 侧同源读数（与 Grafana 面板一致）"
fi
echo "   logs/                   三个服务与 loadgen 的完整输出"

if [ "${#failures[@]}" -gt 0 ]; then
  echo
  echo "有 ${#failures[@]} 项未通过："
  for item in "${failures[@]}"; do echo "  - $item"; done
  exit 1
fi

echo
echo "全部档位执行完毕。"
