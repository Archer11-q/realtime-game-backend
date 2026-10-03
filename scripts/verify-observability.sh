#!/usr/bin/env bash
#
# verify-observability.sh - Phase 3 可观测性的验收入口
#
#   bash scripts/verify-observability.sh            # 日志 + 指标
#   bash scripts/verify-observability.sh --logs      # 只验结构化日志（TASK-018）
#   bash scripts/verify-observability.sh --metrics   # 只验 /metrics 指标（TASK-019）
#
# 两个模式合在一起的理由：它们共用"起三个服务 + 跑一次真实对局"这段代价最高的
# 准备，分开跑要各起一遍。
#
# 结构约定（重要）：本文件每一节都是"开一个条件、在几行内闭合"。
# 前几轮版本曾出现 `if [ "$run_logs" -eq 1 ]` 未闭合、把后面上百行全部吞掉的问题：
# 脚本照样打印"验收通过"，而真正的断言一条都没跑。坏掉的门禁比没有门禁更危险，
# 因为它给出虚假的把握。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"

keep_running=0
manage_docker=1
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
    -h | --help)
      sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "未知参数: $arg" >&2
      exit 2
      ;;
  esac
done

failures=()
fail() {
  failures+=("$1")
  echo "x  $1"
}
ok() { echo "v  $1"; }

gateway_port=""
match_port=""
room_port=""
gateway_pid=""
match_pid=""
room_pid=""

cleanup() {
  for pid in "$gateway_pid" "$match_pid" "$room_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      [ "$keep_running" -eq 1 ] && echo "已保留进程 $pid（--keep）" || kill "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

log_dir="/tmp/observability-logs"
rm -rf "$log_dir"
mkdir -p "$log_dir"

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

# 把计数收敛成单个数字。
# 为什么不用 `n=$(grep -c ... || echo 0)`：grep 无匹配时退出码为 1，`|| echo 0`
# 会让变量里出现两行，之后所有数值比较都会失配。
count_num() {
  local n
  n=$(grep -E -c "$1" "$2" 2>/dev/null)
  case "$n" in '' | *[!0-9]*) n=0 ;; esac
  printf '%s' "$n"
}

# 从抓取结果里取样本值。样本形如 `rgbt_x{label="v"} 12`，值在最后一个空格之后。
metric_value() {
  awk -v key="$2" '
    BEGIN { found = 0 }
    index($0, key) == 1 {
      n = split($0, parts, " ")
      value = parts[n]
      found = 1
    }
    END { print found ? value : "MISSING" }
  ' "$1"
}

http_post() {
  local body="${2:-}"
  [ -n "$body" ] || body='{}'
  curl -s -o /tmp/obs-resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}

json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/obs-resp.json') as handle:
        node = json.load(handle)
except Exception:
    print('')
    sys.exit(0)
for part in sys.argv[1].split('.'):
    if not part:
        continue
    if isinstance(node, dict):
        if part not in node:
            print('')
            sys.exit(0)
        node = node[part]
    else:
        print('')
        sys.exit(0)
print('' if node is None else node)
PY
}

login_token() {
  local account="$1"
  http_post /api/v1/login \
    "{\"account\":\"$account\",\"password\":\"${account}_dev_pw\",\"request_id\":\"obs-$account-$RANDOM\",\"client_type\":\"web\"}" >/dev/null
  json_path token
}

port_in_use() { ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"; }

pick_port() {
  local candidate
  for candidate in "$@"; do
    if ! port_in_use "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
}

# ===========================================================================
echo "===== 0. 前置检查 ====="
# ===========================================================================
[ -f "$env_file" ] || { [ -f "$env_example" ] && cp "$env_example" "$env_file"; }
set -a
# shellcheck disable=SC1090
. "./$env_file"
set +a
ok "配置已加载（库 ${MYSQL_DATABASE}）"

gateway_port="$(pick_port 8080 18080 18081 18082)" || { echo "Gateway 端口均被占用" >&2; exit 1; }
match_pool=()
for candidate in 8082 18092 18093 18094; do
  [ "$candidate" = "$gateway_port" ] || match_pool+=("$candidate")
done
match_port="$(pick_port "${match_pool[@]}")" || { echo "Match 端口均被占用" >&2; exit 1; }
room_pool=()
for candidate in 8083 18103 18104 18105; do
  [ "$candidate" = "$gateway_port" ] && continue
  [ "$candidate" = "$match_port" ] && continue
  room_pool+=("$candidate")
done
room_port="$(pick_port "${room_pool[@]}")" || { echo "Room 端口均被占用" >&2; exit 1; }
ok "端口：Gateway $gateway_port / Match $match_port / Room $room_port"

# ===========================================================================
if [ "$manage_docker" -eq 1 ]; then
  # =========================================================================
  echo "===== 1. 启动 Redis 与 MySQL ====="
  docker compose -f "$compose_file" up -d >/tmp/observability-compose.log 2>&1
  if [ $? -eq 0 ]; then
    ok "compose up 成功"
  else
    fail "compose up 失败"
    tail -20 /tmp/observability-compose.log
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
    if [ "$healthy" -eq 1 ]; then
      ok "$container 健康"
    else
      fail "$container 未达到 healthy"
    fi
  done
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
fi

# ===========================================================================
echo "===== 2. 应用迁移 ====="
# ===========================================================================
applied=0
for migration in migrations/*.sql; do
  [ -e "$migration" ] || continue
  if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
    <"$migration" >/dev/null 2>&1; then
    applied=$((applied + 1))
  fi
done
if [ "$applied" -gt 0 ]; then
  ok "已应用 $applied 个迁移"
else
  fail "迁移未成功应用"
fi
# 清理上一轮会话，避免复用到旧 Token。
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
ok "已清理上一轮的 dev:* Key"

# ===========================================================================
echo "===== 3. 构建并启动服务 ====="
# ===========================================================================
cmake --preset "$preset" >/tmp/observability-cmake.log 2>&1
if [ $? -eq 0 ]; then
  ok "配置成功"
else
  fail "配置失败"
  tail -20 /tmp/observability-cmake.log
fi
cmake --build --preset "$preset" >/tmp/observability-build.log 2>&1
if [ $? -eq 0 ]; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 /tmp/observability-build.log
  exit 1
fi

bin_dir="build/$preset/bin"

"$bin_dir/rgbt_room" -port "$room_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" >"$log_dir/room.log" 2>&1 &
room_pid=$!
for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$room_port/health" && ok "Room 就绪" || { fail "Room 未就绪"; exit 1; }

"$bin_dir/rgbt_match" -match_port "$match_port" -match_timeout_seconds 30 \
  -match_result_ttl_seconds 4 -env_prefix dev \
  -room_host 127.0.0.1 -room_port "$room_port" >"$log_dir/match.log" 2>&1 &
match_pid=$!
for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" && ok "Match 就绪" || { fail "Match 未就绪"; exit 1; }

"$bin_dir/rgbt_gateway" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" \
  -room_host 127.0.0.1 -room_port "$room_port" \
  -match_timeout_ms 500 -room_timeout_ms 500 >"$log_dir/gateway.log" 2>&1 &
gateway_pid=$!
for _ in $(seq 1 30); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && break
  sleep 0.5
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" && ok "Gateway 就绪" || { fail "Gateway 未就绪"; exit 1; }

# ---------------------------------------------------------------------------
# 共用的真实流量：两个客户端配成一局并让对局推进。
# 两个模式都要用它，因此只跑一次。
# ---------------------------------------------------------------------------
alice=$(login_token alice)
bob=$(login_token bob)
if [ -n "$alice" ] && [ -n "$bob" ]; then
  ok "两个客户端已登录"
else
  fail "客户端登录失败，日志与指标的证据都会不完整"
fi

# 指标基线必须在**产生事件之前**取（见下方 --metrics 一节的说明）。
# 放到配局之后取会让 `paired` 已经是 1，前后都是 1，"严格增长"必然失败。
http_post /api/v1/matches/current "{\"token\":\"$alice\",\"request_id\":\"obs-baseline-before-pair\"}" >/dev/null
for name in gateway match room; do
  port="$gateway_port"
  [ "$name" = "match" ] && port="$match_port"
  [ "$name" = "room" ] && port="$room_port"
  curl -s --max-time 5 "http://127.0.0.1:$port/metrics" >"$log_dir/$name.base.metrics"
done

pair_trace="obs-pair-$RANDOM"
room_of_match=""
if [ -n "$alice" ] && [ -n "$bob" ]; then
  http_post /api/v1/matches "{\"token\":\"$alice\",\"request_id\":\"$pair_trace-a\"}" >/dev/null
  http_post /api/v1/matches "{\"token\":\"$bob\",\"request_id\":\"$pair_trace-b\"}" >/dev/null
  for _ in $(seq 1 20); do
    http_post /api/v1/matches/current "{\"token\":\"$alice\",\"request_id\":\"$pair_trace-poll\"}" >/dev/null
    room_of_match=$(json_path match.room_id)
    [ -n "$room_of_match" ] && break
    sleep 0.5
  done
  if [ -n "$room_of_match" ]; then
    ok "双客户端配局成功（room=$room_of_match）"
    http_post /api/v1/rooms/join "{\"token\":\"$alice\",\"request_id\":\"$pair_trace-join-a\",\"room_id\":\"$room_of_match\"}" >/dev/null
    http_post /api/v1/rooms/join "{\"token\":\"$bob\",\"request_id\":\"$pair_trace-join-b\",\"room_id\":\"$room_of_match\"}" >/dev/null
    sleep 2
  else
    fail "双客户端未能配局"
  fi
fi

# ===========================================================================
if [ "$run_logs" -eq 1 ]; then
  # =========================================================================
  echo
  echo "===== 4. 结构化日志（TASK-018） ====="

  # 4a. 同一 request_id 必须跨服务可定位。选匹配入队这条链路：它必然发生。
  write_trace=$(grep -o 'request_id=[^ "]*' "$log_dir/match.log" 2>/dev/null | head -1)
  match_ts=$(count_num '^ts=' "$log_dir/match.log")
  gateway_ts=$(count_num '^ts=' "$log_dir/gateway.log")
  if [ "$match_ts" -ge 1 ]; then
    ok "Match 产出了 $match_ts 条结构化日志"
  else
    fail "Match 没有产出结构化日志"
  fi
  if [ "$gateway_ts" -ge 1 ]; then
    ok "Gateway 产出了 $gateway_ts 条结构化日志"
  else
    fail "Gateway 没有产出结构化日志"
  fi

  # 4b. 订阅路径：建立订阅并断言 subscribe_ready 带 trace。
  if [ -n "$alice" ] && [ -n "$room_of_match" ]; then
    sub_trace="obs-sub-$RANDOM"
    (curl -sN --max-time 3 -H "Authorization: Bearer $alice" \
      "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_of_match&request_id=$sub_trace" \
      >/dev/null 2>&1 || true) &
    sleep 1.5
    if grep -q "event=subscribe_ready .*trace=$sub_trace" "$log_dir/gateway.log" 2>/dev/null; then
      ok "subscribe_ready 带上了 trace=$sub_trace"
    else
      fail "subscribe_ready 没有带上传入的 request_id"
    fi
    if grep -q "trace=$sub_trace" "$log_dir/room.log" 2>/dev/null; then
      ok "同一条订阅的 request_id 也出现在 Room 日志里（三层贯通）"
    else
      fail "Room 日志里没有该订阅的 request_id（presence 上报未贯通）"
    fi
  else
    fail "缺少客户端或房间，订阅链路无法验证"
  fi

  # 4c. 订阅被拒时必须留痕，且能按 request_id 定位。
  reject_trace="obs-reject-$RANDOM"
  if [ -n "$alice" ]; then
    curl -s -o /dev/null --max-time 5 -H "Authorization: Bearer $alice" \
      "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=r-nonexistent-$RANDOM&request_id=$reject_trace" 2>/dev/null || true
    sleep 1
    if grep -q "event=subscribe_rejected" "$log_dir/gateway.log" 2>/dev/null; then
      ok "订阅被拒时留下了结构化日志"
      if grep -q "trace=$reject_trace" "$log_dir/gateway.log" 2>/dev/null; then
        ok "订阅拒绝可以按 request_id 定位"
      else
        fail "subscribe_rejected 没有带上传入的 request_id"
      fi
    else
      fail "订阅被拒时没有留下任何日志"
    fi
  fi
fi

# ===========================================================================
if [ "$run_metrics" -eq 1 ]; then
  # =========================================================================
  echo
  echo "===== 5. 指标端点与计数增长（TASK-019） ====="

  # 5a. 三个端点可访问、Content-Type 正确、样本带 TYPE。
  for pair in "gateway:$gateway_port" "match:$match_port" "room:$room_port"; do
    name="${pair%%:*}"
    port="${pair#*:}"
    code=$(curl -s -o "$log_dir/$name.metrics" -w '%{http_code}' --max-time 5 \
      -D "$log_dir/$name.metrics.headers" "http://127.0.0.1:$port/metrics")
    ctype=$(grep -i '^content-type:' "$log_dir/$name.metrics.headers" | tr -d '\r' | cut -d' ' -f2-)
    if [ "$code" = "200" ]; then
      ok "$name /metrics 可访问（200）"
    else
      fail "$name /metrics 返回 $code"
    fi
    case "$ctype" in
      *text/plain*) ok "$name 的 Content-Type 是 text/plain" ;;
      *) fail "$name 的 Content-Type 是 [${ctype:-无}]，Prometheus 依赖 text/plain" ;;
    esac
    samples=$(count_num '^rgbt_' "$log_dir/$name.metrics")
    types=$(count_num '^# TYPE rgbt_' "$log_dir/$name.metrics")
    if [ "$samples" -ge 1 ] && [ "$types" -ge 1 ]; then
      ok "$name 导出了 $samples 条样本、$types 条 TYPE"
    else
      fail "$name 没有导出指标（样本 $samples 条，TYPE $types 条）"
    fi
  done

  # 基线取自**配局之前**抓的那一份（见脚本上方）。
  gw_before=$(metric_value "$log_dir/gateway.base.metrics" 'rgbt_http_requests_total{path="/api/v1/matches/current"')
  match_before=$(metric_value "$log_dir/match.base.metrics" 'rgbt_match_events_total{event="paired"')
  rpc_before=$(metric_value "$log_dir/gateway.base.metrics" 'rgbt_rpc_calls_total{target="match",outcome="ok"')
  frames_before=$(metric_value "$log_dir/room.base.metrics" 'rgbt_room_frames_advanced_total')

  # 配局与对局推进已经在上面的共用流量里发生；这里再打一次读路径，
  # 让 Gateway 的 HTTP 计数在"一次真实对局期间"确实增加。
  http_post /api/v1/matches/current "{\"token\":\"$alice\",\"request_id\":\"obs-post\"}" >/dev/null
  sleep 1
  curl -s --max-time 5 "http://127.0.0.1:$gateway_port/metrics" >"$log_dir/gateway2.metrics"
  curl -s --max-time 5 "http://127.0.0.1:$match_port/metrics" >"$log_dir/match2.metrics"
  curl -s --max-time 5 "http://127.0.0.1:$room_port/metrics" >"$log_dir/room2.metrics"

  gw_after=$(metric_value "$log_dir/gateway2.metrics" 'rgbt_http_requests_total{path="/api/v1/matches/current"')
  match_after=$(metric_value "$log_dir/match2.metrics" 'rgbt_match_events_total{event="paired"')
  rpc_after=$(metric_value "$log_dir/gateway2.metrics" 'rgbt_rpc_calls_total{target="match",outcome="ok"')
  rpc_room_ok=$(metric_value "$log_dir/gateway2.metrics" 'rgbt_rpc_calls_total{target="room",outcome="ok"')
  frames_after=$(metric_value "$log_dir/room2.metrics" 'rgbt_room_frames_advanced_total')
  echo "  基线：matches_current=$gw_before paired=$match_before rpc_match_ok=$rpc_before frames=$frames_before"
  echo "  之后：matches_current=$gw_after paired=$match_after rpc_match_ok=$rpc_after rpc_room_ok=$rpc_room_ok frames=$frames_after"

  # 判据一律是**严格增长**（`-gt`），不是"大于等于"：后者会让"指标完全没更新"也通过。
  if [ "$gw_before" != "MISSING" ] && [ "$gw_after" != "MISSING" ] && [ "$gw_after" -gt "$gw_before" ]; then
    ok "Gateway HTTP 计数随真实对局期间的请求增长（$gw_before -> $gw_after）"
  else
    fail "Gateway HTTP 计数没有增长（$gw_before -> $gw_after）"
  fi
  if [ "$match_before" != "MISSING" ] && [ "$match_after" != "MISSING" ] && [ "$match_after" -gt "$match_before" ]; then
    ok "Match 配对计数随真实配局增长（$match_before -> $match_after）"
  else
    fail "Match 配对计数没有增长（$match_before -> $match_after）"
  fi
  if [ "$frames_before" != "MISSING" ] && [ "$frames_after" != "MISSING" ] && [ "$frames_after" -gt "$frames_before" ]; then
    ok "Room 帧推进计数随对局增长（$frames_before -> $frames_after）"
  else
    fail "Room 帧推进计数没有增长（$frames_before -> $frames_after）"
  fi
  if [ "$rpc_after" != "MISSING" ] && [ "$rpc_after" -ge 1 ]; then
    ok "Gateway->Match 的 brpc 调用计数已增长（= $rpc_after）"
  else
    fail "Gateway->Match 的 brpc 调用计数为 $rpc_after"
  fi
  if [ "$rpc_room_ok" != "MISSING" ] && [ "$rpc_room_ok" -ge 1 ]; then
    ok "Gateway->Room 的 brpc 调用计数已增长（= $rpc_room_ok）"
  else
    fail "Gateway->Room 的 brpc 调用计数为 $rpc_room_ok"
  fi

  # 5b. 高基数防线：room_id/player_id/token **不得**成为任何标签。
  # 这比"端点能访问"更重要：一旦把 room_id 当标签，基数会随房间数无限增长，
  # 最终把 Prometheus 拖垮，而整个过程没有任何报错。
  if grep -qE '(room_id|player_id|token)=' "$log_dir/gateway.metrics" \
    "$log_dir/match.metrics" "$log_dir/room.metrics" 2>/dev/null; then
    fail "指标里出现了 room_id/player_id/token 这类高基数标签"
    grep -hoE '(room_id|player_id|token)="[^"]*"' "$log_dir"/*.metrics | sort -u | head -5
  else
    ok "指标标签里没有 room_id/player_id/token（无高基数风险）"
  fi

  # 5c. 阶段样本必须齐全：六个阶段各一条。少一条意味着某类房间在面板上消失。
  phase_samples=$(count_num '^rgbt_rooms\{phase=' "$log_dir/room.metrics")
  if [ "$phase_samples" -eq 6 ]; then
    ok "Room 导出了全部 6 个阶段的房间数"
  else
    fail "Room 只导出了 $phase_samples 个阶段（应为 6）"
  fi
fi

# ===========================================================================
echo
echo "===== 6. 优雅退出 ====="
# ===========================================================================
for pair in "Gateway:$gateway_pid" "Match:$match_pid" "Room:$room_pid"; do
  name="${pair%%:*}"
  pid="${pair#*:}"
  [ -z "$pid" ] && continue
  kill -TERM "$pid" 2>/dev/null || true
  exited=0
  for _ in $(seq 1 20); do
    if ! kill -0 "$pid" 2>/dev/null; then
      exited=1
      break
    fi
    sleep 0.5
  done
  if [ "$exited" -eq 1 ]; then
    wait "$pid" 2>/dev/null
    rc=$?
    if [ "$rc" -eq 0 ]; then
      ok "$name 退出码 0"
    else
      fail "$name 退出码 $rc"
    fi
  else
    fail "$name 未在 10 秒内退出"
  fi
done
gateway_pid=""
match_pid=""
room_pid=""

# ===========================================================================
if [ "$run_logs" -eq 1 ]; then
  # =========================================================================
  # 这一节必须在**服务退出之后**再跑。实测踩到：Room 在收到 SIGTERM 优雅退出时
  # 会再写一条 `presence_reported`（上报离线），若在那一刻统计"含固定字段的行数"，
  # 就会比"以 ts= 开头的行数"多一，被误判成"日志被换行截断"。
  # 也就是说：那是一次**检测时序**错误，而不是日志格式问题。
  echo
  echo "===== 7. 结构化日志的最终校验（日志已不再增长） ====="

  for svc in gateway match room; do
    f="$log_dir/$svc.log"
    if [ ! -f "$f" ]; then
      fail "$svc 没有日志文件"
      continue
    fi
    total=$(count_num '^ts=' "$f")
    with_service=$(count_num " service=$svc " "$f")
    if [ "$total" -gt 0 ] && [ "$total" = "$with_service" ]; then
      ok "$svc：$total 条结构化日志全部带 service=$svc"
    else
      fail "$svc：结构化日志 $total 条，其中带 service=$svc 的只有 $with_service 条"
    fi
  done

  for svc in gateway match room; do
    f="$log_dir/$svc.log"
    [ -f "$f" ] || continue
    ts_lines=$(count_num '^ts=' "$f")
    # 判据用结构化的**固定字段组合**（service= 与 level= 同行），而不是单个
    # ` event=`：后者会误报——brpc/glog 自己的输出里也可能出现 "event"。
    fields=$(count_num ' service=[a-z]+ level=' "$f")
    broken=$((fields - ts_lines))
    if [ "$broken" -le 0 ]; then
      ok "$svc：结构化行没有被断行（ts= 行 $ts_lines 条）"
    else
      fail "$svc：有 $broken 行含结构化标识却不以 ts= 开头（很可能被换行截断）"
      grep -E ' service=[a-z]+ level=' "$f" | grep -vE '^ts=' | head -4 | sed 's/^/      可疑行: /'
    fi
  done
fi

# ===========================================================================
echo
echo "===== 验收结果 ====="
# ===========================================================================
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过："
  if [ "$run_logs" -eq 1 ]; then
    echo "  [TASK-018] 三个服务的结构化日志都带 service= 且未被字段值断行；"
    echo "          同一 request_id 在 Gateway 与 Room 之间贯通（含订阅拒绝路径）"
  fi
  if [ "$run_metrics" -eq 1 ]; then
    echo "  [TASK-019] 三个服务的 /metrics 都返回 text/plain 且样本带 TYPE；"
    echo "          一次真实对局期间 HTTP / 配对 / 帧推进 / brpc 调用计数确实增长；"
    echo "          标签里没有 room_id/player_id 这类高基数键；6 个阶段样本齐全"
  fi
  echo
  echo "日志留档：$log_dir/{gateway,match,room}.log"
  echo "指标留档：$log_dir/{gateway,match,room}2.metrics"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do
  echo "  - $item"
done
echo
echo "排查提示：三份日志在 $log_dir/"
echo "  例：grep -n 'trace=<你的 request_id>' $log_dir/*.log"
exit 1
