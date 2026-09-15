#!/usr/bin/env bash
#
# verify.sh - 本地构建与测试验收脚本
#
# 这是 TASK-002 的验收入口，与 CI 执行相同的步骤，保证本地与 CI 行为一致。
# 首次运行需要联网下载 GoogleTest。
#
# 用法：
#   bash scripts/verify.sh              # 依次执行 debug / release / asan
#   bash scripts/verify.sh debug asan   # 只执行指定预设
#
# 退出码：0 表示全部通过；非 0 表示失败，失败信息会打印在末尾。

set -uo pipefail

presets=("$@")
if [ ${#presets[@]} -eq 0 ]; then
  presets=(debug release asan)
fi

cd "$(dirname "$0")/.." || exit 1

echo "工作目录: $(pwd)"
echo "执行预设: ${presets[*]}"
echo

for tool in cmake ninja; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "缺少 $tool，无法继续。" >&2
    exit 1
  fi
done

cmake --version | head -1
ninja --version | sed 's/^/ninja /'
echo

failures=()

for preset in "${presets[@]}"; do
  echo "===== 预设: $preset ====="
  if cmake --preset "$preset"; then
    if cmake --build --preset "$preset"; then
      if ctest --preset "$preset"; then
        echo "----- $preset 通过 -----"
      else
        echo "----- $preset 测试失败 -----"
        failures+=("$preset: ctest 失败")
      fi
    else
      echo "----- $preset 构建失败 -----"
      failures+=("$preset: 构建失败")
    fi
  else
    echo "----- $preset 配置失败 -----"
    failures+=("$preset: 配置失败")
  fi
  echo
done

echo "===== 格式检查 ====="
if bash scripts/check-format.sh; then
  echo "----- 格式检查通过 -----"
else
  failures+=("check-format: 格式不合规")
fi
echo

if [ ${#failures[@]} -ne 0 ]; then
  echo "===== 验收失败 ====="
  for f in "${failures[@]}"; do echo " - $f"; done
  exit 1
fi

echo "===== 验收通过：全部预设构建与测试成功 ====="
