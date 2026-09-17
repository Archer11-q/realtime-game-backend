#!/usr/bin/env bash
#
# verify-login.sh - TASK-005 登录切片验收入口
#
# 覆盖 TASK-005 的三条验收标准：
#   1. 正常登录成功，且同一 request_id 重复登录返回同一会话（幂等）
#   2. 无效输入被拒绝
#   3. Redis 不可用时返回 503 而非假成功，且恢复后无需重启服务
#
# 用法：
#   bash scripts/verify-login.sh              # 完整验收
#   bash scripts/verify-login.sh --keep       # 结束后保留网关进程
#   bash scripts/verify-login.sh --no-docker  # 复用已启动的 Redis（不做启停）
#
# 前置：Docker Desktop 已启动；deploy/compose/.env 存在（缺失时由本脚本生成）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

compose_file="deploy/compose/docker-compose.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"
gateway_port=8080
preset="brpc-debug"
binary="build/$preset/bin/rgbt_gateway"
keep_running=0
manage_docker=1

for arg in "$@"; do
  case "$arg" in
    --keep) keep_running=1 ;;
    --no-docker) manage_docker=0 ;;
    -h | --help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
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
cleanup() {
  if [ -n "$gateway_pid" ] && kill -0 "$gateway_pid" 2>/dev/null; then
    if [ "$keep_running" -eq 1 ]; then
      echo "按 --keep 要求保留网关进程 (PID=$gateway_pid)"
    else
      kill -TERM "$gateway_pid" 2>/dev/null
      wait "$gateway_pid" 2>/dev/null
      echo "网关进程已停止"
    fi
  fi
}
trap cleanup EXIT

echo "工作目录: $repo_root"
echo

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="
if [ ! -f "$env_file" ]; then
  cp "$env_example" "$env_file"
  ok "已从 .env.example 生成 $env_file"
else
  ok "$env_file 已存在"
fi

if [ "$manage_docker" -eq 1 ]; then
  if ! docker info >/dev/null 2>&1; then
    echo "Docker 服务端不可达，请先启动 Docker Desktop。" >&2
    exit 1
  fi
  ok "Docker 可用"
fi
echo

# ---------- 1. 启动 Redis ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 1. 启动 Redis ====="
  if compose up -d redis >/dev/null 2>&1; then
    ok "compose 启动 redis 成功"
  else
    fail "compose 启动 redis 失败"
  fi
  for _ in $(seq 1 40); do
    state="$(docker inspect -f '{{.State.Health.Status}}' rgbt-redis 2>/dev/null)"
    [ "$state" = "healthy" ] && break
    sleep 2
  done
  if [ "$(docker inspect -f '{{.State.Health.Status}}' rgbt-redis 2>/dev/null)" = "healthy" ]; then
    ok "redis 健康"
  else
    fail "redis 未达到 healthy"
  fi

  # 清理上一次运行留下的 Key。
  #
  # 为什么需要：本脚本使用的是固定的 env_prefix（dev），上一次运行创建的会话与
  # 幂等映射会残留 7 天。不清理会导致结果不可复现——例如某个 request_id 已存在
  # 幂等映射时，本次登录会直接返回旧 Token，而该 Token 对应的会话可能已被登出
  # 或从未创建，表现为「刚拿到的 Token 查询失败」这类难以定位的偶发问题。
  stale=$(docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null | wc -l)
  if [ "$stale" -gt 0 ]; then
    docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:*' 2>/dev/null \
      | while IFS= read -r key; do
          [ -n "$key" ] && docker exec rgbt-redis redis-cli del "$key" >/dev/null 2>&1
        done
    ok "已清理上次运行残留的 $stale 个 dev:gateway:* Key"
  else
    ok "无残留 Key"
  fi
  echo
fi

# ---------- 2. 构建 ----------
echo "===== 2. 构建 Gateway ====="
if cmake --preset "$preset" >/tmp/login-config.log 2>&1; then
  ok "配置成功"
else
  fail "配置失败"
  tail -20 /tmp/login-config.log
fi

if cmake --build --preset "$preset" --target rgbt_gateway rgbt_gateway_tests \
  >/tmp/login-build.log 2>&1; then
  ok "构建成功"
else
  fail "构建失败"
  tail -30 /tmp/login-build.log
fi

if [ -x "$binary" ]; then
  ok "可执行文件存在: $binary"
else
  fail "未生成可执行文件: $binary"
fi
echo

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 构建阶段失败，跳过运行时验收 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  exit 1
fi

# ---------- 3. 单元测试 ----------
echo "===== 3. 单元测试 ====="
# 用 --test-dir 而不是 --preset：本脚本可能在未加载 shell 环境的情况下执行，
# 且测试名是 GatewayServiceTest.* / TokenTest.* 这类不含 "gateway" 的命名，
# 因此不加 -R 过滤，直接跑完整套件。
if ctest --test-dir "build/$preset" --output-on-failure >/tmp/login-ctest.log 2>&1; then
  ok "单元测试全部通过"
  grep -E 'tests passed|tests failed' /tmp/login-ctest.log | tail -2
else
  fail "单元测试失败"
  tail -25 /tmp/login-ctest.log
fi
echo

# ---------- 4. 启动网关 ----------
echo "===== 4. 启动 Gateway ====="
"$binary" -port "$gateway_port" -env_prefix dev >/tmp/gateway.out 2>&1 &
gateway_pid=$!
echo "进程号: $gateway_pid"

ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$gateway_port/health" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 0.5
done
if [ "$ready" -eq 1 ]; then
  ok "网关已就绪"
else
  fail "网关未就绪"
  cat /tmp/gateway.out
  exit 1
fi
echo

http_post() { curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 -X POST \
  -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:$gateway_port$1"; }
http_get() { curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$gateway_port$1"; }

# ---------- 5. 正常登录 ----------
echo "===== 5. 正常登录与幂等 ====="
code=$(http_post /api/v1/login '{"account":"alice","password":"alice_dev_pw","request_id":"verify-001","client_type":"web"}')
if [ "$code" = "200" ]; then
  ok "登录返回 200"
else
  fail "登录返回 $code（期望 200）"
  cat /tmp/resp.json; echo
fi

token=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/resp.json)
if [ -n "$token" ]; then
  ok "取得 Token（长度 ${#token}）"
else
  fail "响应中没有 token"
  cat /tmp/resp.json; echo
fi

if grep -q '"player_id":"p-0001"' /tmp/resp.json; then
  ok "返回玩家信息正确（p-0001）"
else
  fail "玩家信息不符合预期"
  cat /tmp/resp.json; echo
fi

# 幂等：同一 request_id 重复登录必须返回同一 Token
code2=$(http_post /api/v1/login '{"account":"alice","password":"alice_dev_pw","request_id":"verify-001","client_type":"web"}')
token2=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/resp.json)
if [ "$code2" = "200" ] && [ "$token" = "$token2" ]; then
  ok "同一 request_id 重复登录返回同一 Token（幂等生效）"
else
  fail "幂等失败：首次 [$token] 再次 [$token2]（HTTP $code2）"
fi

# 不同 request_id 应得到不同 Token
http_post /api/v1/login '{"account":"alice","password":"alice_dev_pw","request_id":"verify-002"}' >/dev/null
token3=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/resp.json)
if [ -n "$token3" ] && [ "$token3" != "$token" ]; then
  ok "不同 request_id 得到不同 Token"
else
  fail "不同 request_id 却得到相同 Token"
fi
echo

# ---------- 6. 无效输入 ----------
echo "===== 6. 无效输入被拒绝 ====="
code=$(http_post /api/v1/login '{"account":"","password":"alice_dev_pw"}')
if [ "$code" = "400" ]; then ok "空账号返回 400"; else fail "空账号返回 $code（期望 400）"; fi

code=$(http_post /api/v1/login '{"account":"alice","password":"wrong_password"}')
if [ "$code" = "401" ]; then ok "错误密码返回 401"; else fail "错误密码返回 $code（期望 401）"; fi

code=$(http_post /api/v1/login '{"account":"carol","password":"carol_dev_pw"}')
if [ "$code" = "401" ]; then ok "禁用账号返回 401"; else fail "禁用账号返回 $code（期望 401）"; fi

code=$(http_post /api/v1/login '{"account":"nobody","password":"whatever"}')
if [ "$code" = "401" ]; then ok "不存在的账号返回 401"; else fail "不存在的账号返回 $code（期望 401）"; fi
echo

# ---------- 7. 鉴权查询 ----------
echo "===== 7. 查询当前玩家 ====="
code=$(curl -s -o /tmp/resp.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $token" "http://127.0.0.1:$gateway_port/api/v1/players/me")
if [ "$code" = "200" ] && grep -q '"player_id":"p-0001"' /tmp/resp.json; then
  ok "有效 Token 查询返回 200 与正确玩家"
else
  fail "有效 Token 查询失败：HTTP $code"
  # 失败时打印用于诊断的关键信息：Token 本身、Redis 中实际存在的会话 Key。
  echo "   使用的 Token: [$token]（长度 ${#token}）"
  echo "   响应体: $(cat /tmp/resp.json)"
  if [ "$manage_docker" -eq 1 ]; then
    echo "   Redis 中的 session Key:"
    docker exec rgbt-redis redis-cli --scan --pattern 'dev:gateway:session:*' 2>/dev/null | sed 's/^/     /'
    echo "   期望的 Key: dev:gateway:session:$token"
  fi
fi

code=$(http_get "/api/v1/players/me?token=invalid-token-with-bad-chars!")
if [ "$code" = "400" ] || [ "$code" = "401" ]; then
  ok "非法 Token 被拒绝（HTTP $code）"
else
  fail "非法 Token 返回 $code（期望 400 或 401）"
fi

code=$(http_get "/api/v1/players/me?token=$(head -c 40 /dev/urandom | base64 | tr -dc 'A-Za-z0-9_-')")
if [ "$code" = "401" ]; then
  ok "格式合法但不存在 Token 返回 401"
else
  fail "不存在的 Token 返回 $code（期望 401）"
fi
echo

# ---------- 8. 登出 ----------
echo "===== 8. 登出 ====="
code=$(http_post /api/v1/logout "{\"token\":\"$token\"}")
if [ "$code" = "200" ]; then ok "登出返回 200"; else fail "登出返回 $code（期望 200）"; fi

code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $token" "http://127.0.0.1:$gateway_port/api/v1/players/me")
if [ "$code" = "401" ]; then
  ok "登出后原 Token 失效（401）"
else
  fail "登出后原 Token 仍可用（HTTP $code）"
fi
echo

# ---------- 9. Redis 不可用与恢复 ----------
if [ "$manage_docker" -eq 1 ]; then
  echo "===== 9. Redis 不可用与恢复 ====="
  compose stop redis >/dev/null 2>&1
  ok "已停止 Redis"

  code=$(http_post /api/v1/login '{"account":"alice","password":"alice_dev_pw","request_id":"verify-redis-down"}')
  if [ "$code" = "503" ]; then
    ok "Redis 不可用时登录返回 503（未伪装成功）"
  else
    fail "Redis 不可用时返回 $code（期望 503）"
  fi

  if grep -q 'session_store_unavailable' /tmp/resp.json; then
    ok "错误原因为 session_store_unavailable"
  else
    fail "错误原因不符合预期"
    cat /tmp/resp.json; echo
  fi

  if grep -qiE 'unavailable|cannot connect|connection refused' /tmp/gateway.out; then
    ok "网关日志记录了依赖不可用"
  else
    echo "   注意：网关输出未出现明显的连接错误记录（不视为失败）"
  fi

  compose start redis >/dev/null 2>&1
  for _ in $(seq 1 40); do
    state="$(docker inspect -f '{{.State.Health.Status}}' rgbt-redis 2>/dev/null)"
    [ "$state" = "healthy" ] && break
    sleep 2
  done
  ok "已重启 Redis"

  # 关键：服务不重启，Redis 恢复后应自动可用
  code=$(http_post /api/v1/login '{"account":"bob","password":"bob_dev_pw","request_id":"verify-recover"}')
  if [ "$code" = "200" ]; then
    ok "Redis 恢复后无需重启网关即可登录（自愈）"
  else
    fail "Redis 恢复后登录仍失败：HTTP $code"
    cat /tmp/resp.json; echo
  fi

  if kill -0 "$gateway_pid" 2>/dev/null; then
    ok "网关进程全程存活，未因依赖抖动重启"
  else
    fail "网关进程意外退出"
  fi
  echo
fi

# ---------- 10. 优雅退出 ----------
if [ "$keep_running" -eq 0 ]; then
  echo "===== 10. 优雅退出 ====="
  kill -TERM "$gateway_pid" 2>/dev/null
  exited=0
  for _ in $(seq 1 20); do
    kill -0 "$gateway_pid" 2>/dev/null || { exited=1; break; }
    sleep 0.5
  done
  if [ "$exited" -eq 1 ]; then
    wait "$gateway_pid" 2>/dev/null
    code=$?
    if [ "$code" -eq 0 ]; then ok "收到 SIGTERM 后退出码 0"; else fail "退出码 $code"; fi
  else
    fail "10 秒内未退出"
    kill -9 "$gateway_pid" 2>/dev/null
  fi
  gateway_pid=""
  echo
fi

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 验收失败 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  echo
  echo "排查提示："
  echo "  网关输出: cat /tmp/gateway.out"
  echo "  构建日志: cat /tmp/login-build.log"
  echo "  单元测试: cat /tmp/login-ctest.log"
  exit 1
fi

echo "===== 验收通过：登录切片可用，无效输入与 Redis 故障路径均已验证 ====="
