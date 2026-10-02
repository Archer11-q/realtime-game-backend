# ADR-0004：浏览器推送由 WebSocket 改为 SSE

- 状态：已接受
- 日期：2026-10-02
- 决策者：项目所有者
- 关联任务：TASK-009
- 部分替代：[ADR-0001](0001-initial-platform-and-stack.md)、
  [ADR-0002](0002-gateway-temporary-player-ownership.md)、
  [ADR-0003](0003-scope-reduction.md) 中「浏览器通信为 HTTP + WebSocket」的表述

## 背景

TASK-009 的原定范围是「Gateway WebSocket 路由与房间消息」。在动手前按项目纪律
先核实框架能力，结论是**这条路在当前技术栈下走不通**：

| 核查项 | 结果 | 证据 |
|---|---|---|
| brpc 是否支持 WebSocket | **完全不支持** | brpc 1.16.0 的 `docs/`、`include/brpc/`、`src/` 三处检索 `websocket` 全部为空。唯一命中来自 Thrift（`TThriftWebSocketServer`），那是 brpc 的传递依赖，与 brpc 无关 |
| 能否在 brpc 之上自实现 WebSocket | **不可行** | brpc 只提供 `Controller::CreateProgressiveAttachment()`，它能持续写**响应体**，但**无法接管底层 socket**。WebSocket 要求握手后把同一连接切换为帧协议并**读取**客户端帧；而在 brpc 下，客户端发来的字节会被当作新的 HTTP 请求解析 |
| 是否有现成 WebSocket 库 | **没有** | vcpkg 只装了 brpc 依赖所需的 boost 子集，**没有 beast**；websocketpp / uwebsockets / libwebsockets 均未安装。引入任何一个都要新增 vcpkg 依赖并重编译依赖链 |

同时确认 brpc **官方支持**另一种服务端推送机制：

- `brpc/controller.h` 的 `CreateProgressiveAttachment()`（第 464 行）与
  `brpc/progressive_attachment.h` 的 `Write()`（第 39 行）。
- `docs/cn/http_service.md` 第 336–350 行明确写着「利用该特性可以轻松实现
  Server-Sent Events(SSE) 服务，从而使客户端能够通过 HTTP 连接从服务器自动接收更新」，
  并给出官方示例 `HttpSSEServiceImpl`（`example/http_c++/http_server.cpp`）。
- 该示例就是一个**普通的 protobuf service 方法**：设置
  `content_type: text/event-stream`，拿 `ProgressiveAttachment`，从后台 bthread 写
  `event: <name>\ndata: <json>\n\n`。形态与现有 `GatewayServiceImpl` 完全一致。

## 决策

**浏览器推送改用 SSE，客户端上行继续使用现有 HTTP POST。**

```text
服务端 -> 浏览器：SSE（GET /api/v1/stream，text/event-stream，长连接）
浏览器 -> 服务端：HTTP POST（加入房间、提交输入等，与现状相同）
```

事件格式沿用已经设计好的版本化信封：

```text
event: room.state
data: {"version":1,"type":"room.state","sequence":12,"timestamp_ms":...,"payload":{...}}

```

TASK-009 实际推送的事件：`session.ready`、`room.state`、`room.finished`、`error`。

## 备选方案

### 方案 B：引入 WebSocket 库（Boost.Beast 或 websocketpp）

优点：

- 真正的双向通道，客户端上行也能复用同一条连接。
- 与文档原定的技术方向一致，文档无需大改。

缺点：

- **必须新增 vcpkg 依赖并重编译**。Boost.Beast 未安装，websocketpp 还额外需要
  boost-asio。这违反 `CLAUDE.md` 第 6 条「不以技术先进为理由增加组件」。
- Gateway 里会同时存在两套浏览器协议栈（brpc HTTP + Beast WS）和两个监听端口，
  故障面与排障成本都翻倍。
- **为当前不需要的能力付代价**：本项目服务端到客户端是单向推送（房间权威状态），
  客户端上行只有低频的「加入房间」「提交攻击」，POST 完全够用。

结论：不采用。

### 方案 C：本阶段不做推送，只保留轮询

优点：

- 零成本，零风险。

缺点：

- TASK-009 变成空任务，Phase 1 的交付物与路线图不符。
- 对局页只能靠轮询刷新血量，做不到推送级别的实时感。

结论：不采用。

## 后果

### 正面

- 零新增依赖，复用 brpc 官方支持且带示例的机制。
- 验收更容易：`curl -N` 就能看到事件流；WebSocket 需要额外写一个客户端。
- **Phase 2 的断线重连更顺**：SSE 规范自带 `Last-Event-ID` 续传语义，
  正好对应「重连后补缺失状态」；WebSocket 要在应用层自己实现。
- 前端更简单：`EventSource` 是浏览器原生 API，自带自动重连。
- **少一个配置项**：SSE 复用 HTTP 端口，`GATEWAY_WS_PORT`（8081）不再需要。

### 负面

- **上行仍是「一次动作一个 HTTP 请求」**。当前输入是低频点击（10 Hz 帧、每帧最多
  结算一次攻击），够用；但**帧级高频输入会成为瓶颈**。若将来要做真正的帧同步
  （每帧上报输入），必须重新评估本 ADR，而不是硬撑。
- SSE 是单向的。若将来出现「服务端需要客户端低延迟回执」的需求，同样要重新评估。
- 需要一次文档口径同步：仓库中共有 47 处提到 WebSocket，涉及 `CLAUDE.md`、章程、
  架构、路线图、接口文档、README、三个 ADR、两个 proto、环境变量示例与 `TASKS.md`。

## 验证方式

- `bash scripts/verify-stream.sh` 全项通过：订阅后收到 `session.ready`；房间帧号
  前进时收到 `room.state` 且 `sequence` 单调递增；帧号未变化时不推送；对局结束后
  收到 `room.finished` 且连接被关闭；轮询接口仍然可用。
- 单元测试断言：**没有订阅者时不产生任何 Gateway→Room 轮询**。
- 全仓库检索 `WebSocket`，剩余出现处只能是本 ADR 的历史论证和 devlog 的历史记录，
  不存在实现指引。

## 替代关系

- **部分替代 ADR-0001**：「HTTP + WebSocket 作为浏览器通信」改为「HTTP + SSE」。
- **部分替代 ADR-0002 / ADR-0003**：两者正文中「WebSocket 属于后续任务」的表述
  改为「浏览器推送（SSE）属于 TASK-009」，其余内容不受影响。
