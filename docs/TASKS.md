# 当前任务

> 状态：Phase 0，TASK-000 已完成，TASK-001 进行中

## 当前里程碑

**M0：项目基线和环境建设**

目标：让项目具备统一方向、AI 可读文档、可构建骨架和第一个可验证的登录垂直切片。

## TASK-000：确认项目文档

- 状态：已完成
- 背景问题：项目方向和 AI 协作方式尚未由项目所有者正式确认。
- 本次目标：审阅 `README.md`、`CLAUDE.md` 和 `docs/` 文档。
- 范围：文档内容、技术栈、服务边界、阶段目标。
- 非范围：不写业务代码。
- 验收标准：项目所有者明确确认或提出修改。
- 后续动作：确认后统一把文档状态从“草案”改为“已确认”。
- 验收结果：项目所有者于 2026-09-14 确认文档和 ADR-0001。

## TASK-001：创建 GitHub 仓库和本地环境

- 状态：进行中
- 背景问题：需要真实 Git 工作流和 WSL/CLion 开发环境。
- 本次目标：
  - 创建或连接 GitHub 仓库 `realtime-game-backend`。
  - 确认仓库名、可见性、License 和初始文件。
  - 在 WSL 的 `~/workspace/realtime-game-backend` 中建立正式项目目录。
  - 配置 CLion WSL Toolchain。
  - 验证 `git remote -v`、分支和 SSH。
- 非范围：不写业务代码。
- 命令辅助：豆包。
- 验收标准：
  - 本地与远程仓库一致。
  - `git status` 清晰。
  - CLion 能打开并识别项目。
  - 没有提交 IDE、构建和密钥文件。
  - 首次提交推送和 CLion WSL Toolchain 最终验证仍待完成。
  - 已完成：正式项目已迁移到 WSL 路径，构建工具和客户端已安装并通过版本检查。
  - 已完成：本地 `main` 分支已初始化，`origin` 已连接并可只读访问。
  - 已完成：Docker Desktop WSL Integration、Engine 和 Compose 已验证可用。
  - 已完成：GitHub SSH 身份认证成功，`origin` 已切换为 SSH 地址。

## TASK-002：工程骨架和 CI

- 状态：待开始
- 依赖：TASK-000、TASK-001
- 背景问题：需要稳定、可复现的 C++ 开发基线。
- 本次目标：
  - CMake、CMakePresets、Ninja。
  - Debug、Release、ASan Preset。
  - 代码格式、静态检查和基础测试。
  - GitHub Actions 构建。
- 非范围：不实现 Gateway 业务。
- 验收标准：
  - WSL 中可构建和运行测试。
  - CI 成功。
  - 无编译警告和无关文件。

## TASK-003：Docker 开发依赖

- 状态：待开始
- 依赖：TASK-002
- 背景问题：本地需要可重复启动的 Redis/MySQL 环境。
- 本次目标：
  - Redis、MySQL Docker Compose。
  - 健康检查、持久化卷、初始化脚本和 `.env.example`。
- 非范围：不启动 Kafka、etcd 和全部服务。
- 验收标准：
  - 一条命令启动依赖。
  - 重启后数据卷保留。
  - 连接配置和停止方式有文档。

## TASK-004：brpc Gateway 基线

- 状态：待开始
- 依赖：TASK-002
- 背景问题：需要验证 brpc/Protobuf 工程集成和服务启动方式。
- 本次目标：
  - 公共 Proto、错误码、Gateway 健康检查和优雅退出。
- 非范围：不实现匹配、房间和 WebSocket 业务。
- 验收标准：
  - Gateway 可启动。
  - 健康检查成功。
  - 测试可通过，错误路径有记录。

## TASK-005：登录垂直切片

- 状态：待开始
- 依赖：TASK-003、TASK-004
- 背景问题：需要验证从客户端请求到持久化会话的最小链路。
- 本次目标：
  - 登录接口。
  - Session 创建和 Redis 存储。
  - 返回玩家信息。
- 非范围：不做注册、第三方登录、匹配。
- 验收标准：
  - 正常登录成功。
  - 无效输入、重复登录和 Redis 不可用路径有测试。
  - 形成 `v0.1-bootstrap` 里程碑。

## 任务完成定义

- 代码、测试、文档和 devlog 同步。
- 用户审阅关键 Diff。
- 所有验收命令实际运行。
- 没有未解释的警告、遗留密钥和构建产物。
- 不属于当前任务的后续问题记录到 Backlog，不直接扩大当前提交。
