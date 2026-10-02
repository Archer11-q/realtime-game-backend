# 接口、数据与协议

> 状态：已确认（2026-09-14）

## 1. 接口原则

- 对外接口面向业务用例，不暴露内部数据表。
- 服务间接口通过 Protobuf 明确定义。
- 所有接口必须定义成功、失败、超时和重试行为。
- 客户端传入的玩家 ID、房间 ID 和状态不可直接信任。
- 接口变化必须保持向后兼容，破坏性变化需要新版本。
- 请求和事件都携带唯一 ID，便于幂等和追踪。

## 2. 浏览器到 Gateway

### HTTP 接口

建议首批接口：

| 方法 | 路径 | 用途 | 状态 |
|---|---|---|---|
| POST | `/api/v1/login` | 使用测试身份登录 | 已实现（TASK-005） |
| GET | `/api/v1/players/me` | 查询当前玩家信息 | 已实现（TASK-005） |
| POST | `/api/v1/logout` | 结束会话 | 已实现（TASK-005） |
| POST | `/api/v1/matches` | 进入匹配 | 已实现（TASK-007） |
| GET | `/api/v1/matches/current` | 查询当前匹配状态 | 已实现（TASK-007） |
| POST | `/api/v1/matches/current/cancel` | 取消匹配 | 已实现（TASK-007） |
| POST | `/api/v1/rooms/join` | 加入房间 | 已实现（TASK-008） |
| POST | `/api/v1/rooms/input` | 提交一次攻击输入 | 已实现（TASK-008） |
| GET | `/api/v1/rooms/state` | 查询房间权威状态（`?room_id=`） | 已实现（TASK-008） |
| GET | `/api/v1/results` | 查询对局结果（`?match_id=`） | 已实现（TASK-008） |
| GET | `/health` | 健康检查 | 已实现（brpc 内置服务） |

接口名称在实现前可以调整，但必须更新本文档。

**已发生的调整（TASK-007）**：取消匹配原设计为 `DELETE /api/v1/matches/current`，
与查询接口共用同一路径。实现时确认 **brpc 的 restful 映射按路径分派，不支持按
HTTP 方法分派**，同一路径无法同时承载 GET 与 DELETE，因此改为
`POST /api/v1/matches/current/cancel`。这不是偏好问题，是框架约束。

**已发生的调整（TASK-008）**：房间状态与对局结果原设计为
`GET /api/v1/rooms/{room_id}` 与 `GET /api/v1/results/{match_id}`。实现时确认
**brpc 的 restful 映射同样不支持 `{name}` 路径参数**，只支持 `*` 通配符；而
`/api/v1/rooms/*` 会与 `/api/v1/rooms/join`、`/api/v1/rooms/input` 这类固定子路径
产生歧义。因此 room_id 与 match_id 改走**查询参数**。同属框架约束。

### 匹配接口的错误语义

匹配是第一个跨服务调用（Gateway -> Match），错误语义必须逐项明确，
否则调用方无法判断「该重试还是该放弃」：

| 情形 | HTTP | `error.reason` | 调用方行为 |
|---|---|---|---|
| 未携带或格式非法 Token | 400 | `token_required` / `token_malformed` | 不重试，先登录 |
| Token 合法但会话不存在 | 401 | `session_not_found` | 不重试，重新登录 |
| 玩家已在队列，或已有未领取的匹配结果 | 200 | 无错误 | **不是错误**，读 `match.state` 即可 |
| 玩家不在队列时取消 | 200 | 无错误 | 幂等成功，读 `match.state`（通常为 `idle`） |
| 排队超时被淘汰 | 200 | 无错误 | `match.state = timeout`，提示重新匹配 |
| Match 不可用（未启动 / 超时） | 503 | `match_unavailable` | 可退避重试，恢复后无需重启 |
| 队列已满 | 429 | `match_queue_full` | 属限流，退避后重试 |
| Match 返回未分类错误 | 500 | `match_internal` | 记录并告警 |

`match.state` 取值：`idle`（未排队且无结果）、`queued`（排队中）、
`matched`（已配对，`match_id` 与 `room_id` 有效）、`timeout`（排队超时，需重新匹配）。
`timeout` 必须与 `idle` 区分：前者提示「重新匹配」，后者是「可以开始匹配」。

**安全约束**：匹配接口的 `player_id` **只能来自会话**，请求体里即使带上该字段也会被
忽略。否则任何登录用户都能替他人入队。

### 房间接口的错误语义（TASK-008）

| 情形 | HTTP | `error.reason` | 调用方行为 |
|---|---|---|---|
| 未携带或格式非法 Token | 400 | `token_required` / `token_malformed` | 不重试，先登录 |
| Token 合法但会话不存在 | 401 | `session_not_found` | 不重试，重新登录 |
| 缺少 room_id / match_id | 400 | `room_id_required` / `match_id_required` | 不重试，修请求 |
| 该玩家不是这一局的成员 | 400 | `not_a_member` | 不重试 |
| 对局尚未开始就提交输入 | 400 | `room_not_playing` | 不重试，等对局开始 |
| 房间或对局结果不存在 | 404 | `room_not_found` / `result_not_found` | 不重试 |
| 对局已结束 | 409 | `room_already_finished` | 不重试 |
| 已结束但结果尚未落库 | 503 | `result_pending` | **可重试**，稍后再查 |
| Room 不可用 | 503 | `room_unavailable` | 可退避重试，恢复后无需重启 |
| Room 的 MySQL 不可用 | 503 | `result_store_unavailable` | 可退避重试 |
| Room 返回未分类错误 | 500 | `room_internal` | 记录并告警 |

`result_pending` 与 `result_not_found` 的区分是关键：前者表示**结果存在但还没
写进数据库**，后者表示**确实没有这条结果**。把前者报成 404 会让客户端以为
这一局没有胜负，而把后者报成 503 会让客户端无限重试一个不会出现的结果。

同理，`result_pending` 时响应里的 `result` 字段必须为空：调用方不得据此推断
胜负。平局的结果本身就是一个 `winner_id` 为空的合法记录，两者不能混淆。

**安全约束**：所有房间接口的 `player_id` **只能来自会话**，请求体里即使带上
该字段也会被忽略。否则任何登录用户都能替他人加入房间或代打。

### 服务端推送（SSE）与消息信封

浏览器推送**不使用 WebSocket**：brpc 1.16.0 不支持它，改用其官方支持的
Server-Sent Events。完整论证见 [ADR-0004](adr/0004-sse-instead-of-websocket.md)。

传输形态：

```text
浏览器 -> Gateway：现有 HTTP 接口（登录、匹配、加入房间、提交输入）
Gateway -> 浏览器：GET /api/v1/stream?room_id=...   text/event-stream 长连接
```

事件的线上格式（`data` 是**单行** JSON，即下面的信封）：

```text
event: room.state
data: {"version":1,"type":"room.state","sequence":12,"timestamp_ms":0,"payload":{}}

```

心跳以 SSE 注释行发送（`: ping`），客户端会忽略它，但它让长连接不被中间层回收。
**心跳不能做成事件**，否则会污染事件流、让客户端的类型分发多一种无意义的分支。

信封字段：

```json
{
  "version": 1,
  "type": "room.state",
  "request_id": "optional-request-id",
  "sequence": 12,
  "timestamp_ms": 0,
  "payload": {}
}
```

要求：

- `version` 用于协议演进。
- `type` 使用小写点分命名，与 SSE 的 `event:` 字段一致。
- `request_id` 只用于需要响应的客户端请求；**服务端主动推送不带该字段**。
- `sequence` 用于房间内消息排序和发现缺口。`room.state` / `room.finished`
  直接用房间帧号，天然单调递增。
- 服务端必须校验消息大小、字段类型和当前连接状态。

推送事件类型（TASK-009 已实现的部分）：

- `session.ready` —— 订阅建立后的第一个事件，带上订阅者与房间
- `room.state` —— 房间权威状态快照，**仅在该房间帧号变化时推送**
- `room.finished` —— 对局结束（含平局与 `aborted`），推送后服务端关闭连接

计划中但**当前未实现**的事件类型（不要在没有对应实现时把它们写进文档之外的地方）：

- `match.updated` —— 匹配状态变化。当前由客户端轮询 `GET /api/v1/matches/current`
  获得。改为推送需要 Gateway 为**每个在线连接**轮询 Match，而匹配状态变化频率低，
  收益不足以换这份负载；若将来前端体验确实需要，再评估。
- `room.joined` / `room.frame` —— 已被 `room.state` 覆盖，不单独定义。

**订阅的访问控制**：`room_id` 由客户端提供，因此服务端必须校验调用者确实是该房间
成员（用一次 `GetRoomState` 即可判定），否则任何登录用户都能长期订阅别人的房间、
看到对方的血量。非成员返回 `400 not_a_member`。

**轮询兜底仍然保留**：`GET /api/v1/rooms/state` 与 `GET /api/v1/results` 不因为
推送上线而删除。断线、代理不支持 SSE、或客户端尚未接入推送时，它们仍是可用路径。

## 3. 服务间 Protobuf

服务集合固定为三个，见 [ADR-0003](adr/0003-scope-reduction.md)：

- `GatewayService`
- `MatchService`（已实现，TASK-007，契约见 `api/proto/match.proto`）
- `RoomService`（已实现，TASK-008，契约见 `api/proto/room.proto`）

**不实现** `PlayerService` 与 `SettlementService`；不要为它们创建 `.proto`。

每个 RPC 必须包含：

- 请求 ID。
- 调用者和目标业务 ID。
- 必要的版本或乐观锁字段。
- 可重试错误码。

`MatchService` 是本项目第一个服务间契约，落地时确认了两条约定：

- 每个 RPC 都带 `request_id`，每个响应都带可判定的 `error.reason`——排查跨服务问题
  时这是唯一的关联键。
- 调用方（Gateway）**把传输层失败与业务错误分开**：`controller.Failed()` 表示对端
  不可用（映射 503），响应里的 `error` 才是业务结果（映射 400/429/500）。
  混在一起会把「队列已满」误报成「服务不可用」。

`RoomService` 是第三个契约，沿用同一套约定，并额外确立一条：

- **幂等键由业务字段承担，而不是 `request_id`**。`CreateRoom` 以 `match_id` 为幂等键、
  对局结果以 `match_id` 为主键；`request_id` 只用于跨服务日志关联。理由是幂等必须
  在**重试方无法保证携带同一个 request_id** 时仍然成立——Match 超时后重试，
  真正保证「不会造出第二个房间」的是 match_id，不是 request_id。
- **把「数据还没准备好」与「数据不存在」分开**：`ROOM_RESULT_PENDING` 与
  `ROOM_NOT_FOUND` 是两个不同的错误码，对应 503 与 404（见第 2 节的错误语义表）。

错误码分类：

| 类型 | 示例 | 调用方行为 |
|---|---|---|
| 输入错误 | 非法参数 | 不重试，返回客户端 |
| 状态冲突 | 玩家已在队列 | 返回当前状态或幂等成功 |
| 资源不存在 | 房间不存在 | 返回明确错误 |
| 暂时失败 | Redis/MySQL 超时 | 有上限地重试 |
| 限流 | 队列已满 | 退避后重试或降级 |
| 内部错误 | 未分类异常 | 记录并告警 |

## 4. 数据存储边界

### Redis

使用场景：

- 登录 Session 和连接映射。
- 匹配队列临时状态。
- 房间短期路由信息。
- 限流和短期幂等标记。

当前落地情况（TASK-007 记录）：**匹配队列没有放进 Redis**。队列所有者是 Match，
Phase 1 用进程内存实现（见 `docs/01-architecture.md` 第 5 节），快照与重启恢复属
Phase 2。这样做避免了「Gateway 直读 Match 的数据」这一所有权变化——若改为
Gateway 直接读 Redis 中的队列状态，必须先写 ADR。

约束：

- 关键数据必须能从 MySQL 或业务事件重建，除非 ADR 明确例外。
- Key 必须带环境和服务前缀。
- TTL 必须明确，禁止无期限堆叠临时 Key。

### MySQL

表清单（只保留有明确所有者的表）：

- `players`（所有者：Gateway）
- `match_results`（所有者：Room/Battle）
- `schema_migrations`（迁移记录）

**不建** `player_stats`、`room_records`、`processed_events`：前两者在当前范围内没有
写入者；`processed_events` 属于事件消费链路，已列为非目标。见
[ADR-0003](adr/0003-scope-reduction.md)。

约束：

- 每个表有明确所有者和访问服务。
- 迁移必须版本化，可审计，不在启动时静默改表。
- 战绩和结算使用唯一约束保护幂等。
- 不使用 MySQL 保存高频帧日志。

#### 已实现的表（截至 TASK-006）

**`players` — 玩家档案**

| 列 | 类型 | 说明 |
|---|---|---|
| `player_id` | `VARCHAR(64)` | 主键，玩家唯一标识，如 `p-0001` |
| `account` | `VARCHAR(64)` | 登录账号名，`uk_players_account` 唯一约束 |
| `display_name` | `VARCHAR(64)` | 展示名 |
| `status` | `VARCHAR(16)` | `active` / `disabled`，默认 `active` |
| `created_at` | `TIMESTAMP` | 创建时间 |
| `updated_at` | `TIMESTAMP` | 更新时间，自动刷新 |

- 所有者：**Gateway**，依据
  [ADR-0002](adr/0002-gateway-temporary-player-ownership.md) 与
  [ADR-0003](adr/0003-scope-reduction.md)。原定的 Player/State 所有者已取消。
- 访问方式：Gateway **只读**，经 `PlayerReader` 接口读取；其余服务不得直接读写。
- **不含密码列**：第一版账号密码保留在代码中作为测试数据，见
  `docs/07-open-decisions.md` 的 D-002。

**`match_results` — 对局结果**

| 列 | 类型 | 说明 |
|---|---|---|
| `match_id` | `VARCHAR(64)` | 主键，同时是结算幂等业务键 |
| `room_id` | `VARCHAR(64)` | 来源房间，可空 |
| `winner_id` | `VARCHAR(64)` | 胜者 `player_id`，平局为 NULL |
| `player_count` | `INT` | 参战人数 |
| `started_at` | `TIMESTAMP` | 开局时间，可空 |
| `finished_at` | `TIMESTAMP` | 结束时间，默认当前时间 |

- 所有者：**Room/Battle**，依据 [ADR-0003](adr/0003-scope-reduction.md)。
  原定的 Settlement 所有者已取消。
- 访问方式：对局结束时由 Room/Battle **同步幂等**写入，以 `match_id` 为幂等业务键；
  **不经过任何消息队列**。重复写入同一 `match_id` 返回已有结果，不产生第二行。

迁移文件位于 `migrations/`，按 `NNN_描述.sql` 命名且必须幂等。
注意：`/docker-entrypoint-initdb.d` 只在数据目录为空时执行一次，
因此新增迁移需显式应用（`scripts/verify-login.sh` 会做这件事）。
`004_seed_test_players.sql` 含测试数据，**仅用于开发环境**，文件头已标注。

### 消息队列：不使用

**不使用 Kafka 或任何消息队列**，见 [ADR-0003](adr/0003-scope-reduction.md)。

因此本项目**没有**领域事件、事件回放、异步结算 Worker、消费位点、死信队列和
消费积压监控。跨服务协作只走 brpc 同步调用，对局结果由 Room/Battle 同步幂等写入
MySQL。

## 5. 幂等规则

以下操作必须定义幂等键：

- 登录会话创建。
- 进入和取消匹配。
- 创建和加入房间。
- 对局结果写入。

推荐：

- 客户端请求使用 `request_id`。
- 对局结果使用 `match_id` 作为唯一业务键。
- Redis 只做短期快速判断，MySQL 唯一约束作为最终保护。
- 幂等冲突返回已有结果，而不是创建新副作用。

## 6. 数据一致性

- 房间实时状态由 Room/Battle Service 负责，其他服务不得直接修改。
- 对局结果写入采用**同步幂等**：一次写入成功即对客户端可见，不存在"处理中"中间态。
- 缓存更新失败不能导致数据库出现错误成功记录。
- Redis 和 MySQL 同时更新时，明确先后顺序、失败补偿和重建策略。
- 跨服务操作不伪装成原子事务；使用状态机和补偿。

## 7. Schema 演进

- Protobuf 字段只追加，不随意修改标签号。
- 删除字段先标记 `reserved`。
- JSON 消息必须包含 `version`。
- 数据库迁移与代码发布分离，确保滚动升级期间兼容。
- Schema 变化必须更新本文档并增加测试。
