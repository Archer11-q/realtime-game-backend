#!/usr/bin/env python3
"""透明 TCP 中继：让「某一个服务的某一个依赖通道」可以被单独切断。

为什么需要它（TASK-023 的核心约束）
------------------------------------
三个服务共用同一个 Redis（6379）和同一个 MySQL（3306）：

    Gateway -> Redis(会话)          Gateway -> MySQL(玩家档案)
    Match   -> Redis(队列快照)      Room    -> MySQL(快照 + 对局结果)

`docker stop rgbt-redis` 会把 Gateway 的会话存储和 Match 的快照存储**一起**打掉，
于是所有 HTTP 请求在鉴权那一步就返回 503，根本走不到被测的那条通道。这个坑
TASK-015 已经踩过一次（见 `scripts/verify-persistence.sh` 第 7 节的注释）。

docker 容器的已发布端口无法在运行期改映射，服务也没有「指向备用 Redis」的开关，
因此隔离只能做在**网络层**：让某个服务连到本中继，而不是直连依赖。

    服务 --(本中继端口)--> 真实依赖

把中继进程杀掉，指向它的那个服务立刻拿到 ECONNREFUSED（已建立的连接被 RST），
而连真实依赖的其它服务完全无感。这就是「按通道注入」的实现方式。

它**不是**代理或中间件：不做协议解析、不改字节、不缓存、不重试，只是把两个
socket 对拷。因此它对链路的唯一影响是多一跳本机环回。

用法
----
    python3 chaos/relay.py --listen 16379 --target 127.0.0.1:6379
    python3 chaos/relay.py --listen 13307 --target 127.0.0.1:3306 --ready-file /tmp/r.ready

就绪信号：绑定成功后向 stdout 打印一行 `relay ready listen=... target=...`
（`--ready-file` 存在时同时写入该文件）。调用方必须等到这一行再开始测，
否则「注入前正常」那一段会因为中继还没起来而失败。

退出：收到 SIGTERM/SIGINT 即关闭监听与全部活动连接并退出（退出码 0）。
"""

from __future__ import annotations

import argparse
import signal
import socket
import sys
import threading

_CHUNK = 65536
_stop = threading.Event()
_listener = None
_active = set()
_active_lock = threading.Lock()


def _remember(sock):
    with _active_lock:
        _active.add(sock)


def _forget(sock):
    with _active_lock:
        _active.discard(sock)


def _shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def _pump(src, dst):
    """单向对拷，任一端断开即关闭两端，让对端立刻看到 EOF 而不是等到超时。"""
    try:
        while not _stop.is_set():
            data = src.recv(_CHUNK)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        _shutdown(src)
        _shutdown(dst)


def _handle(client, target_host, target_port):
    _remember(client)
    upstream = None
    try:
        upstream = socket.create_connection((target_host, target_port), timeout=5)
        upstream.settimeout(None)
        upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        _remember(upstream)
        # 两个方向各一个线程。只有两个 socket，量级是「每服务几条连接」，
        # 不需要连接池或事件循环。
        up = threading.Thread(target=_pump, args=(client, upstream), daemon=True)
        down = threading.Thread(target=_pump, args=(upstream, client), daemon=True)
        up.start()
        down.start()
        up.join()
        down.join()
    except OSError:
        # 目标不可达：直接关闭客户端连接。这是真实依赖不可用时的同一现象。
        pass
    finally:
        _shutdown(client)
        if upstream is not None:
            _shutdown(upstream)
            _forget(upstream)
        _forget(client)


def _on_signal(_signum, _frame):
    _stop.set()
    if _listener is not None:
        try:
            _listener.close()
        except OSError:
            pass
    with _active_lock:
        sockets = list(_active)
    for sock in sockets:
        _shutdown(sock)


def main():
    parser = argparse.ArgumentParser(description="透明 TCP 中继（TASK-023 通道注入）")
    parser.add_argument("--listen", type=int, required=True, help="本机监听端口")
    parser.add_argument("--target", required=True, help="转发目标 host:port（例如 127.0.0.1:6379）")
    parser.add_argument("--ready-file", default="", help="就绪后写入该文件（可选）")
    parser.add_argument("--backlog", type=int, default=128)
    args = parser.parse_args()

    if ":" not in args.target:
        print("x  --target 需要 host:port 形式，收到 %r" % (args.target,), file=sys.stderr)
        return 2
    target_host, target_port_text = args.target.rsplit(":", 1)
    target_port = int(target_port_text)

    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)

    global _listener
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        listener.bind(("127.0.0.1", args.listen))
    except OSError as exc:
        print("x  绑定 127.0.0.1:%d 失败：%s" % (args.listen, exc), file=sys.stderr)
        return 1
    listener.listen(args.backlog)
    _listener = listener

    # 中继自身不引入额外延迟：TCP_NODELAY 让 MySQL/Redis 的小包不被 Nagle 合并，
    # 否则测出来的「检测时间」会混进中继自己的延迟，不再是服务的真实反应。
    listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    ready = "relay ready listen=127.0.0.1:%d target=%s" % (args.listen, args.target)
    print(ready, flush=True)
    if args.ready_file:
        with open(args.ready_file, "w", encoding="utf-8") as handle:
            handle.write(ready + "\n")

    while not _stop.is_set():
        try:
            client, _peer = listener.accept()
        except OSError:
            break
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=_handle, args=(client, target_host, target_port), daemon=True).start()

    return 0


if __name__ == "__main__":
    sys.exit(main())
