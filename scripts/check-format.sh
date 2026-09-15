#!/usr/bin/env bash
#
# check-format.sh - 检查 C++ 源码是否符合 .clang-format
#
# 只检查纳入版本管理的 C++ 源文件，排除 build/ 和第三方代码。
# 用法： bash scripts/check-format.sh
# 退出码：0 表示全部合规；1 表示存在不合规文件或缺少 clang-format。

set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
  echo "缺少 clang-format，无法执行格式检查。" >&2
  echo "Ubuntu 安装方式： sudo apt-get install -y clang-format" >&2
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

echo "格式检查通过，共检查 $(wc -l <<< "$files") 个文件。"
