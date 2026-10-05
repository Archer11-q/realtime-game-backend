#!/usr/bin/env bash
#
# verify-chaos.sh - 故障注入 / 长稳 / 断线重连这一类验收的统一入口
#
# 为什么需要它（2026-10-04 补上）：
#   `scripts/verify-all.sh` 是**快速门禁**（约 4 分钟），因此它只收网络路径与服务
#   行为的验收脚本。故障注入那几个脚本（要停 Redis/MySQL、要 `kill -9`、要占满 fd、
#   要跑几十分钟长稳）不适合放进去，于是它们长期**没有任何统一入口**。
#   后果在 TASK-029 时暴露：改了 `src/gateway/` 之后，没有任何机制会提醒重跑这六个
#   脚本；补跑时 `scripts/verify-reconnect.sh` 果然失败——**TASK-026 当时漏改它**
#   （该脚本自己拉起 Room 却没有 `-drain_timeout_ms 1000`，而 Room 默认排空 30 秒）。
#   这个回归躺了整整一个任务周期，详见 docs/devlog.md「TASK-029 之后的重跑结果」。
#   结论与 verify-all.sh 开头那段一样：**重跑必须是一条命令**，否则一定会被跳过。
#
# 用法：
#   bash scripts/verify-chaos.sh                    # 默认 5 个（不含长稳）
#   bash scripts/verify-chaos.sh --include-soak     # 加上 30 分钟长稳
#   bash scripts/verify-chaos.sh --only drain,reconnect
#   bash scripts/verify-chaos.sh --list
#   bash scripts/verify-chaos.sh --fail-fast
#
# 每个脚本的输出保存在 .run/chaos-<名字>.log，失败时直接看那个文件。
#
# 实测耗时（2026-10-04，本机 16 核 / 11 GiB）：
#   dependency-down ~3 分钟 / process-crash ~4 分钟 / drain ~1 分钟 /
#   connection-storm ~3 分钟 / reconnect ~2 分钟；长稳默认 30 分钟（1 小时上限）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
run_dir="$(pwd)/.run"
mkdir -p "$run_dir"

# 顺序：先故障注入（依赖不可用 -> 进程崩溃），再优雅退出，最后压力类与重连。
# 最后一个不跑长稳（30 分钟），要跑就加 --include-soak。
all_scripts=(dependency-down process-crash drain connection-storm reconnect)
declare -A script_path=(
  [dependency-down]="chaos/verify-dependency-down.sh"
  [process-crash]="chaos/verify-process-crash.sh"
  [drain]="chaos/verify-drain.sh"
  [connection-storm]="chaos/verify-connection-storm.sh"
  [reconnect]="scripts/verify-reconnect.sh"
  [soak]="chaos/verify-soak.sh"
)

# 用 while 而不是 `for arg in "$@"`：后者在循环里 `shift` 不生效，
# 带值的选项（`--only X`）会把 X 当成未知参数——实测踩到过。
include_soak=0
fail_fast=0
only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --include-soak) include_soak=1; shift ;;
    --fail-fast) fail_fast=1; shift ;;
    --list)
      echo "会按顺序运行（不含长稳）："
      for s in "${all_scripts[@]}"; do echo "  ${script_path[$s]}"; done
      echo "加 --include-soak 时额外运行："
      echo "  ${script_path[soak]}"
      exit 0
      ;;
    --only) shift; only="${1:-}"; shift ;;
    --only=*) only="${1#--only=}"; shift ;;
    -h | --help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

if [ "$include_soak" -eq 1 ]; then
  all_scripts+=(soak)
fi

if [ -n "$only" ]; then
  selected=()
  IFS=',' read -r -a wanted <<<"$only"
  for w in "${wanted[@]}"; do
    found=0
    for s in "${all_scripts[@]}"; do
      [ "$s" = "$w" ] && { selected+=("$s"); found=1; }
    done
    [ "$found" -eq 1 ] || { echo "x  未知脚本名：$w（用 --list 看可用名字）" >&2; exit 2; }
  done
  all_scripts=("${selected[@]}")
fi

failed=()
started_ms=$(( $(date +%s%N) / 1000000 ))

for name in "${all_scripts[@]}"; do
  script="${script_path[$name]}"
  log="$run_dir/chaos-$name.log"
  echo
  echo "============================================================"
  echo "  $name  ->  $script"
  echo "============================================================"
  # 每个脚本自己起停服务；跑之前先确保没有上一轮残留。
  bash scripts/dev-down.sh >/dev/null 2>&1 || true
  if bash "$script" >"$log" 2>&1; then
    echo "  通过（详细输出：$log）"
  else
    echo "  失败（详细输出：$log）"
    failed+=("$name")
    tail -n 15 "$log" | sed 's/^/    /'
    if [ "$fail_fast" -eq 1 ]; then
      echo
      echo "x  --fail-fast：在 $name 处停止"
      exit 1
    fi
  fi
done

bash scripts/dev-down.sh >/dev/null 2>&1 || true
elapsed_ms=$(( $(date +%s%N) / 1000000 - started_ms ))

echo
echo "============================================================"
echo "  故障注入验收汇总"
echo "============================================================"
for name in "${all_scripts[@]}"; do
  if printf '%s\n' "${failed[@]:-}" | grep -qx "$name"; then
    printf '  %-18s 失败\n' "$name"
  else
    printf '  %-18s 通过\n' "$name"
  fi
done
echo "  ----------------------------------------------------------"
echo "  总计 ${#all_scripts[@]} 个脚本，耗时 $(( elapsed_ms / 1000 ))s"
echo

if [ "${#failed[@]}" -gt 0 ]; then
  echo "有 ${#failed[@]} 个脚本失败：${failed[*]}"
  exit 1
fi
echo "全部通过。"
