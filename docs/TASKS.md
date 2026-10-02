# 当前任务

> 状态：Phase 1 进行中。TASK-000 至 TASK-007 已完成并合并，TASK-012 已完成。
> **TASK-008（Room/Battle Service）已实现，待项目所有者审阅与验收。**
> 阶段推进依据见 docs/02-roadmap.md 与 docs/devlog.md。
>
> **范围裁剪自 2026-10-02 起生效**：Kafka、etcd、多实例、Kubernetes、独立
> Player/State 服务、独立 Settlement 服务、排行榜、匹配分差放宽**均为非目标，
> 不实现**。完整理由见 [ADR-0003](adr/0003-scope-reduction.md)。
> 本文档中 TASK-002/003/004/006/007 的"非范围"条目是**当时的历史记录**，
> 其含义已由 ADR-0003 强化为"永久不做"。

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
  - 不实现 Gateway 业务、Proto、brpc、Redis、MySQL、服务端推送。
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

- 状态：已完成（2026-09-17）
- 依赖：TASK-002（已完成）、TASK-003（已完成）
- 背景问题：需要验证 brpc/Protobuf 工程集成和服务启动方式。
- 本次目标：
  - 公共 Proto、错误码、Gateway 健康检查和优雅退出。
- 非范围：不实现匹配、房间和服务端推送业务。
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

- 状态：已完成（2026-09-17，已通过 PR #2 合并到 main）
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
  - 不实现匹配、房间、服务端推送。
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
- 验收结果（2026-09-17）：
  - 单元测试 29 项全部通过；端到端 `scripts/verify-login.sh` 连续两次 31/31 通过。
  - CI run #7（功能分支）与 run #8（main）均为 `success`。
  - 项目所有者实际执行验收脚本，发现 3 项失败（HTTP 头中的 Token 未被映射进
    protobuf 字段），已修复并复验通过，详见 `docs/devlog.md`。
  - 结论：**完成**，`v0.1-bootstrap` 里程碑达成。已通过 PR #2 合并到 `main`。

## Phase 1 任务拆分（建议，待项目所有者确认）

Phase 1 已在讨论中确认为「拆成 6 个任务」，但此前只存在于对话中、未落到文档，
此处补记。**下面 2~6 项的边界尚未经项目所有者逐条确认**，如与你的预期不符请直接改。

| 序号 | 任务 | 交付物 | 依赖 |
|---|---|---|---|
| 1 | TASK-006 | 数据模型与 MySQL 迁移 | TASK-003 |
| 2 | TASK-007 | Match Service：匹配队列与配对 | TASK-006 |
| 3 | TASK-008 | Room/Battle Service：房间生命周期与权威状态 | TASK-007 |
| 4 | TASK-009 | Gateway SSE 推送路由与房间消息 | TASK-008 |
| 5 | TASK-010 | Vue 演示页面：登录、大厅、对战、结算 | TASK-009 |
| 6 | TASK-011 | 集成验收：一条命令启动并双客户端完成对局 | TASK-010 |
| — | TASK-012 | 范围裁剪（已先于上面第 3~6 项完成） | TASK-007 |

**范围裁剪对本表的实际影响**：第 3 项 Room/Battle 的职责新增「对局结束时同步幂等
写入 `match_results`」；原定的 Player/State 与 Settlement 服务、排行榜和异步结算
**不在计划内**，见 [ADR-0003](adr/0003-scope-reduction.md)。

拆分原则：每个任务都要有**可独立运行的验收命令**，且不引入下一个任务的组件。
这也是 TASK-007 中「房间只分配 ID、不产生房间状态」的原因——房间真实状态属于 TASK-008。

## TASK-006：数据模型与 MySQL 迁移

- 状态：已完成（2026-09-22）
- 依赖：TASK-003（Redis/MySQL 容器，已完成）
- 背景问题：登录此前使用代码内的明文测试身份，`players` 表不存在。Phase 1 需要
  一个可恢复的数据基线，且「数据模型是否合理」必须靠真实读写验证，而不是只建空表。
- 本次目标：
  - 建立 `players` 与 `match_results` 两张表。
  - 让 Gateway 的玩家档案从数据库读取；密码仍留在代码中作为测试数据。
- 范围：
  - `migrations/002_create_players.sql`、`003_create_match_results.sql`、
    `004_seed_test_players.sql`（后者含测试数据，仅开发环境）。
  - 新增 `libmariadb` vcpkg 依赖并接入 CMake。
  - `src/gateway/`：把 `PlayerDirectory` 拆为「接口 + 内存实现 + 数据库实现」，
    结构对齐现有 `SessionStore`，便于单元测试注入假实现。
  - 测试密码改为 SHA-256 摘要比较，不再留明文常量（复用已有 openssl 依赖）。
  - 新增 ADR-0002：记录「Gateway 暂时直接读 `players` 表」及退出条件。
  - `docs/05-api-and-data.md` 补充两表结构与访问约定。
  - `docs/07-open-decisions.md`：D-002 与 D-003 移入「已确认」。
  - `scripts/verify-login.sh`：新增 `--schema-only`；增加 MySQL 故障与迁移验证。
- 非范围：
  - 不引入真实密码体系（不做注册、不做 bcrypt/Argon2）。
  - 不建 `player_stats`、`room_records`、`processed_events`。
  - 不实现 Match / Room / 服务端推送 / 前端。
  - 不实现 Player/State 服务（ADR-0002 记录其为正式归属）。
- 相关 ADR：ADR-0002。
- 涉及目录：`migrations/`、`src/gateway/`、`tests/unit/gateway/`、`scripts/`、`docs/`。
- 接口变化：对外 HTTP 接口不变；内部新增 `PlayerReader` 接口。
- 数据变化：
  - `players`：主键 `player_id`，`uk_players_account(account)` 唯一约束，
    **无 password 列**。
  - `match_results`：主键 `match_id`（幂等业务键），`winner_id` 可空（平局）。
- 失败场景：
  - MySQL 不可用：返回 503 `player_store_unavailable`，不伪装成功；恢复后无需重启。
  - 账号禁用：401 `account_disabled`。
  - 账号不存在或档案缺失：401 `invalid_credential`，不泄露账号是否存在。
  - 迁移脚本重复执行：幂等，不产生重复行。
- 验收命令（在 WSL 中执行）：
  ```bash
  bash scripts/verify-login.sh                # 完整：迁移 + 建表 + 端到端
  bash scripts/verify-login.sh --schema-only  # 只验证迁移与表结构
  ```
- 测试要求：单元测试用假 `PlayerReader` 覆盖读取失败、禁用、不存在等路径；
  端到端使用真实 MySQL 验证读取链路与故障自愈。
- 回退方式：`git revert` 单次提交；数据库层用 `down -v` 重建（仅开发环境）。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，本轮写入权授予当前执行会话。
- 验收标准：
  - 迁移脚本可重复执行且幂等。
  - `players` 表含 3 行种子数据，`match_results` 表存在。
  - 登录链路改为从数据库读取档案后，端到端验收全项通过。
  - MySQL 不可用返回 503，且恢复后无需重启即可登录。
- 提交边界：允许改动 `migrations/`、`src/gateway/`、`tests/unit/gateway/`、
  `scripts/verify-login.sh`、`docs/`；禁止改动 `src/match/`、`src/room/`、
  `web/`、`deploy/compose/` 的服务定义。
- 验收结果（2026-09-22）：
  - 项目所有者实际执行 `ctest`（46/46）与 `bash scripts/verify-login.sh`（48/48），
    均通过；`cmake --build --preset brpc-debug` 退出码 0。
  - CI 对功能分支与 `main` 均为 `success`。
  - 已通过 PR #3 合并到 `main`，合并提交 `0e58a2f`。
  - 结论：**完成**。
  - 遗留：`.env.example` 的端口默认值维持约定值 8080，端口冲突由
    `scripts/verify-login.sh` 预检并自动挑空闲端口解决（不改程序默认值）。
    `src/gateway/test_credentials.{hpp,cpp}` 命名易被误解为测试文件，改名事项
    已记入 Backlog。

## TASK-007：Match Service 匹配队列与配对

- 状态：进行中（2026-09-22 起，任务单已由项目所有者确认）
- 依赖：TASK-006（数据模型与 MySQL 迁移，已完成并合并，`0e58a2f`）
- 背景问题：Phase 1 的最小闭环要求「两个客户端能匹配进同一房间」。当前只有 Gateway
  的登录切片：没有匹配队列、没有配对逻辑，而且**至今没有任何一次服务间 brpc 调用**
  ——此前的 brpc 只用来把 Gateway 自己暴露成 HTTP 服务。因此本任务同时是「第一个
  服务间调用」的落地，必须先把「Gateway -> Match」的契约和失败语义建起来。
- 本次目标：
  - 建立 `MatchService` 的 Protobuf 契约与独立 brpc 服务进程。
  - 实现入队、取消、查询当前匹配状态、超时淘汰。
  - 实现 Phase 1 的最小配对规则：FIFO、两人一局、不比分数。
  - 配对成功后产生 `match_id`，并通过房间分配接口取得 `room_id`。
  - Gateway 新增三个 HTTP 接口，把请求转发给 Match，不自己保存队列状态。
- 范围：
  - `api/proto/match.proto`：`MatchService` 契约。
  - `src/match/`：服务实现、匹配队列、配对器、房间分配接口、进程入口。
  - `src/match/CMakeLists.txt` + 顶层 `CMakeLists.txt` 接入 `rgbt_match` 与
    `rgbt_match_lib`。
  - `src/gateway/`：三个 HTTP 接口；brpc channel 客户端与不可用处理。
  - `tests/unit/match/`：队列、配对、取消、超时的单元测试。
  - `scripts/verify-match.sh`：端到端验收入口。
- 非范围：
  - 不实现 Room/Battle Service（TASK-008）；房间只分配 ID，不产生房间状态。
  - 不实现服务端推送（TASK-009）；匹配结果由客户端轮询获得。
  - 不实现分差/MMR、不实现多实例分片、不引入 etcd。
  - 不实现队列的 Redis 快照与进程重启恢复（Phase 2）。
  - 不做前端页面（TASK-010）。
  - 不新增 MySQL 表。
- **已确认决策（项目所有者于 2026-09-22 全部选择 A）**：
  1. 队列存放位置：**A) 放在 Match 进程内存**。Redis 只用于可观测性镜像，
     队列快照与重启恢复留 Phase 2。
  2. 配对规则：**A) Phase 1 只做 FIFO 两人一局**，不实现分差放宽与等待时间放宽。
     理由：当前没有任何分数体系，先实现会把未验证的评分模型固化进契约。
  3. 房间分配：**A) 抽象 `RoomAllocator` 接口**，Phase 1 用「生成 room_id 并登记」的
     占位实现，TASK-008 替换为真实调用。Room 的契约由拥有它的 TASK-008 定型。
  4. 客户端获取匹配结果：**A) 轮询 `GET /api/v1/matches/current`**。
     服务端推送属 TASK-009 范围；轮询接口在推送落地后仍作为兜底保留。
  5. 监听端口与配置变量：**`MATCH_HTTP_PORT=8082`**，同步写入
     `deploy/compose/.env.example` 与 `docs/06-operations.md` 的配置清单。
- 相关 ADR：本任务**不修改服务边界或数据所有权**（决策 1、3 均选 A），因此不需要新 ADR。
- 涉及目录：`api/proto/`、`src/match/`、`src/gateway/`、`tests/unit/match/`、
  `scripts/`、顶层 `CMakeLists.txt`、`deploy/compose/.env.example`、`docs/`。
- 接口变化：Gateway 新增 `POST /api/v1/matches`、`GET /api/v1/matches/current`、
  `DELETE /api/v1/matches/current`；新增服务间 `MatchService` 契约。
  实现前必须同步更新 `docs/05-api-and-data.md` 第 2 节的接口表。
- 数据变化：无 MySQL 变更。若采用内存队列，Redis **不新增**匹配相关键；
  若需要可观测性，只加只读镜像键 `dev:match:queue_size`（带 TTL）。
- 失败场景：
  - Match 不可用：入队返回 503 `match_unavailable`，不伪装成功；恢复后无需重启。
  - 玩家已在队列：返回 200 与当前状态（幂等），不视为错误。
  - 玩家不在队列却取消：返回 200（幂等成功），理由与登出保持一致。
  - 队列已满：返回 429 `match_queue_full`，属限流类错误，调用方退避重试。
  - 请求超时：惰性淘汰，查询返回「未匹配」，不返回错误。
  - **不可信任客户端传入的 player_id**：一律使用会话中的 player_id，
    请求体里的同名字段一律忽略，否则可被用来冒充他人入队。
  - 同一玩家不允许同时出现在两个未结束的匹配结果中（架构文档明确要求），
    配对时必须做一次原子性检查。
- 验收命令（在 WSL 中执行）：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug
  bash scripts/verify-match.sh
  ```
- 测试要求：单元测试覆盖入队顺序、两人配对、重复入队幂等、取消、超时淘汰、
  队列上限、同一玩家不可重复配对；端到端覆盖两个账号入队后被配成同一
  `match_id` 与 `room_id`、取消后不再被配对、Match 停机时 Gateway 返回 503、
  Match 恢复后无需重启即可匹配。
- 回退方式：`git revert` 单次提交；Match 为新增进程，回退不影响 Gateway 既有接口。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。
- 写入权说明：按 `CLAUDE.md`「协作纪律」，本轮写入权授予当前执行会话。
- 验收标准：
  - 两个不同账号入队后被配成同一局，`match_id` 与 `room_id` 在两侧一致。
  - 第三个玩家不会被并入一个已配满的局。
  - 取消后不再被配对；重复取消不报错。
  - Match 不可用时 Gateway 返回 503，恢复后无需重启即可匹配。
  - 单元测试与 `scripts/verify-match.sh` 全部实际运行通过。
- 提交边界：允许改动 `api/proto/`、`src/match/`、`src/gateway/`、`tests/unit/`、
  `scripts/`、顶层 `CMakeLists.txt`、`deploy/compose/.env.example`、`docs/`；
  禁止改动 `src/room/`、`web/`、`migrations/`、`deploy/compose/docker-compose.yml`。

## TASK-008：Room/Battle Service：房间生命周期与权威状态

- 状态：已实现（2026-10-02），待项目所有者审阅与验收
- 依赖：TASK-007（已完成并合并，`d6695a9`）、TASK-012（范围裁剪，已完成）
- 背景问题：TASK-007 之后「匹配成功」只得到一个占位的 `room_id`（由 `match_id`
  派生），**不产生任何房间状态**。Phase 1 的退出标准要求「两个客户端完成一场最小
  对战」，因此必须有一个服务真正持有房间的权威状态、推进对局并产出对局结果。
- 本次目标：
  - 建立 `RoomService` 契约与独立 brpc 服务进程。
  - 实现房间生命周期状态机与权威状态推进（固定 tick）。
  - 对局结束时**同步幂等**写入 `match_results`。
  - Match 的 `RoomAllocator` 替换为真实的 brpc 调用。
  - Gateway 新增加入房间、提交输入、查询房间状态、查询对局结果四个接口。
- **对局规则（经项目所有者确认，决策 A）**：两人一局；服务端 10 Hz 推进；
  输入只有攻击，每次扣对手 10；HP 初值 100；一方归零即结束；600 帧（60 秒）后按 HP
  判定，相同为平局。数值集中在 `src/room/room_types.hpp`。
- 范围：
  - `api/proto/room.proto`：`CreateRoom` / `JoinRoom` / `SubmitInput` /
    `GetRoomState` / `GetMatchResult`。
  - `src/room/`：`battle_room`、`room_manager`、`match_result_writer`、
    `mysql_match_result_writer`、`room_service`、`room_main`。
  - `src/match/brpc_room_allocator.*`：真实房间分配；`MatchQueue` 改为
    **两阶段配对**（锁内取人、锁外分配、锁内提交）。
  - `src/gateway/`：`room_client.hpp` + `brpc_room_client.*`；四个 HTTP 接口。
  - `include/common/mysql_connection.*`：原在 `src/gateway/`，现被 Gateway 与 Room
    两个服务使用，按放置规则移入公共目录并单独成目标 `rgbt_common_mysql`。
  - `cmake/generate_service_proto.cmake`：第三个 proto 出现，按 Backlog 记录的
    触发条件抽取公共代码生成函数。
  - `tests/unit/room/`、`tests/unit/match/`、`tests/unit/gateway/` 的新用例。
  - `scripts/verify-room.sh`：端到端验收入口。
- 非范围：
  - 不实现服务端推送（TASK-009）；客户端通过轮询获取状态。
  - 不实现断线重连与宽限期（Phase 2）；`connected` 字段表达「是否在房间内」，
    不是「网络是否连通」。
  - 不实现房间快照的持久化（Phase 2）；快照环形缓冲只存在内存里。
  - 不实现分差/MMR、不实现多实例、不引入 etcd（ADR-0003 非目标）。
- 相关 ADR：不修改服务边界或数据所有权（TASK-012 已把 `match_results` 定为
  Room/Battle 所有），因此不需要新 ADR。
- 失败场景：
  - Room 不可用：`CreateRoom` 失败 → Match 把玩家退回队首等下一次配对，
    **不标记为超时**；Gateway 的房间接口返回 503 `room_unavailable`。
  - MySQL 不可用：对局结果写入失败 → 房间停在 `FINISHING` 并按 1 秒间隔重试；
    查询返回 503 `result_pending` 而**不是** 404，也不返回内存里的胜负。
  - 对局已结束：再加入或提交输入返回 409 `room_already_finished`。
  - 非本局成员：加入返回 400 `not_a_member`。
  - 玩家在房间分配期间取消：这一局作废，剩余玩家退回队列；
    已创建但无人加入的房间由 Room 的等待超时（30 秒）自行回收。
- 已知限制（有意保留，写在这里避免被当成缺陷）：
  - 已结束但未落库的对局只存在于内存里，**Room 进程重启会丢失**。属 Phase 2。
  - 匹配队列与房间都在进程内存，单进程内有效（ADR-0003 已确认不做多实例）。
  - 结果写入失败时**无限重试**，不设放弃上限。理由是不可再生的数据优于内存占用；
    代价是 MySQL 长期不可用会让 `FINISHING` 房间持续累积。属 Phase 2。
- 验收命令（在 WSL 中执行）：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug --output-on-failure
  bash scripts/verify-room.sh
  ```
- 提交边界：允许改动 `api/proto/`、`src/`、`include/`、`tests/`、`cmake/`、`scripts/`、
  顶层 `CMakeLists.txt`、`deploy/compose/.env.example`、`docs/`；
  禁止改动 `web/`、`migrations/`（表结构不需要变更）。

## TASK-009：Gateway SSE 推送路由与房间消息

- 状态：待验收
- 背景问题：TASK-008 之后房间状态只能靠客户端轮询 `/api/v1/rooms/state` 获得。
  轮询能跑通，但它是「客户端按固定间隔问」，对局页的血量刷新最多滞后一个轮询间隔，
  且 N 个客户端会产生 N 份等价的查询。Phase 1 的目标是「双浏览器完成一场对战」，
  这需要服务端能把状态变化推出去。
- **范围变更（项目所有者 2026-10-02 确认）**：原定用 WebSocket 实现。动手前核实框架
  能力时发现 **brpc 1.16.0 完全不支持 WebSocket**（文档/头文件/源码三处检索为空），
  且 `CreateProgressiveAttachment` 只能持续写响应体、无法接管 socket 读取客户端帧，
  因此「在 brpc 上自实现 WebSocket」也不成立。改用 brpc 官方支持的 SSE。
  完整论证见 [ADR-0004](adr/0004-sse-instead-of-websocket.md)。
- 本次目标：把房间状态的传递方式从「客户端轮询」改为「服务端推送」，保留轮询兜底。
- 范围：
  - `GatewayService.StreamEvents`：`GET /api/v1/stream?room_id=...`，成功时返回
    `text/event-stream` 长连接。
  - `StreamHub`：订阅表 + 推送驱动。**只对有订阅者的房间**按 100 ms 轮询
    `RoomService.GetRoomState`；帧号变化才推送；同房间多订阅者扇入成一次轮询。
  - 事件：`session.ready`、`room.state`、`room.finished`（含 `aborted`）。
  - 心跳：以 SSE 注释行 `: ping` 发送，不作为事件。
  - 订阅的访问控制：用一次 `GetRoomState` 校验调用者是该房间成员，
    非成员返回 `400 not_a_member`。
  - 优雅退出：`CloseAll()` + `Stop()`，否则长连接会拖住 `brpc::Server::Stop()`。
  - 删除从未被读过的 `GATEWAY_WS_PORT`（SSE 复用 HTTP 端口）。
  - 全仓库 47 处 WebSocket 表述同步为 SSE。
- 非范围：
  - **不实现 `match.updated` 推送**。匹配状态变化频率低，改为推送需要 Gateway 为
    每个在线连接轮询 Match，收益不值这份负载；客户端继续轮询
    `/api/v1/matches/current`。理由写在 `docs/05-api-and-data.md` 第 2 节。
  - 不做帧级高频上行。客户端上行仍是「一次动作一个 HTTP 请求」。
  - 不删轮询接口（`/api/v1/rooms/state`、`/api/v1/results` 保留为兜底）。
  - 不改 Room 契约（不引入 brpc streaming RPC 订阅）。
  - 不做断线重连补帧（Phase 2）。
  - 不引入任何 WebSocket 库（Boost.Beast 未安装，会新增 vcpkg 依赖）。
- 相关 ADR：**ADR-0004（新增）**；部分替代 ADR-0001/0002/0003 中的 WebSocket 表述。
- 涉及目录：`api/proto/`、`src/gateway/`、`tests/unit/gateway/`、`scripts/`、
  `deploy/compose/`、`docs/`。
- 接口变化：新增 `StreamEvents` RPC 与 `GET /api/v1/stream`；删除 `GATEWAY_WS_PORT`。
- 数据变化：无（不涉及表结构与迁移）。
- 失败场景：
  - 缺 Token → 400 `token_required`；缺 room_id → 400 `room_id_required`。
  - 房间不存在 → 404 `room_not_found`。
  - 非本局成员 → 400 `not_a_member`（**安全关键**，否则能偷看别人血量）。
  - 无 HTTP 上下文（单元测试直接调用）→ 500 `stream_requires_http`，
    且**不登记订阅**。
  - 客户端断开 → 下一次写失败时清理订阅，不需客户端发任何东西。
  - Room 抖动 → 保持连接不关闭（恢复后继续推送），也不刷 error 事件。
- 已知限制：
  - 上行是「一次动作一个 HTTP 请求」。当前输入是低频点击（10 Hz 帧、每帧最多结算
    一次攻击），够用；**帧级高频输入会成为瓶颈**，那时要重新评估 ADR-0004。
  - 房间结束但结果未落库（`finishing`）期间仍会推送状态；只有 `finished` /
    `aborted` 才关流。
- 验收命令：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug --output-on-failure
  bash scripts/verify-stream.sh     # SSE 推送全链路
  bash scripts/verify-room.sh       # 回归：轮询与房间链路未被破坏
  bash scripts/verify-match.sh      # 回归：匹配链路未被破坏
  ```
- 测试要求：新增 13 个 `StreamHubTest` 用例与 6 个 Gateway 侧 SSE 用例；
  并发代码必须跑 ASan（见下）。
- 回退方式：`git revert` 本任务提交。若只想关掉推送，不传 `StreamHub` 给
  `GatewayServiceImpl` 即可——推送接口会返回 503 `stream_unavailable`，
  房间与匹配的轮询链路完全不受影响。

## TASK-010：Vue 演示页面：登录、大厅、对战、结算

- 状态：待验收
- 背景问题：到 TASK-009 为止服务端链路已经完整（匹配 → 房间 → 对战 → 结果，
  以及 SSE 推送），但**没有任何东西能把它演示出来**。Phase 1 的退出标准是
  「双浏览器完成匹配、进房和对战」，没有前端就无法验证，也无法演示。
- 本次目标：浏览器里能完整走通「登录 → 匹配 → 进房对战 → 结算」，
  并且服务端推送在界面上真实可见。
- 范围：
  - `web/` 下的 Vue 3 + TypeScript + Vite 应用。
  - 四个视图：登录、大厅（匹配）、对战（Canvas）、结算。
  - `BattleCanvas.vue`：Canvas 渲染双方血条、帧号与推送序号。
  - `src/api/stream.ts`：用 `fetch` + `ReadableStream` 消费 SSE。
  - `vite.config.ts`：把 `/api` 代理到 Gateway，浏览器侧同源，无需 CORS。
  - 单元测试：SSE 解析的边界（注释、多行 data、CRLF、冒号后空格、无 data 行）。
  - `scripts/verify-web.sh`：端到端验收。
- 非范围（明确不做，避免前端膨胀）：
  - **不引 `vue-router`**。四个视图是同一会话的四个阶段，不是可独立寻址的页面。
  - **不引 Pinia / Vuex**。跨组件共享的状态只有会话那几项。
  - **不引 UI 组件库、不引 axios**。
  - 不做断线重连 UI（Phase 2）。
  - **不改进 CI**。本轮不加前端 CI job：本地的可运行性已由 `verify-web.sh` 覆盖，
    前端稳定后再纳入（依据 CLAUDE.md 第 6 条）。
  - 不改任何后端接口。
- 相关 ADR：ADR-0004（SSE 取代 WebSocket，决定了前端如何消费推送）。
- 涉及目录：`web/`、`scripts/`、`deploy/compose/`、`docs/`。
  后端源码不在本任务范围内。
- 接口变化：无。
- 数据变化：无。
- **环境变化**：WSL 里原本没有 Node，本任务安装了 `node v22.22.2` 到
  `~/tools/node`（用户态，未用 sudo）。安装方式与两种被否决的方式
  （nvm 源不可达、apt 需要 sudo）记录在 `docs/06-operations.md` 第 1 节。
- 失败场景（界面必须说清楚，不能装作成功）：
  - 依赖不可用（Redis / MySQL / Match / Room）→ 按 `error.reason` 给出具体提示，
    而不是「请求失败」。
  - 会话失效 → 提示重新登录。
  - 订阅被拒（`not_a_member`）→ 明确说明不是这一局的成员。
  - 对局结束但结果未落库（`result_pending`）→ 界面区分「胜负」（来自推送）
    与「结果已落库」（来自一次查询），不把两者混为一谈。
  - SSE 断开 → 顶栏显示「推送已断开」，不静默。
- 已知限制：
  - 不做断线重连（Phase 2）。
  - 匹配状态靠轮询感知，不推送（理由见 `docs/05-api-and-data.md` 第 2 节）。
  - 不做本地乐观更新：点攻击后血量要等服务端下一帧推送才变。
    这是刻意的——本地先减血会制造第二种真相。
  - `verify-web.sh` **覆盖不到渲染**（Canvas 画得对不对、按钮状态），
    它验证的是网络路径与 SSE 经过代理的行为。渲染由脚本结尾打印的人工步骤确认。
- 验收命令：
  ```bash
  bash scripts/verify-web.sh
  # 或分步：
  cd web && npm ci && npm run test && npm run type-check && npm run build
  ```
- 测试要求：`vitest` 覆盖 SSE 解析（12 个用例）；`vue-tsc` 无错误；
  `verify-web.sh` 断言全部通过。
- 回退方式：`git revert` 本任务提交。前端不参与后端构建，
  删除 `web/` 与 `scripts/verify-web.sh` 不影响任何 C++ 目标。

## TASK-011：集成验收：一条命令启动并双客户端完成对局

- 状态：待验收
- 背景问题：Phase 1 的其余退出标准都已满足（`scripts/verify-web.sh` 已经证明
  两个身份能登录、匹配、进同一房间并打完整局），但**启动流程散落在四个脚本文档
  和各处命令里**，没有人能照着一条命令把整套环境跑起来。
  另外「重跑既有验收脚本」这条纪律在 TASK-009 期间被证明落不了地——
  TASK-008 改坏 `verify-match.sh` 后十天无人发现。纪律需要一条命令来承载。
- 本次目标：一条命令起齐集成环境；一条命令跑完整套验收。
- 范围：
  - `scripts/dev-up.sh`：起 Redis/MySQL（容器）+ 三个 C++ 服务 + Vue 前端，
    应用迁移、清理上一轮会话、检查端口、等就绪、打印人工验证步骤。
  - `scripts/dev-down.sh`：按**进程组**停掉全部进程并按端口兜底核对，
    可选 `--with-docker` 一并停容器。数据卷保留。
  - `scripts/verify-all.sh`：按顺序跑 6 个验收脚本，输出汇总表与失败项摘要。
  - `.run/` 作为运行时产物目录（pid、日志），加入 `.gitignore`。
  - 修正 `docs/06-operations.md` 中与实现不符的「集成环境」描述。
- 非范围：
  - **不把应用服务容器化**（项目所有者 2026-10-02 确认）。理由与环境冲突的
    处理见 `docs/06-operations.md` 第 2 节；已记入 Backlog。
  - 不改任何服务代码、接口与数据。
  - 不做故障注入（Phase 4）。
  - 不把 `verify-all.sh` 接进 CI：它需要 Docker 与四个本机进程，
    当前的 CI 形态不适合（Phase 5 再评估）。
- 相关 ADR：无新增。ADR-0003 的非目标不受影响。
- 涉及目录：`scripts/`、`docs/`、`.gitignore`。
- 接口变化：无。
- 数据变化：无。
- 失败场景：
  - 端口被占用 → 明确报错并指出占用进程，不静默换端口（换端口会让前端的代理
    目标与用户手上的 URL 对不上）。
  - Docker 不可达 → 启动前检查并明确报错。
  - 某个服务启动失败 → 直接打印该服务日志的末尾，不让用户自己去猜。
  - `dev-down.sh` 在无进程时重复执行 → 必须安全（幂等）。
  - pid 文件丢失或过期 → 按端口兜底清理，并断言端口确实释放。
- 验收命令：
  ```bash
  bash scripts/dev-up.sh --no-build    # 起齐环境并自检
  bash scripts/dev-down.sh             # 停干净
  bash scripts/verify-all.sh           # 全套验收（实测 142 秒，见 devlog）
  ```
- 测试要求：`dev-up.sh` → 经代理跑通一局 → `dev-down.sh` 的闭环必须实测；
  `dev-down.sh` 必须在无进程时也安全；`verify-all.sh` 必须跑完全套并给出汇总。
- 回退方式：`git revert` 本任务提交。三个脚本是纯新增，删掉不影响任何构建。

## Phase 2 任务拆分（2026-10-02 项目所有者确认，5 个任务）

Phase 0 与 Phase 1 已全部完成并合并。Phase 2 的拆分原则与 Phase 1 相同：
**每个任务都要有可独立运行的验收命令，且不引入下一个任务的组件。**

| 顺序 | 编号 | 任务 | 依赖 |
|---|---|---|---|
| 1 | TASK-013 | 房间记录表与快照写入 | TASK-011 |
| 2 | TASK-014 | 房间重启恢复与恢复边界 | TASK-013 |
| 3 | TASK-015 | 匹配队列的 Redis 快照与重启恢复 | TASK-011 |
| 4 | TASK-016 | 断线重连与宽限期 | TASK-014 |
| 5 | TASK-017 | 推送连续性与恢复时间报告 | TASK-016 |

**Phase 3、4、5 暂不拆分。** 只把未验证的拆分写进文档，后面改起来要改两处，
而且写多了会让「计划」看起来像「承诺」。这三个阶段的范围与退出标准保留在
`docs/02-roadmap.md`，各自开始时再拆。

Phase 2 要还的历史欠账（此前各任务明确标注为 "Phase 2" 的）：Match 队列的 Redis
快照（TASK-007）、房间快照持久化与"已结束未落库的对局重启丢失"（TASK-008）、
断线重连与宽限期（TASK-008）、SSE 订阅在 Gateway 重启后丢失（TASK-009）、
前端断线重连 UI（TASK-010）、Redis 启用 AOF（TASK-003）。

### TASK-013：房间记录表与快照写入

- 状态：进行中
- 背景问题：`01-architecture.md` 第 5 节已把「房间权威状态」的恢复策略写成
  **「从最近快照恢复」**，但房间状态至今只存在于进程内存的环形缓冲里。
  Room 一重启，进行中的对局就无声消失，连"这里曾经有一局"都查不到。
  没有落盘的快照，TASK-014 的恢复无从谈起。
- 本次目标：把房间快照定期写入 MySQL，并确立**快照可以丢弃**这一失败策略。
- 范围：
  - `migrations/005_create_rooms.sql`：房间记录表。
  - `src/room/room_repository.hpp` + `mysql_room_repository.*`：只做写入。
  - `RoomManager::Tick` 到达快照间隔时取快照副本，**锁外**写 MySQL。
  - 快照间隔常量与失败处理策略。
- 非范围：不做启动恢复（TASK-014）；不改 `match_results` 的写入路径；
  不做房间历史查询接口（没有消费者）；不引入 Redis 缓存这层（MySQL 足够）。
- 相关 ADR：无新增，落实的是 `01-architecture.md` 第 5 节已确认的恢复策略。
- 涉及目录：`migrations/`、`src/room/`、`tests/unit/room/`、`scripts/`、`docs/`。
- 接口变化：无。数据变化：新增 `rooms` 表。
- **关键设计决定**（本任务最需要审阅的部分）：

  1. **单表 + 离散列，不用 JSON 快照列**。房间字段全部离散成列，而不是把快照
     序列化成一个不透明字段。理由：schema 本身就是"能恢复什么"的文档；JSON 列
     会把这件事藏起来，并立刻带来格式版本管理问题。`kPlayersPerRoom = 2` 是
     TASK-008 已由项目所有者确认的对局规则，因此 `p1_*` / `p2_*` 两列不是
     未经验证的假设。
     **被否决的备选**：`rooms` + `room_players` 子表。它不把"2"写进 schema，
     但恢复路径要 join、快照写入要用事务包两条语句，而人数在 ADR-0003 下不会变。
     收益不成立，成本立刻发生。

  2. **快照间隔 1 秒（10 帧）**，由常量控制。对战 10 Hz、最长 600 帧（60 秒）。
     1 秒间隔意味着重启后最多回退 1 秒进度，也就是最多重放或少算一次攻击
     （每次 10 点血，占满血 10%）。1 Hz/房间 对 MySQL 的压力可以忽略。
     更密不划算，更疏偏差难解释。

  3. **快照写入失败不重试、不阻塞，下一次覆盖**。这是本任务最重要的区分：
     **对局结果不可丢弃**（所以 `FINISHING` 无限重试），
     **房间快照可以丢弃**（下一帧的写入天然覆盖它）。因此快照绝不能套用
     `match_results` 那套重试逻辑——那会让一次 MySQL 抖动在内存里堆起一批
     过期快照，且毫无收益。写入失败只记一条日志，Tick 继续推进对局。

  4. **持锁期间不写 MySQL**（沿用 TASK-008 的既有约束）。快照副本在锁内取，
     写入在锁外做。房间推进线程不能因为一次 MySQL 超时停摆。
- 失败场景：MySQL 不可用 → 对局继续推进，只记日志，恢复后下一个间隔自然写成功；
  房间在两次快照之间结束 → `FINISHED` 后再写一次终态；`ABORTED` 房间同样落
  终态记录，便于事后核对。
- 已知限制：快照是**周期性**的，不构成"精确恢复"，偏差范围由 TASK-014 定义并实测；
  快照表会随对局数增长，本任务不做清理策略（没有容量证据，见 CLAUDE.md 第 6 条）。
- 验收命令：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug --output-on-failure
  bash scripts/verify-persistence.sh     # 新增：快照确实落库且随帧推进
  bash scripts/verify-room.sh            # 回归：房间链路未被破坏
  ```
- 测试要求：单元测试覆盖「到达间隔才写」「写入失败不影响 Tick」「快照内容与内存
  状态一致」「结束后写终态」；端到端覆盖「对局进行中 `rooms` 表有该 `match_id`
  的行且 `frame` 在增长」「停止 MySQL 后对局仍能打完」。
- 回退方式：`git revert`。快照写入是旁路，去掉它房间功能不受影响
  （只是回到"重启即丢失"）。

### TASK-014：房间重启恢复与恢复边界

- 状态：待确认
- 背景问题：TASK-013 把快照写进了 MySQL，但没有人读它。
  `01-architecture.md` 第 5 节写的「从最近快照恢复」仍未兑现。
- 本次目标：Room 启动时从 `rooms` 表恢复未结束的房间，并**明确定义恢复边界**。
- 范围：`RoomRepository` 的读取路径；启动时扫描并重建未结束的房间；
  恢复时的状态机合法性校验；**恢复边界文档化**（最多丢多少进度、哪些字段精确、
  哪些近似）；`FINISHING` 房间重启后被重新纳入落库重试（它在内存里消失过，
  但结果不能丢——这是 TASK-008 留下的已知限制）。
- 非范围：不恢复 SSE 订阅（属 Gateway，TASK-017）；不恢复匹配队列（TASK-015）；
  不做跨节点迁移（ADR-0003 非目标）。
- 失败场景：快照损坏 / 表缺列 / 状态非法 / 双方都不在房间内 → 一律标记 `ABORTED`
  并记录原因，**不静默丢弃、也不伪装成正常对局**。
- 验收命令：`bash scripts/verify-persistence.sh`（新增「重启恢复」一节）
- 测试要求：单元测试覆盖各类损坏快照的处理；端到端覆盖「对局进行中 `kill -9`
  Room，重启后房间仍在且能打完」，并记录**实测的进度丢失量**。
- 回退方式：`git revert`。恢复是启动路径上的旁路，去掉后退回"重启即丢失"。

### TASK-015：匹配队列的 Redis 快照与重启恢复

- 状态：待确认
- 背景问题：TASK-007 的已知限制原文——「**Match 重启即丢失排队状态**」。
  排队中的玩家会突然变成 `idle`，且没有任何解释。
- 本次目标：Match 的排队状态写入 Redis，重启后重建。
- 范围：队列快照写入 Redis（入队 / 配对 / 取消 / 超时时更新）；
  Match 启动时重建队列并**重新判定超时**（重启期间的时间要算进去）；
  分配中（`allocating`）的条目在重启后的处理策略。
- 非范围：不做跨实例共享队列（ADR-0003 非目标）；不改配对规则。
- 失败场景：Redis 不可用时的行为（明确报错还是降级为纯内存）**需要项目所有者决定**。
- 验收命令：`bash scripts/verify-persistence.sh`（新增「队列恢复」一节）
- 测试要求：单元测试覆盖快照序列化与超时重算；端到端覆盖「排队中重启 Match，
  玩家仍在队列里」。
- 回退方式：`git revert`。

### TASK-016：断线重连与宽限期

- 状态：待确认
- 背景问题：`disconnected` 目前**没有任何实际语义**——TASK-008 的 `connected`
  字段只表达"是否已加入房间"，网络断开根本不被感知。对局中刷新页面或断网，
  玩家就永久缺席，而对局仍会继续推进到他输。
- 本次目标：把「断线」变成可观测、可恢复、有期限的状态。
- 范围：Gateway 感知 SSE 流断开（写失败已经能感知）并标记会话；
  宽限期语义落地（期内重连回到原房间，到期后按规则回收）；前端断线提示与自动重连。
- 非范围：不做跨设备接管；不做排队中的断线保位（未开始的对局没有位置可言）。
- 验收命令：`bash scripts/verify-reconnect.sh`（新增）
- 测试要求：单元测试覆盖宽限期状态机；端到端覆盖「对局中断开 → 宽限期内重连 →
  仍在原房间且血量正确」与「超过宽限期 → 资源被回收」。
- 回退方式：`git revert`。

### TASK-017：推送连续性与恢复时间报告

- 状态：待确认
- 背景问题：Gateway 重启后所有 SSE 订阅丢失，客户端只能自己发现；且断线期间
  错过的 `room.state` 无法补发——SSE 规范自带的 `Last-Event-ID` 语义一直没用上
  （ADR-0004 已指出这一点）。
- 本次目标：重连后能补上缺失的事件；并给出 Phase 2 的恢复时间汇总。
- 范围：SSE 端点解析 `Last-Event-ID`，从房间快照历史里补发缺失事件；
  快照历史的保留策略（现在是内存环形缓冲 `kMaxSnapshotHistory = 128`）；
  汇总 Phase 2 各场景的实测恢复时间与数据丢失边界。
- 非范围：不做事件的持久化重放（领域事件属 ADR-0003 非目标）。补发只覆盖仍在
  内存历史窗口内的事件，窗口外明确告知客户端「需要全量刷新」。
- 验收命令：`bash scripts/verify-reconnect.sh`（扩展）
- 回退方式：`git revert`。

## TASK-012：范围裁剪——把非目标写进文档

> **编号说明**：本任务在编号上排在 Phase 1 计划之后，但**实际执行时间早于
> TASK-008 起的所有待办任务**。原因是它不实现任何功能，只是把已经确认的范围
> 裁剪写进文档；若强行插入编号，会导致 `api/proto/` 与 `src/` 中大量指向
> TASK-008/009 的注释被无意义地改写，扩大文档提交的 Diff。

- 状态：文档改动已完成（2026-10-02），待项目所有者审阅与合并
- 依赖：TASK-007（已完成并合并，`d6695a9`）
- 背景问题：原路线图的 Phase 4（etcd/多实例）与 Phase 5（Kafka/Settlement Worker），
  以及 Player/State 独立服务，已被项目所有者确认为非目标。但**文档里仍把它们写成
  "后续阶段"**。后续 AI 读到这些描述仍会按计划实现，因此必须把"不做"写进文档，
  而不是只存在于对话里。
- 本次目标：
  - 新增 ADR-0003，记录裁剪范围、理由、备选方案和连带影响。
  - 把全部文档中的相关描述改为"非目标 / 不实现"。
  - 处理连带影响：`players` 与 `match_results` 两张表的所有权。
- 范围（本任务**只改文档与注释，不改任何行为代码**）：
  - 新增 `docs/adr/0003-scope-reduction.md`。
  - `CLAUDE.md`：规则 5、技术方向、服务边界、数据所有权、迭代准入条件、
    当前阶段，并新增"范围裁剪"硬性约束一节。
  - `README.md`：项目定位、目标架构图、技术基线表、非目标、仓库结构、完成标准。
  - `docs/00-charter.md`：问题表、功能目标、成功标准、非目标、风险。
  - `docs/01-architecture.md`：上下文图、服务职责、数据流、状态归属、失败模型、
    部署演进、接口、变更规则。
  - `docs/02-roadmap.md`：阶段总览重排，新增"已取消的阶段"一节。
  - `docs/04-quality-and-observability.md`：移除消息队列相关测试与指标。
  - `docs/05-api-and-data.md`：服务列表、表清单与所有权、Kafka 章节、幂等与一致性。
  - `docs/06-operations.md`：故障环境、Runbook、配置清单、备份与发布。
  - `docs/07-open-decisions.md`：D-004 与 D-005 改为已关闭结论。
  - `docs/adr/0001`、`docs/adr/0002`：标注部分替代关系。
  - `deploy/compose/.env.example`：移除 `KAFKA_BROKERS` 与 `ETCD_ENDPOINTS`。
  - `migrations/002`、`003` 与若干源文件注释中的过期所有权/阶段引用。
- 非范围：
  - 不实现任何功能，不修改服务行为、接口契约或数据表结构。
  - 不删除 `migrations/002`、`003`（迁移只追加，不回溯删除）。
  - 不改动既有测试与验收脚本的行为。
- 相关 ADR：新增 ADR-0003；部分替代 ADR-0001 与 ADR-0002。
- 验收标准：
  - 全仓库检索 `Kafka`、`etcd`、`Settlement`、`Player/State`、`多实例`，
    剩余出现处均为"非目标"说明或历史记录，**不存在实现指引**。
  - `CLAUDE.md`、README、charter、architecture、roadmap 的口径一致。
  - 服务列表只有 Gateway、Match、Room/Battle 三个。
  - `deploy/compose/.env.example` 不再包含 Kafka 与 etcd 变量。
- 回退方式：`git revert` 本任务提交。全部为文档改动，不影响构建与运行。
- 提交边界：允许改动 `CLAUDE.md`、`README.md`、`docs/`、`deploy/compose/.env.example`、
  `migrations/*.sql` 的注释、`CMakeLists.txt` 与 `src/` 的注释；禁止改动任何可执行逻辑。

## Backlog：后续待办

- **应用服务的容器化**（TASK-011 期间决定延后）。给三个 C++ 服务与前端写
  Dockerfile，并加进 `deploy/compose/docker-compose.yml`。它是「现场演示可复现」
  与「故障注入」的前置条件，属于 Phase 4/5。
  **第一步应该是限时构建实测，而不是先写 Dockerfile**：容器里要用 vcpkg 从源码
  重建 brpc，首次耗时与内存占用都未实测，而 Docker Desktop 只分到 11 GiB。
  拿到真实数字再决定是否值得做。背景见 `docs/06-operations.md` 第 2 节。
- 将 brpc 预设与 Compose 配置纳入 CI 覆盖。`scripts/verify-all.sh` 需要 Docker
  与四个本机进程，当前 CI 形态不适合，一并在这里评估。
- 实现 `docs/05-api-and-data.md` 中其余接口（匹配、房间、结果）。
- Token 刷新与长期会话策略（Phase 2）。
- 抽取 Protobuf 代码生成的公共 CMake 逻辑。TASK-007 引入了第二个 proto
  （`api/proto/match.proto`），`src/match/CMakeLists.txt` 与
  `src/gateway/CMakeLists.txt` 中的生成写法现在**确实重复了**。TASK-007 故意不做
  抽取：把公共 CMake 函数的重构混进「新增服务」的提交里会降低可审阅性。
  建议在第三个 proto 出现、或把代码生成整体上移到顶层时一并处理。
- 为 Gateway 增加区分存活与就绪的健康检查端点（`/health/ready`），
  同时检查 Redis 与 MySQL。
- 把 `src/gateway/test_credentials.{hpp,cpp}` 改名为 `dev_accounts.{hpp,cpp}`。
  它与测试无关：里面是**开发期固定测试身份**（账号 + 口令摘要），被产品代码在登录时
  调用并编入 `rgbt_gateway_lib`，只因名字带 `test` 而容易被误认为测试文件，也容易
  被误认为「测试代码不该进产品二进制」。改名需同步 CMake、测试与文档三处；
  等出现第二个调用方（Player 服务）时，连同「提升到 `include/common/`」一起做。

## 任务完成定义

- 代码、测试、文档和 devlog 同步。
- 用户审阅关键 Diff。
- 所有验收命令实际运行。
- 没有未解释的警告、遗留密钥和构建产物。
- **改变了某个服务的依赖或启动方式时，必须重跑受影响的既有验收脚本，并在
  devlog 里记录重跑结果。** 这条是 2026-10-02 补上的：TASK-008 把 Match 的房间
  分配换成真实 brpc 调用后，`scripts/verify-match.sh` 因为不启动 Room 而**全部配对
  失败**，直到十天后有人重跑它才发现。只验新脚本、不重跑旧脚本，等于让缺口静默积累。
- 不属于当前任务的后续问题记录到 Backlog，不直接扩大当前提交。
