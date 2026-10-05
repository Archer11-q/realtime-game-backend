#!/usr/bin/env bash
#
# check-docs.sh - 文档一致性核对（TASK-034，Phase 5）
#
# 为什么要有它：文档一致性在这个项目里反复出问题（Phase 4 收口时就修了 4 处过期
# 状态；TASK-032/033 又各发现几处）。人工核对总是漏，因此把**能机检的**部分做成
# 脚本，并入快速门禁。原则见任务单：规则要写窄、只查能机检的，否则会误报；
# 同时不能是"永远通过"的空壳——它必须能在状态被改坏时失败。
#
# 检查项（每项都是"坏了就退出码非 0"）：
#   C1  markdown 引用的相对文件/目录存在（README.md、CLAUDE.md、docs/**/*.md）
#   C2  scripts/verify-all.sh 列出的脚本都存在
#   C3  scripts/verify-chaos.sh 列出的脚本都存在
#   C4  Phase 5 收口状态一致：README / CLAUDE / roadmap / TASKS 四处都有 Phase 5，
#       且 docs/TASKS.md 里没有遗留的「待确认」任务
#   C5  新文档已入索引：docs/README.md 包含 09-runbook 与 benchmarks
#   C6  关键脚本语法正确（bash -n）
#
# 用法：
#   bash scripts/check-docs.sh          # 全量检查
#   bash scripts/check-docs.sh --list   # 列出检查项
#   bash scripts/check-docs.sh --only C1,C2   # 只跑指定检查

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
repo_root="$(pwd)"

failures=0
list_only=0
only=""
# 用 while 而不是 `for arg in "$@"`：后者在循环里 shift 不生效（verify-chaos.sh 踩过）
while [ $# -gt 0 ]; do
  case "$1" in
    --list) list_only=1; shift ;;
    --only=*) only="${1#--only=}"; shift ;;
    --only) shift || true; only="${1:-}"; shift 2>/dev/null || true ;;
    -h | --help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

want() { # <检查项>：返回 0 表示要跑
  [ -n "$only" ] || return 0
  case ",$only," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}
note() { echo "  $*"; }
pass() { echo "v  $*"; }
fail() { echo "x  $*"; failures=$((failures + 1)); }

if [ "$list_only" -eq 1 ]; then
  echo "可机检的文档一致性检查项："
  grep -oE '^#   C[0-9] +[^（(]+' "$0" | sed 's/^#   //'
  exit 0
fi

# ---------------------------------------------------------------- C1 文件引用
if want C1; then
  echo "===== C1 markdown 引用的相对文件/目录存在 ====="
  # 收集需要扫描的 md 文件（排除 raw 数据与 .run 运行时产物）
  mapfile -t md_files < <(
    find "$repo_root" -name '*.md' \
      -not -path "$repo_root/docs/benchmarks/raw/*" \
      -not -path "$repo_root/.git/*" \
      -not -path "$repo_root/.run/*" \
      -not -path "$repo_root/build/*" \
      -not -path "$repo_root/web/node_modules/*" \
      -not -path "$repo_root/node_modules/*" | sort
  )
  [ "${#md_files[@]}" -gt 0 ] || fail "没有扫到任何 markdown 文件"
  checked=0
  for md in "${md_files[@]}"; do
    dir="$(dirname "$md")"
    # grep -n 输出 <行号>:<匹配>；匹配形如 ](目标)
    while IFS= read -r mline; do
      lineno="${mline%%:*}"
      link="${mline#*](}"
      link="${link%)*}"
      target="$link"
      case "$target" in
        "" | \#* | http://* | https://* | mailto:* | data:*) continue ;;
      esac
      # 去掉 #锚点 与 行号后缀（:NN 是行号标注，不是路径）
      target="${target%%#*}"
      target="${target%%:[0-9]*}"
      case "$target" in
        /*) abs="$repo_root$target" ;;
        ./*|../*) abs="$(realpath -m "$dir/$target")" ;;
        *)
          # 先按"相对当前 md 所在目录"试，再按"相对仓库根"试
          if [ -e "$dir/$target" ]; then abs="$dir/$target"
          elif [ -e "$repo_root/$target" ]; then abs="$repo_root/$target"
          else abs="" ; fi
          ;;
      esac
      checked=$((checked + 1))
      if [ -z "$abs" ] || [ ! -e "$abs" ]; then
        # 允许的例外：`.run/`（运行时产物，不入库）
        case "$target" in .run/*|../.run/*) continue ;; esac
        fail "$(realpath --relative-to="$repo_root" "$md"):$lineno 链接目标不存在：$target"
      fi
    done < <(grep -noE '\]\([^)]+\)' "$md" || true)
  done
  pass "共核对 $checked 个相对链接"
fi

# ---------------------------------------------------------------- C2 verify-all
if want C2; then
  echo "===== C2 scripts/verify-all.sh 列出的脚本存在 ====="
  all=$(sed -n 's/^all_scripts=(//p' scripts/verify-all.sh | tr -d ')')
  for name in $all; do
    if [ -f "scripts/$name.sh" ]; then
      pass "scripts/$name.sh"
    else
      fail "scripts/verify-all.sh 引用 scripts/$name.sh 但文件不存在"
    fi
  done
fi

# ---------------------------------------------------------------- C3 verify-chaos
if want C3; then
  echo "===== C3 scripts/verify-chaos.sh 列出的脚本存在 ====="
  while IFS= read -r line; do
    [[ "$line" =~ ^[[:space:]]*\[([^]]+)\]=\"([^\"]+)\" ]] || continue
    name="${BASH_REMATCH[1]}"
    path="${BASH_REMATCH[2]}"
    if [ -f "$path" ]; then
      pass "$name -> $path"
    else
      fail "scripts/verify-chaos.sh 引用 $name -> $path 但文件不存在"
    fi
  done < <(grep -E '^\s*\[[^]]+\]="[^"]+"' scripts/verify-chaos.sh)
fi

# ---------------------------------------------------------------- C4 阶段状态
if want C4; then
  echo "===== C4 Phase 5 收口状态一致 ====="
  for needle in "Phase 5" "## 8. Phase 5" "## Phase 5 任务拆分"; do
    case "$needle" in
      "Phase 5") targets="README.md CLAUDE.md docs/02-roadmap.md" ;;
      "## 8. Phase 5") targets="docs/02-roadmap.md" ;;
      "## Phase 5 任务拆分") targets="docs/TASKS.md" ;;
    esac
    for f in $targets; do
      if grep -qF -- "$needle" "$f"; then
        pass "$f 含 $needle"
      else
        fail "$f 缺少 $needle（阶段状态过期？）"
      fi
    done
  done
  # TASKS.md 不应残留「待确认」任务（Phase 5 收口完成判据）
  if grep -qF "状态：**待确认**" docs/TASKS.md; then
    fail "docs/TASKS.md 仍有「状态：**待确认**」的任务"
  else
    pass "docs/TASKS.md 无「待确认」任务"
  fi
fi

# ---------------------------------------------------------------- C5 文档索引
if want C5; then
  echo "===== C5 新文档已入索引 ====="
  for needle in "09-runbook.md" "benchmarks/README.md"; do
    if grep -qF -- "$needle" docs/README.md; then
      pass "docs/README.md 含 $needle"
    else
      fail "docs/README.md 未索引 $needle"
    fi
  done
fi

# ---------------------------------------------------------------- C6 语法
if want C6; then
  echo "===== C6 关键脚本 bash -n ====="
  for s in scripts/verify-all.sh scripts/verify-chaos.sh scripts/demo.sh scripts/check-docs.sh; do
    if bash -n "$s" 2>/dev/null; then
      pass "$s"
    else
      fail "$s 语法错误"
    fi
  done
fi

echo
if [ "$failures" -gt 0 ]; then
  echo "文档一致性检查失败 $failures 项。"
  exit 1
fi
echo "文档一致性检查全部通过。"
