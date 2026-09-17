# Realtime Game Backend

> 面向小型实时对战产品的分布式游戏服务端参考实现
>
> C++20 / brpc / Protobuf / WebSocket / Redis / MySQL / Kafka / etcd / Vue 3

## 项目定位

本项目是一套面向小型实时对战产品的分布式游戏服务端参考实现，重点验证主流后端
技术栈、分布式服务设计、数据一致性、故障恢复、可观测性和工程交付能力。

它不是通用 RPC 框架，也不是完整商业游戏，而是一套可运行、可部署、可压测、
可故障注入和可现场演示的实时对战服务端。

## 解决的问题

实时对战业务通常会遇到以下问题：

- 单机服务无法承载持续增长的连接和房间。
- 玩家断线后，会话、房间和战斗状态容易丢失。
- 匹配、房间、结算、排行榜之间的数据边界不清晰。
- 请求重试可能造成重复结算或重复入队。
- 服务节点故障后缺少发现、恢复和迁移能力。
- 只有日志，没有指标、链路和可复现的性能基线。

本项目通过明确服务边界、持久化关键状态、引入幂等和可观测性来逐步解决这些问题。
只有真实问题出现后，才引入对应的分布式组件，避免为了展示技术而堆叠组件。

## 核心用户流程

```text
登录
  -> 进入大厅
  -> 发起匹配
  -> 匹配成功并创建房间
  -> WebSocket 进入对战
  -> 发生断线并重连
  -> 对局结束
  -> 结算、排行榜和回放事件
```

## 目标架构

```text
浏览器演示页 / 机器人客户端
             |
       HTTP + WebSocket
             |
       Gateway Service
             |
      brpc + Protobuf
             |
   +---------+----------+-----------+
   |                    |           |
Match Service     Room/Battle    Player/State
                   Service         Service
   |                    |           |
   +---------+----------+-----------+
             |
    Settlement Worker

Redis：会话、缓存、排行榜
MySQL：玩家、房间、战绩
Kafka：领域事件、异步结算、回放
etcd：服务注册、发现、租约
```

详细边界见 [架构设计](docs/01-architecture.md)，迭代依据见
[迭代路线图](docs/02-roadmap.md)。

## 技术基线

| 领域 | 选择 | 用途 |
|---|---|---|
| 开发环境 | WSL2 Ubuntu 26.04 LTS + CLion | Linux 开发、调试和 WSL Toolchain |
| 语言与构建 | C++20、GCC 15.2、CMake 4.2.3、Ninja 1.13.2、vcpkg | 主流 C++ 工程构建 |
| 服务通信 | brpc + Protobuf | 内部服务调用和协议契约 |
| 客户端通信 | HTTP + WebSocket | 浏览器登录、状态推送和实时消息 |
| 数据存储 | Redis + MySQL | 会话、缓存、持久化和战绩 |
| 事件系统 | Kafka | 领域事件、异步结算和回放 |
| 协调服务 | etcd | 注册、发现、租约和节点信息 |
| 前端演示 | Vue 3 + TypeScript + Vite + Canvas | 可视化和端到端演示 |
| 可观测性 | Prometheus + Grafana + OpenTelemetry | 指标、日志、链路和面板 |
| 工程质量 | GoogleTest、ASan、TSan、UBSan | 测试、内存和并发检查 |
| 部署 | Docker Compose、GitHub Actions | 本地集成环境和持续集成 |

技术选型的决策记录见
[ADR-0001](docs/adr/0001-initial-platform-and-stack.md)。

## 明确的非目标

- 不做通用 RPC 框架，不重复实现基础通信能力。
- 不追求商业级游戏功能，不做复杂渲染、美术、账号平台和支付系统。
- 不一开始就拆成大量微服务，不默认采用 Kubernetes。
- 不以代码量或版本号作为迭代成果。
- 不在没有容量证据时做性能优化，不编造压测数字。
- 不让多个 AI 同时修改同一分支。

## 当前状态

当前处于 `Phase 0：项目基线和环境建设`。项目方向、服务边界、任务流程和验收标准
已于 2026-09-14 确认，代码尚未实现。仓库已连接
`git@github.com:Archer11-q/realtime-game-backend.git`，首次提交为
`f685be0 chore: 初始化仓库基础框架`。

TASK-001 尚有一个收尾项：WSL 正式目录需要与 GitHub 对齐后，作为唯一正式开发
环境。当前任务见 [TASKS.md](docs/TASKS.md)，开发过程见
[devlog.md](docs/devlog.md)。

## 仓库结构

```text
realtime-game-backend/
├── api/proto/                 # Protobuf 接口契约
├── src/
│   ├── gateway/               # HTTP/WebSocket 网关（含本服务内部头文件）
│   ├── match/                 # 匹配服务
│   ├── room/                  # 房间和战斗服务
│   ├── player/                # 玩家和状态服务
│   └── settlement/            # 异步结算 Worker
├── include/common/            # 跨服务公共基础设施
├── tests/
│   ├── unit/                  # 单元测试，按服务分子目录（unit/<service>/）
│   ├── integration/
│   └── e2e/
├── bench/                     # 压测和容量工具
├── chaos/                     # 故障注入工具
├── web/                       # Vue 演示页面
├── deploy/
│   ├── compose/               # Docker Compose
│   └── monitoring/            # Prometheus/Grafana 配置
├── migrations/                # MySQL 版本化迁移
├── scripts/                   # 开发和运维脚本
└── docs/                      # 设计、ADR、任务与运行文档
```

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
6. [当前任务](docs/TASKS.md)

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
- 业务进程重启后恢复关键状态，重复结算不会产生重复战绩。
- 服务节点故障时完成发现、降级或迁移。
- Grafana 展示连接数、QPS、延迟、错误率、房间数和消息积压。
- 形成可复现压测基线、故障注入结果、架构文档和 Runbook。

具体阶段性验收见 [验收标准](docs/04-quality-and-observability.md) 和
[路线图](docs/02-roadmap.md)。
