#!/usr/bin/env bash
#
# check-format.sh - 检查 C++ 源码是否符合 .clang-format
#
# 只检查纳入版本管理的 C++ 源文件，排除 build/ 和第三方代码。
# 用法： bash scripts/check-format.sh
# 退出码：0 表示全部合规；1 表示存在不合规文件、缺少 clang-format，或版本不符。
#
# **为什么必须校验版本**：clang-format 的默认换行与对齐策略在各大版本之间会变，
# 同一个文件用 18 判定"不合规"、用 21 判定"合规"是常态。此前本脚本不校验版本，
# 结果 CI（ubuntu-24.04 的 `clang-format 18`）与开发环境（21.x）判定不一致：
# **本地绿、CI 红**，而 CI 的格式门禁事实上无法被本地复现。这类"漂移"比不做检查
# 更糟——它让人怀疑门禁本身而不是代码。
#
# 版本要求写在这里而不是只写在 CI 配置里：两边读同一处，改一处即可。

set -euo pipefail

# 允许的最低主版本。开发环境（WSL Ubuntu 26.04）实测为 21.1.x；
# CI 通过 apt.llvm.org 安装同一主版本。
required_major=21

if ! command -v clang-format >/dev/null 2>&1; then
  echo "缺少 clang-format，无法执行格式检查。" >&2
  echo "安装方式（版本必须是 $required_major.x）：" >&2
  echo "  sudo apt-get install -y clang-format-$required_major" >&2
  echo "  或见 docs/06-operations.md 的「clang-format 版本」一节" >&2
  exit 1
fi

version_line=$(clang-format --version)
actual_major=$(printf '%s' "$version_line" | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p')
if [ -z "$actual_major" ]; then
  echo "无法从 '${version_line}' 解析 clang-format 主版本。" >&2
  exit 1
fi
if [ "$actual_major" -ne "$required_major" ]; then
  echo "clang-format 主版本不符：需要 ${required_major}.x，实际为 ${actual_major}.x" >&2
  echo "  实际版本：${version_line}" >&2
  echo "  为什么必须一致：换行/对齐策略跨版本会变，不一致会导致「本地绿、CI 红」。" >&2
  echo "  修复：安装 clang-format-${required_major}，或让 PATH 里的 clang-format 指向它。" >&2
  exit 1
fi

cd "$(git rev-parse --show-toplevel)"

files=$(find include src tests -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null | sort)

if [ -z "$files" ]; then
  echo "未发现需要检查的 C++ 源文件。"
  exit 0
fi

failed=0
while IFS= read -r file; do
  if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
    echo "格式不合规: $file"
    failed=1
  fi
done <<< "$files"

if [ "$failed" -ne 0 ]; then
  echo
  echo "可执行以下命令自动修复： find include src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i"
  exit 1
fi

echo "格式检查通过，共检查 $(wc -l <<< "$files") 个文件（clang-format ${actual_major}.x）。"
