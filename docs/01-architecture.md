# 架构设计

> 状态：已确认（2026-09-14），**2026-10-02 按 [ADR-0003](adr/0003-scope-reduction.md)
> 裁剪范围：服务固定为 Gateway / Match / Room-Battle 三个，不实现 Player/State、
> Settlement、Kafka、etcd 与多实例。**

## 1. 架构原则

1. 先保证单机端到端正确。项目范围内**不进入多实例**，深度优先于广度，
   见 [ADR-0003](adr/0003-scope-reduction.md)。
2. 服务的拆分依据是数据所有权、故障隔离和独立扩展需求，而不是微服务数量。
3. 同步请求走 RPC，异步事实和后续处理走事件。
4. 每个状态必须有明确所有者、生命周期、持久化策略和恢复方式。
5. 所有可能重试的操作必须幂等。
6. 先有基线、指标和失败证据，再做优化或增加组件。
7. 前端只负责演示和操作，不承载业务真相。

## 2. 系统上下文

```text
玩家浏览器 / 机器人客户端
             |
             | HTTP：登录、查询、操作入口
             | WebSocket：连接状态、房间消息、对战事件
             v
       Gateway Service
             |
             | brpc + Protobuf
             v
 +-----------+------------+
 |                        |
 v                        v
Match Service      Room/Battle Service
 |                        |
 |                        | 对局结束时同步幂等写入
 +-----------+------------+
             |
             v
        Redis / MySQL
```

**服务集合固定为三个**，见 [ADR-0003](adr/0003-scope-reduction.md)。
原目标架构中的 Player/State 服务、Settlement Worker、Kafka 与 etcd
**已列为非目标，不实现**；跨服务协作只走 brpc 同步调用。

## 3. 服务职责

### Gateway Service

职责：

- 接收 HTTP 和 WebSocket 连接。
- 校验登录凭证并建立逻辑会话。
- 管理心跳、连接状态、限流和优雅关闭。
- 将客户端请求转换为内部 brpc 调用。
- 向指定玩家推送消息，不直接修改房间权威状态。

不负责：

- 匹配算法。
- 房间生命周期和战斗状态。
- 玩家长期档案和结算逻辑。

### Match Service

职责：

- 维护匹配队列。
- 处理进入、取消和超时。
- 根据规则选择玩家并请求创建房间。
- 保证同一玩家不重复进入多个有效队列或匹配结果。

不负责：

- 保存战斗帧状态。
- 直接向客户端发送消息。
- 玩家长期数据持久化。

### Room/Battle Service

职责：

- 创建、加入、开始和销毁房间。
- 保存当前房间的权威状态。
- 接收玩家输入、推进逻辑帧并生成房间消息。
- 保存必要快照，支持重连和故障恢复。
- 对局结束时**同步幂等**写入 `match_results`（以 `match_id` 为幂等业务键）。

不负责：

- 玩家登录和连接管理。
- 全局匹配策略。
- 玩家长期档案。

#### 房间生命周期（TASK-008 落地）

```text
CREATED ──► WAITING ──► PLAYING ──► FINISHING ──► FINISHED ──► CLOSED
   │           │            │
   └───────────┴────────────┴──► ABORTED ──► CLOSED
```

- `FINISHING` 是**必须存在的独立状态**：已分出胜负但结果尚未落库。写入失败时房间
  停在此状态并按固定间隔重试，查询接口返回 `result_pending`（503），
  **不会**返回内存里的胜负。这样进程崩溃后客户端不会拿到一个查不到的结论。
- `ABORTED`：等待玩家加入超时等异常终止。**不写** `match_results`——
  写一行 winner 为空的记录等于把「没打成」伪装成「打平了」。
- `CLOSED` 不是显式状态：房间在结束后保留一段固定时长（供客户端轮询领取结果），
  到期由 Room 自己的 tick 回收；回收后历史对局仍可从 MySQL 查询。

#### 对局规则（TASK-008 决策 A，数值定义在 `src/room/room_types.hpp`）

- 两人一局，与 Match 的 FIFO 两人配对一致。
- 服务端按 10 Hz 推进权威状态；客户端只提交输入，不参与判定。
- 输入只有「攻击」。**每个玩家每帧最多结算一次**：伤害按帧结算，允许同帧多次
  会变成「谁点得快谁赢」，那是延迟决定胜负，不是对局规则。
- 一方 HP 归零则该方落败；达到最大帧数时 HP 高者胜，相同为平局。
- 单次 tick 最多推进固定帧数，超出的部分丢弃。进程被挂起后的补偿会造成 CPU 尖峰，
  而这段时间的输入本来也已失去意义。

### 已取消的服务（不实现）

以下服务**不实现**，见 [ADR-0003](adr/0003-scope-reduction.md)。
不要为它们创建目录、proto、进程入口或 Compose 服务：

| 原服务 | 原职责 | 现在的处理方式 |
|---|---|---|
| Player/State | 玩家资料、会话映射、战绩查询、排行榜 | `players` 表改由 Gateway 拥有；排行榜移出范围 |
| Settlement Worker | 消费结算事件、幂等更新战绩和排行榜 | 结算并入 Room/Battle 的结束流程，同步幂等写入，不消费事件 |

## 4. 核心数据流

### 4.1 登录

```text
浏览器
  -> Gateway HTTP 登录
  -> 校验账号/测试凭证
  -> 从 players 表获取玩家档案（Gateway 拥有该表，见 ADR-0002/ADR-0003）
  -> Redis 建立会话
  -> 返回 Token 和玩家信息
```

### 4.2 匹配

```text
浏览器
  -> Gateway 发起匹配（HTTP）
  -> Gateway 从会话取出 player_id，调用 MatchService.EnqueueMatch
  -> Match 入队并尝试配对（FIFO，两人一局）
  -> 配对成立后，Match 在**锁外**调用 RoomService.CreateRoom
  -> Room 以 match_id 为幂等键创建房间，返回 room_id
  -> Gateway 返回当前状态；客户端轮询 /api/v1/matches/current 领取结果
  -> 玩家通过 HTTP 加入房间（WebSocket 属 TASK-009）
```

落地情况（TASK-008 更新，避免把计划当成已实现）：

- **「Match 请求 Room 创建房间」已经发生**（TASK-008）。`RoomAllocator` 的真实实现
  `BrpcRoomAllocator` 调用 `RoomService.CreateRoom`，以 `match_id` 为幂等键。
- **分配在队列锁之外**：配对拆成「锁内取人并标记分配中 → 锁外 brpc 调用 →
  锁内提交」三步。持锁调用远程会让一次对端超时卡住整个队列。
  分配失败时玩家退回队首，而不是被标记为超时——Room 不可用不是玩家的错。
- 「通知玩家」仍用**轮询**实现，WebSocket 推送属 TASK-009。轮询接口在
  WebSocket 落地后仍作为兜底保留。
- 配对规则只有 FIFO 两人一局，**没有分差放宽**：当前没有任何分数体系，
  先实现等于把未验证的评分模型固化进契约。

### 4.3 对战

目标形态（WebSocket 落地后）：

```text
浏览器输入
  -> Gateway WebSocket
  -> Room 校验输入并推进逻辑帧
  -> Room 广播状态或帧数据
  -> Gateway 推送至房间内玩家
  -> 结束条件满足
  -> Room 同步幂等写入对局结果（不走消息队列）
```

**TASK-008 的实际形态**（WebSocket 属 TASK-009，因此这里用 HTTP 轮询）：

```text
浏览器轮询 /api/v1/rooms/state?room_id=...
  -> Gateway 鉴权后转发 RoomService.GetRoomState
  -> Room 返回权威快照（帧号、双方 HP、状态）
浏览器提交输入 POST /api/v1/rooms/input
  -> Gateway 从会话取 player_id，转发 RoomService.SubmitInput
  -> Room 记录本帧攻击，在下一个 tick 结算
Room 自己的推进线程每 50 ms 调用一次 RoomManager::Tick
  -> 服务端权威推进，客户端不参与判定
```

关键点：**推进不依赖客户端请求**。房间的帧推进由 Room 进程自己的定时线程驱动，
否则「双方都不请求」就会让对局永远停在原地。

### 4.4 断线重连

```text
连接异常
  -> Gateway 标记 Session 为 DISCONNECTED
  -> 保留房间位置和宽限期
  -> 玩家携带 Session/Token 重连
  -> Gateway 校验会话
  -> 从房间读取快照/缺失帧
  -> 恢复 ACTIVE 并重新绑定连接
```

### 4.5 对局结果写入（同步幂等，不走消息队列）

```text
对局满足结束条件
  -> Room 以 match_id 为幂等业务键写入 match_results
  -> 重复写入同一 match_id 返回已有结果，不产生第二行
  -> 客户端查询对局结果
```

这里**不使用消息队列**：结算只有一次幂等写入，异步化不带来收益，反而增加故障面。
依据见 [ADR-0003](adr/0003-scope-reduction.md)。

## 5. 状态归属

| 状态 | 所有者 | 存储 | 恢复策略 |
|---|---|---|---|
| WebSocket 连接 | Gateway | 内存 | 客户端重连后重建 |
| 玩家会话 | Gateway | Redis | 使用 Session 映射恢复 |
| 玩家档案 | Gateway | MySQL（`players`） | 数据库恢复 |
| 匹配队列 | Match | 内存（Phase 1）；Redis 快照属 Phase 2 | 当前重启即丢失；快照与重建在 Phase 2 定义 |
| 房间权威状态 | Room/Battle | 内存 + 快照 | 从最近快照恢复 |
| 对局结果 | Room/Battle | MySQL（`match_results`） | 数据库恢复，`match_id` 保证幂等 |

任何模块不得绕过所有者直接修改数据。

**所有权变更（2026-10-02）**：`players` 表的正式所有者由 Gateway 承担，依据
[ADR-0002](adr/0002-gateway-temporary-player-ownership.md) 与
[ADR-0003](adr/0003-scope-reduction.md)。Gateway 对该表**只读**，测试数据由迁移脚本
写入。原定的 Player/State 与 Settlement 所有者已取消；排行榜与领域事件移出范围。

## 6. 一致性和幂等

- 登录和匹配请求使用 `request_id` 作为幂等键。
- 结算使用 `match_id` 作为业务唯一键。
- 房间状态变化通过状态机限制合法迁移。
- Redis 缓存允许失效后从 MySQL 重建。
- 跨服务调用失败时，由调用方明确重试、回滚或补偿策略。
- 不承诺无法实现的“端到端严格一次”，优先实现幂等和可安全重试。

## 7. 失败模型

| 故障 | 预期行为 |
|---|---|
| 浏览器断网 | Session 进入 DISCONNECTED，宽限期内保留房间位置 |
| Gateway 崩溃 | 客户端重连，Gateway 从 Redis/Player 恢复会话 |
| Match 崩溃 | 已完成匹配不丢失；队列状态按既定策略恢复或重建 |
| Room 崩溃 | 根据快照和事件恢复，无法恢复时明确结束并通知玩家 |
| MySQL 暂时不可用 | 写请求失败并返回可重试错误，不写假成功 |
| Redis 暂时不可用 | 关键会话降级或请求失败，不静默继续错误状态 |
| 重复写入对局结果 | `match_id` 幂等键阻止重复行 |

## 8. 部署演进

### 阶段一：本地单实例

- C++ 服务在 WSL 中运行。
- Redis、MySQL 使用 Docker。
- 目标是端到端功能正确和可调试。

### 阶段二：Docker Compose 集成

- 全部服务与依赖通过 Compose 启动。
- 使用健康检查、网络、卷和配置注入。
- 目标是可复现和现场演示。

### 阶段三：故障注入和可靠性验证（当前路线的终点）

- **不做多实例，不引入 etcd、Kafka 和 Kubernetes**，依据
  [ADR-0003](adr/0003-scope-reduction.md)。
- 在单实例内验证：进程崩溃恢复、断线重连与宽限期、依赖不可用降级、
  优雅退出与排空、连接风暴、长稳运行。
- 每个故障场景都需要可重复执行的注入脚本和实测数据，不靠推断。

## 9. 对外与对内接口

- 浏览器到 Gateway：HTTP + WebSocket，消息使用版本化 JSON 信封。
- C++ 服务之间：brpc + Protobuf，接口定义位于 `api/proto/`。
- 异步领域事件：**不使用**。跨服务协作只走 brpc 同步调用，见
  [ADR-0003](adr/0003-scope-reduction.md)。
- 日志和指标：结构化字段，统一请求 ID、会话 ID、房间 ID 和匹配 ID。

详细约束见 [接口、数据与协议](05-api-and-data.md)。

## 10. 安全边界

- 第一版使用测试账号和 Token，不实现完整账号体系。
- 所有客户端输入必须校验，不能信任房间 ID、玩家 ID 和帧号。
- 日志不输出 Token、密码和完整敏感载荷。
- 服务配置通过环境变量注入，仓库只保存 `.env.example`。
- 注意区分两类地址：宿主机的 `REDIS_HOST`/`MYSQL_HOST` 用于 WSL 中直接连接的
  客户端，容器内互访必须使用 Compose 服务名 `redis`/`mysql` 和容器端口
  6379/3306；宿主机的 `REDIS_PORT`/`MYSQL_PORT` 只影响端口映射。
- 后续加入请求限流、连接数限制和异常行为统计。

## 11. 代码与测试的放置规则

### 头文件

本项目只有一处集中头文件目录 `include/common/`，它的定义是**用途**而非**位置**：

| 位置 | 放什么 | 判断依据 |
|---|---|---|
| `include/common/` | 被两个及以上服务使用的公共代码 | 有多方依赖 |
| `src/<service>/` | 只属于该服务的头文件，与实现放在一起 | 只有本服务依赖 |

判断依据是**依赖方向**，不是“它是不是头文件”：

- 只有一个服务使用的头文件提升到 `include/`，会让公共目录逐渐变成无归属的
  杂物间，因此**不因整齐而提升**。
- 反过来，若某个头文件被第二个服务使用时，**必须**移入 `include/common/`，
  避免服务之间通过源码目录互相依赖。

示例（截至 TASK-005）：

- `include/common/token.hpp`：会话 Token 生成。Gateway 使用；若将来出现第二个
  使用方（例如拆出独立鉴权服务），再按依赖方向评估是否保留在公共目录。
- `src/gateway/error.hpp`：只服务 Gateway，且依赖 `gateway.pb.h`。放进 `include/`
  会诱导其他服务反向依赖 Gateway 的契约，故留在服务目录内。

### 测试

- 单元测试统一放在 `tests/unit/<service>/`，与 `src/<service>/` 一一对应。
- 集成测试与端到端测试分别放 `tests/integration/` 和 `tests/e2e/`。
- 测试**不放在 `src/` 内**：`src/<service>/` 只负责编译该服务本身，
  不判断测试依赖是否可用，也不定义测试目标。
- 测试文件使用与实现相同的 include 路径，因为实现所在的库已把自身目录以
  `PUBLIC` 方式暴露。

### 构建顺序

- `CMakeLists.txt` 先处理 `src`，再处理 `tests`。
- GoogleTest 的解析由 `tests/` 自行完成；未启用 vcpkg 时跳过依赖 vcpkg 的服务测试，
  通过 `if(TARGET <lib>)` 判断，而不是让服务目录感知测试依赖。

## 12. 架构变更规则

以下变化必须先写 ADR：

- 增加或合并服务。
- 改变状态所有者。
- 改变同步 RPC 与异步事件的边界。
- 引入新的中间件或第二套协议。
- 修改一致性、幂等或恢复策略。

**额外前置条件**：Kafka、etcd、多实例部署、Kubernetes、独立 Player/State 服务、
独立 Settlement 服务、排行榜、匹配分差放宽已由
[ADR-0003](adr/0003-scope-reduction.md) 列为非目标。引入它们**必须先从"替代关系"
角度撤销或修改 ADR-0003**；只写一个新 ADR 说明"现在需要了"不足以生效。
