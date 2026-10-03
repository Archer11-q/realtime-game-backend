#!/usr/bin/env bash
#
# verify-trace.sh - TASK-021 链路追踪（trace id 贯通）的验收入口。
#
# 为什么要单独一个脚本而不是塞进 verify-observability.sh：TASK-018 的日志一节验的是
# "结构化日志的**格式**与 service 字段"，TASK-019/020 的指标一节验的是"指标端点与
# 抓取"。本脚本验的是第三件事——**同一个 id 能不能把一次请求在三个服务里的环节
# 串成一条有序的链**。三者的失败原因完全不同，混在一起会让"哪一层坏了"更难判断。
#
#   bash scripts/verify-trace.sh            # 完整（启动依赖 + 三个服务）
#   bash scripts/verify-trace.sh --keep     # 保留进程，便于手工翻日志
#   bash scripts/verify-trace.sh --no-docker
#
# 结构约定（沿袭 verify-observability.sh）：每一节都是"开一个条件、在几行内闭合"。
# 坏掉的门禁比没有门禁更危险，因为它给出虚假的把握（TASK-019 期间踩过一次）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"

keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
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

log_dir="/tmp/trace-logs"
rm -rf "$log_dir"
mkdir -p "$log_dir"

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

http_post() {
  local body="${2:-}"
  [ -n "$body" ] || body='{}'
  curl -s -o /tmp/trace-resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}

json_path() {
  python3 - "$1" <<'PY'
import json, sys
try:
    with open('/tmp/trace-resp.json') as handle:
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

# 结构化日志行 -> `ts=...|service=...|event=...|key=value|...`
#
# 为什么必须在这里正确解析引号：logging.hpp 的转义规则是"值里含空格/引号/等号时
# 用双引号包起来"。用 `grep -o 'trace=[^ ]*'` 这类朴素切分会在遇到带空格的值时
# 把字段读错，而"读错字段"与"字段不存在"在断言里长得一样——都会变成假失败。
parse_logs() {
  local trace="$1"
  python3 - "$log_dir" "$trace" <<'PY'
import glob, os, sys

log_dir, trace = sys.argv[1], sys.argv[2]
needle = 'trace=' + trace

def parse_line(line):
    fields, i, n = [], 0, len(line)
    while i < n:
        while i < n and line[i] == ' ':
            i += 1
        start = i
        while i < n and line[i] not in (' ', '='):
            i += 1
        if i >= n or line[i] != '=':
            while i < n and line[i] != ' ':
                i += 1
            continue
        key = line[start:i]
        i += 1  # '='
        if i < n and line[i] == '"':
            i += 1
            value_chars = []
            while i < n:
                if line[i] == '\\' and i + 1 < n:
                    value_chars.append(line[i + 1])
                    i += 2
                    continue
                if line[i] == '"':
                    i += 1
                    break
                value_chars.append(line[i])
                i += 1
            value = ''.join(value_chars)
        else:
            value_start = i
            while i < n and line[i] != ' ':
                i += 1
            value = line[value_start:i]
        fields.append((key, value))
    return fields

rows = []
for path in sorted(glob.glob(os.path.join(log_dir, '*.log'))):
    svc = os.path.basename(path)[:-4]
    with open(path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            line = line.rstrip('\n')
            if not line.startswith('ts=') or needle not in line:
                continue
            fields = parse_line(line)
            kv = dict(fields)
            if kv.get('trace') != trace:
                continue
            rows.append((kv.get('ts', ''), svc, kv.get('event', ''), kv.get('level', ''),
                         fields))

rows.sort(key=lambda r: (r[0], r[1]))
for ts, svc, event, level, fields in rows:
    rest = '|'.join('%s=%s' % (k, v) for k, v in fields
                    if k not in ('ts', 'service', 'level', 'event', 'trace'))
    print('%s|%s|%s|%s|%s' % (ts, svc, event, level, rest))
PY
}

# 该 trace 是否在某个服务里出现过某个事件。
has_event() {
  local trace="$1" svc="$2" event="$3"
  local rows
  rows=$(parse_logs "$trace")
  printf '%s\n' "$rows" | awk -F'|' -v s="$svc" -v e="$event" '$2 == s && $3 == e { found = 1 } END { exit !found }'
}

event_count() {
  local trace="$1" svc="$2" event="$3"
  local rows
  rows=$(parse_logs "$trace")
  printf '%s\n' "$rows" | awk -F'|' -v s="$svc" -v e="$event" '$2 == s && $3 == e { n++ } END { print n + 0 }'
}

# 在某个服务里，该 trace 的 ts 是否单调不减（同一毫秒允许相等）。
#
# 这是"调用序"的唯一机器可判据。**不做跨服务的 ts 排序断言**：三个进程各自取
# 系统时钟，毫秒级先后可能受调度影响而乱序，那不是缺陷。真正能证明顺序的是
# 调用方与被调用方之间的因果关系（A 发出请求 -> B 收到），而它在日志里的体现
# 就是"两边的 trace 相同"——由上面的 has_event 覆盖。
timestamps_monotonic() {
  local trace="$1" svc="$2"
  local rows
  rows=$(parse_logs "$trace")
  printf '%s\n' "$rows" | awk -F'|' -v s="$svc" '
    $2 == s {
      if (prev != "" && $1 < prev) { bad = 1 }
      prev = $1
    }
    END { exit bad ? 1 : 0 }'
}

require_cross_service() {
  local trace="$1" description="$2"
  shift 2
  local missing=()
  while [ "$#" -ge 2 ]; do
    local svc="$1" event="$2"
    shift 2
    if ! has_event "$trace" "$svc" "$event"; then
      missing+=("$svc/$event")
    fi
  done
  if [ "${#missing[@]}" -eq 0 ]; then
    ok "$description（trace=$trace）"
  else
    fail "$description 缺少环节：${missing[*]}（trace=$trace）"
  fi
}

check_monotonic() {
  local trace="$1"
  for svc in gateway match room; do
    if timestamps_monotonic "$trace" "$svc"; then
      ok "trace=$trace 在 $svc 内的时序单调不减"
    else
      fail "trace=$trace 在 $svc 内的时序出现回退"
      parse_logs "$trace" | awk -F'|' -v s="$svc" '$2 == s' | sed 's/^/      /'
    fi
  done
}

count_num() {
  local n
  n=$(grep -E -c "$1" "$2" 2>/dev/null)
  case "$n" in '' | *[!0-9]*) n=0 ;; esac
  printf '%s' "$n"
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
  docker compose -f "$compose_file" up -d >/tmp/trace-compose.log 2>&1
  if [ $? -eq 0 ]; then
    ok "compose up 成功"
  else
    fail "compose up 失败"
    tail -20 /tmp/trace-compose.log
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
echo "===== 2. 应用迁移并清理上一轮会话 ====="
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

# 清掉上一轮的 dev:* Key（会话 + 匹配队列快照）。
#
# 为什么必须清：Match 会把「已配对但客户端还没领取」的结果写进 Redis 快照并在启动时
# 恢复（TASK-015）。上一轮留下的那条 matched 记录指向**上一轮的 room_id**，而那个
# 房间在 Room 侧是从快照恢复出来的、双方从未真正加入过，于是本次运行会拿到一个
# 永远不会推进到 finished 的房间——表现为"对局在 40 次攻击内没结束"，与实际缺陷
# 毫无关系。实测踩到：房间号 r-KeqHSWXC5SN8MAAPkvdUZLQI 在两次运行里完全相同。
docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null |
  while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
left=$(docker exec rgbt-redis redis-cli --scan --pattern 'dev:*' 2>/dev/null | wc -l)
if [ "${left:-0}" -eq 0 ]; then
  ok "已清理上一轮的 dev:* Key（会话与匹配队列快照）"
else
  fail "仍有 $left 个 dev:* Key 未清理，本轮结论可能被上一轮状态污染"
fi

# 清掉上一轮留下的房间快照（MySQL `rooms` 表）。
#
# 为什么也必须清：Room 启动时会把未结束的房间从最近快照恢复回内存（TASK-014），
# 而那些房间的 `p1_joined/p2_joined` 都已经是 true。上一轮被中断的一局因此在
# **对局没结束（state=playing）**的情况下"复活"，并且：
#   * 它的 room_id 与 match_id 依然有效，于是新会话的玩家会被 Match 分配给这间房；
#   * 它的双方**从未真正加入过本次连接**，对局既不推进也不判负。
# 结果是脚本在 40 次攻击后报"对局未在预期时间内结束"，而真实原因与本任务的
# trace 贯通毫无关系。实测证据：两次运行的房间号完全相同
# （r--eaSBuEGbOMw97V8AqjPlGA2），Room 启动日志里能看到 `room_restored ... phase=playing`
# 而**没有**本次运行的 `room_created`。
#
# 这是**开发环境**的清理（`rooms` 只是可丢弃的快照，见 TASK-013 决策 3），
# 与清 Redis 的 dev:* 是同一件事；不涉及任何生产语义。
if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
  -e "DELETE FROM rooms;" >/dev/null 2>&1; then
  ok "已清理上一轮的房间快照（rooms 表）"
else
  fail "清理 rooms 表失败，本轮可能复用到上一轮的房间"
fi

# ===========================================================================
echo "===== 3. 构建并启动服务 ====="
# ===========================================================================
cmake --preset "$preset" >/tmp/trace-cmake.log 2>&1 &&
  ok "配置成功" || { fail "配置失败"; tail -20 /tmp/trace-cmake.log; }
cmake --build --preset "$preset" >/tmp/trace-build.log 2>&1
if [ $? -eq 0 ]; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 /tmp/trace-build.log
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
  -match_result_ttl_seconds 60 -env_prefix dev \
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

# ===========================================================================
echo
echo "===== 4. 五条关键路径的 trace 贯通 ====="
# ===========================================================================

login_trace="vtr-login-$RANDOM"
logout_trace="vtr-logout-$RANDOM"

# --- 4a. 登录 -----------------------------------------------------------------
http_post /api/v1/login \
  "{\"account\":\"alice\",\"password\":\"alice_dev_pw\",\"request_id\":\"$login_trace\",\"client_type\":\"web\"}" >/dev/null
alice=$(json_path token)
http_post /api/v1/login \
  "{\"account\":\"bob\",\"password\":\"bob_dev_pw\",\"request_id\":\"vtr-login-bob-$RANDOM\",\"client_type\":\"web\"}" >/dev/null
bob=$(json_path token)

if [ -n "$alice" ] && [ -n "$bob" ]; then
  ok "两个客户端已登录"
else
  fail "客户端登录失败，后面的链路都没有意义"
fi

require_cross_service "$login_trace" "登录：Gateway 留下一条可按 trace 检索的记录" \
  gateway request_done
# op 必须与路径对应：登录链路记成 login，而不是 unknown。
if parse_logs "$login_trace" | grep -q '|op=login|'; then
  ok "登录的 request_done 带 op=login"
else
  fail "登录的 request_done 缺少 op=login"
  parse_logs "$login_trace" | sed 's/^/      /'
fi
check_monotonic "$login_trace"

# --- 4b. 进入匹配（Gateway -> Match -> Room，跨三个服务） ----------------------
# 关键点：`room_created` 的 trace 必须与 alice 的入队请求**同一个**。
# 这正是本任务修掉的缺陷——此前 BrpcRoomAllocator 传的是 match_id 那个伪 id，
# 于是 Room 侧记下的 trace 在 Gateway/Match 的日志里根本查不到。
match_trace="vtr-match-$RANDOM"
room_of_match=""
if [ -n "$alice" ] && [ -n "$bob" ]; then
  http_post /api/v1/matches "{\"token\":\"$alice\",\"request_id\":\"$match_trace\"}" >/dev/null
  # bob 入队即触发配对；配对用**队首玩家（alice）**的 request_id 作为这一局的 trace。
  http_post /api/v1/matches "{\"token\":\"$bob\",\"request_id\":\"$match_trace-b\"}" >/dev/null
  for _ in $(seq 1 20); do
    http_post /api/v1/matches/current "{\"token\":\"$alice\",\"request_id\":\"$match_trace-poll\"}" >/dev/null
    room_of_match=$(json_path match.room_id)
    [ -n "$room_of_match" ] && break
    sleep 0.5
  done
  if [ -n "$room_of_match" ]; then
    ok "双客户端配局成功（room=$room_of_match）"
  else
    fail "双客户端未能配局"
  fi
fi

require_cross_service "$match_trace" "匹配：Gateway/Match/Room 三个服务都在同一条链上" \
  gateway match_enqueue_ok \
  match match_enqueued \
  room room_created
check_monotonic "$match_trace"

# --- 4c. 创建/加入房间 ---------------------------------------------------------
join_trace="vtr-join-$RANDOM"
if [ -n "$alice" ] && [ -n "$room_of_match" ]; then
  http_post /api/v1/rooms/join \
    "{\"token\":\"$alice\",\"request_id\":\"$join_trace\",\"room_id\":\"$room_of_match\"}" >/dev/null
  http_post /api/v1/rooms/join \
    "{\"token\":\"$bob\",\"request_id\":\"$join_trace-b\",\"room_id\":\"$room_of_match\"}" >/dev/null
  sleep 1
  require_cross_service "$join_trace" "加入房间：Gateway 与 Room 都在同一条链上" \
    gateway request_done \
    room room_joined
  check_monotonic "$join_trace"

  # 双方加入后对局才开始推进。这里显式确认一次，避免后面的"结算"因为
  # 房间根本没进入 playing 而失败——那种失败与 trace 完全无关，却会污染结论。
  join_state=""
  for _ in $(seq 1 20); do
    http_post "/api/v1/rooms/state?room_id=$room_of_match" \
      "{\"token\":\"$alice\",\"request_id\":\"vtr-state-$RANDOM\"}" >/dev/null
    join_state=$(json_path room.state)
    [ "$join_state" = "playing" ] && break
    sleep 0.5
  done
  if [ "$join_state" = "playing" ]; then
    ok "双方加入后对局进入 playing"
  else
    fail "对局未进入 playing（实际 state=[$join_state]）"
  fi
else
  fail "缺少客户端或房间，加入房间链路无法验证"
fi

# --- 4d. 断线重连（断线 -> 保持断线 -> 带 Last-Event-ID 重连） -----------------
# 订阅是长连接，它的 request_id 由 query string 传入（GET 没有请求体，而 brpc
# 不会把 query 映射进 protobuf 字段——这是 TASK-018 踩过的坑）。
#
# 这里必须**显式**覆盖断线与重连两步，而不是"起一条订阅就完事"：
# TASK-016 的语义是"只要有一方处于断线状态，对局暂停推进"。若 4d 结束后不把它
# 接回来，后面的结算会永远打不完——这不是缺陷，是设计。实测踩到过：房间的
# 帧号与血量在 alice 断开后的十几秒里一动不动（frame 恒为 163），
# 攻击全部返回 200 却不产生任何伤害。
sub_trace="vtr-sub-$RANDOM"
reconnect_trace="vtr-reconnect-$RANDOM"
sub_pid=""
if [ -n "$alice" ] && [ -n "$room_of_match" ]; then
  # 一次性订阅也会带 request_id：这正是"谁在什么时候连上/断开"要能查到的原因。
  curl -sN --max-time 4 -H "Authorization: Bearer $alice" \
    "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_of_match&request_id=$sub_trace" \
    >/dev/null 2>&1 &
  sub_pid=$!
  sleep 2.5
  require_cross_service "$sub_trace" "断线重连：订阅建立与 presence 上报同一条链" \
    gateway subscribe_ready \
    room presence_reported
  check_monotonic "$sub_trace"
  # 等它真的断开（Gateway 在写失败时上报 offline，Room 侧因此记为断线）。
  wait "$sub_pid" 2>/dev/null
  sub_pid=""
  sleep 1.5
else
  fail "缺少客户端或房间，订阅链路无法验证"
fi

# --- 4e. 对局结算（重连 -> 推进到结束 -> 结果落库 -> 查询） ---------------------
# 这一步必须真的打完一局：只查一次结果证明不了"结算链路"，因为结果可能还没落库。
# 每次攻击扣对手 10、满血 100，因此打满 10 次即一方归零（数值见 room_types.hpp）。
#
# 三件事必须同时成立，否则"打不完"的失败原因会与 trace 完全无关：
#   1. **断线的一方要真的重连回来**。只要有一方 offline，对局就暂停推进
#      （TASK-016），攻击会照常返回 200 但一点血都不掉。
#   2. `room_id` 走**查询参数**（`?room_id=`）。查询接口没有请求体，而 brpc 的
#      restful 映射不支持 `{name}` 路径参数。把它放进请求体会得到
#      `room_id_required`，循环空跑到底。
#   3. 双方在整个结算期间保持订阅。
state_trace="vtr-state"
room_state_of() {
  http_post "/api/v1/rooms/state?room_id=$room_of_match" \
    "{\"token\":\"$1\",\"request_id\":\"$state_trace-$RANDOM\"}" >/dev/null
  json_path room.state
}
online_count_of_room() {
  room_state_of "$alice" >/dev/null
  python3 - /tmp/trace-resp.json <<'PY'
import json, sys
try:
    room = json.load(open(sys.argv[1])).get('room') or {}
except Exception:
    room = {}
print(sum(1 for p in room.get('players', []) if p.get('online')))
PY
}

settle_pids=()
keep_subscription() {
  local token="$1" tag="$2" last_event_id="${3:-}"
  if [ -n "$last_event_id" ]; then
    curl -sN --max-time 90 -H "Authorization: Bearer $token" \
      -H "Last-Event-ID: $last_event_id" \
      "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_of_match&request_id=$tag" \
      >/dev/null 2>&1 &
  else
    curl -sN --max-time 90 -H "Authorization: Bearer $token" \
      "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=$room_of_match&request_id=$tag" \
      >/dev/null 2>&1 &
  fi
  settle_pids+=("$!")
}

settle_trace="vtr-result-$RANDOM"
finished=0
finish_reason=""
reconnected=""
if [ -n "$alice" ] && [ -n "$bob" ] && [ -n "$room_of_match" ]; then
  # 重连 alice：头里带一个**落后于当前帧号**的 Last-Event-ID，于是这次订阅
  # 既补发了错过的帧、又把它自己重新标记为在线（两者都是"重连"该有的行为）。
  # 帧号从当前状态里取，取到就减 3 帧，保证落在补发窗口内。
  room_state_of "$alice" >/dev/null
  current_frame=$(json_path room.frame)
  since_frame=""
  case "$current_frame" in
    '' | *[!0-9]*) since_frame="" ;;
    *)
      if [ "$current_frame" -gt 3 ]; then
        since_frame=$((current_frame - 3))
      else
        since_frame=0
      fi
      ;;
  esac
  keep_subscription "$alice" "$reconnect_trace" "$since_frame"
  keep_subscription "$bob" "vtr-play-b-$RANDOM"

  for _ in $(seq 1 60); do
    online_count=$(online_count_of_room)
    [ "$online_count" = "2" ] && break
    sleep 0.25
  done
  if [ "$online_count" = "2" ]; then
    if [ -n "$since_frame" ]; then
      ok "双方在线且 alice 已带 Last-Event-ID=$since_frame 重连"
    else
      ok "双方在线（当前帧号不适用，按首次订阅重连）"
    fi
    reconnected=1
    # 重连这条路径本身也必须能按 trace 取出来：`subscribe_ready` 在 Gateway、
    # `presence_reported`（online=true）在 Room。
    require_cross_service "$reconnect_trace" "重连：订阅建立与上线上报同一条链" \
      gateway subscribe_ready \
      room presence_reported
    check_monotonic "$reconnect_trace"
  else
    fail "只有 ${online_count} 方在线，对局会被暂停（结算链路无法验证）"
  fi

  for _ in $(seq 1 60); do
    [ "$reconnected" = "1" ] || break
    http_post /api/v1/rooms/input \
      "{\"token\":\"$alice\",\"request_id\":\"$settle_trace-a\",\"room_id\":\"$room_of_match\"}" >/dev/null
    state=$(room_state_of "$alice")
    [ "$state" = "finished" ] && { finished=1; finish_reason=$(json_path room.finish_reason); break; }
    http_post /api/v1/rooms/input \
      "{\"token\":\"$bob\",\"request_id\":\"$settle_trace-b\",\"room_id\":\"$room_of_match\"}" >/dev/null
    state=$(room_state_of "$alice")
    [ "$state" = "finished" ] && { finished=1; finish_reason=$(json_path room.finish_reason); break; }
    sleep 0.2
  done

  for pid in "${settle_pids[@]}"; do
    kill "$pid" 2>/dev/null
  done
  wait "${settle_pids[@]}" 2>/dev/null
fi

if [ "$finished" -eq 1 ]; then
  ok "对局已结束（finish_reason=$finish_reason）"
else
  fail "对局未在预期时间内结束，结算链路无法验证"
fi

if [ "$finished" -eq 1 ]; then
  # 拿这一局的 match_id：结果查询以它为键（而不是 room_id）。
  http_post /api/v1/matches/current "{\"token\":\"$alice\",\"request_id\":\"vtr-whoami-$RANDOM\"}" >/dev/null
  result_match_id=$(json_path match.match_id)

  # 结果写入由 Room 在结束时**同步**完成；这里查一次结果，用查询自己的 trace
  # 证明"结算是可检索的"，并顺便验证结果真的落库了（而不是只在内存里）。
  result_trace="$settle_trace-verify"
  result_winner=""
  if [ -n "$result_match_id" ]; then
    http_post /api/v1/results \
      "{\"token\":\"$alice\",\"request_id\":\"$result_trace\",\"match_id\":\"$result_match_id\"}" >/dev/null
    result_winner=$(json_path result.winner_id)
  fi
  require_cross_service "$result_trace" "结算：Gateway 的查询与 Room 的结果读取在同一条 trace 上" \
    gateway request_done \
    room match_result_queried
  check_monotonic "$result_trace"

  if [ -n "$(json_path result.match_id)" ]; then
    ok "对局结果已落库并可查询（match=$result_match_id winner=${result_winner:-平局}）"
  else
    fail "对局结果查不到（对局已结束，说明落库链路有问题或仍在重试）"
  fi
fi

# --- 4f. 登出 -----------------------------------------------------------------
if [ -n "$alice" ]; then
  http_post /api/v1/logout "{\"token\":\"$alice\",\"request_id\":\"$logout_trace\"}" >/dev/null
  require_cross_service "$logout_trace" "登出：同一个 trace 可检索" gateway request_done
fi

# ===========================================================================
echo
echo "===== 5. trace 的完整性与不变量 ====="
# ===========================================================================

# 5a. 每个 HTTP 请求都留一条 request_done：抽一条已知会被拒绝的请求，
#     确认**失败路径**同样留痕（此前这类路径在 Gateway 侧一条记录都没有）。
reject_trace="vtr-reject-$RANDOM"
curl -s -o /dev/null --max-time 5 \
  "http://127.0.0.1:$gateway_port/api/v1/stream?room_id=r-nonexistent-$RANDOM&request_id=$reject_trace" \
  2>/dev/null || true
sleep 1
if has_event "$reject_trace" gateway request_done; then
  ok "被拒绝的请求同样留下 request_done（trace=$reject_trace）"
else
  fail "被拒绝的请求没有留下 request_done（trace=$reject_trace）"
fi

# 5b. trace 不能是编造的：请求里没带 request_id 时，日志里**不该**出现 trace=。
#     做法：先记下日志行数，再打一个不带 request_id 的业务请求，检查新增行里
#     有没有 `event=request_done trace=`。
before_lines=$(wc -l <"$log_dir/gateway.log")
http_post /api/v1/matches/current '{"token":"'"$bob"'"}' >/dev/null
sleep 1
new_lines=$(tail -n "+$((before_lines + 1))" "$log_dir/gateway.log" 2>/dev/null | grep -c 'event=request_done' || true)
fabricated=$(tail -n "+$((before_lines + 1))" "$log_dir/gateway.log" 2>/dev/null | grep -c 'event=request_done trace=' || true)
if [ "${new_lines:-0}" -ge 1 ] && [ "${fabricated:-0}" -eq 0 ]; then
  ok "未带 request_id 的请求留下了 request_done，且**没有编造** trace（$new_lines 条）"
else
  fail "未带 request_id 的请求行为不符（request_done=$new_lines，含 trace 的=$fabricated）"
fi

# 5c. 结构化行没有被字段值断行（trace 里带引号/空格时会暴露转义问题）。
for svc in gateway match room; do
  f="$log_dir/$svc.log"
  [ -f "$f" ] || continue
  total=$(count_num '^ts=' "$f")
  fields=$(count_num ' service=[a-z]+ level=' "$f")
  if [ "$fields" -le "$total" ]; then
    ok "$svc：$total 条结构化行，没有被换行截断"
  else
    fail "$svc：有 $((fields - total)) 行含结构化标识却不以 ts= 开头"
  fi
done

# ===========================================================================
echo
echo "===== 6. 留档 ====="
# ===========================================================================
echo "日志留档：$log_dir/{gateway,match,room}.log"
echo "按 trace 取一条完整调用序："
echo "  bash scripts/verify-trace.sh --keep   # 保留进程后再执行："
echo "  grep 'trace=<你的 request_id>' $log_dir/*.log"

# ===========================================================================
echo
echo "===== 验收结果 ====="
# ===========================================================================
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过："
  echo "  [TASK-021] 登录 / 进入匹配 / 加入房间 / 断线重连 / 对局结算 五条关键路径"
  echo "          都能用同一个 trace id 在 Gateway、Match、Room 三个服务的结构化"
  echo "          日志里取出完整且时序单调的调用序；"
  echo "          Gateway 的每个 HTTP 响应（含失败路径）都留下 request_done；"
  echo "          未带 request_id 时不编造 trace"
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
