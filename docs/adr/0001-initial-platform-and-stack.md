# ADR-0001：初始开发平台与技术栈

- 状态：已接受
- 日期：2026-09-14
- 决策者：项目所有者
- 关联任务：TASK-000、TASK-001、TASK-002

## 背景

项目需要一个可运行、可调试、可部署的 C++ Linux 开发环境，同时要覆盖国内
C++ 后端和游戏服务端常用技术栈。项目优先验证主流服务端工程能力，不重复实现
通用通信框架和基础中间件。

## 决策

采用以下初始技术栈：

- WSL2 Ubuntu 26.04 LTS + CLion Remote Toolchain。
- C++20 + GCC 15.2 + CMake 4.2.3 + Ninja 1.13.2 + vcpkg 2026-07-27。
- Git 2.53.0。
- clang-tidy 21.1.8 + pkg-config 2.5.1。
- brpc + Protobuf 3.21.12 作为服务间通信。
- HTTP + WebSocket 作为浏览器通信。
- Redis + MySQL 作为第一版数据组件。
- Kafka 用于后续异步和回放阶段。
- etcd 用于后续多实例和服务发现阶段。
- Docker Compose 作为本地集成环境。
- GitHub Actions 作为 CI。
- Prometheus + Grafana + OpenTelemetry 作为可观测性方案。
- Vue 3 + TypeScript + Vite + Canvas 作为演示客户端。

Ubuntu 和已安装工具版本以 2026-09-14 的 WSL2 实测环境为准。vcpkg 安装于
`/home/archer/tools/vcpkg`，当前仓库提交为
`a1cae005c39be7b18ba319fced856b68d7276271`，工具版本为
`2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`。后续必须通过
CMakePresets、vcpkg manifest 和 CI 配置保证工具链可复现。

## 备选方案

### 自研专用 RPC

优点：

- 可以针对实时对战场景做定制。
- 能完全控制协议和处理流程。

缺点：

- 维护成本高，容易重复实现成熟框架能力。
- 无法集中验证主流服务端技术栈和工程交付能力。

结论：不作为本项目服务框架，服务间通信采用 brpc + Protobuf。

### gRPC

优点：

- 跨语言和云原生生态更通用。
- 文档和社区更大。

缺点：

- 浏览器不能直接访问标准 C++ gRPC。
- 对当前游戏服务端和国内 C++ 岗位的贴近度略低。

结论：暂不采用。若未来需要跨语言服务，再新增 ADR 评估适配层。

### Kubernetes

优点：

- 更接近生产部署。

缺点：

- 当前阶段没有真实调度和多节点需求。
- 会显著增加调试和演示成本。

结论：暂不采用。先使用 Docker Compose，满足明确触发条件后再评估 k3s。

## 后果

### 正面

- 技术栈贴近 C++ 后端和游戏服务端岗位。
- 底层、工程和数据链路形成互补。
- 本地开发和集成部署路径清晰。

### 负面

- brpc 和 C++ 依赖的构建复杂度较高。
- 需要同时学习多个中间件，必须严格按阶段引入。
- WSL 与 Windows 路径混用可能带来性能或权限问题。

## 验证方式

- 在 WSL 中成功构建最小 CMake 工程。
- brpc Gateway 健康检查通过。
- Docker Compose 能稳定启动第一版依赖。
- Phase 0 和 Phase 1 的退出标准全部满足。

## 替代关系

无。
