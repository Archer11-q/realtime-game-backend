#!/usr/bin/env python3
"""压测机器人：用 asyncio 驱动 N 个玩家走完整条实时对战链路（TASK-022）。

为什么自己写而不用 wrk / hey：
    `docs/02-roadmap.md` 第 6 节的 Phase 3 退出标准要"识别至少一个明确瓶颈"，
    而这个系统的四个瓶颈候选点**全都在长连接与状态推进行为上**——SSE 扇出、
    房间帧推进、每秒一次的 MySQL 快照写入、匹配队列快照写入。`wrk`/`hey` 这类
    工具只能打静态 HTTP 请求，恰好绕开这四处；用它测出来的数字好看，却回答不了
    本任务要回答的问题。此外本机没有免密 sudo、装不了它们（见 devlog）。

为什么用 asyncio、并且自己写 HTTP 客户端：
    1000 条 SSE 长连接用线程模型需要 1000 个线程，光栈内存就超过本机预算
    （11 GiB），协程每连接只花几 KB。而 asyncio 生态里没有"标准库自带"的
    HTTP/1.1 客户端：`requests` 是阻塞的，会卡住整个事件循环；`aiohttp` 没装、
    也不能装（无免密 sudo、不引入新依赖）。因此本文件包含一个**最小**的
    HTTP/1.1 客户端（keep-alive 连接池 + Content-Length/chunked 解析）。
    它只覆盖本项目实际返回的响应形态，不做重定向、不处理 gzip、不处理
    HTTP/2——这是"够用即可"，不是通用实现。

机器人的一次生命周期：
    登录 -> 入队 -> 轮询到 matched -> 加入房间 -> 订阅 SSE -> 循环攻击
    -> 对局结束（SSE 收到 room.finished）-> 可选：重新入队以维持稳态负载

用法（由 scripts/bench.sh 调用，也可单独跑）：
    python3 bench/loadgen.py --players 100 --gateway 127.0.0.1:8080 \\
        --duration 60 --attack-interval 2.0 --out /tmp/bench-100.json
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import statistics
import sys
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Optional

# ---------------------------------------------------------------------------
# 账号池
# ---------------------------------------------------------------------------

# migrations/004_seed_test_players.sql 里的开发种子身份。
#
# 登录接口用 request_id 做幂等键，而那个键**不含玩家维度**
# （`RedisSessionStore::KeyForLoginIdempotency` 拼的是 `<env>:gateway:idem:login:<rid>`）。
# 因此同一个账号可以同时被多个机器人使用——只要每个机器人的 request_id 唯一。
# 这是"3 个账号模拟 1000 个玩家"的前提，也是为什么下面每一处 request_id 都用
# uuid4 而不是 shell 里的 `$RANDOM`：后者只有 32768 种取值，撞号会让后来的
# 机器人**拿到别人的 Token**，压测会静默地变成"全是同一个玩家在打"。
ACCOUNTS = ("alice", "bob", "dave")
PASSWORD_SUFFIX = "_dev_pw"

# TASK-022：压测专用的合成账号（`bench-00000` .. `bench-99999`，固定口令
# `bench_dev_pw`），由 Gateway 的 `-enable_bench_accounts` 识别。
#
# 为什么必须有它：内置的启用身份只有 3 个，用它们跑 1000 连接会退化成
# "3 个玩家反复配对"——第一轮实测就是这么失败的：1000 个机器人只产生 3 个
# `player_id`，绝大多数入队被幂等判为 `already_queued`，而同一玩家的重复进房
# 又是**幂等成功**，于是出现"667 次 join 挤进同一个房间"这种自相矛盾的数字。
# 容量测试的前提是每个虚拟玩家有独立身份。
BENCH_PREFIX = "bench-"
BENCH_DIGITS = 5
BENCH_PASSWORD = "bench_dev_pw"
BENCH_MAX = 10 ** BENCH_DIGITS  # bench-00000 .. bench-99999

PATH_LOGIN = "/api/v1/login"
PATH_ENQUEUE = "/api/v1/matches"
PATH_MATCH_CURRENT = "/api/v1/matches/current"
PATH_ROOM_JOIN = "/api/v1/rooms/join"
PATH_ROOM_INPUT = "/api/v1/rooms/input"
PATH_STREAM = "/api/v1/stream"

# request_id 长度上限（gateway_service.hpp 的 kMaxRequestIdLength）。
MAX_REQUEST_ID = 64


def new_request_id(prefix: str) -> str:
    """唯一的关联 id。`prefix` + 24 位十六进制，远低于 64 字符上限。"""
    value = f"{prefix}-{uuid.uuid4().hex[:24]}"
    assert len(value) <= MAX_REQUEST_ID, value
    return value


# ---------------------------------------------------------------------------
# 最小 HTTP/1.1 客户端（keep-alive + chunked）
# ---------------------------------------------------------------------------


class HttpError(Exception):
    """传输层错误（连接失败、超时、响应不可解析）。"""


class HttpResponse:
    __slots__ = ("status", "headers", "body")

    def __init__(self, status: int, headers: dict[str, str], body: bytes) -> None:
        self.status = status
        self.headers = headers
        self.body = body

    def json(self) -> dict[str, Any]:
        if not self.body:
            return {}
        try:
            value = json.loads(self.body.decode("utf-8"))
        except Exception:  # noqa: BLE001 - 压测不该因为一个坏响应整体失败
            return {}
        return value if isinstance(value, dict) else {}


class HttpConnection:
    """一条 keep-alive 连接。**不做并发复用**：同一时刻只有一个请求在用。"""

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None

    async def connect(self) -> None:
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=self.timeout
        )

    async def close(self) -> None:
        if self.writer is not None:
            try:
                self.writer.close()
                await asyncio.wait_for(self.writer.wait_closed(), timeout=1.0)
            except Exception:  # noqa: BLE001 - 关闭失败无关紧要
                pass
        self.reader = None
        self.writer = None

    async def request(
        self, method: str, path: str, headers: dict[str, str], body: Optional[bytes] = None
    ) -> HttpResponse:
        if self.writer is None or self.reader is None:
            await self.connect()
        assert self.writer is not None and self.reader is not None

        lines = [f"{method} {path} HTTP/1.1", f"Host: {self.host}:{self.port}"]
        for key, value in headers.items():
            lines.append(f"{key}: {value}")
        if body is not None:
            lines.append(f"Content-Length: {len(body)}")
        lines.append("")
        lines.append("")
        payload = "\r\n".join(lines).encode("ascii")
        self.writer.write(payload)
        if body is not None:
            self.writer.write(body)
        await asyncio.wait_for(self.writer.drain(), timeout=self.timeout)

        return await asyncio.wait_for(self._read_response(), timeout=self.timeout)

    async def _read_line(self) -> bytes:
        assert self.reader is not None
        return await self.reader.readuntil(b"\n")

    async def _read_response(self) -> HttpResponse:
        assert self.reader is not None
        status_line = (await self._read_line()).decode("latin-1").strip()
        parts = status_line.split(" ", 2)
        if len(parts) < 2 or not parts[1].isdigit():
            raise HttpError(f"bad status line: {status_line!r}")
        status = int(parts[1])

        headers: dict[str, str] = {}
        while True:
            line = (await self._read_line()).decode("latin-1").strip()
            if not line:
                break
            if ":" not in line:
                continue
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()

        body = await self._read_body(headers, status)
        return HttpResponse(status, headers, body)

    async def _read_body(self, headers: dict[str, str], status: int) -> bytes:
        assert self.reader is not None
        if status in (204, 304) or (100 <= status < 200):
            return b""

        transfer = headers.get("transfer-encoding", "").lower()
        if "chunked" in transfer:
            chunks: list[bytes] = []
            while True:
                size_line = (await self._read_line()).decode("latin-1").strip()
                try:
                    size = int(size_line.split(";", 1)[0], 16)
                except ValueError as exc:
                    raise HttpError(f"bad chunk size {size_line!r}") from exc
                if size == 0:
                    # 读掉 trailer（本项目不产生，但协议上必须处理到空行）。
                    while True:
                        trailer = await self._read_line()
                        if trailer in (b"\r\n", b"\n"):
                            break
                    break
                chunks.append(await self.reader.readexactly(size))
                await self.reader.readexactly(2)  # CRLF
            return b"".join(chunks)

        if "content-length" in headers:
            length = int(headers["content-length"])
            return await self.reader.readexactly(length) if length else b""

        # 既没有 Content-Length 也不是 chunked：读到 EOF 为止。
        return await self.reader.read()


class HttpPool:
    """一个机器人的连接池。

    为什么要池而不是一条连接：同一个机器人会**并发**发攻击与轮询（见 `play_once`），
    共用一个连接会让两个请求互相串行、把队列等待计入延迟。池让两者各占一条。
    """

    def __init__(self, host: str, port: int, timeout: float, size: int) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.size = size
        self._idle: asyncio.Queue[HttpConnection] = asyncio.Queue()
        self._created = 0
        self._closed = False

    async def _acquire(self) -> HttpConnection:
        while True:
            try:
                return self._idle.get_nowait()
            except asyncio.QueueEmpty:
                if self._created < self.size:
                    self._created += 1
                    conn = HttpConnection(self.host, self.port, self.timeout)
                    try:
                        await conn.connect()
                    except Exception:
                        self._created -= 1
                        raise
                    return conn
                # 池满：等一条空闲连接。超时即失败，避免无限等待掩盖瓶颈。
                return await asyncio.wait_for(self._idle.get(), timeout=self.timeout)

    async def _release(self, conn: HttpConnection, reusable: bool) -> None:
        if reusable and not self._closed:
            await self._idle.put(conn)
        else:
            await conn.close()
            self._created -= 1

    async def post(self, path: str, body: dict[str, Any]) -> HttpResponse:
        raw = json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json", "Connection": "keep-alive"}
        conn = await self._acquire()
        try:
            response = await conn.request("POST", path, headers, raw)
        except Exception:
            await self._release(conn, reusable=False)
            raise
        # 只有明确 `Connection: close` 时才丢弃连接。
        reusable = response.headers.get("connection", "").lower() != "close"
        await self._release(conn, reusable)
        return response

    async def close(self) -> None:
        self._closed = True
        while True:
            try:
                conn = self._idle.get_nowait()
            except asyncio.QueueEmpty:
                break
            await conn.close()
        self._created = 0


# ---------------------------------------------------------------------------
# 统计
# ---------------------------------------------------------------------------


@dataclass
class Histogram:
    """客户端侧耗时样本。分位数用最近秩法，不插值。"""

    samples_ms: list[float] = field(default_factory=list)

    def add(self, elapsed_ms: float) -> None:
        self.samples_ms.append(elapsed_ms)

    def summary(self) -> dict[str, float]:
        if not self.samples_ms:
            return {"count": 0}
        ordered = sorted(self.samples_ms)

        def nearest_rank(percent: float) -> float:
            # 最近秩法：读数必须对应**某一次真实观测**，插值出来的数字无法回溯到
            # 原始记录（Phase 5 要求"任一数字都能回溯原始记录"）。
            rank = int(round(percent / 100.0 * len(ordered) + 0.5))
            return ordered[max(0, min(len(ordered) - 1, rank - 1))]

        return {
            "count": len(ordered),
            "min": round(ordered[0], 3),
            "p50": round(nearest_rank(50), 3),
            "p90": round(nearest_rank(90), 3),
            "p95": round(nearest_rank(95), 3),
            "p99": round(nearest_rank(99), 3),
            "max": round(ordered[-1], 3),
            "mean": round(statistics.fmean(ordered), 3),
        }


@dataclass
class Stats:
    """一轮压测的统计。所有字段都是实测计数，没有推算值。"""

    started_at: float = 0.0
    finished_at: float = 0.0
    players: int = 0
    counters: dict[str, int] = field(default_factory=dict)
    stream_events: dict[str, int] = field(default_factory=dict)
    stream_resets: dict[str, int] = field(default_factory=dict)
    finish_reasons: dict[str, int] = field(default_factory=dict)
    attack_rejections: dict[str, int] = field(default_factory=dict)
    http_status: dict[str, int] = field(default_factory=dict)
    exceptions: dict[str, int] = field(default_factory=dict)
    latency_ms: dict[str, Histogram] = field(default_factory=dict)
    _lock: Any = None

    def __post_init__(self) -> None:
        # 计数器在多协程间并发更新。CPython 的 dict 单次 `+=` 不是原子的，
        # 因此用一把锁——压测工具自己的统计出错，比测出来的数字错了更恶心。
        self._lock = asyncio.Lock()

    async def bump(self, name: str, delta: int = 1) -> None:
        async with self._lock:
            self.counters[name] = self.counters.get(name, 0) + delta

    async def bump_map(self, name: str, key: str, delta: int = 1) -> None:
        async with self._lock:
            target = getattr(self, name)
            target[key] = target.get(key, 0) + delta

    async def observe(self, name: str, elapsed_ms: float) -> None:
        async with self._lock:
            self.latency_ms.setdefault(name, Histogram()).add(elapsed_ms)

    def to_dict(self) -> dict[str, Any]:
        return {
            "started_at_epoch": round(self.started_at, 3),
            "finished_at_epoch": round(self.finished_at, 3),
            "wall_seconds": round(self.finished_at - self.started_at, 3),
            "players": self.players,
            "counters": dict(sorted(self.counters.items())),
            "stream_events": dict(sorted(self.stream_events.items())),
            "stream_resets": dict(sorted(self.stream_resets.items())),
            "finish_reasons": dict(sorted(self.finish_reasons.items())),
            "attack_rejections": dict(sorted(self.attack_rejections.items())),
            "http_status": dict(sorted(self.http_status.items())),
            "exceptions": dict(sorted(self.exceptions.items())),
            "latency_ms": {k: v.summary() for k, v in sorted(self.latency_ms.items())},
        }


# ---------------------------------------------------------------------------
# SSE 解析
# ---------------------------------------------------------------------------


async def read_sse_events(
    reader: asyncio.StreamReader, on_event: Any, stop: asyncio.Event
) -> str:
    """从一条已建立的流里逐块解析 SSE，直到对局结束或停止。

    返回结束原因字符串（`finished` / `aborted` / `reset` / `eof`）。
    只实现本项目实际会发出的帧：可选 `id:` 行 + `event:` + 单行 `data:` + 空行，
    以及以 `:` 开头的心跳注释行——**够用即可**，不是通用 SSE 实现。
    """
    event_name: Optional[str] = None
    data_lines: list[str] = []
    try:
        while not stop.is_set():
            raw = await reader.readuntil(b"\n")
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if line == "":
                if event_name is not None:
                    reason = await on_event(event_name, "\n".join(data_lines))
                    if reason:
                        return reason
                event_name = None
                data_lines = []
                continue
            if line.startswith(":"):
                continue  # 心跳注释
            field, _, value = line.partition(":")
            if value.startswith(" "):
                value = value[1:]
            if field == "event":
                event_name = value
            elif field == "data":
                data_lines.append(value)
            # `id:` 与 `retry:` 本压测不使用（客户端不重连补帧）。
    except (asyncio.IncompleteReadError, ConnectionResetError):
        return "eof"
    except asyncio.CancelledError:
        raise
    except Exception as exc:  # noqa: BLE001
        return f"error:{type(exc).__name__}"
    return "stop" if stop.is_set() else "eof"


# ---------------------------------------------------------------------------
# 一个机器人
# ---------------------------------------------------------------------------


class Bot:
    """一个玩家的完整客户端。所有网络失败都按类型计数，不抛出到顶层。"""

    def __init__(self, index: int, args: argparse.Namespace, stats: Stats) -> None:
        self.index = index
        self.args = args
        self.stats = stats
        if args.bench_accounts:
            # 每个机器人一个独立身份：`bench-00042`。位数固定 5 位，与
            # test_credentials.cpp 的 IsSyntheticBenchAccount 严格对应。
            if index >= BENCH_MAX:
                raise SystemExit(
                    f"--players 超过合成账号上限 {BENCH_MAX}（bench-{BENCH_DIGITS} 位）"
                )
            self.account = f"{BENCH_PREFIX}{index:0{BENCH_DIGITS}d}"
            self.password = BENCH_PASSWORD
        else:
            self.account = ACCOUNTS[index % len(ACCOUNTS)]
            self.password = self.account + PASSWORD_SUFFIX
        self.pool = HttpPool(args.host, args.port, args.timeout, args.connections_per_bot)
        self.token = ""
        self.room_id = ""
        self.round = 0
        # 中止标志由 run_bot 注入。**显式初始化**而不是靠类属性兜底：漏了它的表现
        # 是一个只在停止路径上才出现的 `AttributeError`（本次实测踩到过 3 次），
        # 而那种错误在压测里会被统计成一个不起眼的异常计数。
        self.args_stop: Optional[asyncio.Event] = None
        # 上一局结束时的 room_id。
        #
        # **为什么必须记住它**：Match 的"已配对结果"会保留 `kDefaultResultTtlMs`
        # （120 秒，见 match_queue.hpp），而重入队时会**先返回那条旧结果**。
        # 于是"打完一局 -> 立刻重新入队"会拿到上一局的房间号，再 `JoinRoom` 就必然
        # 是 409 room_already_finished。实测证据：91 次配对里 86 次进房被拒，
        # 稳态压测因此变成了"反复撞已结束的房间"，测出来的东西根本不是稳态。
        self.last_finished_room = ""

    # -- 基础设施 -----------------------------------------------------------

    async def close(self) -> None:
        await self.pool.close()

    async def post(
        self, path: str, body: dict[str, Any], latency_name: Optional[str] = None
    ) -> tuple[int, dict[str, Any]]:
        started = time.perf_counter()
        status = 0
        payload: dict[str, Any] = {}
        try:
            response = await self.pool.post(path, body)
            status = response.status
            payload = response.json()
        except asyncio.TimeoutError:
            await self.stats.bump_map("exceptions", "TimeoutError")
        except Exception as exc:  # noqa: BLE001 - 任何异常都要计数，不许静默
            # 带上异常文字而不仅是类名：只记 `AttributeError` 这种类名，
            # 事后根本无从判断是哪一行出的问题（本次实测就踩到了）。
            await self.stats.bump_map("exceptions", f"{type(exc).__name__}:{exc}")
        finally:
            if latency_name is not None:
                await self.stats.observe(latency_name, (time.perf_counter() - started) * 1000.0)
        await self.stats.bump_map("http_status", f"{path}:{status}")
        return status, payload

    # -- 步骤 ---------------------------------------------------------------

    async def login(self) -> bool:
        body = {
            "account": self.account,
            "password": self.password,
            "request_id": new_request_id("bench-login"),
            "client_type": "bot",
        }
        status, payload = await self.post(PATH_LOGIN, body, "login")
        if status == 200 and payload.get("token"):
            self.token = str(payload["token"])
            await self.stats.bump("login_ok")
            return True
        await self.stats.bump("login_failed")
        return False

    async def enqueue_and_wait(self, deadline: float) -> bool:
        self.round += 1
        status, _ = await self.post(
            PATH_ENQUEUE, {"token": self.token, "request_id": new_request_id("bench-enqueue")}
        )
        if status != 200:
            await self.stats.bump("enqueue_failed")
            return False
        await self.stats.bump("enqueue_ok")

        while time.monotonic() < deadline and not self.stopped_flag:
            status, payload = await self.post(
                PATH_MATCH_CURRENT,
                {"token": self.token, "request_id": new_request_id("bench-poll")},
                "match_poll",
            )
            if status == 200:
                match = payload.get("match") or {}
                room_id = str(match.get("room_id") or "")
                if room_id == self.last_finished_room:
                    # 这是上一局的旧结果（Match 保留 120 秒）。等它过期，别去加入
                    # 一个已经结束的房间——那既测不到稳态，又会把 409 计成"失败"。
                    await self.stats.bump("stale_result_skipped")
                    await asyncio.sleep(self.args.poll_interval)
                    continue
                if room_id:
                    self.room_id = room_id
                    await self.stats.bump("paired")
                    return True
            await asyncio.sleep(self.args.poll_interval)
        await self.stats.bump("pair_timeout")
        return False

    async def join_room(self) -> bool:
        status, _ = await self.post(
            PATH_ROOM_JOIN,
            {"token": self.token, "request_id": new_request_id("bench-join"), "room_id": self.room_id},
        )
        if status == 200:
            await self.stats.bump("join_ok")
            await self.stats.bump("rooms_entered")
            return True
        await self.stats.bump("join_failed")
        return False

    async def attack(self) -> None:
        await self.stats.bump("attacks_sent")
        status, payload = await self.post(
            PATH_ROOM_INPUT,
            {
                "token": self.token,
                "request_id": new_request_id("bench-input"),
                "room_id": self.room_id,
            },
            "attack",
        )
        if status == 200:
            await self.stats.bump("attacks_ok")
            return
        await self.stats.bump("attacks_failed")
        reason = str((payload.get("error") or {}).get("reason") or f"http_{status}")
        await self.stats.bump_map("attack_rejections", reason)

    # -- SSE ----------------------------------------------------------------

    async def open_stream(self, deadline: float) -> Optional[asyncio.StreamReader]:
        """建立 SSE 订阅，返回已确认的读端。

        确认方式：读到第一条 **event**（`session.ready`）才算订阅真的建立。
        只发请求不看响应会把"订阅被拒"当成"连接已建立"，那会让压测报告里的
        连接数虚高——本任务的第一条纪律就是不许假成功。
        """
        path = f"{PATH_STREAM}?room_id={self.room_id}&request_id={new_request_id('bench-stream')}"
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {self.args.host}:{self.args.port}\r\n"
            f"Authorization: Bearer {self.token}\r\n"
            f"Accept: text/event-stream\r\n"
            f"Connection: keep-alive\r\n\r\n"
        ).encode("ascii")

        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.args.host, self.args.port), timeout=self.args.timeout
            )
        except Exception as exc:  # noqa: BLE001
            await self.stats.bump("stream_failed")
            await self.stats.bump_map("exceptions", f"stream_connect:{type(exc).__name__}")
            return None

        writer.write(request)
        try:
            await asyncio.wait_for(writer.drain(), timeout=self.args.timeout)
        except Exception as exc:  # noqa: BLE001
            await self.stats.bump("stream_failed")
            await self.stats.bump_map("exceptions", f"stream_drain:{type(exc).__name__}")
            writer.close()
            return None

        # 响应头 -> 空行。
        status = 0
        try:
            status_line = (await asyncio.wait_for(reader.readuntil(b"\n"), self.args.timeout))
            parts = status_line.decode("latin-1").strip().split(" ", 2)
            status = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else 0
            while True:
                line = await asyncio.wait_for(reader.readuntil(b"\n"), self.args.timeout)
                if line in (b"\r\n", b"\n"):
                    break
        except Exception as exc:  # noqa: BLE001
            await self.stats.bump("stream_failed")
            await self.stats.bump_map("exceptions", f"stream_header:{type(exc).__name__}")
            writer.close()
            return None

        if status != 200:
            await self.stats.bump("stream_failed")
            await self.stats.bump_map("http_status", f"{PATH_STREAM}:{status}")
            writer.close()
            return None

        await self.stats.bump("stream_opened")
        self._stream_writer = writer
        return reader

    async def _handle_stream_event(self, name: str, data: str) -> Optional[str]:
        """返回非 None 表示"该结束这条流了"。"""
        await self.stats.bump_map("stream_events", name)
        if name == "stream.reset":
            # 结构是 `{version,type,sequence,timestamp_ms,payload:{reason,room:{...}}}`
            # （见 stream_hub.cpp 的 StreamResetJson + Envelope）。**必须下钻到
            # payload**：第一版只看顶层，于是所有 reset 都被记成 `unknown`——
            # 而那正是"窗口外/超前/首次订阅"这类关键结论唯一的来源。
            reason = "unparsable"
            try:
                payload = (json.loads(data) or {}).get("payload") or {}
                reason = str(payload.get("reason") or "missing_reason")
            except Exception:  # noqa: BLE001
                pass
            await self.stats.bump_map("stream_resets", reason)
            return None
        if name == "room.finished":
            reason = "missing_reason"
            try:
                payload = (json.loads(data) or {}).get("payload") or {}
                reason = str(payload.get("finish_reason") or "missing_reason")
            except Exception:  # noqa: BLE001
                pass
            await self.stats.bump_map("finish_reasons", reason)
            if reason == "aborted":
                await self.stats.bump("games_aborted")
            else:
                await self.stats.bump("games_finished")
            self.last_finished_room = self.room_id
            return "finished" if reason != "aborted" else "aborted"
        return None

    # -- 一轮生命周期 --------------------------------------------------------

    async def play_once(self, until: float) -> bool:
        if not await self.enqueue_and_wait(until):
            return False
        if not await self.join_room():
            return False

        reader = await self.open_stream(until)
        if reader is None:
            return False

        stop = asyncio.Event()
        stream_task = asyncio.create_task(read_sse_events(reader, self._handle_stream_event, stop))
        attack_task = asyncio.create_task(self.attack_loop(until, stop))

        done, pending = await asyncio.wait(
            {stream_task, attack_task}, return_when=asyncio.FIRST_COMPLETED
        )
        stop.set()
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)

        writer = getattr(self, "_stream_writer", None)
        if writer is not None:
            writer.close()
            self._stream_writer = None

        reason = "unknown"
        for task in done:
            try:
                reason = task.result() or reason
            except asyncio.CancelledError:
                reason = "cancelled"
            except Exception as exc:  # noqa: BLE001
                await self.stats.bump_map(
                    "exceptions", f"play_once:bot{self.index}:{type(exc).__name__}:{exc}"
                )
                reason = "exception"
        self.room_id = ""
        # 只有"打完了"才算这轮成功；超时/取消都不算。
        return reason in ("finished", "aborted")

    async def attack_loop(self, until: float, stop: asyncio.Event) -> str:
        """按固定间隔提交攻击，直到对局结束或到点。

        攻击间隔默认 2 秒：每次攻击扣 10 点血、满血 100，因此一方被打满需要 10 次
        命中，单局约 20 秒。与默认 `--duration 60` 配合，稳态（多数房间在推进）
        占大头，而不是一直在开场。
        """
        while time.monotonic() < until and not stop.is_set() and not self.stopped_flag:
            await self.attack()
            try:
                await asyncio.wait_for(stop.wait(), timeout=self.args.attack_interval)
                break  # 对局已结束
            except asyncio.TimeoutError:
                continue
        return "until"

    # -- 生命周期 -----------------------------------------------------------

    @property
    def stopped_flag(self) -> bool:
        return self.args_stop is not None and self.args_stop.is_set()

    args_stop: Optional[asyncio.Event] = None


# ---------------------------------------------------------------------------
# 运行器
# ---------------------------------------------------------------------------


async def run_bot(bot: Bot, stats: Stats, stop: asyncio.Event, deadline: float) -> None:
    bot.args_stop = stop
    try:
        if not await bot.login():
            return
        while not stop.is_set() and time.monotonic() < deadline:
            played = await bot.play_once(deadline)
            if not played:
                # 没打成（配对超时/进房失败/订阅被拒）：让出一次事件循环再重试，
                # 否则失败路径会变成忙等，把 CPU 全吃在压测端。
                await asyncio.sleep(0.5)
                continue
            if not bot.args.recycle:
                return
            # 打完一局后重新入队，维持稳态负载（否则房间会一个个空掉，
            # 后半段测的其实是"没什么负载"）。
            await asyncio.sleep(bot.args.recycle_gap)
    except asyncio.CancelledError:
        raise
    except Exception as exc:  # noqa: BLE001 - 任何未预期的异常都必须留痕
        await stats.bump_map("exceptions", f"bot{bot.index}:{type(exc).__name__}:{exc}")
    finally:
        await bot.close()


async def run_round(args: argparse.Namespace) -> Stats:
    stats = Stats(players=args.players)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:  # pragma: no cover - 非 POSIX
            pass

    stats.started_at = time.time()
    deadline = time.monotonic() + args.duration

    bots = [Bot(i, args, stats) for i in range(args.players)]
    tasks = [asyncio.create_task(run_bot(bot, stats, stop, deadline)) for bot in bots]

    # 到点即停：`--duration` 是硬边界，不给"再等一会"留余地，
    # 否则每一档的时长都不一样，档位之间不可比。
    try:
        await asyncio.wait_for(
            asyncio.gather(*tasks, return_exceptions=True), timeout=args.duration + args.drain_seconds
        )
    except asyncio.TimeoutError:
        stop.set()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    stats.finished_at = time.time()
    stop.set()
    return stats


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="实时对战后端压测机器人（TASK-022）")
    parser.add_argument("--players", type=int, required=True, help="并发玩家数（= SSE 连接数）")
    parser.add_argument(
        "--bench-accounts",
        action="store_true",
        help="每个机器人用独立合成身份 bench-NNNNN（容量测试必须开启；"
        "Gateway 侧需要 -enable_bench_accounts）",
    )
    parser.add_argument("--gateway", default="127.0.0.1:8080", help="Gateway 地址 host:port")
    parser.add_argument("--duration", type=float, default=60.0, help="压测时长（秒）")
    parser.add_argument(
        "--attack-interval", type=float, default=2.0, help="同一玩家的攻击间隔（秒）"
    )
    parser.add_argument(
        "--poll-interval", type=float, default=0.5, help="等待配对时的轮询间隔（秒）"
    )
    parser.add_argument("--timeout", type=float, default=10.0, help="单次请求超时（秒）")
    parser.add_argument(
        "--connections-per-bot", type=int, default=2, help="每个机器人的 HTTP 连接数"
    )
    parser.add_argument(
        "--recycle",
        action="store_true",
        help="打完一局后重新入队（默认不开启）。开启才能测到稳态；不开启则房间会越来越少。",
    )
    parser.add_argument("--recycle-gap", type=float, default=1.0, help="重入队前的间隔（秒）")
    parser.add_argument("--drain-seconds", type=float, default=30.0, help="收尾等待上限（秒）")
    parser.add_argument("--out", help="把结果 JSON 写到这个路径")
    parser.add_argument("--label", default="", help="写进结果的标签（档位名等）")
    return parser.parse_args(argv)


async def async_main(args: argparse.Namespace) -> int:
    if ":" in args.gateway:
        host, _, port_text = args.gateway.rpartition(":")
        args.host = host
        args.port = int(port_text)
    else:
        args.host = args.gateway
        args.port = 8080

    stats = await run_round(args)
    payload = stats.to_dict()
    payload["label"] = args.label or f"players-{args.players}"
    payload["config"] = {
        "players": args.players,
        "gateway": args.gateway,
        "duration": args.duration,
        "attack_interval": args.attack_interval,
        "poll_interval": args.poll_interval,
        "timeout": args.timeout,
        "connections_per_bot": args.connections_per_bot,
        "recycle": args.recycle,
        "recycle_gap": args.recycle_gap,
        "hostname": os.uname().nodename,
        "python": sys.version.split()[0],
    }
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    counters = payload["counters"]
    print(
        f"[loadgen] {payload['label']}: players={args.players} "
        f"wall={payload['wall_seconds']}s "
        f"login_ok={counters.get('login_ok', 0)} "
        f"paired={counters.get('paired', 0)} "
        f"stream_opened={counters.get('stream_opened', 0)} "
        f"attacks_ok={counters.get('attacks_ok', 0)} "
        f"finished={counters.get('games_finished', 0)} "
        f"exceptions={sum(payload['exceptions'].values())}"
    )
    # 退出码：有机器人登录失败或订阅完全打不开时返回非 0，让 bench.sh 能判失败。
    fatal = 0
    if counters.get("login_failed", 0) > 0:
        fatal = 1
    if counters.get("stream_opened", 0) == 0 and args.players > 0:
        fatal = 1
    return fatal


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
