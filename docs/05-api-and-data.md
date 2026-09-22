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
| GET | `/api/v1/rooms/{room_id}` | 查询房间状态 | 待 TASK-008 |
| GET | `/api/v1/results/{match_id}` | 查询对局结果 | 待 Phase 5 |
| GET | `/health` | 健康检查 | 已实现（brpc 内置服务） |

接口名称在实现前可以调整，但必须更新本文档。

**已发生的调整（TASK-007）**：取消匹配原设计为 `DELETE /api/v1/matches/current`，
与查询接口共用同一路径。实现时确认 **brpc 的 restful 映射按路径分派，不支持按
HTTP 方法分派**，同一路径无法同时承载 GET 与 DELETE，因此改为
`POST /api/v1/matches/current/cancel`。这不是偏好问题，是框架约束。

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

### WebSocket 消息信封

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
- `type` 使用小写点分命名。
- `request_id` 只用于需要响应的客户端请求。
- `sequence` 用于房间内消息排序和发现缺口。
- 服务端必须校验消息大小、字段类型和当前连接状态。

首批消息类型：

- `session.ready`
- `match.updated`
- `room.joined`
- `room.state`
- `room.frame`
- `room.finished`
- `error`

## 3. 服务间 Protobuf

建议服务：

- `GatewayService`
- `MatchService`（已实现，TASK-007，契约见 `api/proto/match.proto`）
- `RoomService`
- `PlayerService`
- `SettlementService`

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
- 排行榜视图。
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

建议表：

- `players`
- `player_stats`
- `match_results`
- `room_records`
- `processed_events`
- `schema_migrations`

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

- 所有者：Player/State（正式归属）。
- 访问方式：Phase 1 期间由 Gateway **只读**，依据
  [ADR-0002](adr/0002-gateway-temporary-player-ownership.md)；其余服务不得直接读写。
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

- 所有者：Settlement（正式归属）。
- 访问方式：Phase 1 不写入，仅建表；Settlement Worker 在 Phase 5 落地后写入。

迁移文件位于 `migrations/`，按 `NNN_描述.sql` 命名且必须幂等。
注意：`/docker-entrypoint-initdb.d` 只在数据目录为空时执行一次，
因此新增迁移需显式应用（`scripts/verify-login.sh` 会做这件事）。
`004_seed_test_players.sql` 含测试数据，**仅用于开发环境**，文件头已标注。

### Kafka

建议事件：

- `match.created`
- `match.cancelled`
- `room.created`
- `room.finished`
- `player.disconnected`
- `player.reconnected`

事件要求：

- 使用 `event_id`、`event_type`、`occurred_at` 和 `schema_version`。
- 使用业务键分区，例如 `room_id` 或 `match_id`。
- 消费方必须假设消息可能重复和乱序。
- 消费成功后记录处理结果，支持安全重试。

## 5. 幂等规则

以下操作必须定义幂等键：

- 登录会话创建。
- 进入和取消匹配。
- 创建和加入房间。
- 对局结算。
- 排行榜更新。

推荐：

- 客户端请求使用 `request_id`。
- 业务结算使用 `match_id` 作为唯一业务键。
- Redis 只做短期快速判断，MySQL 唯一约束作为最终保护。
- 幂等冲突返回已有结果，而不是创建新副作用。

## 6. 数据一致性

- 房间实时状态由 Room Service 负责，其他服务不得直接修改。
- 结算采用最终一致，客户端查询需要区分“处理中”和“已完成”。
- 缓存更新失败不能导致数据库出现错误成功记录。
- Redis 和 MySQL 同时更新时，明确先后顺序、失败补偿和重建策略。
- 跨服务操作不伪装成原子事务；使用状态机、事件和补偿。

## 7. Schema 演进

- Protobuf 字段只追加，不随意修改标签号。
- 删除字段先标记 `reserved`。
- JSON 消息必须包含 `version`。
- 数据库迁移与代码发布分离，确保滚动升级期间兼容。
- Schema 变化必须更新本文档并增加测试。
