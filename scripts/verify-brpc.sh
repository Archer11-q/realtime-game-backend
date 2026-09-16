#!/usr/bin/env bash
#
# verify-brpc.sh - 已由 verify-login.sh 取代
#
# 本脚本原先用于 TASK-004 的 brpc 工具链验证，构建目标是 rgbt_brpc_smoke。
# 进入 TASK-005 后，该冒烟程序已被正式的 Gateway 实现取代并删除，
# 因此本脚本不再构建任何东西，只把调用者指向当前有效的验收入口。
#
# 保留本文件而不是直接删除，是为了让此前记录的验收命令
# （见 docs/devlog.md 的 TASK-004 记录）仍然能找到入口，不至于静默失效。

set -uo pipefail

cat <<'NOTICE'
verify-brpc.sh 已废弃。

TASK-004 的 brpc 验证目标是确认 CMake + GCC 能编译并链接 brpc，
该结论已记录在 docs/devlog.md 的「TASK-004 实施记录」中，无需重复执行。

当前有效的验收入口：

  bash scripts/verify-login.sh    TASK-005：Gateway 登录切片端到端验收

其余通用命令：

  bash scripts/verify.sh          既有 debug / release / asan 三预设构建与测试
  bash scripts/verify-deps.sh     Redis / MySQL 容器依赖

如需只验证 brpc 依赖本身是否就绪，直接使用 brpc-debug 预设：

  cmake --preset brpc-debug && cmake --build --preset brpc-debug
NOTICE
