#!/usr/bin/env bash
#
# verify-brpc.sh - TASK-004 验收入口
#
# 目的：证明 vcpkg 提供的 brpc 能在本工程中编译、链接、启动、响应健康检查
#       并优雅退出。这是“先验证再全量”策略的验证步骤。
#
# 用法：
#   bash scripts/verify-brpc.sh            # 完整验收
#   bash scripts/verify-brpc.sh --build    # 只配置与构建
#
# 前置条件：
#   vcpkg 已安装 brpc： vcpkg install brpc --triplet x64-linux
#
# 退出码：0 通过；非 0 失败。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

vcpkg_root="${VCPKG_ROOT:-$HOME/tools/vcpkg}"
toolchain="$vcpkg_root/scripts/buildsystems/vcpkg.cmake"
preset="brpc-debug"
port=8090
build_only=0

for arg in "$@"; do
  case "$arg" in
    --build) build_only=1 ;;
    -h | --help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
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

echo "工作目录: $(pwd)"
echo

# ---------- 0. 前置检查 ----------
echo "===== 0. 前置检查 ====="

if [ ! -f "$toolchain" ]; then
  echo "未找到 vcpkg toolchain: $toolchain" >&2
  echo "请确认 VCPKG_ROOT 指向 vcpkg 根目录。" >&2
  exit 1
fi
ok "vcpkg toolchain 存在"

brpc_include="$vcpkg_root/installed/x64-linux/include/brpc"
if [ -d "$brpc_include" ]; then
  ok "brpc 头文件已安装"
else
  echo "brpc 未安装。请先执行：" >&2
  echo "  $vcpkg_root/vcpkg install brpc --triplet x64-linux" >&2
  exit 1
fi

brpc_lib="$(find "$vcpkg_root/installed/x64-linux/lib" -maxdepth 1 -name 'libbrpc*' 2>/dev/null | head -1)"
if [ -n "$brpc_lib" ]; then
  ok "brpc 库文件: $(basename "$brpc_lib")"
else
  fail "未找到 brpc 库文件"
fi
echo

# ---------- 1. 配置与构建 ----------
echo "===== 1. 配置与构建（预设 $preset） ====="
echo "命令: cmake --preset $preset"
# 预设自身已通过 toolchainFile 指向 vcpkg，无需在命令行再传 CMAKE_TOOLCHAIN_FILE。
# 这也让本脚本在 VCPKG_ROOT 未导出的非交互式 shell 中同样可用。
if cmake --preset "$preset" 2>&1 | tail -15; then
  ok "配置成功"
else
  fail "配置失败"
fi

if cmake --build --preset "$preset" --target rgbt_brpc_smoke 2>&1 | tail -20; then
  ok "构建成功"
else
  fail "构建失败"
fi

binary="build/$preset/bin/rgbt_brpc_smoke"
if [ -x "$binary" ]; then
  ok "可执行文件存在: $binary"
else
  fail "未生成可执行文件: $binary"
fi
echo

if [ "$build_only" -eq 1 ]; then
  if [ ${#failures[@]} -ne 0 ]; then
    echo "===== 验收失败 ====="
    for f in "${failures[@]}"; do echo " - $f"; done
    exit 1
  fi
  echo "===== 仅构建验收通过 ====="
  exit 0
fi

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 前置或构建阶段失败，跳过运行时验证 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  exit 1
fi

# ---------- 2. 启动服务 ----------
echo "===== 2. 启动 brpc 服务 ====="
"$binary" -port "$port" >/tmp/brpc-smoke.out 2>&1 &
server_pid=$!
echo "进程号: $server_pid"

# 等待端口就绪，最多 15 秒
ready=0
for _ in $(seq 1 30); do
  if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$port/health" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 0.5
done

if [ "$ready" -eq 1 ]; then
  ok "服务已就绪并响应请求"
else
  fail "服务在 15 秒内未就绪"
  echo "--- 启动输出 ---"
  cat /tmp/brpc-smoke.out
fi
echo

# ---------- 3. 健康检查 ----------
echo "===== 3. 健康检查 ====="
health_code=$(curl -s -o /tmp/health-body.txt -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$port/health" 2>/dev/null)
health_body=$(cat /tmp/health-body.txt 2>/dev/null)

if [ "$health_code" = "200" ]; then
  ok "GET /health 返回 200"
else
  fail "GET /health 返回 $health_code（期望 200）"
fi

# brpc 内置 /health 返回纯文本 OK；这里同时接受纯文本与 JSON 两种形式，
# 只要求响应体非空且体现出健康语义。
if echo "$health_body" | grep -qiE 'ok|healthy'; then
  ok "健康检查响应体符合预期: $(echo "$health_body" | head -c 80)"
else
  fail "健康检查响应体不符合预期: $health_body"
fi
echo

# ---------- 4. 错误路径（证明路由生效） ----------
echo "===== 4. 错误路径 ====="
notfound_code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$port/definitely-not-a-route" 2>/dev/null)
if [ "$notfound_code" = "404" ]; then
  ok "未知路径返回 404，路由确实生效"
else
  fail "未知路径返回 $notfound_code（期望 404，若为 200 说明路由未生效）"
fi

status_code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  "http://127.0.0.1:$port/status" 2>/dev/null)
if [ "$status_code" = "200" ]; then
  ok "brpc 内置 /status 返回 200，确认 brpc 自身服务正常"
else
  fail "brpc 内置 /status 返回 $status_code（期望 200）"
fi
echo

# ---------- 5. 优雅退出 ----------
echo "===== 5. 优雅退出 ====="
kill -TERM "$server_pid" 2>/dev/null
exited=0
for _ in $(seq 1 20); do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    exited=1
    break
  fi
  sleep 0.5
done

if [ "$exited" -eq 1 ]; then
  wait "$server_pid" 2>/dev/null
  exit_code=$?
  if [ "$exit_code" -eq 0 ]; then
    ok "收到 SIGTERM 后正常退出，退出码 0"
  else
    fail "退出码为 $exit_code（期望 0）"
  fi
else
  fail "收到 SIGTERM 后 10 秒内未退出"
  kill -9 "$server_pid" 2>/dev/null
fi

if grep -q '已优雅退出' /tmp/brpc-smoke.out; then
  ok "输出中确认走了优雅退出路径"
else
  fail "未观察到优雅退出输出"
fi

echo "--- 服务完整输出 ---"
cat /tmp/brpc-smoke.out
echo

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 验收失败 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  exit 1
fi

echo "===== 验收通过：brpc 可编译、可链接、可启动、健康检查正常、可优雅退出 ====="
