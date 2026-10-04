#!/usr/bin/env python3
"""长稳压载与主动断连/重连探针（TASK-027）。

职责：维持 N 个玩家**持续处于"在房间里 + 有一条 SSE"**的状态，并按周期主动断开
其中一部分（关掉 socket）让它们重连。

三个设计点，都是第一版实测踩出来的：

1. **必须自己维持"有局可打"。** 不攻击的对局会在 600 帧（60 秒）后按超时结束，
   服务端随即发 `room.finished` 并关闭流。第一版只建立一次连接，于是压载跑到
   60~90 秒就全空了（实测 SSE 连接数从 12 掉到 4）。现在每个玩家有一个
   maintainer 循环：流一结束就判断"是房间打完了还是被我们断开了"，打完就重新排队
   换一局，被断开就原地重连（同一房间，`JoinRoom` 幂等）。
2. **断连用 `Bot._stream_writer.close()`**（显式关 socket），而不是只取消读取任务：
   后者在服务端看来连接还在，要等下一次写（心跳最长 15 秒）才发现，测不出"立即回落"。
3. **退出时取消任务，而不是只置停止标志。** 读取任务可能正阻塞在没有流量的
   `readuntil()` 上，`stop.set()` 之后它不会自己返回——第一版因此在压载结束后
   永远不退出（实测卡了 400 秒还没结束）。

复用 `bench/loadgen.py` 的网络层（登录/入队/配对/进房/开流），理由同
`chaos/storm_baseline.py`：那几步踩过的坑都已经写在 loadgen 的注释里。

用法：
  python3 chaos/soak_churn.py --gateway 127.0.0.1:8080 --players 50 \\
      --duration 1800 --churn-every 60 --churn-fraction 0.2 --out /tmp/soak.json
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
    parser = argparse.ArgumentParser(description="长稳压载与断连/重连探针（TASK-027）")
    parser.add_argument("--gateway", default="127.0.0.1:8080", help="Gateway 地址 host:port")
    parser.add_argument("--players", type=int, default=50, help="并发玩家数（= SSE 连接数）")
    parser.add_argument("--index-start", type=int, default=0, help="合成身份起始序号")
    parser.add_argument("--duration", type=float, default=1800.0, help="压载时长（秒）")
    parser.add_argument("--churn-every", type=float, default=60.0, help="每隔多少秒断开一批")
    parser.add_argument(
        "--churn-fraction", type=float, default=0.2, help="每批断开的比例（0~1）"
    )
    parser.add_argument("--timeout", type=float, default=10.0, help="单次请求超时（秒）")
    parser.add_argument("--out", help="把结果 JSON 写到这个路径")
    return parser.parse_args(argv)


async def async_main(args):
    host, _, port_text = args.gateway.rpartition(":")
    bot_args = loadgen.parse_args(
        [
            "--players", str(args.players),
            "--bench-accounts",
            "--gateway", args.gateway,
            "--timeout", str(args.timeout),
            "--connections-per-bot", "1",
            "--duration", str(args.duration),
        ]
    )
    # loadgen 是在它自己的 async_main 里把 --gateway 拆成 host/port 的。
    bot_args.host = host
    bot_args.port = int(port_text or "8080")

    stats = loadgen.Stats(players=args.players)
    bots = []
    for index in range(args.players):
        bot = loadgen.Bot(index, bot_args, stats)
        account_index = args.index_start + index
        bot.account = "%s%0*d" % (loadgen.BENCH_PREFIX, loadgen.BENCH_DIGITS, account_index)
        bot.password = loadgen.BENCH_PASSWORD
        bots.append(bot)

    deadline = time.monotonic() + args.duration
    stop = asyncio.Event()

    counters = {
        "connected": 0,        # 当前在线的流（maintainer 维护）
        "streams_opened": 0,   # 累计建立过多少条流
        "requeued": 0,         # 因为"房间打完了"而重新排队的次数
        "reconnected": 0,      # 因为"被我们断开"而原地重连的次数
        "open_failed": 0,
        "pair_failed": 0,
        "login_failed": 0,
        "events": 0,
    }
    end_reasons = {}

    async def on_event(_name, _data):
        counters["events"] += 1
        return None

    async def ensure_room(bot, deadline):
        """拿到一个**新**房间。反复「入队 + 轮询」，而不是只入队一次。

        为什么不能直接用 `loadgen.Bot.enqueue_and_wait`：它只在开头入队一次，
        之后一直轮询。而对局刚打完时 Match 仍保留着**上一局的结果**（TTL 内），
        轮询会拿到那个已经结束的房间，被 `last_finished_room` 判为 stale 后继续
        轮询——于是玩家**永远等不到新房间**，连接数就再也回不到 N。
        第一轮 30 分钟跑到 171 秒时实测到连接数从 50 掉到 20 并卡住，就是这个原因。
        """
        while not stop.is_set() and time.monotonic() < deadline:
            await bot.post(
                loadgen.PATH_ENQUEUE,
                {"token": bot.token, "request_id": loadgen.new_request_id("soak-enqueue")},
            )
            for _ in range(12):  # 最多等约 3.6 秒
                await asyncio.sleep(0.3)
                status, payload = await bot.post(
                    loadgen.PATH_MATCH_CURRENT,
                    {"token": bot.token, "request_id": loadgen.new_request_id("soak-poll")},
                )
                if status != 200:
                    continue
                match = payload.get("match") or {}
                room_id = str(match.get("room_id") or "")
                if room_id and room_id != bot.last_finished_room:
                    bot.room_id = room_id
                    return True
            # 这一轮没配上（多半是旧结果还没过期）→ 再来一轮，重新入队。
        return False

    async def maintain(bot):
        """维持"这个玩家在房间里并且有一条流"。流结束 → 判断原因 → 重连或换局。"""
        while not stop.is_set() and time.monotonic() < deadline:
            # 先登录（一次即可，会话在 Redis 里）。**这一步不能漏**：漏了的话
            # enqueue 会带着空 token 发出去，服务端一律 400 invalid_argument，
            # 表现成"配对一直失败"，很难一眼看出根因（第一版就是这么错的）。
            if not bot.token:
                if not await bot.login():
                    counters["login_failed"] += 1
                    await asyncio.sleep(1.0)
                    continue

            if not bot.room_id:
                if not await ensure_room(bot, deadline):
                    counters["pair_failed"] += 1
                    await asyncio.sleep(1.0)
                    continue
                await bot.join_room()

            reader = await bot.open_stream(deadline)
            if not isinstance(reader, asyncio.StreamReader):
                counters["open_failed"] += 1
                bot.room_id = ""  # 开不出来就换一局，别在原地死循环
                await asyncio.sleep(0.5)
                continue

            counters["streams_opened"] += 1
            counters["connected"] += 1
            started = time.monotonic()
            reason = await loadgen.read_sse_events(reader, on_event, stop)
            counters["connected"] -= 1
            lived = time.monotonic() - started
            end_reasons[reason] = end_reasons.get(reason, 0) + 1

            if stop.is_set() or time.monotonic() >= deadline:
                break
            if reason in ("finished", "aborted") or lived < 2.0:
                # 房间打完了（或新流立刻被关：说明这一局已经结束）→ 换一局。
                counters["requeued"] += 1
                bot.last_finished_room = bot.room_id
                bot.room_id = ""
            else:
                # 被我们主动断开（reason 通常是 eof）→ 原地重连同一房间。
                counters["reconnected"] += 1

    tasks = [asyncio.create_task(maintain(bot)) for bot in bots]

    # 等压载真正建立：至少有一半的流在线，才开始计时与采样。
    ready_deadline = time.monotonic() + min(120.0, max(30.0, args.duration / 4))
    while time.monotonic() < ready_deadline:
        if counters["connected"] >= max(1, args.players // 2):
            break
        await asyncio.sleep(0.2)

    print(
        "soak ready connected=%d/%d streams_opened=%d pair_failed=%d open_failed=%d epoch=%.3f"
        % (counters["connected"], args.players, counters["streams_opened"],
           counters["pair_failed"], counters["open_failed"], time.time()),
        flush=True,
    )
    if counters["connected"] == 0:
        print("soak fatal: 一条流都没建立，压载无意义", flush=True)
        stop.set()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        return 1

    batch = max(1, int(round(args.players * args.churn_fraction)))
    if batch > args.players:
        batch = args.players

    cycle = 0
    closed_total = 0
    cursor = 0
    next_churn = time.monotonic() + args.churn_every
    while time.monotonic() < deadline:
        await asyncio.sleep(0.2)
        if time.monotonic() < next_churn:
            continue
        next_churn += args.churn_every
        cycle += 1

        closed = 0
        for _ in range(batch):
            index = cursor % args.players
            cursor += 1
            writer = getattr(bots[index], "_stream_writer", None)
            if writer is not None:
                writer.close()
                bots[index]._stream_writer = None
                closed += 1
        closed_total += closed
        print(
            "soak churn cycle=%d closed=%d connected=%d opened_total=%d requeued=%d "
            "reconnected=%d events=%d"
            % (cycle, closed, counters["connected"], counters["streams_opened"],
               counters["requeued"], counters["reconnected"], counters["events"]),
            flush=True,
        )

    connected_at_end = counters["connected"]

    # 停止：**必须取消任务**。读取任务可能正阻塞在没有流量的 readuntil() 上，
    # 只置 stop 它不会自己返回（第一版因此永不退出）。
    stop.set()
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    for bot in bots:
        await bot.close()

    payload = {
        "players": args.players,
        "index_start": args.index_start,
        "duration_seconds": args.duration,
        "churn_every_seconds": args.churn_every,
        "churn_fraction": args.churn_fraction,
        "cycles": cycle,
        "closed_total": closed_total,
        "connected_at_end": connected_at_end,
        "streams_opened": counters["streams_opened"],
        "requeued": counters["requeued"],
        "reconnected": counters["reconnected"],
        "open_failed": counters["open_failed"],
        "pair_failed": counters["pair_failed"],
        "login_failed": counters["login_failed"],
        "events_total": counters["events"],
        "end_reasons": end_reasons,
    }
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(
        "soak done cycles=%d closed=%d connected_at_end=%d streams_opened=%d "
        "requeued=%d reconnected=%d events=%d"
        % (cycle, closed_total, connected_at_end, counters["streams_opened"],
           counters["requeued"], counters["reconnected"], counters["events"]),
        flush=True,
    )
    return 0


def main():
    args = parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())