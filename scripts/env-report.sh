#!/usr/bin/env bash
#
# env-report.sh - 只读环境检查脚本
#
# 用途：在 WSL 正式开发环境中盘点仓库状态、工具链版本和构建依赖，
#       输出可粘贴进 devlog 的实测结果。本脚本不修改任何文件和配置。
#
# 用法： bash scripts/env-report.sh

set -uo pipefail

section() { printf '\n=== %s ===\n' "$1"; }

section "发行版与内核"
grep -E '^PRETTY_NAME' /etc/os-release 2>/dev/null || echo "无法读取 /etc/os-release"
uname -sr

section "资源"
echo "CPU 逻辑核: $(nproc)"
free -h 2>/dev/null | head -2

section "仓库状态"
repo_dir="${1:-$HOME/workspace/realtime-game-backend}"
if [ ! -d "$repo_dir/.git" ]; then
  echo "未找到 git 仓库: $repo_dir"
else
  cd "$repo_dir" || exit 1
  echo "路径: $(pwd)"
  echo "--- 最近提交 ---"
  git log --oneline -n 3 2>&1
  echo "--- 分支 ---"
  git branch -vv 2>&1
  echo "--- 工作区 ---"
  status="$(git status --short 2>&1)"
  if [ -z "$status" ]; then echo "(干净)"; else echo "$status"; fi
  echo "--- 远程 ---"
  git remote -v 2>&1
fi

section "工具链"
for tool in git cmake ninja g++ gcc clang-format clang-tidy vcpkg docker protoc \
            pkg-config ctest ccache; do
  if command -v "$tool" >/dev/null 2>&1; then
    printf '%-14s OK      %s\n' "$tool" "$(command -v "$tool")"
  else
    printf '%-14s MISSING\n' "$tool"
  fi
done

section "版本"
cmake --version 2>/dev/null | head -1
g++ --version 2>/dev/null | head -1
ninja --version 2>/dev/null | sed 's/^/ninja /'
git --version 2>/dev/null
clang-format --version 2>/dev/null

section "构建依赖头文件"
for header in /usr/include/openssl/ssl.h /usr/include/gflags /usr/include/glog; do
  if [ -e "$header" ]; then echo "存在: $header"; else echo "缺失: $header"; fi
done

section "Docker"
docker info --format '{{.ServerVersion}}' 2>&1 | head -2

section "完成"
echo "本脚本为只读检查，未修改任何文件。"
