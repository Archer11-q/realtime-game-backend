#!/usr/bin/env bash
#
# chaos/lib.sh - 统一故障注入与「检测时间 / 恢复时间」测量夹具（TASK-023）
#
# 为什么是脚本而不是 chaos/ 下的 C++ 工具：注入动作全是进程与容器操作
# （起停中继、停启容器、杀进程），C++ 只会多一层编译。
#
# 本文件只提供**原语**：注入、测量、就绪等待、断言、报文。
# 具体场景在 chaos/verify-*.sh 里，用这些原语拼出来。
#
# 设计要点
# --------
# 1. **按通道注入**（本任务的核心约束）。三个服务共用同一个 Redis 与同一个
#    MySQL，直接 `docker stop rgbt-redis` 会同时打掉 Gateway 的会话存储和
#    Match 的快照存储——所有 HTTP 请求在鉴权那一步就 503，根本走不到被测通道
#    （TASK-015 踩过这个坑）。因此注入做在**网络层**：让某个服务连
#    `chaos/relay.py` 而不是直连依赖，杀掉中继就只切断那一个服务的依赖。
#    容器级的 `stop_dependency` / `start_dependency` 保留给「整个依赖停机」
#    这一类场景（TASK-024 及之后会用到）。
#
# 2. **测量是两个数字，不是一个**。任务单要求每个通道都输出
#      * 检测时间（detection）= 注入依赖不可用 -> 首次观测到预期失败
#      * 恢复时间（recovery） = 依赖恢复可用 -> 首次观测到业务自愈
#    恢复时间**不含**重启依赖本身要花的时间（容器 healthy 要几秒到几十秒），
#    否则数字衡量的是 docker 的启动速度，不是服务的自愈速度。
#
# 3. 调用方必须先 source 本文件、再 source verify-dependency-down.sh 里
#    同名的运行时变量；本文件不定义具体端口与账号。

# ---------------------------------------------------------------------------
# 失败收集与输出
# ---------------------------------------------------------------------------
CHAOS_FAILURES=()
CHAOS_TIMINGS=()

chaos_fail() { CHAOS_FAILURES+=("$1"); echo "x  $1"; }
chaos_ok() { echo "v  $1"; }
chaos_info() { echo "   $1"; }
chaos_step() { echo; echo "===== $* ====="; }

# 记录一行计时，最后统一打印汇总表。
#   record_time <通道> <检测|恢复> <毫秒> <说明>
record_time() {
  local channel="$1" kind="$2" ms="$3" note="$4"
  CHAOS_TIMINGS+=("${channel}|${kind}|${ms}|${note}")
}

chaos_has_failures() { [ "${#CHAOS_FAILURES[@]}" -gt 0 ]; }

chaos_print_failures() {
  if ! chaos_has_failures; then
    return 0
  fi
  echo
  echo "注入验收失败 ${#CHAOS_FAILURES[@]} 项："
  local item
  for item in "${CHAOS_FAILURES[@]}"; do echo "  - $item"; done
  return 1
}

# 打印「场景 | 检测时间 | 恢复时间」汇总表。
chaos_print_time_table() {
  echo
  echo "===== 检测时间与恢复时间汇总（本机实测） ====="
  printf '%-34s %-10s %-12s %s\n' "通道 / 场景" "类型" "耗时" "说明"
  printf '%-34s %-10s %-12s %s\n' "----------------------------------" "----------" "------------" "----"
  local row channel kind ms note
  for row in "${CHAOS_TIMINGS[@]}"; do
    IFS='|' read -r channel kind ms note <<<"$row"
    if [ "$ms" = "0" ]; then
      printf '%-34s %-10s %-12s %s\n' "$channel" "$kind" "0 ms" "$note"
    else
      printf '%-34s %-10s %-12s %s\n' "$channel" "$kind" "${ms} ms" "$note"
    fi
  done
}

# ---------------------------------------------------------------------------
# 时间与端口
# ---------------------------------------------------------------------------
# 时间基准：**不用 `date`**。
#
# 本机实测（coreutils 9.x）：`date +%s%3N` 并不把纳秒截断成 3 位，而是输出
# 「epoch 秒 + 字面量 3 + 纳秒」——比真实毫秒大 1000 倍左右
# （例：`date +%s%3N` = 179110103575678273，而 `date +%s` = 1791101035）。
# 拿它当毫秒用，所有时间都会变成天文数字。因此这里改用 bash 自带的 EPOCHREALTIME
# （bash ≥ 5.0，`秒.微秒`），乘法与截断全部走整数运算，不依赖外部命令。
#
# 说明：`scripts/bench.sh` 也在用 `date +%s%3N` 做「墙上时长」。它的一致性不受
# 影响（首尾同源相减），但那些秒数与真实秒不同源，属既有代码的独立问题，
# 已记入 `docs/devlog.md` 的 TASK-023 实施记录，不在本任务改动范围内。
now_us() { # 自 epoch 起的微秒（字符串按位拼成整数，无浮点）
  local t="${EPOCHREALTIME}"          # 形如 1791101035.755782
  echo "${t/.}"                       # 1791101035755782
}

now_ms() { echo $(( $(now_us) / 1000 )); }

elapsed_ms() { # <起始毫秒>
  echo $(( $(now_ms) - $1 ))
}

# 返回「起始到现在的毫秒」。参数由 now_us 取得，因此**不要**传 now_ms 的值：
# 微秒串被 1000 整除即可，两端都按微秒算能保留亚毫秒精度。
elapsed_ms_from_us() { # <起始微秒>
  echo $(( ($(now_us) - $1) / 1000 ))
}

# 端口是否有进程在监听。**必须锚定行尾**：grep -q ':63' 会把 6300 也算进去。
port_listening() {
  ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"
}

# 等待端口被释放（发完 SIGTERM 后确认真的不再接受连接）。
wait_port_released() { # <端口> [超时秒数]
  local port="$1" timeout="${2:-10}"
  local deadline=$(( $(now_ms) + timeout * 1000 ))
  while port_listening "$port"; do
    [ "$(now_ms)" -ge "$deadline" ] && return 1
    sleep 0.05
  done
  return 0
}

wait_port_listening() { # <端口> [超时秒数]
  local port="$1" timeout="${2:-5}"
  local deadline=$(( $(now_ms) + timeout * 1000 ))
  while ! port_listening "$port"; do
    [ "$(now_ms)" -ge "$deadline" ] && return 1
    sleep 0.05
  done
  return 0
}

# ---------------------------------------------------------------------------
# 通道中继（chaos/relay.py）
# ---------------------------------------------------------------------------
relay_pids=()
relay_ports=()

# start_relay <名字> <监听端口> <目标 host:port>
#
# 必须等到中继打印 ready 才返回：否则「注入前正常」那一段会因为中继尚未监听
# 而失败，把夹具问题误报成产品缺陷。
start_relay() {
  local name="$1" listen="$2" target="$3"
  local log="$CHAOS_LOG_DIR/relay-$name.log"
  if port_listening "$listen"; then
    chaos_fail "中继 $name 端口 $listen 已被占用，无法建立隔离通道"
    return 1
  fi
  : >"$log"
  python3 chaos/relay.py --listen "$listen" --target "$target" >>"$log" 2>&1 &
  local pid=$!
  relay_pids+=("$pid")
  relay_ports+=("$listen")
  mkdir -p "$CHAOS_RUN_DIR"
  echo "$pid" >"$CHAOS_RUN_DIR/relay-$name.pid"
  local i
  for i in $(seq 1 50); do
    grep -q 'relay ready' "$log" 2>/dev/null && { chaos_ok "中继 $name 就绪（127.0.0.1:$listen -> $target，pid $pid）"; return 0; }
    kill -0 "$pid" 2>/dev/null || { chaos_fail "中继 $name 启动即退出：$(tail -3 "$log" | tr '\n' ' ')"; return 1; }
    sleep 0.1
  done
  chaos_fail "中继 $name 未在 5 秒内就绪"
  return 1
}

# stop_relay <名字> <端口>：切断该通道，并等端口真的释放。
#
# 等端口释放不是形式：若监听还在，服务下一次连接仍会成功，「注入」根本没生效，
# 测出来的检测时间会是 0——那属于测试错误（任务单明确列为失败场景）。
stop_relay() {
  local name="$1" listen="$2"
  local pidfile="$CHAOS_RUN_DIR/relay-$name.pid"
  local pid=""
  [ -f "$pidfile" ] && pid="$(cat "$pidfile")"
  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    chaos_fail "中继 $name 不在运行，无法注入（pidfile $pidfile）"
    return 1
  fi
  kill -TERM "$pid" 2>/dev/null || true
  local i
  for i in $(seq 1 40); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.05
  done
  kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  rm -f "$pidfile"
  if ! wait_port_released "$listen" 5; then
    chaos_fail "中继 $name 停止后端口 $listen 仍被监听：注入未生效"
    return 1
  fi
  return 0
}

# restart_relay <名字> <端口> <目标>：把通道恢复。
restart_relay() {
  start_relay "$1" "$2" "$3"
}

# watch_relay <名字> <端口>：等待该通道恢复可用。恢复计时用这个，不用容器健康检查。
watch_relay() {
  local name="$1" listen="$2"
  local deadline=$(( $(now_ms) + 10000 ))
  while ! port_listening "$listen"; do
    [ "$(now_ms)" -ge "$deadline" ] && { chaos_fail "中继 $name 未在 10 秒内恢复监听"; return 1; }
    sleep 0.02
  done
  return 0
}

# ---------------------------------------------------------------------------
# 容器依赖（整个依赖停机；TASK-024 及之后会用到）
# ---------------------------------------------------------------------------
dependency_up() { # <容器名>
  [ "$(docker inspect -f '{{.State.Health.Status}}' "$1" 2>/dev/null)" = "healthy" ]
}

stop_dependency() { # <容器名>
  docker stop "$1" >/dev/null 2>&1 || return 1
  docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null | grep -q false
}

wait_dependency_healthy() { # <容器名> [超时秒数]
  local name="$1"
  local timeout="${2:-120}"
  local start_us deadline
  start_us="$(now_us)"
  deadline=$(( $(now_ms) + timeout * 1000 ))
  while ! dependency_up "$name"; do
    [ "$(now_ms)" -ge "$deadline" ] && return 1
    sleep 0.2
  done
  DEP_HEALTHY_SECONDS="$(awk -v a="$start_us" -v b="$(now_us)" 'BEGIN { printf "%.1f", (b - a) / 1000000 }')"
  return 0
}

# ---------------------------------------------------------------------------
# 连通性探针
# ---------------------------------------------------------------------------
# probe_tcp <host> <port>：能建立 TCP 连接即成功。
probe_tcp() {
  python3 - "$1" "$2" <<'PY'
import socket, sys
try:
    with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2):
        pass
    sys.exit(0)
except OSError:
    sys.exit(1)
PY
}

# probe_redis <host> <port>：发一个真实的 PING 并校验 +PONG。
#
# 只做 TCP 连接不够：探测一个「能连上但不转发」的中继会误判为健康。
probe_redis() {
  python3 - "$1" "$2" <<'PY'
import socket, sys
try:
    with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2) as s:
        s.settimeout(2)
        s.sendall(b"PING\r\n")
        sys.exit(0 if b"+PONG" in s.recv(64) else 1)
except OSError:
    sys.exit(1)
PY
}

# probe_mysql <host> <port>：读一次 MySQL 握手包首包。
#
# 为什么读握手而不是直接连上就算：本题只关心「这条通道能不能到达 MySQL」，
# 不需要认证。服务自己的连接池会做完整认证，这里只要证明链路通。
probe_mysql() {
  python3 - "$1" "$2" <<'PY'
import socket, struct, sys
try:
    with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2) as s:
        s.settimeout(2)
        header = b""
        while len(header) < 4:
            chunk = s.recv(4 - len(header))
            if not chunk:
                sys.exit(1)
            header += chunk
        length = struct.unpack("<I", header[:3] + b"\x00")[0]
        sys.exit(0 if length > 0 else 1)
except OSError:
    sys.exit(1)
PY
}

# ---------------------------------------------------------------------------
# HTTP 报文（经 Gateway；服务自身的 /metrics、/health 由 curl 直接访问）
# ---------------------------------------------------------------------------
# 调用方必须提供：GW_PORT（Gateway 端口）、MATCH_PORT（Match 端口）、
# CHAOS_RESP_FILE（响应落盘位置）。
http_req() { # <GET|POST|DELETE> <path> [token] [body]
  local method="$1" path="$2" token="${3:-}" body="${4:-}"
  local args=(-s -o "$CHAOS_RESP_FILE" -w '%{http_code}' --max-time 8 -X "$method")
  [ -n "$token" ] && args+=(-H "Authorization: Bearer $token")
  args+=(-H 'Content-Type: application/json')
  [ -n "$body" ] && args+=(-d "$body")
  curl "${args[@]}" "http://127.0.0.1:$GW_PORT$path" 2>/dev/null
}

http_post() { http_req POST "$1" "${2:-}" "${3:-}"; }
http_get() { http_req GET "$1" "${2:-}" ""; }
http_delete() { http_req DELETE "$1" "${2:-}" ""; }

http_get_metric() { # <端口> <路径>
  curl -s --max-time 4 "http://127.0.0.1:$1$2" 2>/dev/null
}

# jq_path <点分路径>：从 CHAOS_RESP_FILE 里取值。空/不存在时打印空串。
jq_path() {
  python3 - "$1" "$CHAOS_RESP_FILE" <<'PY'
import json, sys
path, filename = sys.argv[1], sys.argv[2]
try:
    with open(filename, encoding="utf-8") as handle:
        node = json.load(handle)
except Exception:
    print("")
    sys.exit(0)
for part in path.split("."):
    if not part:
        continue
    if isinstance(node, dict) and part in node:
        node = node[part]
    else:
        print("")
        sys.exit(0)
print("" if node is None else node)
PY
}

# json_get <键>：从 CHAOS_RESP_FILE 顶层取值。
json_get() { jq_path "$1"; }

# ---------------------------------------------------------------------------
# 轮询与三段式测量
# ---------------------------------------------------------------------------
# wait_until <命令...>：在超时内反复执行命令直到它返回 0。
#
# 命中时返回 0 并打印耗时（毫秒）；超时返回 1（打印空）。
# 前 10 次不 sleep：本机环回上的失败通常是毫秒级的，加 sleep 会让测出来的
# 「检测时间」变成轮询周期的整数倍，那是夹具的粒度，不是服务的反应速度。
wait_until() {
  local timeout_ms="${CHAOS_WAIT_TIMEOUT_MS:-12000}"
  local start; start="$(now_ms)"
  local deadline=$(( start + timeout_ms ))
  local i=0
  while :; do
    if "$@"; then
      echo $(( $(now_ms) - start ))
      return 0
    fi
    i=$(( i + 1 ))
    [ "$(now_ms)" -ge "$deadline" ] && return 1
    if [ "$i" -gt 10 ]; then sleep 0.05; fi
  done
}

# 探针：都返回 0/1，供 wait_until 使用。
probe_gw_login_ok() { [ "$(http_post /api/v1/login "" "{\"account\":\"$PROBE_ACCOUNT\",\"password\":\"$PROBE_PASSWORD\",\"client_type\":\"web\"}")" = "200" ]; }
probe_gw_login_503() { [ "$(http_post /api/v1/login "" "{\"account\":\"$PROBE_ACCOUNT\",\"password\":\"$PROBE_PASSWORD\",\"client_type\":\"web\"}")" = "503" ]; }
probe_gw_me_ok() { [ "$(http_get /api/v1/players/me "$1")" = "200" ]; }
probe_gw_match_ok() { [ "$(http_get /api/v1/matches/current "$1")" = "200" ]; }
probe_gw_room_state_ok() { [ "$(http_get "/api/v1/rooms/state?room_id=$2" "$1")" = "200" ]; }

# 期望的 HTTP 状态与 error.reason 是否同时命中。
expect_response() { # <期望状态> <期望 reason> [允许 reason 为空]
  local want_status="$1" want_reason="$2"
  local got_status="${CHAOS_LAST_STATUS:-}"
  local got_reason
  got_reason="$(jq_path error.reason)"
  if [ "$got_status" != "$want_status" ]; then
    echo "HTTP $got_status（期望 $want_status）"
    return 1
  fi
  if [ -n "$want_reason" ] && [ "$got_reason" != "$want_reason" ]; then
    echo "reason=[$got_reason]（期望 $want_reason）"
    return 1
  fi
  return 0
}

# 统一的一次报文 + 判定，把结果计入失败列表。
check_http() { # <描述> <方法> <path> <token> <body> <期望状态> [期望 reason]
  local desc="$1" method="$2" path="$3" token="$4" body="$5" want="$6" reason="${7:-}"
  CHAOS_LAST_STATUS="$(http_req "$method" "$path" "$token" "$body")"
  # 只用命令替换的退出码判定，**不要在这里动 `set -e`**：
  # 在函数里 `set -e` 打开的是**整个 shell** 的 errexit，不是这个函数的作用域。
  # 后果很隐蔽——脚本会在之后任意一个"返回非 0 但不致命"的命令处提前退出
  # （实测：play_game 里 `wait_until ... >/dev/null` 超时后整个脚本无声结束，
  # 且没有任何失败信息，看起来像"跑到一半被杀了"）。
  local detail status
  detail="$(expect_response "$want" "$reason")"
  status=$?
  if [ "$status" -eq 0 ]; then
    chaos_ok "$desc -> HTTP $want${reason:+ $reason}"
    return 0
  fi
  chaos_fail "$desc -> $detail"
  return 1
}

# ---------------------------------------------------------------------------
# 日志断言
# ---------------------------------------------------------------------------
log_has() { grep -qF -- "$2" "$CHAOS_LOG_DIR/$1" 2>/dev/null; }

log_count() { grep -cF -- "$2" "$CHAOS_LOG_DIR/$1" 2>/dev/null || echo 0; }

log_mark() { wc -l <"$CHAOS_LOG_DIR/$1" 2>/dev/null || echo 0; }

# 从第 N 行之后开始找（用于区分「注入前就有」与「注入后才出现」）。
log_has_since() { # <日志名> <标记行号> <匹配串>
  local file="$CHAOS_LOG_DIR/$1" mark="$2" needle="$3"
  [ -f "$file" ] || return 1
  tail -n "+$(( mark + 1 ))" "$file" 2>/dev/null | grep -qF -- "$needle"
}

# wait_log_until <日志名> <匹配串>：在超时内反复检查日志里出现了该串。
#
# 命中时返回 0 并打印耗时（毫秒），超时返回 1。
#
# **它测的是"日志观测时间"，粒度受轮询间隔限制**（默认 100 ms）。日志是行缓冲写出的，
# 没有逐行时间戳，因此做不到毫秒级判定。只在"HTTP 侧看不到失败"的通道上用它
# （例如 Match 的快照写入失败：业务照常成功，失败只体现在日志与指标里）。
wait_log_until() { # <日志名> <匹配串> [超时毫秒]
  local name="$1" needle="$2" timeout_ms="${3:-8000}"
  local start; start="$(now_ms)"
  local deadline=$(( start + timeout_ms ))
  while :; do
    if grep -qF -- "$needle" "$CHAOS_LOG_DIR/$name" 2>/dev/null; then
      echo $(( $(now_ms) - start ))
      return 0
    fi
    [ "$(now_ms)" -ge "$deadline" ] && return 1
    sleep 0.1
  done
}

# match_metric_queue_length：从 Match 的 /metrics 读队列长度 gauge。
#
# 与 redis:<queue> 的 LLEN 是两件事：前者是**内存里的权威队列**，后者是**快照**。
# 通道 2 要同时看这两个：Redis 不可用时内存队列照常增长而快照停在 0。
match_metric_queue_length() {
  curl -s --max-time 4 "http://127.0.0.1:$MATCH_PORT/metrics" 2>/dev/null |
    python3 -c '
import sys
value = 0
for line in sys.stdin:
    if line.startswith("rgbt_match_queue_length"):
        parts = line.split()
        if len(parts) >= 2:
            value = parts[-1]
print(value)
'
}

# gw_metric_status_count <状态码>：Gateway /metrics 里该 HTTP 状态码的累计计数。
#
# 为什么需要它（TASK-023 实测发现）：Gateway 的**依赖不可用路径不写日志**——
# `FillError()` 只填错误体并返回状态码，没有任何 LogWarn/LogInfo。因此
# `session_store_unavailable` / `player_store_unavailable` 这两类失败在日志里
# **查不到**，唯一可观测的痕迹就是 `/metrics` 的
# `rgbt_http_requests_total{status="503"}` 与 HTTP 响应体里的 reason。
# 本函数让脚本能在不读日志的前提下证明「注入确实产生了一次 503」。
# 该缺口已记入 docs/devlog.md 的 TASK-023 实施记录，修复属产品代码改动，
# 不在本任务（只加测试工具）的范围内。
gw_metric_status_count() {
  curl -s --max-time 4 "http://127.0.0.1:$GW_PORT/metrics" 2>/dev/null |
    python3 -c '
import re, sys
want = sys.argv[1]
total = 0
for line in sys.stdin:
    m = re.match(r"rgbt_http_requests_total\{.*status=\"([0-9]{3})\".*\}\s+([0-9.eE+]+)", line)
    if m and m.group(1) == want:
        total += int(float(m.group(2)))
print(total)
' "$1"
}

# 读当前 CHAOS_RESP_FILE 里 error.reason 的值（表驱动的断言用）。
response_reason() { jq_path error.reason; }

# match_snapshot_metric <ok|failed>：Match 队列快照写入次数的 gauge（TASK-019 起导出）。
#
# 为什么用它而不是读日志（实测踩到）：写入失败**不一定**产生新的日志行——
# 失败路径只在 `MatchQueue::Save` 返回 false 时记一次 warn，而 Redis 客户端可能
# 在旧连接上"写成功"（连接在 Relay 被杀后并未立刻失效）。指标是计数器，读它才
# 能可靠回答"这一次注入有没有让写入失败"。日志只作为辅助证据。
match_snapshot_metric() {
  curl -s --max-time 4 "http://127.0.0.1:$MATCH_PORT/metrics" 2>/dev/null |
    python3 -c '
import re, sys
want = sys.argv[1]
value = 0
for line in sys.stdin:
    if line.startswith("rgbt_queue_snapshot_total"):
        m = re.search(r"outcome=\"([a-z]+)\"", line)
        if m and m.group(1) == want:
            parts = line.split()
            if len(parts) >= 2:
                value = parts[-1]
print(value)
' "$1"
}

# enqueue_with_retry <token> <请求 id> [尝试次数]
#
# 用途：服务的**瞬时**错误不该被记成注入的结论。
#
# 场景（实测踩到）：任何让 Match 重启的步骤之后，Gateway 侧到 Match 的 brpc 长连接
# 已经死了，但 brpc 不会立刻知道；紧随其后的第一次入队会拿到
# `match_unavailable`（HTTP 503），而 Match 本身完全健康。若夹具直接把它算成失败，
# 结论就错了。这里对**入队本身**做有限重试，并在全部失败时返回 1 让调用方报错。
enqueue_with_retry() { # <token> <请求 id> [尝试次数]
  local token="$1" request_id="$2" tries="${3:-8}" i code
  for i in $(seq 1 "$tries"); do
    code="$(http_post /api/v1/matches "$token" "{\"request_id\":\"$request_id\"}")"
    if [ "$code" = "200" ]; then
      return 0
    fi
    sleep 0.25
  done
  chaos_info "入队重试 $tries 次后仍失败：request_id=$request_id 最后 HTTP $code reason=$(jq_path error.reason)"
  return 1
}
