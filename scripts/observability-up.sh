#!/usr/bin/env bash
#
# observability-up.sh - 拉起 Prometheus + Grafana，并做一次真实抓取自检
#
#   bash scripts/observability-up.sh
#
# 为什么脚本里带自检而不只是 `docker compose up`：
#   监控栈"容器起来了"与"真的抓到了指标"是两件事。实测踩过多次：容器 healthy、
#   Grafana 能打开，而 Prometheus 的 target 全是 down（容器访问不到宿主上的服务）。
#   因此这里在起完之后立刻用 Prometheus 自己的 API 验证 target 是否 up，
#   不通过就明确报错并打印排查线索。
#
# 退出码：0 表示两个容器健康且三个 target 至少有一个 up；1 表示有明确失败。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

compose_file="deploy/compose/docker-compose.observability.yml"
env_file="deploy/compose/.env"
env_example="deploy/compose/.env.example"

failures=()
fail() {
  failures+=("$1")
  echo "x  $1"
}
ok() { echo "v  $1"; }

# 服务端口必须传给 Prometheus：抓取目标由这几个变量决定。
# 默认值与 prometheus.yml 保持一致；调用方可以覆盖。
: "${RGBT_GATEWAY_PORT:=8080}"
: "${RGBT_MATCH_PORT:=8082}"
: "${RGBT_ROOM_PORT:=8083}"
: "${PROMETHEUS_PORT:=9090}"
: "${GRAFANA_PORT:=3000}"
export RGBT_GATEWAY_PORT RGBT_MATCH_PORT RGBT_ROOM_PORT PROMETHEUS_PORT GRAFANA_PORT

echo "===== 前置检查 ====="
if ! docker info >/dev/null 2>&1; then
  echo "Docker 引擎不可用。请先启动 Docker Desktop。" >&2
  exit 1
fi
ok "Docker 引擎可用"

[ -f "$env_file" ] || { [ -f "$env_example" ] && cp "$env_example" "$env_file"; }

for port in "$PROMETHEUS_PORT" "$GRAFANA_PORT"; do
  if ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$port\$"; then
    # 端口被占用时可能是上一次没停干净，给出可操作的信息而不是直接失败。
    if docker ps --format '{{.Names}}' | grep -qE '^rgbt-(prometheus|grafana)$'; then
      ok "端口 $port 已被本项目的监控容器占用（可先执行 observability-down.sh）"
    else
      fail "端口 $port 被别的进程占用"
    fi
  else
    ok "端口 $port 空闲"
  fi
done

echo
echo "===== 拉起监控栈 ====="
# 服务端口通过环境变量传给容器（prometheus.yml 里用 ${...} 引用，
# 容器侧由 --config.expand-env 展开）。
# 显式接收退出码：写成 `cmd; if [ $? -eq 0 ]` 时，`if` 里取到的不是 cmd 的状态
# （实测把成功的 up 报成了失败）。
compose_rc=0
docker compose -f "$compose_file" up -d >/tmp/observability-up.log 2>&1 || compose_rc=$?
if [ "$compose_rc" -eq 0 ]; then
  ok "compose up 成功"
else
  fail "compose up 失败（退出码 $compose_rc）"
  tail -20 /tmp/observability-up.log
fi

echo
echo "===== 等待健康 ====="
wait_healthy() {
  local container="$1" label="$2"
  for _ in $(seq 1 40); do
    if [ "$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null)" = "healthy" ]; then
      ok "$label 健康"
      return 0
    fi
    sleep 2
  done
  fail "$label 未在 80 秒内 healthy"
  docker logs --tail 15 "$container" 2>&1 | sed 's/^/    /'
  return 1
}
wait_healthy rgbt-prometheus "Prometheus"
wait_healthy rgbt-grafana "Grafana"

echo
echo "===== 抓取自检（关键：容器能不能抓到宿主上的服务）====="
prom="http://127.0.0.1:${PROMETHEUS_PORT}"

# 先看服务本身在不在：不在的话抓取必然失败，而线索完全不同。
for pair in "Gateway:$RGBT_GATEWAY_PORT" "Match:$RGBT_MATCH_PORT" "Room:$RGBT_ROOM_PORT"; do
  name="${pair%%:*}"
  port="${pair#*:}"
  if curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$port/metrics"; then
    ok "$name 的 /metrics 在宿主机上可访问（端口 $port）"
  else
    echo "!  $name 的 /metrics 不可访问（端口 $port）——服务可能没在跑；"
    echo "   监控栈可以先起，但要看到曲线需要先启动服务（scripts/dev-up.sh）"
  fi
done

# 等至少两轮抓取（scrape_interval 是 5 秒）。
sleep 12
targets_json=$(curl -s --max-time 5 "$prom/api/v1/targets" 2>/dev/null)
if [ -z "$targets_json" ]; then
  fail "Prometheus API 无响应（$prom/api/v1/targets）"
else
  summary=$(printf '%s' "$targets_json" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    print("PARSE_FAIL")
    sys.exit(0)
targets = data.get("data", {}).get("activeTargets", [])
up = [t for t in targets if t.get("health") == "up"]
down = [t for t in targets if t.get("health") != "up"]
print("%d %d" % (len(up), len(down)))
for t in down:
    print("DOWN %s %s" % (t.get("labels", {}).get("job"), t.get("lastError", "")))
' 2>/dev/null)
  up_count=$(printf '%s' "$summary" | head -1 | cut -d' ' -f1)
  down_count=$(printf '%s' "$summary" | head -1 | cut -d' ' -f2)

  if [ "${up_count:-0}" -ge 1 ]; then
    ok "Prometheus 有 $up_count 个 target 处于 up"
  else
    fail "Prometheus 没有任何 target 处于 up（容器访问不到宿主上的服务）"
    printf '%s' "$summary" | tail -n +2 | sed 's/^/    /'
  fi
  if [ "${down_count:-0}" -ge 1 ]; then
    echo "!  有 $down_count 个 target 未 up（服务没起时属预期）："
    printf '%s' "$summary" | tail -n +2 | sed 's/^/    /'
  fi
fi

echo
echo "===== 结果 ====="
echo "  Prometheus: $prom"
echo "  Grafana:    http://127.0.0.1:${GRAFANA_PORT}  （匿名只读，直接打开）"
echo "  面板:       Realtime Game Backend / Realtime Game Backend — 总览"
echo
if [ "${#failures[@]}" -eq 0 ]; then
  echo "监控栈已就绪。"
  exit 0
fi
echo "存在 ${#failures[@]} 项失败："
for item in "${failures[@]}"; do echo "  - $item"; done
exit 1
