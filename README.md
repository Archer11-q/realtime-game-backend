# Realtime Game Backend

> 面向小型实时对战产品的游戏服务端参考实现
>
> C++20 / brpc / Protobuf / SSE / Redis / MySQL / Docker Compose / Vue 3

## 项目定位

本项目是一套面向小型实时对战产品的游戏服务端参考实现，重点验证主流后端技术栈、
服务边界与数据所有权设计、有状态服务的持久化与恢复、故障处理、可观测性和工程
交付能力。

它不是通用 RPC 框架，也不是完整商业游戏，而是一套可运行、可部署、可压测、
可故障注入和可现场演示的实时对战服务端。

## 解决的问题

实时对战业务通常会遇到以下问题：

- 玩家断线后，会话、房间和战斗状态容易丢失。
- 匹配、房间和对局结果之间的数据边界不清晰。
- 请求重试可能造成重复入队或重复写入对局结果。
- 进程崩溃后房间状态缺少明确可验证的恢复边界。
- 依赖（Redis/MySQL）不可用时容易写出"假成功"。
- 只有日志，没有指标、链路和可复现的性能基线。

本项目通过明确服务边界、持久化关键状态、引入幂等、故障注入和可观测性来逐个解决
这些问题，**每个结论都必须有可重复执行的验证脚本和实测数据**。

### 明确不做的事

本项目**不接入消息队列、服务发现、多实例或容器编排**。这不是"还没做"，
而是经过评估的取舍：当前架构里没有它们的真实消费者，引入只会增加故障面和调试
成本。完整理由与替代方案见 [ADR-0003](docs/adr/0003-scope-reduction.md)。

## 核心用户流程

```text
登录
  -> 进入大厅
  -> 发起匹配
  -> 匹配成功并创建房间
  -> SSE 推送进入对战
  -> 发生断线并重连
  -> 对局结束
  -> Room/Battle 同步幂等写入对局结果
  -> 查询对局结果
```

## 目标架构

**服务集合固定为三个**（见 [ADR-0003](docs/adr/0003-scope-reduction.md)）：

```text
浏览器演示页 / 机器人客户端
             |
       HTTP + SSE
             |
       Gateway Service          ← 会话、鉴权、路由、限流
             |
      brpc + Protobuf
             |
   +---------+----------+
   |                    |
Match Service     Room/Battle Service   ← 房间状态、快照、对局结果
   |                    |
   +---------+----------+
             |
        Redis / MySQL

Redis：会话、缓存、短期状态
MySQL：players（Gateway 拥有）、match_results（Room/Battle 拥有）
```

**不实现**：Kafka、etcd、Player/State 服务、Settlement 服务、多实例、Kubernetes。
跨服务协作只走 brpc 同步调用；对局结果由 Room/Battle 同步幂等写入，不走异步链路。

详细边界见 [架构设计](docs/01-architecture.md)，迭代依据见
[迭代路线图](docs/02-roadmap.md)。

## 技术基线

| 领域 | 选择 | 用途 |
|---|---|---|
| 开发环境 | WSL2 Ubuntu 26.04 LTS + CLion | Linux 开发、调试和 WSL Toolchain |
| 语言与构建 | C++20、GCC 15.2、CMake 4.2.3、Ninja 1.13.2、vcpkg | 主流 C++ 工程构建 |
| 服务通信 | brpc + Protobuf | 内部服务调用和协议契约 |
| 客户端通信 | HTTP + SSE（服务端推送） | 浏览器登录、状态推送和实时消息 |
| 数据存储 | Redis + MySQL | 会话、缓存、玩家档案和对局结果 |
| 前端演示 | Vue 3 + TypeScript + Vite + Canvas | 可视化和端到端演示 |
| 可观测性 | Prometheus + Grafana + OpenTelemetry | 指标、日志、链路和面板 |
| 工程质量 | GoogleTest、ASan、TSan、UBSan | 测试、内存和并发检查 |
| 部署 | Docker Compose、GitHub Actions | 本地集成环境和持续集成 |

技术选型的决策记录见
[ADR-0001](docs/adr/0001-initial-platform-and-stack.md)；
范围裁剪见 [ADR-0003](docs/adr/0003-scope-reduction.md)。

## 明确的非目标

- 不做通用 RPC 框架，不重复实现基础通信能力。
- 不追求商业级游戏功能，不做复杂渲染、美术、账号平台和支付系统。
- 不以代码量或版本号作为迭代成果。
- 不在没有容量证据时做性能优化，不编造压测数字。
- 不让多个 AI 同时修改同一分支。

### 已确认的非目标（不做，且不是"延后"）

依据 [ADR-0003](docs/adr/0003-scope-reduction.md)，以下内容**不实现**：

- Kafka 及任何消息队列、领域事件、事件回放、异步结算 Worker
- etcd 及任何服务注册、发现、租约机制
- 多实例部署与水平扩展
- Kubernetes / k3s / 容器编排
- 独立的 Player/State 服务与独立的 Settlement 服务
- 排行榜
- 匹配分差放宽 / MMR / 评分体系

要恢复其中任何一项，必须先撤销或修改 ADR-0003。

## 当前状态

当前处于 `Phase 1：最小业务闭环`。Phase 0 已完成（2026-09-17），其退出标准均已
验证：CI 在功能分支与 main 上均为 success，登录链路的单元测试与端到端验收全部通过。

已完成的能力：

- Gateway 提供登录、查询当前玩家、登出三个接口，会话存于 Redis。
- 玩家档案存于 MySQL，登录时从 `players` 表读取（Gateway 拥有该表，见
  ADR-0002 与 ADR-0003）。
- brpc + Protobuf 的构建与运行链路已验证（vcpkg 提供依赖）。
- Redis 与 MySQL 通过 Docker Compose 启动，含健康检查与数据卷。
- 两者的不可用路径均返回 503，且恢复后无需重启服务。
- CI 覆盖三个构建预设与代码格式检查。

当前任务见 [TASKS.md](docs/TASKS.md)，开发过程见
[devlog.md](docs/devlog.md)。

## 仓库结构

```text
realtime-game-backend/
├── api/proto/                 # Protobuf 接口契约（gateway / match / room）
├── src/
│   ├── gateway/               # HTTP/SSE 网关（含本服务内部头文件）
│   ├── match/                 # 匹配服务
│   └── room/                  # 房间和战斗服务
├── include/common/            # 跨服务公共基础设施
├── tests/
│   ├── unit/                  # 单元测试，按服务分子目录（unit/<service>/）
│   ├── integration/
│   └── e2e/
├── bench/                     # 压测和容量工具
├── chaos/                     # 故障注入工具
├── web/                       # Vue 演示页面
├── deploy/
│   ├── compose/               # Docker Compose（仅 Redis + MySQL）
│   └── monitoring/            # Prometheus/Grafana 配置
├── migrations/                # MySQL 版本化迁移
├── scripts/                   # 开发和运维脚本
└── docs/                      # 设计、ADR、任务与运行文档
```

> **不要创建 `src/player/` 与 `src/settlement/`**：这两个服务已由
> [ADR-0003](docs/adr/0003-scope-reduction.md) 列为非目标。

> **放置规则**：只有被两个及以上服务使用的代码才放 `include/common/`；
> 服务内部头文件与实现一起放在 `src/<service>/`，单元测试放
> `tests/unit/<service>/`。判断依据是依赖方向，不是“它是不是头文件”。
> 完整规则见[架构设计](docs/01-architecture.md)第 11 节。

## 目录与环境约定

- **唯一正式开发环境**：WSL2 Ubuntu 26.04 LTS 的
  `~/workspace/realtime-game-backend`。
- 构建、测试和验收命令只在 WSL 中执行；CLion 通过 WSL Toolchain 打开该目录。
- Windows 目录不作为构建和提交来源，避免出现两个工作树各自演进导致分叉。
- 若须使用 Windows 副本临时编辑，必须明确说明，并在完成后同步回 WSL 目录。
- 不要提交构建目录、IDE 配置、密钥、日志和个人本机路径。

## 文档阅读顺序

所有 AI 和开发者必须先阅读：

1. [项目协作规范](CLAUDE.md)
2. [文档索引](docs/README.md)
3. [项目章程](docs/00-charter.md)
4. [架构设计](docs/01-architecture.md)
5. [迭代路线图](docs/02-roadmap.md)
6. [范围裁剪 ADR-0003](docs/adr/0003-scope-reduction.md)（**必读：定义了不做什么**）
7. [当前任务](docs/TASKS.md)

## 开发与验收方式

项目采用“单一执行者 + 多辅助者”模式：

- 项目所有者：确认需求、审阅代码、运行验收并决定是否合并，按任务授予代码
  写入权。
- 执行者：持有当前任务的写入权，负责代码、测试和技术文档。
- 辅助者：不持写入权，负责环境与命令操作、概念解释、报错分析和独立审查。

每个任务遵循“任务单 -> 分支 -> 实现 -> 自测 -> PR -> 用户验收 -> 合并 ->
更新 devlog”的流程。任何 AI 在开始编码前都必须先阅读当前任务和相关 ADR。

## 完成标准

项目最终至少能够现场演示：

- 两个浏览器完成登录、匹配、进入房间和一场最小对战。
- 客户端断线后在宽限期内恢复会话和房间位置。
- 业务进程 `kill -9` 后恢复关键状态，重复写入不产生重复对局结果。
- Redis/MySQL 不可用时返回明确错误（不写假成功），恢复后无需重启。
- 优雅退出：收到 SIGTERM 后停止接收新房间、等待活跃对局结束。
- Grafana 展示连接数、QPS、延迟、错误率、房间数和重连次数。
- 形成可复现压测基线、故障注入结果、架构文档和 Runbook。
- 能解释每个技术选择的替代方案，**包括为什么不做 Kafka、etcd 和多实例**。

具体阶段性验收见 [验收标准](docs/04-quality-and-observability.md) 和
[路线图](docs/02-roadmap.md)。
