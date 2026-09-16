# 当前任务

> 状态：Phase 0，TASK-000 已完成，TASK-001 待收尾（WSL 正式目录对齐），
> TASK-002 实测与 CI 均已通过（待最终确认），TASK-003 进行中

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
- 已完成：正式项目已迁移到 WSL 路径，构建工具和客户端已安装并通过版本检查。
- 已完成：本地 `main` 分支已初始化，`origin` 已连接并可只读访问。
- 已完成：Docker Desktop WSL Integration、Engine 和 Compose 已验证可用。
- 已完成：GitHub SSH 身份认证成功，`origin` 已切换为 SSH 地址。
- 已完成：首次提交与推送，`f685be0`，36 个文件，本地与 `origin/main` 无差异；
  提交内不含 `.idea/`、构建产物和密钥。该提交从 Windows 副本发出，内容可移植。
- 已完成：CLion WSL Toolchain 经项目所有者确认可用（2026-09-15）。
- 待完成（唯一剩余项）：WSL 的 `~/workspace/realtime-game-backend` 实测为**无提交
  的空仓库**，需将其改名封存后从 GitHub 重新 clone，使 WSL 成为唯一正式开发目录；
  完成后 Windows 目录冻结为备份，不再用于构建和提交。
- 注意：不得对空仓库执行 `git pull`（无关联历史会失败）；正式构建验收必须在 WSL
  执行，当前执行环境只能做配置级校验。

## TASK-002：工程骨架和 CI

- 状态：进行中
- 依赖：TASK-000、TASK-001（WSL 目录对齐可并行完成）
- 背景问题：需要稳定、可复现的 C++ 开发基线。
- 本次目标：
  - CMake、CMakePresets、Ninja。
  - Debug、Release、ASan Preset。
  - 代码格式、静态检查和基础测试。
  - GitHub Actions 构建。
- 范围：
  - 根 `CMakeLists.txt`：项目声明、C++20、严格警告、CTest、安装规则不启用。
  - `CMakePresets.json`：`debug`、`release`、`asan` 三个 Preset，统一使用 Ninja。
  - `.clang-format`、`.clang-tidy` 和格式检查脚本。
  - `include/common/` + `src/common/` 放置最小公共代码，并用 GoogleTest 覆盖。
  - `tests/unit/` 接入 GoogleTest（CMake `FetchContent`，暂不引入 vcpkg）。
  - `.github/workflows/ci.yml`：配置、构建、测试。
- 非范围：
  - 不实现 Gateway 业务、Proto、brpc、Redis、MySQL、WebSocket。
  - 不引入 vcpkg manifest、Docker Compose、Kafka、etcd。
  - 不新建服务目录和业务头文件。
- 相关 ADR：ADR-0001（技术栈与平台）。本轮不新增 ADR。
- 涉及目录：仓库根、`include/common/`、`src/common/`、`tests/unit/`、`scripts/`、
  `.github/workflows/`。以上目录结构调整需在 `01-architecture.md` 中体现。
- 接口变化：无 Proto 与 HTTP 接口；仅新增构建入口和 `common` 公共库目标。
- 数据变化：无。
- 失败场景：
  - 首次配置需联网下载 GoogleTest；离线或网络受限会导致配置失败。
  - CMake 4.x 对旧版依赖的 `cmake_minimum_required` 兼容性可能触发配置错误。
  - Sanitizer Preset 与 Ninja 组合需要保证 `-fsanitize` 同时作用于编译和链接。
  - `-Werror` 打开后，任何新增警告都会直接失败；第三方依赖产生的警告需隔离。
  - CI 中的 CMake 与 GCC 版本若低于本地，可能无法满足 C++20 与 CMake 4.x 语法。
- 验收命令（在 WSL 中执行）：
  ```bash
  cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure
  cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
  cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure
  ```
- 测试要求：GoogleTest 单元测试覆盖 `common` 公共代码；Debug 与 ASan 构建的测试
  必须全部通过；无新增编译警告（`-Werror` 生效）。
- 回退方式：本任务只新增文件，不改动现有代码。回退即删除新增文件并恢复
  `CMakePresets.json` 等配置；`git revert` 单个提交即可，不影响 `docs/` 既有内容。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，写入权按任务授予。本轮写入权授予当前
  执行会话（DeepSeek 会话代理），范围为 TASK-002 涉及目录；知会类协作者本轮不写入。
- 验收标准：
  - WSL 中可构建和运行测试。
  - CI 成功。
  - 无编译警告和无关文件。
- 实施结果（2026-09-15）：
  - 已完成并实测通过：三个 Preset 的配置、构建与测试；格式检查；静态检查。
    实测数据见 `docs/devlog.md` 的「TASK-002 实施记录」。
  - 交付提交：`431adf2 build: 搭建 CMake/CTest/CI 工程骨架`（分支
    `feat/task-002-build-skeleton`），已通过 PR #1 合并到 `main`，合并提交
    `eb67a6c`。
  - `CI 成功` 已满足：合并后工作流被注册为 `active`，CI 运行两次均
    `completed / success`（run #1 功能分支、run #2 `main`）。
  - 未决风险：系统缺少 brpc 相关开发头文件（`openssl`、`gflags`、`glog`），
    属 TASK-004 前置条件，安装方式届时决策。
  - 结论：三项验收标准均已满足，等待项目所有者最终确认后关闭本任务。

## TASK-003：Docker 开发依赖

- 状态：进行中
- 依赖：TASK-002（已完成）；Docker Desktop 已实测可用
- 背景问题：本地需要可重复启动的 Redis/MySQL 环境。
- 本次目标：
  - Redis、MySQL Docker Compose。
  - 健康检查、持久化卷、初始化脚本和 `.env.example`。
- 范围：
  - `deploy/compose/docker-compose.yml`：Redis 与 MySQL 两个服务。
  - 具名卷 `redis-data`、`mysql-data`；healthcheck；`restart: unless-stopped`。
  - `deploy/compose/README.md`：启动、健康检查、日志、停止、清理、备份与恢复。
  - `.env.example` 按 `docs/06-operations.md` 补充 Compose 需要的变量并加注释；
    与 compose 文件同目录放置于 `deploy/compose/`。
  - `migrations/001_create_schema_migrations.sql`：最小建表脚本，验证初始化链路。
  - `scripts/verify-deps.sh`：依赖环境的验收入口（一条命令）。
- 非范围：
  - 不启动 Kafka、etcd 和任何 C++ 服务。
  - 不实现业务表结构，不接入服务代码。
  - 不引入 Dockerfile、镜像构建和多节点编排。
- 相关 ADR：ADR-0001。本轮不新增 ADR。
- 涉及目录：`deploy/compose/`、`migrations/`、`scripts/`。
- 接口变化：无。仅新增本地依赖服务的端口与配置约定。
- 数据变化：新增两个具名 Docker 卷，以及 `realtime_game` 库中的
  `schema_migrations` 表。
- 失败场景：
  - 首次启动需要拉取镜像（Redis 约 40 MB、MySQL 约 200 MB），网络受限会失败。
  - 宿主机 6379 或 3306 端口被占用会导致启动失败。
  - 修改 `.env` 中的 MySQL 密码后，既有数据卷仍使用旧密码，会出现认证失败；
    需要 `down -v` 重建或改回原密码。
  - 数据卷被 `down -v` 删除会导致数据丢失，文档必须显式警告。
  - MySQL 初始化脚本只在空数据目录执行一次，重复启动不会重跑。
- 验收命令（在 WSL 中执行）：
  ```bash
  bash scripts/verify-deps.sh
  ```
  该脚本内部覆盖：一条命令启动、等待健康、连通性验证、重启后数据保留、
  停止，并输出每一项的真实结果。
- 测试要求：不适用单元测试（本任务只涉及 Compose 配置与脚本）；以
  `verify-deps.sh` 的连通性与持久化验证作为验收依据。
- 回退方式：`docker compose -f deploy/compose/docker-compose.yml down` 停止容器；
  如需彻底回退，`down -v` 删除具名卷，并 `git revert` 本次提交。只新增文件，
  不改动既有代码，回退不影响 TASK-002 成果。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，本轮写入权授予当前执行会话，范围为
  TASK-003 涉及目录。
- 验收标准：
  - 一条命令启动依赖。
  - 重启后数据卷保留。
  - 连接配置和停止方式有文档。
- 待确认选择（实施前由项目所有者过目）：
  - 镜像版本：`redis:8.0`（对应本机 redis-cli 8.0.5）、`mysql:8.4`
    （对应本机 mysql 客户端 8.4.11），均用 Docker 官方镜像。
  - Redis 持久化：本轮使用默认 RDB 快照。AOF 留到 Phase 2 引入，理由是
    `CLAUDE.md` 规定 Redis 不作为唯一真相，现在开启 AOF 属提前引入能力。
- 实施结果（2026-09-15）：
  - 已完成并实测通过：一条命令启动、健康检查、连通性、初始化脚本生效、
    重启后数据保留、停止后数据卷保留。实测数据见 `docs/devlog.md` 的
    「TASK-003 实施记录」。
  - 验收命令：`bash scripts/verify-deps.sh`（`--keep` 保留容器，`--down-only` 只停止）。
  - 服务端实测版本：MySQL `8.4.11`；迁移脚本执行成功，`schema_migrations` 计数为 1。
  - 未决风险：修改 `.env` 密码不影响已有数据卷；`down -v` 会删数据（文档已警告）；
    Compose 配置未纳入 CI 校验。
  - 结论：三项验收标准均已满足，等待项目所有者审阅与合并。

## TASK-004：brpc Gateway 基线

- 状态：进行中
- 依赖：TASK-002（已完成）、TASK-003（已完成）
- 背景问题：需要验证 brpc/Protobuf 工程集成和服务启动方式。
- 本次目标：
  - 公共 Proto、错误码、Gateway 健康检查和优雅退出。
- 非范围：不实现匹配、房间和 WebSocket 业务。
- 验收标准：
  - Gateway 可启动。
  - 健康检查成功。
  - 测试可通过，错误路径有记录。
- **本轮实施策略（经项目所有者确认的方案 C）：先验证，再全量。**
  即先用 vcpkg 装通 brpc 并在本工程中链接运行，确认工具链可用，
  之后再决定是否把 GTest 等其他依赖一并迁入 vcpkg、以及是否进入完整 Gateway 实现。
  本轮**不**实现完整 Gateway 业务，只交付“brpc 可用”的可验证证据。
- 范围：
  - `vcpkg` 经典模式安装 `brpc`（含 protobuf、gflags、glog、openssl、thrift 等依赖）。
  - 工程接入 vcpkg toolchain，新增独立的 `brpc` 预设，不影响既有
    `debug`/`release`/`asan` 三个预设与 CI。
  - 最小可运行的 brpc 服务与健康检查，作为链接与启动证据。
  - `scripts/verify-brpc.sh`：本任务的验收入口。
- 非范围：
  - 不定义 `api/proto/` 下的正式接口契约（留到业务任务）。
  - 不把 GTest 迁入 vcpkg manifest（待 brpc 验证通过后单独决策）。
  - 不引入 etcd、Kafka、多节点。
- 相关 ADR：ADR-0001。若最终决定全量迁入 vcpkg manifest，需新增 ADR。
- 涉及目录：`cmake/`（如有）、`src/gateway/`、`scripts/`、根 `CMakeLists.txt`、
  `CMakePresets.json`。
- 接口变化：本轮只新增健康检查接口，不改动既有对外契约。
- 数据变化：无。
- 失败场景（**以下均已实测确认，非推测**）：
  - **GitHub release 附件被限速**：实测 34 KB/s，62 MB 的 CMake 需约 31 分钟，
    且 vcpkg 重试不从断点继续，会反复失败。已通过 `ghfast.top` 加速通道解决
    （实测 819–931 KB/s）。
  - **vcpkg 要求自带 CMake 4.4.3，高于本机 4.2.3**：已预先将 CMake 放入
    `$VCPKG_ROOT/downloads/`，vcpkg 会直接复用而不再下载。
  - **本机 vcpkg 不是 git 克隆**（无 `.git`），因此**无法使用 manifest 的
    `builtin-baseline` 固定版本**。本轮采用经典模式安装；若后续要固定版本，
    需要把 vcpkg 重新克隆为 git 仓库。
  - **内存限制**：本机 11 GiB，brpc/protobuf/openssl 并行编译有 OOM 风险，
    已限制 `VCPKG_MAX_CONCURRENCY=4`。
  - **无法自行安装系统包**：`sudo` 需要密码，因此不能退回 apt 安装方案作为兜底；
    若 vcpkg 路线失败，需要项目所有者手动执行 apt 安装。
  - **git clone 无法走加速通道**：实测超时，因此任何需要 git clone 的 port
    都会失败；brpc 及其依赖均为固定 tag 的 archive 下载，不受影响。
- 验收命令（在 WSL 中执行）：
  ```bash
  bash scripts/verify-brpc.sh
  ```
- 测试要求：最小 brpc 服务需能启动并响应健康检查；停止时能优雅退出。
- 回退方式：删除新增的 vcpkg 预设与源文件，恢复 `CMakeLists.txt`；
  vcpkg 安装的依赖可保留（不影响既有构建），必要时
  `rm -rf $VCPKG_ROOT/installed $VCPKG_ROOT/buildtrees` 清理。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，本轮写入权授予当前执行会话，范围为
  TASK-004 涉及目录。
- 实施结果（2026-09-16，方案 C 的验证目标已达成）：
  - vcpkg 经典模式安装 brpc 成功：共 73 个包，brpc 1.16.0 本体编译耗时 17 分钟，
    总计 19 分钟。依赖 protobuf 6.33.4、thrift 0.24.0、openssl 3.6.4、
    abseil、gflags、glog、leveldb、zlib、libevent、boost 1.92.0 全部就绪。
  - **工具链可用性已验证**：CMake 4.2.3 + GCC 15.2.0 能编译并链接 brpc，
    这是本任务的核心目的。
  - 新增 `brpc-debug` 预设与 `src/gateway/smoke_main.cpp` 冒烟程序。
  - 验收命令：`bash scripts/verify-brpc.sh`，退出码 0。
  - 实测结果：配置成功、构建成功、服务启动、`GET /health` 返回 200 且响应体为
    `OK`、未知路径返回 404（证明路由生效）、brpc 内置 `/status` 返回 200、
    收到 SIGTERM 后退出码 0 且输出「已优雅退出」。
  - 回归确认：既有 `debug`/`release`/`asan` 三预设仍各 4/4 通过，格式检查通过，
    接入 vcpkg 未影响原有构建与 CI。
  - 未完成（本轮非范围）：未定义 `api/proto/` 正式契约；未实现完整 Gateway；
    GTest 未迁入 vcpkg；Compose 与 brpc 预设未纳入 CI。
  - 结论：方案 C 的验证目标（brpc 是否可用）已达成，等待项目所有者确认后再决定
    是否进入完整 Gateway 实现。

## TASK-005：登录垂直切片

- 状态：进行中
- 依赖：TASK-003（已完成）、TASK-004（brpc 工具链验证部分已完成）
- 背景问题：需要验证从客户端请求到持久化会话的最小链路。
- 本次目标：
  - 登录接口。
  - Session 创建和 Redis 存储。
  - 返回玩家信息。
- 非范围：不做注册、第三方登录、匹配。
- **范围调整（经项目所有者确认，2026-09-16）**：
  TASK-004 只完成了「brpc 工具链可用性验证」，其原始范围中的公共 Proto、
  错误码、健康检查与优雅退出尚未实现。经确认，这些剩余项**并入本任务**，
  避免两轮重复搭建 Gateway 骨架。因此本任务的实际范围是
  「Gateway 最小可运行服务 + 登录切片」。
- 范围：
  - `api/proto/gateway.proto`：登录、查询当前玩家、登出三个接口与统一错误体。
  - `include/common/token.hpp` + `src/common/token.cpp`：Token 生成与格式校验。
  - `src/gateway/`：服务实现、测试账号目录、Redis 会话存储、服务入口、错误码映射。
  - HTTP 状态码映射：错误语义必须体现在传输层，而不只是 JSON 体。
  - `scripts/verify-login.sh`：端到端验收入口。
- 非范围：
  - 不实现注册系统，不把账号密码写入 MySQL。
  - 不实现 Token 刷新（只做短期会话）。
  - 不实现匹配、房间、WebSocket。
- 相关 ADR：本轮不新增 ADR，但落地了 `docs/07-open-decisions.md` 的 D-002 决策。
- 涉及目录：`api/proto/`、`include/common/`、`src/common/`、`src/gateway/`、
  `scripts/`、`tests/`、根 `CMakeLists.txt`、`CMakePresets.json`。
- 接口变化：新增 3 个 HTTP 接口，路径依 `docs/05-api-and-data.md` 第 2 节
  （`POST /api/v1/login`、`GET /api/v1/players/me`、`POST /api/v1/logout`）。
- 数据变化：Redis 新增两类 Key，均带 `<env>:gateway:` 前缀并设置 TTL：
  - `<env>:gateway:session:<token>`：会话 Hash
  - `<env>:gateway:idem:login:<request_id>`：登录幂等映射
- 失败场景：
  - Redis 不可用：返回 503 UNAVAILABLE，不伪装成功；服务不退出，Redis 恢复后
    无需重启即可继续服务。
  - 无效输入（空账号、超长字段、非法 client_type）：返回 400，不访问依赖。
  - 凭据错误或账号禁用：返回 401；账号不存在与密码错误返回同一 reason，
    避免泄露账号是否存在。
  - 重复 request_id：返回同一 Token，不重复创建会话。
  - 并发同 request_id：`SET NX` 保证只有一个胜出，失败方清理自己写入的会话
    并返回胜出者 Token。
- 验收命令（在 WSL 中执行）：
  ```bash
  bash scripts/verify-login.sh
  ```
- 测试要求：单元测试用内存假存储覆盖全部错误路径；端到端测试覆盖正常登录、
  幂等、无效输入与 Redis 故障恢复。
- 回退方式：删除新增文件并恢复 `CMakeLists.txt`、`CMakePresets.json`；
  Redis 中的 Key 有 TTL，无需手工清理。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，本轮写入权授予当前执行会话。
- 验收标准：
  - 正常登录成功。
  - 无效输入、重复登录和 Redis 不可用路径有测试。
  - 形成 `v0.1-bootstrap` 里程碑。

## TASK-006（Backlog）：后续待办

- 将 brpc 预设与 Compose 配置纳入 CI 覆盖。
- 实现 `docs/05-api-and-data.md` 中其余接口（匹配、房间、结果）。
- Token 刷新与长期会话策略（Phase 2）。

## 任务完成定义

- 代码、测试、文档和 devlog 同步。
- 用户审阅关键 Diff。
- 所有验收命令实际运行。
- 没有未解释的警告、遗留密钥和构建产物。
- 不属于当前任务的后续问题记录到 Backlog，不直接扩大当前提交。
