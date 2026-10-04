#!/usr/bin/env python3
"""连接风暴的基线探针（TASK-025）。

作用：在风暴**之前**就建立 N 条 SSE 长连接并一直持有，按秒记录每条流收到多少事件。
风暴结束后用它的输出回答本任务的核心问题之一：**已建立的连接是否受影响**
（有没有被断开、风暴期间还在不在收推送）。

为什么复用 `bench/loadgen.py` 而不另写一套 HTTP/SSE 客户端：
登录 / 入队 / 等配对 / 进房 / 开流这几步 loadgen 都已经实现，而且带踩坑记录
（见 loadgen.py 里 `Bot` 的注释：结果 TTL 会让"打完一局立刻重入队"撞上旧房间等）。
重写一遍只会把同样的坑再踩一次；这里只把它当成"一个玩家的网络层"来用。

身份：默认用合成身份 `bench-00990` 起的连续 N 个（`--index-start`），与风暴用的
`bench-00000` 起那一段**错开**。否则基线玩家会被风暴当成同一个 `player_id`，
测出来的失败是夹具造成的，不是服务行为。

基线**不攻击**：不攻击的对局要跑到 600 帧（60 秒）才按超时结束，因此只要观察窗口
短于 60 秒，基线流就不会因为"对局正常结束"而关闭——这样"流断了"才能归因到风暴。

用法：
  python3 chaos/storm_baseline.py --gateway 127.0.0.1:8080 --connections 10 \\
      --index-start 990 --duration 45 --out /tmp/baseline.json
"""

import argparse
import asyncio
import json
import os
import sys
import time

_BENCH_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench")
sys.path.insert(0, _BENCH_DIR)

import loadgen  # noqa: E402  （必须先加 path）


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="连接风暴基线探针（TASK-025）")
    parser.add_argument("--gateway", default="127.0.0.1:8080", help="Gateway 地址 host:port")
    parser.add_argument("--connections", type=int, default=10, help="要持有的 SSE 连接数")
    parser.add_argument("--index-start", type=int, default=990, help="合成身份起始序号")
    parser.add_argument("--duration", type=float, default=45.0, help="观察窗口（秒）")
    parser.add_argument("--timeout", type=float, default=10.0, help="单次请求超时（秒）")
    parser.add_argument("--tick-seconds", type=float, default=1.0, help="按秒输出的间隔")
    parser.add_argument("--out", help="把结果 JSON 写到这个路径")
    return parser.parse_args(argv)


class Probe:
    """按秒分桶的事件计数。"""

    def __init__(self):
        self.total = 0
        self.by_name = {}
        self.by_second = {}

    def bump(self, name):
        self.total += 1
        self.by_name[name] = self.by_name.get(name, 0) + 1
        second = int(time.time())
        self.by_second[second] = self.by_second.get(second, 0) + 1


async def async_main(args):
    host, _, port_text = args.gateway.rpartition(":")
    bot_args = loadgen.parse_args(
        [
            "--players", str(args.connections),
            "--bench-accounts",
            "--gateway", args.gateway,
            "--timeout", str(args.timeout),
            "--connections-per-bot", "1",
            "--duration", str(args.duration),
        ]
    )
    # loadgen 自己是在 async_main 里把 `--gateway` 拆成 host/port 的（`Bot` 用的是
    # `args.host` / `args.port`）。这里只调用了它的 parse_args，因此必须自己补上这两项，
    # 否则 `Bot.__init__` 会以 `AttributeError: 'Namespace' object has no attribute
    # 'host'` 失败（首次运行实测踩到）。
    bot_args.host = host
    bot_args.port = int(port_text or "8080")

    probe = Probe()
    stats = loadgen.Stats(players=args.connections)
    bots = []
    for index in range(args.connections):
        bot = loadgen.Bot(index, bot_args, stats)
        account_index = args.index_start + index
        bot.account = "%s%0*d" % (loadgen.BENCH_PREFIX, loadgen.BENCH_DIGITS, account_index)
        bot.password = loadgen.BENCH_PASSWORD
        bots.append(bot)

    deadline = time.monotonic() + args.duration
    logins = await asyncio.gather(*(bot.login() for bot in bots), return_exceptions=True)
    login_ok = sum(1 for value in logins if value is True)

    await asyncio.gather(
        *(bot.enqueue_and_wait(deadline) for bot in bots), return_exceptions=True
    )
    paired = sum(1 for bot in bots if bot.room_id)
    await asyncio.gather(*(bot.join_room() for bot in bots), return_exceptions=True)

    readers = await asyncio.gather(
        *(bot.open_stream(deadline) for bot in bots), return_exceptions=True
    )

    stop = asyncio.Event()
    reasons = {}

    async def on_event(name, _data):
        probe.bump(name)
        return None

    async def pump(tag, reader):
        reason = await loadgen.read_sse_events(reader, on_event, stop)
        reasons[tag] = reason
        return reason

    tasks = []
    for index, reader in enumerate(readers):
        if isinstance(reader, asyncio.StreamReader):
            tasks.append(asyncio.create_task(pump(index, reader)))

    opened = len(tasks)
    print(
        "baseline ready opened=%d/%d login_ok=%d paired=%d epoch=%.3f"
        % (opened, args.connections, login_ok, paired, time.time()),
        flush=True,
    )

    # 按秒输出：连接数与本秒事件数。风暴窗口由 shell 侧用 ts 对齐，
    # 这样"风暴期间基线还在收推送"是可对账的，而不是只看总数。
    end_at = time.time() + args.duration
    last_total = probe.total
    while time.time() < end_at:
        await asyncio.sleep(args.tick_seconds)
        connected = sum(1 for task in tasks if not task.done())
        print(
            "baseline tick ts=%d connected=%d events_this_second=%d events_total=%d"
            % (int(time.time()), connected, probe.total - last_total, probe.total),
            flush=True,
        )
        last_total = probe.total

    # 必须在 stop.set() **之前**统计存活连接：stop 之后所有 pump 任务都会结束，
    # 在那里统计只会永远得到 0——首次运行就是这样假失败的。
    # `reasons` 里此刻已有的条目，就是"在观察窗口结束前就断掉的流"，
    # 这正是"连接有没有被风暴打断"的直接证据。
    connected_at_end = sum(1 for task in tasks if not task.done())
    closed_during_window = dict(reasons)

    stop.set()
    await asyncio.gather(*tasks, return_exceptions=True)
    for bot in bots:
        await bot.close()

    payload = {
        "connections": args.connections,
        "index_start": args.index_start,
        "login_ok": login_ok,
        "paired": paired,
        "stream_opened": opened,
        "still_connected": connected_at_end,
        "closed_during_window": closed_during_window,
        "end_reasons": reasons,
        "events_total": probe.total,
        "events_by_name": dict(sorted(probe.by_name.items())),
        "events_by_second": {str(k): v for k, v in sorted(probe.by_second.items())},
        "wall_seconds": args.duration,
    }
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(
        "baseline done opened=%d still_connected=%d events_total=%d"
        % (opened, payload["still_connected"], probe.total),
        flush=True,
    )
    return 0 if opened > 0 else 1


def main():
    args = parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())