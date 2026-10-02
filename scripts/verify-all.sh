#!/usr/bin/env bash
#
# verify-all.sh - 跑完整套验收，最后给出汇总
#
# 为什么需要这个脚本（2026-10-02 补上）：
#   TASK-008 把 Match 的房间分配换成真实 brpc 调用后，scripts/verify-match.sh
#   因为不启动 Room 而全部配对失败——但当时只验了新脚本，没人重跑旧脚本，
#   这个回归一直躺着，直到 TASK-009 期间偶然重跑才暴露。
#   光有「改变依赖的任务必须重跑既有验收脚本」这条纪律不够，
#   重跑必须是一条命令，否则它一定会被跳过。
#
# 用法：
#   bash scripts/verify-all.sh              # 全套（实测 142 秒，见下）
#   bash scripts/verify-all.sh --list       # 只列出会跑哪些
#   bash scripts/verify-all.sh --only room,stream   # 只跑指定几个
#   bash scripts/verify-all.sh --fail-fast  # 第一个失败就停
#
# 每个脚本的输出保存在 .run/verify-<名字>.log，失败时直接看那个文件。
#
# 实测耗时（2026-10-02，本机 16 核 / 11 GiB，ccache 已预热）：
#   verify 29s / verify-login 53s / verify-match 38s / verify-room 32s
#   / verify-stream 11s / verify-web 21s / verify-persistence 44s，合计 228 秒。
# 冷缓存（ccache 为空）时 verify.sh 会明显更久，因为它要重新编译三个预设。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
run_dir="$(pwd)/.run"
mkdir -p "$run_dir"

# 顺序有依赖关系：先构建与格式，再按「登录 -> 匹配 -> 房间 -> 推送 -> 前端 ->
# 持久化」由下而上。前一个失败时后面的通常也会失败，因此默认不用 fail-fast，
# 一次性看到全部结果更有用。
#
# ⚠ 2026-10-02 修正：这里原本有**两行** all_scripts，第二行漏掉了 verify-persistence，
# 把第一行整个覆盖掉，于是 TASK-013 的验收脚本从来没有被这条"一条命令"跑到——
# 正是本脚本开头那段理由所警告的情况，只是这次坑在脚本自己身上。
# 数组只保留一行；新增验收脚本时改这一行。
all_scripts=(verify verify-login verify-match verify-room verify-stream verify-web verify-persistence)

fail_fast=0
only=""
for arg in "$@"; do
  case "$arg" in
    --fail-fast) fail_fast=1 ;;
    --list)
      echo "会按顺序运行："
      for s in "${all_scripts[@]}"; do echo "  scripts/$s.sh"; done
      exit 0
      ;;
    --only)
      shift || true
      only="${1:-}"
      ;;
    --only=*) only="${arg#--only=}" ;;
    -h | --help)
      sed -n '2,20p' "$0" | sed 's/^\{0,1\}# \{0,1\}//;s/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

scripts=()
if [ -n "$only" ]; then
  IFS=',' read -r -a wanted <<<"$only"
  for name in "${all_scripts[@]}"; do
    for w in "${wanted[@]}"; do
      # 允许写 verify-room 或 room，两种都认。
      if [ "$name" = "$w" ] || [ "$name" = "verify-$w" ]; then
        scripts+=("$name")
      fi
    done
  done
  [ "${#scripts[@]}" -gt 0 ] || { echo "没有匹配的脚本：$only" >&2; exit 2; }
else
  scripts=("${all_scripts[@]}")
fi

echo "============================================================"
echo "  完整验收：${#scripts[@]} 个脚本，按顺序运行"
echo "  日志目录：$run_dir"
echo "============================================================"

declare -a names=() results=() durations=()
overall=0
total_start=$(date +%s)

for name in "${scripts[@]}"; do
  log="$run_dir/verify-$name.log"
  echo
  echo "########## scripts/$name.sh ##########"
  start=$(date +%s)
  # 不用管道接 tail：$(...) 捕获会丢掉退出码，且管道会让 PIPESTATUS 变复杂。
  bash "scripts/$name.sh" >"$log" 2>&1
  rc=$?
  end=$(date +%s)
  elapsed=$((end - start))

  names+=("$name")
  results+=("$rc")
  durations+=("$elapsed")

  if [ "$rc" -eq 0 ]; then
    echo "v  通过（${elapsed}s）"
  else
    echo "x  失败，退出码 $rc（${elapsed}s）"
    echo "   失败项："
    grep -E '^x  ' "$log" | head -10 | sed 's/^/     /'
    echo "   完整日志：$log"
    overall=1
    if [ "$fail_fast" -eq 1 ]; then
      echo
      echo "遇到失败，按 --fail-fast 停止。"
      break
    fi
  fi
done

total_end=$(date +%s)

echo
echo "============================================================"
echo "  验收汇总"
echo "============================================================"
for i in "${!names[@]}"; do
  if [ "${results[$i]}" -eq 0 ]; then
    printf '  %-16s 通过   %4ss\n' "${names[$i]}" "${durations[$i]}"
  else
    printf '  %-16s 失败   %4ss  (退出码 %s)\n' \
      "${names[$i]}" "${durations[$i]}" "${results[$i]}"
  fi
done
echo "  --------------------------------------------------------"
printf '  总计 %s 个脚本，耗时 %ss\n' "${#names[@]}" "$((total_end - total_start))"

if [ "$overall" -eq 0 ]; then
  echo
  echo "全部通过。"
  echo "注意：这些脚本覆盖的是网络路径与服务行为。"
  echo "      渲染（Canvas 画面、按钮状态、视图切换）需要人工确认，"
  echo "      步骤见 scripts/dev-up.sh 启动后打印的说明。"
else
  echo
  echo "存在失败项。逐个查看 .run/verify-*.log 定位。"
fi

exit "$overall"
