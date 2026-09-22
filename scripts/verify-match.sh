#!/usr/bin/env bash
#
# verify-match.sh - TASK-007 匹配切片验收入口
#
# 覆盖 TASK-007 的验收标准：
#   1. 两个不同账号入队后被配成同一局，match_id 与 room_id 两侧一致
#   2. 第三个玩家不会被并入一个已配满的局
#   3. 取消后不再被配对；重复取消不报错
#   4. 排队超时后状态变为 timeout，与 idle 区分
#   5. Match 不可用时 Gateway 返回 503，恢复后无需重启 Gateway 即可匹配
#   6. 两个进程收到 SIGTERM 后都能优雅退出
#
# 用法：
#   bash scripts/verify-match.sh                # 完整验收（含启停依赖）
#   bash scripts/verify-match.sh --no-docker    # 复用已启动的 Redis/MySQL
#   bash scripts/verify-match.sh --keep         # 结束后保留进程
#
# 前置：Docker Desktop 已启动；deploy/compose/.env 存在（缺失时由本脚本生成）。
#
# 为什么需要 MySQL：登录仍然要读 players 表（ADR-0002）。匹配切片本身不碰 MySQL，
# 但入队必须先登录拿到 Token，因此这里一并把 Redis 与 MySQL 起起来。
#
# 为了让超时路径可以在几秒内验证，本脚本给 Match 传了很短的超时与结果保留时长
# （-match_timeout_seconds 2 -match_result_ttl_seconds 5）。这是**测试配置**，
# 不是默认值；默认值见 src/match/match_main.cpp。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
preset="brpc-debug"
gateway_binary="build/$preset/bin/rgbt_gateway"
match_binary="build/$preset/bin/rgbt_match"

# 端口。候选列表都以文档中的约定值为首选；被占用时依次尝试其它端口。
# 必须显式预检：端口冲突时进程启动失败，而请求会打到占用端口的那个服务上，
# 表现为难以定位的 404，而不是「端口冲突」这种可定位的错误。
gateway_port=""
match_port=""
gateway_candidates=(8080 18080 18081 18090)
match_candidates=(8082 18092 18093 18094)

match_timeout_seconds=2
match_result_ttl_seconds=5
keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

failures=()
fail() { failures+=("$1"); echo "x  $1"; }
ok() { echo "v  $1"; }
compose() { docker compose -f "$compose_file" "$@"; }

gateway_pid=""
match_pid=""
cleanup() {
  for pid in "$gateway_pid" "$match_pid"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      if [ "$keep_running" -eq 1 ]; then
        echo "已保留进程 $pid（--keep）"
      else
        kill "$pid" 2>/dev/null || true
      fi
    fi
  done
}
trap cleanup EXIT

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="
if [ ! -f "$env_file" ]; then
  if [ -f "$env_example" ]; then
    cp "$env_example" "$env_file"
    ok "已从 .env.example 生成 $env_file"
  else
    echo "缺少 $env_file 与 $env_example" >&2
    exit 1
  fi
else
  ok "$env_file 已存在"
fi

# 必须先加载 .env 再读取 MYSQL_*，否则未绑定变量会直接让脚本以退出码 1 中止。
set -a
. "./$env_file"
set +a
ok "配置已加载（库: ${MYSQL_DATABASE} 账号: ${MYSQL_USER}）"

port_in_use() {
  ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"
}
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

gateway_port="$(pick_port "${gateway_candidates[@]}")" || {
  echo "Gateway 候选端口均被占用：${gateway_candidates[*]}" >&2
  exit 1
}
ok "Gateway 选用空闲端口 $gateway_port"

# Match 端口必须与 Gateway 不同，因此把已选中的端口从候选中剔除。
match_pool=()
for candidate in "${match_candidates[@]}"; do
  [ "$candidate" = "$gateway_port" ] && continue
  match_pool+=("$candidate")
done
match_port="$(pick_port "${match_pool[@]}")" || {
  echo "Match 候选端口均被占用：${match_pool[*]}" >&2
  exit 1
}
ok "Match 选用空闲端口 $match_port"
echo

# ---------- 1. 依赖 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis 与 MySQL ====="
  if ! docker info >/dev/null 2>&1; then
    echo "Docker 服务端不可达，请先启动 Docker Desktop。" >&2
    exit 1
  fi
  for service in redis mysql; do
    if compose up -d "$service" >/dev/null 2>&1; then
      ok "compose 启动 $service 成功"
    else
      fail "compose 启动 $service 失败"
    fi
  done

  for pair in "redis:rgbt-redis" "mysql:rgbt-mysql"; do
    service="${pair%%:*}"
    container="${pair#*:}"
    healthy=0
    for _ in $(seq 1 45); do
      state="$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)"
      if [ "$state" = "healthy" ]; then
        healthy=1
        break
      fi
      sleep 2
    done
    if [ "$healthy" -eq 1 ]; then
      ok "$container 健康"
    else
      fail "$container 未达到 healthy（service=$service）"
    fi
  done

  # 匹配切片不写库，但登录要读 players，因此迁移必须已应用。
  applied=0
  for file in migrations/*.sql; do
    [ -e "$file" ] || continue
    if docker exec -i rgbt-mysql mysql -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      <"$file" >/dev/null 2>&1; then
      applied=$((applied + 1))
    fi
  done
  if [ "$applied" -gt 0 ]; then
    ok "已应用 $applied 个迁移脚本（幂等，可重复执行）"
  else
    fail "迁移脚本未成功应用"
  fi

  # 清理上一次运行留下的会话与幂等映射，保证结果可复现。
  stale=$(docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null | wc -l)
  docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null |
    while read -r key; do docker exec rgbt-redis redis-cli DEL "$key" >/dev/null 2>&1; done
  ok "已清理上次运行残留的 $stale 个 dev:gateway:* Key"
  echo
else
  echo "===== 1. 跳过依赖启停（--no-docker） ====="
  echo
fi

# ---------- 2. 构建 ----------
echo "===== 2. 构建 ====="
if cmake --preset "$preset" >/tmp/match-cmake.log 2>&1; then
  ok "配置成功"
else
  fail "配置失败"
  tail -20 /tmp/match-cmake.log
fi
if cmake --build --preset "$preset" >/tmp/match-build.log 2>&1; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 /tmp/match-build.log
  exit 1
fi
for bin in "$gateway_binary" "$match_binary"; do
  if [ -x "$bin" ]; then
    ok "可执行文件存在: $bin"
  else
    fail "缺少可执行文件: $bin"
    exit 1
  fi
done
echo

# ---------- 3. 单元测试 ----------
echo "===== 3. 单元测试 ====="
if ctest --test-dir "build/$preset" --output-on-failure >/tmp/match-ctest.log 2>&1; then
  ok "单元测试全部通过"
  grep -E 'tests passed|tests failed' /tmp/match-ctest.log | tail -2
else
  fail "单元测试失败"
  tail -25 /tmp/match-ctest.log
fi
echo

# ---------- 4. 启动 Match 与 Gateway ----------
echo "===== 4. 启动 Match 与 Gateway ====="
"$match_binary" -match_port "$match_port" \
  -match_timeout_seconds "$match_timeout_seconds" \
  -match_result_ttl_seconds "$match_result_ttl_seconds" >/tmp/match.out 2>&1 &
match_pid=$!
echo "Match 进程号: $match_pid"

match_ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null; then
    match_ready=1
    break
  fi
  sleep 0.5
done
if [ "$match_ready" -eq 1 ]; then
  ok "Match 已就绪（brpc 内置 /health）"
else
  fail "Match 未就绪"
  cat /tmp/match.out
  exit 1
fi

"$gateway_binary" -port "$gateway_port" -env_prefix dev \
  -mysql_host "$MYSQL_HOST" -mysql_port "$MYSQL_PORT" \
  -mysql_user "$MYSQL_USER" -mysql_password "$MYSQL_PASSWORD" \
  -mysql_database "$MYSQL_DATABASE" \
  -match_host 127.0.0.1 -match_port "$match_port" >/tmp/gateway.out 2>&1 &
gateway_pid=$!
echo "Gateway 进程号: $gateway_pid"

gateway_ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" 2>/dev/null; then
    gateway_ready=1
    break
  fi
  sleep 0.5
done
if [ "$gateway_ready" -eq 1 ]; then
  ok "Gateway 已就绪"
else
  fail "Gateway 未就绪"
  cat /tmp/gateway.out
  exit 1
fi
echo

http_post() {
  local body="${2:-}"
  [ -n "$body" ] || body='{}'
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 -X POST \
    -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$gateway_port$1"
}
http_get() {
  curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
    -H "Authorization: Bearer ${2:-}" "http://127.0.0.1:$gateway_port$1"
}
json_field() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" /tmp/resp.json; }

login() {
  http_post /api/v1/login \
    "{\"account\":\"$1\",\"password\":\"$2\",\"request_id\":\"$3\",\"client_type\":\"web\"}" \
    >/dev/null
  json_field token
}

echo "===== 5. 登录测试账号 ====="
# 只使用 alice 与 bob。carol 在种子数据里是 disabled，专门用于覆盖「禁用账号被拒绝」
# 这条失败路径，不能用于任何需要成功登录的流程。
#
# 因此「第三个玩家不会被并入已配满的局」这一条**无法在端到端层面覆盖**——它需要
# 三个可用身份。该不变量由单元测试覆盖：
#   MatchQueueTest.CompletedMatchDoesNotAbsorbLaterPlayers
#   MatchQueueTest.FifoOrderDecidesWhoIsPairedFirst
# 端到端改为验证强度接近的「两次匹配互相独立」：已完成的局不会被后续请求复用。
alice_token=$(login alice alice_dev_pw "verify-match-alice")
bob_token=$(login bob bob_dev_pw "verify-match-bob")
for pair in "alice:$alice_token" "bob:$bob_token"; do
  name="${pair%%:*}"
  value="${pair#*:}"
  if [ -n "$value" ]; then
    ok "$name 登录成功"
  else
    fail "$name 登录失败"
  fi
done
echo

# ---------- 6. 初始状态 ----------
echo "===== 6. 未入队时状态为 idle ====="
code=$(http_get /api/v1/matches/current "$alice_token")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "idle" ]; then
  ok "未入队返回 200 且 state=idle"
else
  fail "未入队状态异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$gateway_port/api/v1/matches/current")
if [ "$code" = "400" ] && grep -q 'token_required' /tmp/resp.json; then
  ok "缺少 Token 返回 400 token_required"
else
  fail "缺少 Token 的响应异常：HTTP $code"
  cat /tmp/resp.json; echo
fi
echo

# ---------- 7. 两人配对 ----------
echo "===== 7. 两人入队并配对 ====="
code=$(http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-1\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "queued" ]; then
  ok "alice 入队返回 200 且 state=queued"
else
  fail "alice 入队异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

code=$(http_post /api/v1/matches "{\"token\":\"$bob_token\",\"request_id\":\"m-bob-1\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "matched" ]; then
  ok "bob 入队后双方立即配对（state=matched）"
else
  fail "bob 入队异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi
bob_match_id=$(json_field match_id)
bob_room_id=$(json_field room_id)

code=$(http_get /api/v1/matches/current "$alice_token")
alice_state=$(json_field state)
alice_match_id=$(json_field match_id)
alice_room_id=$(json_field room_id)
if [ "$code" = "200" ] && [ "$alice_state" = "matched" ]; then
  ok "alice 查询到已配对"
else
  fail "alice 查询异常：HTTP $code state=${alice_state:-<空>}"
  cat /tmp/resp.json; echo
fi

if [ -n "$alice_match_id" ] && [ "$alice_match_id" = "$bob_match_id" ]; then
  ok "双方 match_id 一致（$alice_match_id）"
else
  fail "match_id 不一致：alice=[$alice_match_id] bob=[$bob_match_id]"
fi
if [ -n "$alice_room_id" ] && [ "$alice_room_id" = "$bob_room_id" ]; then
  ok "双方 room_id 一致（$alice_room_id）"
else
  fail "room_id 不一致：alice=[$alice_room_id] bob=[$bob_room_id]"
fi

# 同局玩家列表必须包含双方，且只有两人。
count=$(grep -o '"player_ids":\[[^]]*\]' /tmp/resp.json | grep -o 'p-000[0-9]' | wc -l)
if [ "$count" -eq 2 ] && grep -q 'p-0001' /tmp/resp.json && grep -q 'p-0002' /tmp/resp.json; then
  ok "player_ids 含双方且恰好两人"
else
  fail "player_ids 不符合预期（数量 $count）"
  cat /tmp/resp.json; echo
fi

# 幂等：同一玩家重复入队不报错，且返回既有匹配结果而不是新建一局。
code=$(http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-2\"}")
again_state=$(json_field state)
again_match_id=$(json_field match_id)
if [ "$code" = "200" ] && [ "$again_state" = "matched" ] && [ "$again_match_id" = "$alice_match_id" ]; then
  ok "重复入队幂等：仍返回同一 match_id"
else
  fail "重复入队异常：HTTP $code state=${again_state:-<空>} match_id=[$again_match_id]"
fi
echo

# ---------- 8. 两次匹配互相独立 ----------
echo "===== 8. 已完成的局不会被后续请求复用 ====="
first_match_id="$alice_match_id"
first_room_id="$alice_room_id"

# 等匹配结果超过保留期，alice 与 bob 回到 idle，可以重新匹配。
sleep $((match_result_ttl_seconds + 1))
http_get /api/v1/matches/current "$alice_token" >/dev/null
state=$(json_field state)
if [ "$state" = "idle" ]; then
  ok "结果保留期过后回到 idle"
else
  fail "结果未按保留期清理：state=${state:-<空>}"
fi

http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-3\"}" >/dev/null
code=$(http_post /api/v1/matches "{\"token\":\"$bob_token\",\"request_id\":\"m-bob-3\"}")
second_match_id=$(json_field match_id)
second_room_id=$(json_field room_id)
if [ "$code" = "200" ] && [ -n "$second_match_id" ] && [ "$second_match_id" != "$first_match_id" ]; then
  ok "第二次匹配得到新的 match_id（$second_match_id）"
else
  fail "第二次匹配异常：HTTP $code match_id=[$second_match_id] 首次=[$first_match_id]"
fi
if [ -n "$second_room_id" ] && [ "$second_room_id" != "$first_room_id" ]; then
  ok "第二次匹配得到新的 room_id"
else
  fail "room_id 未更新：[$second_room_id]"
fi
count=$(grep -o '"player_ids":\[[^]]*\]' /tmp/resp.json | grep -o 'p-000[0-9]' | wc -l)
if [ "$count" -eq 2 ]; then
  ok "第二次匹配仍然恰好两人（已完成的局没有被复用堆积玩家）"
else
  fail "第二次匹配的 player_ids 数量异常（$count）"
  cat /tmp/resp.json; echo
fi
echo

# ---------- 9. 取消匹配 ----------
echo "===== 9. 取消匹配（幂等） ====="
# 等结果过期，alice 回到 idle 才能重新入队并被取消。
sleep $((match_result_ttl_seconds + 1))
http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-4\"}" >/dev/null
code=$(http_post /api/v1/matches/current/cancel "{\"token\":\"$alice_token\",\"request_id\":\"c-alice-1\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "idle" ]; then
  ok "取消成功，状态回到 idle"
else
  fail "取消异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

# 重复取消不报错：与登出「重复调用返回成功」保持一致。
code=$(http_post /api/v1/matches/current/cancel "{\"token\":\"$alice_token\",\"request_id\":\"c-alice-2\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "idle" ]; then
  ok "重复取消仍返回 200 idle（幂等）"
else
  fail "重复取消异常：HTTP $code state=${state:-<空>}"
fi

# 取消后重新入队应回到 queued；此时 bob 不在队列中，因此不会立刻配对。
code=$(http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-5\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "queued" ]; then
  ok "取消后重新入队回到 queued"
else
  fail "重新入队异常：HTTP $code state=${state:-<空>}"
fi
echo

# ---------- 10. 排队超时 ----------
echo "===== 10. 排队超时（Match 侧超时为 ${match_timeout_seconds}s） ====="
sleep $((match_timeout_seconds + 1))
code=$(http_get /api/v1/matches/current "$alice_token")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "timeout" ]; then
  ok "超时后状态为 timeout（与 idle 区分）"
else
  fail "超时状态异常：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

# 等待超时标记过期，alice 回到 idle，供下一步复用。
sleep $((match_result_ttl_seconds + 1))
code=$(http_get /api/v1/matches/current "$alice_token")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "idle" ]; then
  ok "超时标记过期后回到 idle"
else
  fail "过期后状态异常：HTTP $code state=${state:-<空>}"
fi
echo

# ---------- 11. Match 不可用与恢复 ----------
echo "===== 11. Match 不可用时返回 503，恢复后无需重启 ====="
kill "$match_pid" 2>/dev/null || true
wait "$match_pid" 2>/dev/null
match_pid=""
ok "已停止 Match"

code=$(http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-6\"}")
if [ "$code" = "503" ] && grep -q 'match_unavailable' /tmp/resp.json; then
  ok "Match 不可用时入队返回 503 match_unavailable（未伪装成功）"
else
  fail "Match 不可用时返回 $code（期望 503 match_unavailable）"
  cat /tmp/resp.json; echo
fi

code=$(http_get /api/v1/matches/current "$alice_token")
if [ "$code" = "503" ]; then
  ok "Match 不可用时查询也返回 503"
else
  fail "Match 不可用时查询返回 $code（期望 503）"
fi

"$match_binary" -match_port "$match_port" \
  -match_timeout_seconds "$match_timeout_seconds" \
  -match_result_ttl_seconds "$match_result_ttl_seconds" >/tmp/match-restart.out 2>&1 &
match_pid=$!
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$match_port/health" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
ok "已重启 Match"

code=$(http_post /api/v1/matches "{\"token\":\"$alice_token\",\"request_id\":\"m-alice-7\"}")
state=$(json_field state)
if [ "$code" = "200" ] && [ "$state" = "queued" ]; then
  ok "Match 恢复后无需重启 Gateway 即可匹配"
else
  fail "Match 恢复后仍不可用：HTTP $code state=${state:-<空>}"
  cat /tmp/resp.json; echo
fi

# 网关进程全程存活：依赖抖动不应导致它退出或被反复重启。
if kill -0 "$gateway_pid" 2>/dev/null; then
  ok "Gateway 进程全程存活（未因 Match 抖动退出）"
else
  fail "Gateway 进程已退出"
fi
echo

# ---------- 12. 优雅退出 ----------
echo "===== 12. 优雅退出 ====="
for pair in "Gateway:$gateway_pid" "Match:$match_pid"; do
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
      ok "$name 收到 SIGTERM 后退出码 0"
    else
      fail "$name 退出码 $rc（期望 0）"
    fi
  else
    fail "$name 未在 10 秒内退出"
  fi
done
gateway_pid=""
match_pid=""
echo

# ---------- 结果 ----------
echo "===== 验收结果 ====="
if [ "${#failures[@]}" -eq 0 ]; then
  echo "验收通过：两人可配成同一局且 match_id/room_id 一致；重复入队幂等；"
  echo "          已完成的局不会被复用；取消幂等；超时状态可区分；"
  echo "          Match 不可用返回 503 且恢复后无需重启；两个进程均可优雅退出"
  echo
  echo "未在端到端覆盖：「第三个玩家不会被并入已配满的局」需要三个可用测试身份，"
  echo "          种子数据只提供两个（carol 是 disabled）。该不变量由单元测试覆盖："
  echo "          MatchQueueTest.CompletedMatchDoesNotAbsorbLaterPlayers"
  exit 0
fi

echo "验收失败 ${#failures[@]} 项："
for item in "${failures[@]}"; do
  echo "  - $item"
done
echo
echo "排查提示：Gateway 日志 /tmp/gateway.out，Match 日志 /tmp/match.out"
exit 1
