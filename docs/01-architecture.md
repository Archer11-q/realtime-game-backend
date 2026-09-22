# 架构设计

> 状态：已确认（2026-09-14）

## 1. 架构原则

1. 先保证单机端到端正确，再根据真实瓶颈进入分布式阶段。
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
 +-----------+-----------+------------+
 |                       |            |
 v                       v            v
Match Service      Room/Battle    Player/State
 |                  Service         Service
 |                       |            |
 +-----------+-----------+------------+
             |
             v
     Kafka Domain Events
             |
             v
    Settlement Worker
             |
             v
     MySQL / Redis
```

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
- 产生结算和战斗领域事件。

不负责：

- 玩家登录和连接管理。
- 全局匹配策略。
- 排行榜和长期战绩写入。

### Player/State Service

职责：

- 管理玩家基本资料、会话映射和战绩查询。
- 读取或维护排行榜视图。
- 为模块提供不跨数据所有权的状态查询。

不负责：

- 实时战斗循环。
- 房间帧推进。
- 直接消费客户端 WebSocket。

### Settlement Worker

职责：

- 消费房间产生的结算事件。
- 在幂等保护下更新战绩、积分和排行榜。
- 支持失败重试和重复消息处理。
- 记录处理结果和可观测性指标。

不负责：

- 同步阻塞客户端请求。
- 修改房间实时状态。

## 4. 核心数据流

### 4.1 登录

```text
浏览器
  -> Gateway HTTP 登录
  -> 校验账号/测试凭证
  -> Player/State 获取玩家信息
  -> Redis 建立会话
  -> 返回 Token 和玩家信息
```

### 4.2 匹配

```text
浏览器
  -> Gateway 发起匹配（HTTP）
  -> Gateway 从会话取出 player_id，调用 MatchService.EnqueueMatch
  -> Match 入队并尝试配对（FIFO，两人一局）
  -> 匹配成功，分配 room_id
  -> Gateway 返回当前状态；客户端轮询 /api/v1/matches/current 领取结果
  -> 玩家通过 WebSocket 进入房间
```

Phase 1 的落地差异（TASK-007 记录，避免把计划当成已实现）：

- 「Match 请求 Room 创建房间」这一步**尚未发生**。Room/Battle Service 属 TASK-008，
  因此匹配成功只分配一个 `room_id`（由 `match_id` 派生），不产生任何房间状态。
  接口已抽象为 `RoomAllocator`，TASK-008 替换实现即可。
- 「通知玩家」当前用**轮询**实现，WebSocket 推送属 TASK-009。轮询接口在
  WebSocket 落地后仍作为兜底保留。
- 配对规则只有 FIFO 两人一局，**没有分差放宽**：当前没有任何分数体系，
  先实现等于把未验证的评分模型固化进契约。

### 4.3 对战

```text
浏览器输入
  -> Gateway WebSocket
  -> Room 校验输入并推进逻辑帧
  -> Room 广播状态或帧数据
  -> Gateway 推送至房间内玩家
  -> 结算条件满足
  -> Room 发布领域事件
```

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

### 4.5 异步结算

```text
Room 发布 MatchFinished
  -> Kafka
  -> Settlement 消费
  -> 幂等检查
  -> 更新 MySQL 战绩/积分
  -> 更新 Redis 排行榜
  -> 记录处理结果
```

## 5. 状态归属

| 状态 | 所有者 | 存储 | 恢复策略 |
|---|---|---|---|
| WebSocket 连接 | Gateway | 内存 | 客户端重连后重建 |
| 玩家会话 | Gateway/Player | Redis + MySQL | 使用 Session 映射恢复 |
| 匹配队列 | Match | 内存（Phase 1）；Redis 快照属 Phase 2 | 当前重启即丢失；快照与重建在 Phase 2 定义 |
| 房间权威状态 | Room | 内存 + 快照 | 从最近快照和事件恢复 |
| 玩家档案和战绩 | Player/Settlement | MySQL | 数据库恢复 |
| 排行榜 | Player/State | Redis | 数据可从 MySQL 重建 |
| 领域事件 | Kafka | Kafka 日志 | 按位点重放 |

任何模块不得绕过所有者直接修改数据。

**已知例外（Phase 1）**：`players` 表由 Gateway 直接**读取**，用于玩家档案校验。
这是带退出条件的临时安排，依据
[ADR-0002](adr/0002-gateway-temporary-player-ownership.md)；Player/State 服务落地后
必须收敛。Gateway 对该表只读不写，测试数据由迁移脚本写入。

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
| Kafka 积压 | 异步结算延迟，增加告警和背压，不影响已有对局 |
| 重复结算 | 幂等键阻止重复战绩 |

## 8. 部署演进

### 阶段一：本地单实例

- C++ 服务在 WSL 中运行。
- Redis、MySQL 使用 Docker。
- 目标是端到端功能正确和可调试。

### 阶段二：Docker Compose 集成

- 全部服务与依赖通过 Compose 启动。
- 使用健康检查、网络、卷和配置注入。
- 目标是可复现和现场演示。

### 阶段三：多实例和协调服务

- 仅在出现容量或故障需求时引入 etcd。
- 支持多 Gateway 和多 Match 实例。
- Room 根据状态和恢复模型决定是否迁移。

### 阶段四：事件和故障恢复

- 在结算异步化、回放和统计需求明确后引入 Kafka。
- 加入故障注入、恢复测试、背压和容量治理。
- 暂不默认引入 Kubernetes。

## 9. 对外与对内接口

- 浏览器到 Gateway：HTTP + WebSocket，消息使用版本化 JSON 信封。
- C++ 服务之间：brpc + Protobuf，接口定义位于 `api/proto/`。
- 异步领域事件：Kafka + Protobuf 或带版本的 JSON，Topic 规则另立 ADR。
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

- `include/common/token.hpp`：会话 Token 生成。Gateway 使用，Player、Settlement
  后续也会使用，故放在公共目录。
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
- 从 Docker Compose 升级到 Kubernetes。
