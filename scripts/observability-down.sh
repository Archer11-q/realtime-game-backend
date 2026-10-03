#!/usr/bin/env bash
#
# observability-down.sh - 停掉监控栈
#
#   bash scripts/observability-down.sh            # 停容器，保留数据卷
#   bash scripts/observability-down.sh --purge     # 连数据卷一起删（配置与面板会重新 provisioning）
#
# 为什么默认保留数据卷：Prometheus 的历史数据与 Grafana 的 admin 设置都在卷里，
# 每次停都删会让"上次看到的曲线"无法复查。要彻底干净时显式 --purge。
#
# **不触碰业务依赖**（Redis/MySQL）：它们由 docker-compose.yml 管理，
# 停监控时顺手停掉数据库会让下一次跑测试莫名其妙地失败。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.yml"
observability_file="deploy/compose/docker-compose.observability.yml"

purge=0
for arg in "$@"; do
  case "$arg" in
    --purge) purge=1 ;;
    -h | --help)
      sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "未知参数: $arg" >&2
      exit 2
      ;;
  esac
done

echo "===== 停止监控栈 ====="
# 显式接收退出码，理由同 observability-up.sh。
down_rc=0
if [ "$purge" -eq 1 ]; then
  docker compose -f "$observability_file" down -v >/tmp/observability-down.log 2>&1 || down_rc=$?
  action="停止并删除数据卷"
else
  docker compose -f "$observability_file" down >/tmp/observability-down.log 2>&1 || down_rc=$?
  action="停止（保留数据卷）"
fi

if [ "$down_rc" -eq 0 ]; then
  echo "v  已$action"
else
  echo "x  compose down 失败（退出码 $down_rc）"
  tail -15 /tmp/observability-down.log
  exit 1
fi

echo
echo "===== 确认业务依赖仍在运行（监控栈与它们无关）====="
for container in rgbt-redis rgbt-mysql; do
  if [ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" = "true" ]; then
    echo "v  $container 仍在运行"
  else
    echo "!  $container 未运行（如需使用请执行 docker compose -f $compose_file up -d）"
  fi
done

exit 0
