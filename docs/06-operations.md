# 环境与运维

> 状态：已确认（2026-09-14）

## 1. 开发环境

| 项目 | 选择 |
|---|---|
| 主机 | Windows 11 |
| Linux 环境 | WSL2 Ubuntu 26.04 LTS |
| C++ IDE | CLion Remote Toolchain |
| 辅助编辑器 | VS Code |
| 终端 | Windows Terminal |
| C++ 编译器 | GCC 15.2 |
| 构建 | CMake 4.2.3 + Ninja 1.13.2 + GCC 15.2 |
| 版本控制 | Git 2.53.0 |
| Protobuf | 3.21.12 |
| 静态检查 | clang-tidy 21.1.8 |
| 配置检查 | pkg-config 2.5.1 |
| Redis 客户端 | redis-cli 8.0.5 |
| MySQL 客户端 | mysql 8.4.11 |
| 依赖管理 | vcpkg 2026-07-27，仓库提交 `a1cae005c39be7b18ba319fced856b68d7276271` |
| 容器 | Docker Desktop 4.74.0、Engine 29.4.3、Compose v5.1.3 |

项目正式源码目录固定为：

```text
~/workspace/realtime-game-backend
```

不要在 `/mnt/d` 中长期构建大型 C++ 项目。`D:\CLion\realtime-game-backend`
仅用于初始文档整理和迁移前备份，不作为正式构建或 Git 仓库路径，避免 CLion、
Docker 和文件监听出现性能或权限差异。

以上工具版本来自 2026-09-14 的 WSL2 实测环境。vcpkg 安装于
`/home/archer/tools/vcpkg`，并通过 `/usr/local/bin/vcpkg` 提供命令入口；其
工具版本为 `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`。后续必须由
CMakePresets、vcpkg manifest 和 CI 配置固定工具链及仓库提交。

Docker Desktop 的 Ubuntu WSL Integration 已于 2026-09-14 验证生效。
`docker version` 能同时返回 Client 和 Server，Docker Compose 插件可正常调用。

## 2. 环境分层

### 本地开发

- C++ 服务直接在 WSL 中运行和调试。
- Redis、MySQL 等依赖通过 Docker Compose 启动。
- 前端通过 Vite 开发服务器运行。

### 集成环境

- 所有 C++ 服务、Web、Redis、MySQL 通过 Docker Compose 启动。
- 使用固定网络、卷、端口和配置。
- 用于端到端测试和现场演示。

### 故障环境

- 在集成环境基础上加入故障注入。
- 模拟服务停止、网络延迟、Redis/MySQL/Kafka 异常。
- 仅用于稳定性阶段，不默认长期开启。

## 3. 配置规则

- 配置通过环境变量注入。
- 仓库保存 `.env.example`，不保存真实 `.env`。
- 与 Docker Compose 相关的 `.env` 和 `.env.example` 与 compose 文件同目录放置
  （`deploy/compose/`），因为 compose 的约定是从 compose 文件所在目录自动读取
  `.env`。放在其他位置会导致每次执行都需要额外传 `--env-file`，容易漏写。
- 禁止把 Token、数据库密码和私钥提交到 Git。
- 配置项必须有默认值、类型、单位和取值范围说明。
- 服务启动时校验关键配置，缺失时快速失败并输出明确错误。

建议配置分类：

```text
APP_ENV
LOG_LEVEL
GATEWAY_HTTP_PORT
GATEWAY_WS_PORT
REDIS_HOST
REDIS_PORT
MYSQL_HOST
MYSQL_PORT
MYSQL_DATABASE
MYSQL_USER
MYSQL_PASSWORD
KAFKA_BROKERS
ETCD_ENDPOINTS
```

## 4. 推荐的本地启动方式

最终目标：

```bash
docker compose -f deploy/compose/docker-compose.yml up -d
```

在实现具体服务之前，不提供未经验证的启动命令。每条启动说明必须包含：

- 前置依赖。
- 启动命令。
- 健康检查方式。
- 日志查看方式。
- 停止和清理方式。
- 数据卷备份和恢复方式。

## 5. Runbook

以下场景在实现对应能力后补全具体命令：

### Gateway 无法启动

1. 检查端口占用。
2. 检查配置文件和环境变量。
3. 检查 Redis/内部服务连通性。
4. 查看结构化启动日志。
5. 使用健康检查确认恢复。

### Redis 不可用

1. 确认影响范围：会话、缓存、匹配队列或排行榜。
2. 判断请求应失败、降级还是重建缓存。
3. 禁止写假成功。
4. 记录从故障开始到恢复的时间。
5. 验证关键会话和房间状态。

### MySQL 不可用

1. 确认写请求是否立刻失败。
2. 暂停结算消费或进入重试队列。
3. 恢复后验证幂等和重复消息。
4. 检查是否需要补偿。

### 房间节点故障

1. 确认节点是否失去租约。
2. 停止向故障节点分配房间。
3. 根据快照和事件制定恢复或结束策略。
4. 通知受影响玩家。
5. 记录丢失边界和恢复耗时。

### Kafka 积压

1. 查看 consumer lag。
2. 判断是生产突增还是消费阻塞。
3. 检查重复消息、数据库延迟和重试风暴。
4. 必要时限流生产或扩容消费者。
5. 不让异步积压阻塞实时对局主路径。

## 6. 备份和恢复

- MySQL 定期导出并在测试环境验证恢复。
- Redis 数据视为可重建缓存时，必须验证重建流程。
- Kafka 的保留期、主题分区和重放策略必须记录。
- Docker 数据卷不能只创建不验证恢复。
- 每次恢复演练记录恢复点、恢复时间和数据差异。

## 7. 发布和回滚

发布前：

- 固定代码版本和依赖版本。
- 检查数据库迁移兼容性。
- 运行构建、测试、ASan/TSan。
- 确认监控和日志已开启。
- 确认回滚版本和数据兼容。

发布后：

- 检查错误率、延迟和资源使用。
- 检查关键业务链路。
- 观察 Kafka 消费和数据库写入。
- 发现异常时按预定义条件回滚或停止流量。
