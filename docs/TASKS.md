# 当前任务

> 状态：**Phase 1 与 Phase 2 均已完成并合并到 `main`；Phase 3 的 5 个任务
> 全部完成，等待项目所有者按退出标准验收。**
> `main` 已包含 **TASK-000 ~ TASK-017**：TASK-013 ~ TASK-016 分别经
> PR #10 ~ #13 合并；**TASK-017 经 PR #15 合并（merge commit `a61495a7`，
> 2026-10-02）**。
> Phase 2 的退出标准逐条对照表见 `docs/devlog.md` 的
> 「Phase 2 退出标准对照表」，恢复时间的测量结果与上界见同文件的
> 「推送连续性与恢复时间汇总」。
> **Phase 3**（可观测性和容量基线）已由项目所有者确认拆为 5 个任务
> （TASK-018 ~ TASK-022，见本文档「Phase 3 任务拆分」一节）；
> **TASK-018 ~ TASK-022 全部完成并实测通过，Phase 3 已结束**
> （TASK-019 经 PR #18、TASK-020 经 PR #19、TASK-021 经 PR #20、
> TASK-022 经 PR #21 合并，merge commit `94b0393`）。
> **Phase 3 的退出标准逐条对照表见 `docs/devlog.md`**（含五条未测项）。
>
> **Phase 4**（故障注入与可靠性验证）的任务单已拆分（**TASK-023 ~ TASK-027**，
> 见本文档「Phase 4 任务拆分」一节；原 TASK-028 已由项目所有者裁决移出本阶段）。
> **TASK-023 已完成、实测通过并合并**（2026-10-04；提交 `ed92604`，PR #23，
> merge commit `c6abac2`；交付物与实测数字见该任务单的「实施结果」）。
> **TASK-024 ~ TASK-027 仍是「待确认」**：未获得项目所有者逐个确认前，
> 不写任何实现代码。
>
> **2026-10-03 状态清理**：TASK-019 的任务单此前停留在"待确认"，而它早已实现并
> 经 PR #18 合并；已按 `git log` 更正。同一轮也修正了本文档顶部的进度描述
> （此前写的是"TASK-018 待开工"，已过期两个任务）。
>
> **2026-10-02 分支粒度纠正**：上一轮误按 Phase 1 的「一任务一分支」建了
> `feat/task-014-*` / `feat/task-015-*` 两条分支（并建议 squash 合并），
> 与第 9 节「一阶段一分支 + 一任务一提交 + **不要 squash 整条分支**」相悖。
> 已把两个任务各压成**一个提交**放回 `feat/phase-2`，并删除多余分支。
> 起因是漏读了 `03-development-workflow.md`——它就在必读顺序里。
> 教训：**分支与提交流程属于"读文档"的一部分，不能凭 Phase 1 的印象行事。**
>
> **2026-10-02 状态清理**：本次把 TASK-001/002/003/007 的「进行中」与
> TASK-008 ~ TASK-013 的「待验收」更正为实际状态。此前这些标注与 git 状态不符
> （它们早已合并），会让人误以为还有一堆任务挂着。更正的依据是
> `git log origin/main` 与各提交的合并点，不是印象。
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

- 状态：已完成（WSL 的 `~/workspace/realtime-game-backend` 后来成为正式开发目录，
  Windows 目录保留为只读副本）
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

- 状态：已完成（已合并到 main：`eb67a6c`，PR #1）
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

- 状态：已完成（已合并到 main）
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

- 状态：已完成（2026-09-22 起，任务单已由项目所有者确认；已合并到 main：`d6695a9`，PR #4）
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

- 状态：已完成（2026-10-02 实现；已合并到 main：`2dc1ca9`，PR #6）
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

- 状态：已完成（已合并到 main：`cc8a8dc`，PR #7）
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

- 状态：已完成（已合并到 main：`87725bd`，PR #8）
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

- 状态：已完成（已合并到 main：`cde8918`，PR #10）
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

**Phase 3 已于 2026-10-04 完成并合并**，其拆分见本文档「Phase 3 任务拆分」一节。
**Phase 4 的任务单已拆分**（TASK-023 ~ TASK-027，见「Phase 4 任务拆分」一节），
其中 **TASK-023 已完成、实测通过并合并到 `main`**（2026-10-04，提交 `ed92604`，
PR #23；见该任务单的「实施结果」），**TASK-024 ~ TASK-027 仍处于「待确认」**：
按本项目的约定，任务单只写范围与验收标准，逐个确认后才开工，避免把「计划」写成
「承诺」。
**Phase 5 仍未拆分**：它的范围与退出标准保留在 `docs/02-roadmap.md` 第 8 节，
开始前再拆——Phase 4 的实测结论会直接影响 Phase 5 要收口什么。

Phase 2 要还的历史欠账（此前各任务明确标注为 "Phase 2" 的）：Match 队列的 Redis
快照（TASK-007）、房间快照持久化与"已结束未落库的对局重启丢失"（TASK-008）、
断线重连与宽限期（TASK-008）、SSE 订阅在 Gateway 重启后丢失（TASK-009）、
前端断线重连 UI（TASK-010）、Redis 启用 AOF（TASK-003）。

### TASK-013：房间记录表与快照写入

- 状态：已完成（已合并到 main：`cde8918`，PR #10）
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

- 状态：**已实现，待项目所有者审阅与验收**（2026-10-02）。
  按 [03-development-workflow.md](03-development-workflow.md) 第 9 节的
  分支粒度，本任务是 `feat/phase-2` 上的**单个提交**（提交信息带任务号），
  不单独建分支。
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
- 涉及目录：`src/room/`、`src/gateway/`、`include/common/`、`tests/unit/`、
  `scripts/`、`docs/`。
- **实施结果（2026-10-02）**——完整经过见 `docs/devlog.md` 的「TASK-014 实施记录」：
  - 接手的 `wip/task-014-partial`（`a35b3ef`）自带一个「存在崩溃待查」，
    本任务把它查清并修完。崩溃根因**不在恢复逻辑里**，而是三个问题：
    1. `MysqlConnection` 只创建一条连接却被 ticker 线程与启动线程并发使用
       （`room_main.cpp` 第 110/137/145 行），协议流被打乱后 `mysql_close()`
       拆 TLS 触发 `OpenSSL internal error: refcount error` → abort。
       **A/B 实测：修复前 7/10 崩溃 → 修复后 10/10 存活。**
       修复方式是给该连接加内部互斥；这同时修掉了 Gateway（brpc worker 共用一条
       连接）的同类隐患。
    2. 新增的 7e 并发检查又暴露出**同类缺陷的第二个实例**：`RedisSessionStore`
       共用一条 hiredis 连接且无锁，并发登录时 `SET … NX` 的 `+OK` 被另一个
       线程的 `GET` 当成 Token 读走（响应体里真的出现过 `"token":"OK"`）。
       同样加锁修复。
    3. `verify-all.sh` 里 `all_scripts` 写了两行，第二行把 `verify-persistence`
       覆盖掉，导致 TASK-013 的验收脚本从未被这条命令跑到（**已修正**）。
  - 同时修掉 WIP 中三处**不可能通过**的验收断言：7a 在错误的时刻度量恢复位置
    （房间恢复后立刻继续推进）、7b 的前提自相矛盾（要求"在没有 MySQL 的情况下
    从 MySQL 恢复"）、第 6 节的 2 秒观察窗太窄（快照写入阻塞 ticker 线程时
    推进是突发式的）。
  - 验收结果（WSL，16 核 / 11 GiB）：
    `--clean-first` 全量重建 **0 error / 0 warning**；`ctest` **185/185 通过**；
    `check-format.sh` 通过（69 个文件）；`verify-all.sh` **7/7 通过（228 秒）**；
    `verify-persistence.sh` **连跑 3 次全部退出码 0**，恢复点精确等于最后一次快照
    （30 / 30 / 29 帧），实测进度丢失 **0 / 0 / 1 帧**（上界 10 帧），
    12 个并发登录 3 次均 12/12 成功。
  - **ASan 已运行且干净**：Room 启动恢复路径 5 次运行存活 5/5、
    **0 条 ASan/UBSan 报告**；Gateway 12 个并发登录 12/12 成功、0 条报告。
    为此补上了两个基础设施缺口：三个**服务可执行文件**此前没有调用
    `rgbt_apply_sanitizer`（ASan 版服务根本无法链接，等于 ASan 从未覆盖过服务进程），
    以及新增 `brpc-asan` 预设（原来的 `asan` 预设不接 vcpkg，覆盖不到
    `MysqlConnection` / `RedisSessionStore` / `room_manager`）。
  - **TSan 已运行，但当前配置下不具指向性**（结论与理由已写入 devlog）：
    新增 `brpc-tsan` 预设并跑通（Room 恢复 3/3 存活、Gateway 12/12 登录成功），
    但产生 21 条报告**全部落在 brpc 内部**——vcpkg 的 brpc 没有用 TSan 插桩，
    `butil::Mutex`、`cpuwide_time_ns` 这类实现在 TSan 看来就是无同步的裸访问；
    来自本项目源码的只有 2 帧且都是对象构造点，不是竞争访问。
    要让它成为可用的门禁，必须先用 TSan 重建 brpc 依赖链（记入 Backlog）。
    **因此并发缺陷的证据仍然来自 A/B 复现（7/10 → 0/10）、ASan 与直接现象
    （响应体里出现 `"token":"OK"`），不是来自 TSan。**
  - 新增的已知限制（已写入 `docs/01-architecture.md`）：快照写入在 ticker 线程上，
    MySQL 不可用时该线程会阻塞在连接超时上，因此对局推进变成突发式而非严格 10 Hz。
    要把依赖延迟完全从推进线程摘掉需要独立写入线程。**（2026-10-04 追记：
    TASK-022 的容量基线已给出证据——500 档位每秒 301 次房间快照写入，服务端
    延迟仍在毫秒级，因此这条的优先级低于匹配队列快照那条；见
    `docs/benchmarks/README.md` 第 5 节与「Phase 4 任务拆分」的决策 2。）**

### TASK-015：匹配队列的 Redis 快照与重启恢复

- 状态：**已实现，待项目所有者审阅与验收**（2026-10-02）
- 背景问题：TASK-007 的已知限制原文——「**Match 重启即丢失排队状态**」。
  排队中的玩家会突然变成 `idle`，且没有任何解释。
- 本次目标：Match 的排队状态写入 Redis，重启后重建。
- 范围：队列快照写入 Redis（入队 / 配对 / 取消 / 超时时更新）；
  Match 启动时重建队列并**重新判定超时**（重启期间的时间要算进去）；
  分配中（`allocating`）的条目在重启后的处理策略。
- 非范围：不做跨实例共享队列（ADR-0003 非目标）；不改配对规则；
  不做队列的准实时一致性（快照允许丢，见下）；不引入 Redis 之外的新组件。
- **已确认决策（项目所有者 2026-10-02 选择 B，理由见下）**：
  **Redis 不可用时降级为纯内存，不阻塞匹配。** 快照写失败只记日志、不重试、
  不影响入队与配对；启动时读不到快照则**不恢复任何队列条目并如实报告**
  （不假装"恢复了一个空队列"），降级状态通过日志与健康检查暴露。
  理由：队列的权威状态在 Match 进程内存里，Redis 只是它的旁路快照
  （`CLAUDE.md` 已写明「Redis 用于短期状态、缓存和会话，**不作为唯一真相**」）；
  这与房间快照的既有策略一致——**快照可以丢弃，不阻塞对局**。
  若改成"Redis 不可用就 503"，等于把「备份存储挂了」升级成「核心功能挂了」，
  与本项目「快照可丢弃」的统一口径相反。
- **关键设计决定**（本任务最需要审阅的部分）：
  1. **快照内容是「整份重写」，不是增量**。Redis 用一个 `LIST`
     （`<env>:match:queue`）保存全部条目，写入时 `MULTI/DEL/RPUSH.../EXEC`
     原子替换。理由：队列条目之间有顺序（FIFO），增量更新要自己维护顺序，
     而整份重写天然给出"要么是旧的完整队列，要么是新的完整队列"。
     队列上限 1000，整份重写的开销可以忽略。
  2. **快照与写入必须成对串行化**。快照在队列锁内取、写入在锁外做
     （沿用 Room 的"锁内取、锁外写"规则，避免一次 Redis 超时卡住整个队列）。
     但只做到这一点会引入新问题：两个线程可能各自取到快照 S1（较早）与 S2（较晚），
     却按 S2、S1 的顺序写入，把队列**写回退**。因此额外用一把
     `snapshot_order_mutex_` 把"取快照 + 写快照"整体串行化，
     锁序固定为 `snapshot_order_mutex_ → mutex_`，且**不在持有队列锁时做 I/O**。
  3. **只持久化两种条目：排队中（Q）与已配对（M）**。
     已超时的条目不必持久化：重启后玩家本就是 `idle`，与超时状态对外等价。
     已配对条目必须持久化，否则"配对成功但客户端还没领取"的那一局会随着
     重启消失——而那一局的房间在 Room 里是**真实存在**的。
  4. **`allocating` 条目在重启后退回队列**（而不是丢弃，也不是直接当成已配对）。
     重启后无法知道 `CreateRoom` 是否已经成功；而 Room 侧以 `match_id` 保证幂等，
     重新分配会拿回同一个 `room_id`，因此"退回"是安全的，丢弃则会让玩家
     无缘无故从队列消失。代价是可能出现一个无人加入的孤儿房间，
     由 Room 的等待超时（30 秒）自行回收——这是既有行为。
  5. **条目编码是长度前缀的纯文本，不是 JSON**。本仓库没有 JSON 库，
     自己写一个解析器属于引入未验证的复杂度。格式为
     `版本 + 类型 + 各字段（字符串用 <len>:<bytes>，整数用十进制 + ';'）`，
     解析与序列化都是**纯函数**，可以脱离 Redis 单元测试。
     留一个显式版本号是为了让"以后改格式"能被识别，而不是解析出乱七八糟的队列。
  6. **快照里不存 `request_id`**。它唯一的用途是入队幂等，而去重以 `player_id`
     为键：重启后同一客户端用同一 `request_id` 重试会被判为 `kAlreadyQueued`，
     语义不变。同时这也让快照里**不出现任何客户端可控的字符串**，编码不必担心转义。
- 失败场景：Redis 不可用 → 降级为纯内存（见上）；快照损坏 / 版本不认识 →
  **跳过该条并记日志**，不静默丢弃也不整体拒绝；启动读失败 →
  `load_failed`，一条都不恢复；`allocating` → 退回队列。
- 验收命令：`bash scripts/verify-persistence.sh`（新增「队列恢复」一节）
- 测试要求：单元测试覆盖编码/解码往返与损坏输入、恢复后的超时重算
  （含"停机时间跨过超时"）、`allocating` 退回队列、快照写失败不影响入队与配对；
  端到端覆盖「排队中重启 Match，玩家仍在队列里」与「Redis 停机时匹配仍可用」。
- 涉及目录：`src/match/`、`tests/unit/match/`、`scripts/`、`docs/`。
- 回退方式：`git revert`。快照是旁路，去掉后退回"重启即丢失"。
- **实施结果（2026-10-02）**——完整经过见 `docs/devlog.md` 的「TASK-015 实施记录」：
  - 新增 `src/match/match_queue_store.hpp`（快照类型 + **纯函数**编解码 + 存储接口）、
    `match_queue_store.cpp`（长度前缀文本格式，显式版本号）、
    `redis_match_queue_store.hpp/.cpp`（LIST + 管道化的 `MULTI/EXEC` 整份替换，
    内部互斥）。
  - `MatchQueue` 新增 `Restore(now_ms)`、`MakeSnapshotLocked()`、`PersistSnapshot()`；
    超时/过期清算改为返回"是否变化"，只有真的变了才写快照，**轮询不产生写放大**。
  - `match_main.cpp`：新增 `-redis_host/-redis_port/-redis_timeout_ms/-env_prefix`，
    并在**开始接受请求之前**执行恢复，把恢复结果打进启动日志。
  - 新增 18 个单元测试（编解码往返与各类损坏输入、FIFO 重建、超时重算、
    已配对结果的多玩家重建、损坏行跳过、读失败不恢复、写失败不阻塞、
    分配中条目按排队写、轮询不写放大）；`verify-persistence.sh` 新增 7f 一节。
  - 验收：`ctest` **203/203 通过**（原 185 + 新增 18），构建 0 warning，
    格式检查通过（74 个文件）；`verify-persistence.sh` **退出码 0**，
    其中 7f 实测：入队后 Redis 有 1 条快照；`kill -9` Match 后重启玩家仍为
    `queued`；指向不可用端口时**匹配照常成功**且日志明确记录降级。
  - **已知限制（未修，需要证据）**：快照写入在入队请求路径上。Redis **拒绝连接**
    时几乎不花时间，但 Redis **挂起**（不回包）时会多等一个 Redis 超时
    （默认 500 ms），与 Gateway 的 `match_timeout_ms`（也是 500 ms）同量级，
    理论上可能让一次入队超时。要彻底消除需要把快照写入移出请求路径
    （独立写入线程或定时器），属于引入并发的改动，先要有证据，记入 Backlog。
  - 测试期间发现并修正了一个**测错对象**的验收：第一版 7f 用
    `docker stop rgbt-redis` 来测 Match 的降级，但 Gateway 的会话也在同一个 Redis 上，
    停掉它之后所有 HTTP 请求会先因鉴权失败返回 503，根本走不到 Match。
    改为只让 Match 的快照存储指向一个不可用端口，链路其余部分保持完好。


### TASK-016：断线重连与宽限期

- 状态：**已实现，待项目所有者审阅与验收**（2026-10-02）。
  按第 9 节的分支粒度，本任务是 `feat/phase-2` 上的**单个提交**。
- 背景问题：`disconnected` 此前**没有任何实际语义**——TASK-008 的 `connected`
  字段只表达"是否已加入房间"，网络断开根本不被感知。对局中刷新页面或断网，
  玩家就永久缺席，而对局仍会继续推进到他输。
- 本次目标：把「断线」变成可观测、可恢复、有期限的状态。
- **已确认决策（项目所有者 2026-10-02 全部选定）**：
  1. **宽限期 30 秒**（`kReconnectGraceMs`，与"等待玩家加入"的超时同量级）。
  2. **宽限期内对局暂停推进**：帧号与血量全部冻结，重连后接着打。
     这样"回来"是精确的，不需要任何补偿或重放。
  3. **到期处置**：只有一方断线 → 判断线方负（新增 `finish_reason = disconnect`，
     **产生胜负**并写入 `match_results`）；双方都断线 → 本局作废（`ABORTED`），
     不写结果、不造假胜负。
  4. 双方都断线时，判据是**最晚的断线时刻 + 宽限期**（等最后一位）。
     理由：Gateway 重启会让所有订阅同时断开，若按第一位到期就作废，
     任何一次 Gateway 重启都会立刻毁掉所有进行中的对局。
- 范围：`room.proto` 新增 `SetPlayerPresence` + `PlayerState.online` +
  `FINISH_REASON_DISCONNECT`；Room 的 presence 与宽限状态机；Gateway 在订阅
  建立/断开时上报并在推送里带上 `online`；SSE 推送判据从"帧号"改为"状态签名"；
  前端自动重连与状态提示；`scripts/verify-reconnect.sh`（新增）。
- 非范围：不做跨设备接管；不做排队中的断线保位；不做 `Last-Event-ID` 补帧（TASK-017）。
- 相关 ADR：无新增。presence 属于房间权威状态的一部分，不改变服务边界或数据所有权。
- **关键设计决定**：
  1. **`online` 与 `connected` 是两个字段**。`connected` 表示"在房间里"，
     已被 `rooms.pN_joined` 与快照校验（"PLAYING 的房间必须双方都已加入"）依赖，
     改它的语义会破坏 TASK-014 的恢复逻辑。断线的玩家**仍然在房间里**。
  2. **presence 与宽限计时不落库**（`rooms` 表不加列）。Room 重启后把双方当作在线、
     计时清零，客户端重连后会重新上报。代价是"Room 重启期间到期的对局不会被判负"，
     记入 `docs/01-architecture.md` 的已知限制。
  3. **上报失败不重试、不阻塞推送**：`online` 是尽力而为的事实同步，
     下一次连接状态变化会自然覆盖它。
  4. **推送判据必须是完整状态签名**（帧号 + 血量 + online + 阶段）。
     只比帧号会让客户端在整个宽限期里收不到任何事件——而"对方断线了"恰恰是
     那一刻最该知道的事。
- 失败场景：Room 不可用时上报失败 → 只记日志，客户端轮询兜底仍可用；
  对局已结束时上报 → 返回 `ALREADY_FINISHED` 而不是"成功"（避免掩盖时序错误）；
  客户端断网 → 由写失败感知，**同样**上报 offline（实测发现原实现漏了这条路径）。
- 验收命令：`bash scripts/verify-reconnect.sh`
- 测试要求：单元测试覆盖宽限期状态机（8 个用例）与 StreamHub 的 presence 上报
  （5 个用例）；端到端覆盖「断开 → 暂停 → 期内重连 → 血量/帧号一致 → 继续推进」、
  「超过宽限期判断线方负并落库」、「双方都断线作废且不写结果」。
- 回退方式：`git revert` 单个提交。
- 涉及目录：`api/proto/`、`src/room/`、`src/gateway/`、`tests/unit/`、`web/`、
  `scripts/`、`docs/`。
- **实施结果（2026-10-02）**——完整经过见 `docs/devlog.md` 的「TASK-016 实施记录」：
  - 验收结果：构建 **0 warning**；`ctest` **216/216**（203 + 新增 13）；
    `check-format.sh` 通过（74 文件）；
    `scripts/verify-reconnect.sh` **退出码 0**（真实等待两个 30 秒宽限期，
    含"双方都断线作废"一段）；`scripts/verify-web.sh` **退出码 0（43 项通过）**；
    前端 `vitest` **29 通过**（原 12 + 新增 17）、`vue-tsc` 与 `vite build` 均通过。
  - 过程中抓到并修掉的两个真问题：**客户端断网路径漏上报**（写失败清理不经过
    `Unsubscribe`，因此从不报 offline——由新单测锁定）、以及 Gateway 的
    proto→字符串/字段映射漏了 `disconnect` 与 `online`（表现为"断线判负"被显示成
    `none`，由验收脚本抓到）。
  - 已知限制：presence 不落库（Room 重启重置）；宽限期是固定常量，不按对局进度调整；
    前端未做真实断线端到端（无组件测试环境，未引入依赖）。


### TASK-017：推送连续性与恢复时间报告

- 状态：**已完成（已合并到 main：`a61495a7`，PR #15，2026-10-02）**。
  分支提交 `2d2edf29`，父提交 `335f149`（TASK-016）；合并是**真正的 merge commit**
  （两个父提交），不是 squash。
- 背景问题：Gateway 重启后所有 SSE 订阅丢失，客户端只能自己发现；且断线期间
  错过的 `room.state` 无法补发——SSE 规范自带的 `Last-Event-ID` 语义一直没用上
  （[ADR-0004](adr/0004-sse-instead-of-websocket.md) 已指出这一点）。
- 本次目标：重连后能补上缺失的事件；并给出 Phase 2 的恢复时间汇总。
- **一个必须先说清楚的前提（TASK-016 带来的变化）**：宽限期内对局**暂停推进**，
  因此**帧号在断线期间根本不前进**，客户端错过的帧天然极少。所以"补帧"的价值
  主要覆盖两类情况，而不是"断线 30 秒就要补 300 帧"：
  1. 客户端在断开**之前**就已经落后（网络慢、渲染卡顿、处理不过来）；
  2. 同一房间有多条订阅（多标签页）互相追赶。
  这也意味着**不需要**为了这个功能调大 `kMaxSnapshotHistory`（128 帧 ≈ 12.8 秒）；
  实测数字会写进恢复时间汇总，如果不成立再调。
- 范围：
  1. Room 新增 `GetRoomSnapshotsSince`：从内存环形缓冲返回帧号大于给定值的快照，
     供 Gateway 补发（受 `kMaxSnapshotHistory` 限制，超出即"窗口外"）。
  2. SSE 的 `room.state` / `room.finished` 事件补上 `id: <frame>`。
  3. Gateway 的 `GET /api/v1/stream` 解析 `Last-Event-ID` 请求头。
  4. 订阅建立时：窗口内 → 先按序补发缺失帧，再进入实时推送（**补发的事件也带 id**，
     客户端可判断是否连续）；窗口外（或 id 非法/缺失）→ 发一个 `stream.reset`
     事件并附当前完整状态，**明确告知"需要全量刷新"**，不假装补上了。
  5. 保留策略与恢复时间汇总：`kMaxSnapshotHistory` 的依据与上限写入文档；
     把 TASK-013 ~ TASK-016 的实测恢复时间与数据丢失边界汇总成一张表。
- 非范围：不做事件的持久化重放（领域事件属 [ADR-0003](adr/0003-scope-reduction.md)
  非目标）；不引入新组件；不为多标签页做专门优化。
- **本轮由我代为决定、需要你确认或否决的两点**：
  1. **补发窗口不做超出内存缓冲的承诺**。窗口内补发，窗口外明确 `stream.reset`。
     理由：不引入持久化、不对客户端撒谎（"保证补发最近 N 秒"需要真正的持久化
     才能兑现）；而暂停语义使实际缺口极小。
  2. **`id` 用帧号，不用自增序号**。帧号在房间内单调递增、与快照天然对齐，
     重启后仍可比大小；自增序号做不到这一点，且要额外维护"序号→状态"的映射。
- 失败场景：`Last-Event-ID` 缺失或非法 → 不补发，直接按当前状态推（等同今天的行为，
  不报错）；请求的 id 早于窗口 → `stream.reset` + 完整状态；补发过程中 Room 不可用 →
  不补发、不关闭连接（沿用 TASK-009 的取舍），下一次 Tick 照常推当前状态。
- 验收命令：`bash scripts/verify-reconnect.sh`（扩展「补发」一节）
- 测试要求：单元测试覆盖 `Last-Event-ID` 解析（缺失/非法/窗口内/窗口外）、
  补发顺序与去重、`stream.reset` 的触发条件；端到端覆盖「带一个落后的
  `Last-Event-ID` 重连 → 收到补发的中间帧 → 帧号连续且最终与实时状态一致」。
- 回退方式：`git revert` 单个提交。补发是订阅建立路径上的旁路：去掉后退回
  "重连只拿当前快照"。
- 涉及目录：`api/proto/`、`src/room/`、`src/gateway/`、`tests/unit/`、
  `scripts/`、`docs/`。
- 备注（收尾 Phase 2 的一部分）：本任务完成后，`docs/02-roadmap.md` 的 Phase 2
  状态改为已完成，并把"Phase 2 的退出标准逐条对照表"补进 `docs/devlog.md`。
- **实施结果（2026-10-02）**——完整经过见 `docs/devlog.md` 的「TASK-017 实施记录」：
  - 新增 `RoomService.GetRoomSnapshotsSince`（含 `SnapshotWindowStatus`）、
    `BattleRoom::SnapshotsAfter` / `MaxSnapshotFrame`、`RoomManager::GetSnapshotsSince`。
  - Gateway 侧：`RoomClient::GetSnapshotsSince`（接口 + brpc 实现）、
    SSE 事件的 `id:` 行、`Last-Event-ID` 解析、订阅建立时的补发与
    `stream.reset`；新增 `SubscribeReport` 让补发结果可被查询与断言。
  - 前端：`parseSseBlock` 解析 `id:`，`streamRoom` 发送 `Last-Event-ID`，
    重连时带上最后收到的帧号，并处理新事件 `stream.reset`。
  - **任务单里两处相互矛盾的地方已由项目所有者裁决（2026-10-02）**：失败场景写
    "`Last-Event-ID` 缺失或非法 → 不补发，直接按当前状态推（等同今天的行为，不报错）"，
    而范围第 4 条又写"（或 id 非法/缺失）→ 发一个 `stream.reset`"。
    裁决结果：**按失败场景那一条实现**——头缺失（第一次订阅）不补发、也不发
    `stream.reset`，由第一次 Tick 正常推当前状态；头存在但**非法**仍发
    `stream.reset`（`reason = id_malformed`），因为那是客户端的 bug，静默当成
    首次订阅会让它以为自己拿到了连续的事件。
    代码上就是 `StreamHub::BackfillSubscription` 里"头缺失"提前返回的那个分支，
    相关用例改为 `FreshSubscriptionWithoutLastEventIdGetsNormalStatePush`。
  - 验收状态（2026-10-02）：**已在 WSL 全绿通过，并合并到 `main`（PR #15，
    merge commit `a61495a7`）**。项目所有者执行的验收命令见
    `docs/devlog.md` 的「TASK-017 实施记录」。
  - 交付过程的教训（已写入 devlog，值得留在这里）：本任务最初在
    `D:\CLion\realtime-game-backend`（迁移前的备份副本，git 记录停在 TASK-007
    且无编译器）上实现，因此一度无法提交与推送；期间还误把"TASK-016 的全绿"
    当成本任务的验收结果。**根因是没有先核对"我改的地方是不是正式仓库"**——
    `docs/06-operations.md` 第 31-33 行已写明正式仓库只有
    `~/workspace/realtime-game-backend`。最终用"从远端克隆 + 归一化行尾 +
    `git diff` 生成 patch + WSL 侧 `git apply`"的方式搬运，
    比逐文件 rsync 更安全（不受 mtime 与 DrvFs 权限位影响）。


## Phase 3 任务拆分（2026-10-02 项目所有者确认，5 个任务）

Phase 2 已全部完成并合并（TASK-013 ~ TASK-017，`main` 含至 PR #15）。Phase 3 的
范围与退出标准见 `docs/02-roadmap.md` 第 6 节。

**进度（2026-10-03）**：TASK-018 / 019 / 020 / 021 均已完成并实测通过
（019 经 PR #18、020 经 PR #19 合并）。**TASK-022 待项目所有者确认后开工**
（它的两个前置问题见该任务单：压测客户端形态、1000 连接可行性）。

拆分原则与前两个阶段一致：**每个任务都要有可独立运行的验收命令，且不引入下一个
任务的组件。** 与前两阶段的差别是：本阶段的任务**交付物是"可验证的事实"而不是
功能**——判定标准是"能不能定位问题、能不能复现数字"，而不是"接口能不能调通"。

| 顺序 | 编号 | 任务 | 依赖 |
|---|---|---|---|
| 1 | TASK-018 | 结构化日志与请求 ID 贯通 | 无 |
| 2 | TASK-019 | 指标暴露（`/metrics`） | 无（可与 018 并行） |
| 3 | TASK-020 | Prometheus + Grafana 接入 | TASK-019 |
| 4 | TASK-021 | 关键路径 trace id 贯通 | TASK-018 |
| 5 | TASK-022 | 容量基线与首份报告 | TASK-020 |

**已确认的三项决策（2026-10-02，项目所有者）**：

1. **监控组件单独一份 `deploy/compose/docker-compose.observability.yml`**，
   不放进日常的 `docker-compose.yml`——否则每次开发与每次 `verify-all.sh`
   都要多起两个容器。日常启动流程（`dev-up.sh`）保持只起 Redis + MySQL。
2. **链路追踪先做 trace id 贯通，不引入 OTLP collector**：五条关键路径共用同一个
   id 写进结构化日志即可满足"任一错误可以定位到服务、请求、会话或房间"。
   真正的 span 导出留到 Phase 5 前再评估（需要额外的 C++ 依赖与 collector）。
3. **任务单一次写完，逐个开工**：TASK-018 确认后即可开始；
   TASK-019 及之后在开工前由项目所有者逐个确认（避免把"计划"写成"承诺"）。

### TASK-018：结构化日志与请求 ID 贯通

- 状态：**已完成并实测通过**（2026-10-03）。
  完整经过与实测数字见 `docs/devlog.md` 的「TASK-018 实施记录」。
- 已完成：
  - `include/common/logging.hpp` + `src/common/logging.cpp`（单行 `key=value`、
    恒定字段、值转义、服务名）。
  - `request_id` 经 proto 既有字段贯通 Gateway → Match → Room；
    `RoomClient` 六个方法增加 `request_id` 形参，替换掉实现里自拼的伪 id。
  - **30 处 `std::fprintf(stderr, ...)` 全部改造完毕**（`logging.hpp` 里仅剩
    注释中的历史引用）。已接入的关键路径包括：`match_enqueued`、
    `match_enqueue_ok` / `match_enqueue_failed` / `subscribe_ready` /
    `subscribe_rejected`、`room_created` / `room_joined` / `presence_reported`、
    `room_restored` / `room_snapshot_rejected` / `result_persist_failed`、
    `queue_snapshot_*`、`create_room_call_failed` / `create_room_rejected`、
    `mysql_*`、`service_start_failed` 等。
  - `scripts/verify-observability.sh --logs`（新增）与
    `tests/unit/common/logging_test.cpp`（16 个用例）。
  - **既有验收脚本同步更新**：`verify-persistence.sh` 里 5 处断言由"匹配中文日志
    文本"改为"匹配结构化事件名"（`event=room_restored` 等）。这一点是必须的：
    改日志格式等于改服务输出，脚本不同步就会出现假失败。
  - 实测：构建 0 error / 0 warning；`ctest` 253/253；格式检查 77 文件通过；
    `verify-observability.sh --logs` 退出码 0（**三层贯通**）；
    `verify-all.sh` **7/7 通过**（204 秒）。
- 剩余（可选的后续改进，不属于本任务验收范围）：
  - 登录、结果查询等路径尚未补结构化日志（目前只覆盖关键路径）。
  - `verify-all.sh` 尚未把 `verify-observability.sh` 纳入常规门禁。
- 依赖：无
- 背景问题：Phase 2 排查问题时最耗时的一环是**把一次请求在三份 stderr 日志里
  对上**。现状是 30 处 `std::fprintf(stderr, "[service] ...")`（`[gateway]` /
  `[room]` / `[match]` / `[mysql]`），人眼可读但机器不可解析，且只有部分行带
  `match_id` / `room_id`。Phase 3 的退出标准第一条是"任一错误可以定位到服务、
  请求、会话或房间"——现在做不到，因为**没有任何一个 id 是三份日志共有的**。
- 本次目标：让每一条日志自带服务名与关联 id，并且同一次请求在三个服务里的
  日志可以用同一个 id 串起来。
- 范围：
  - `include/common/logging.hpp` + `src/common/logging.cpp`：一个极薄的
    结构化日志函数（`key=value` 文本、单行、带服务名与时间戳）。
    **不引入第三方日志库**（spdlog 等属于新增依赖，且 brpc/glog 已经在产物里，
    再多一套只会增加迁移面）。
  - 三个服务入口各初始化一次服务名（`service=gateway|match|room`）。
  - 把既有 30 处 `fprintf(stderr, ...)` 逐步改为结构化输出，**不改变日志内容
    与语义**，只改变形态（这是可回归的：同一条失败路径仍然看得见同样的字段）。
  - `request_id` 贯通：Gateway 的 HTTP 入口已有 `request_id`；Gateway→Match、
    Gateway→Room、Match→Room 的 brpc 调用把它带过去（proto 里已有
    `request_id` 字段，无需改契约）；Room 的 ticker 线程没有请求上下文，
    用 `room_id` 与 `match_id` 关联即可。
  - `scripts/verify-observability.sh`（新增，本任务只做日志一节）：
    制造一条已知错误（例如带无效 room_id 订阅），断言该错误在所有相关服务的
    日志里都能按同一个 request_id 找到。
- 非范围：
  - 不改任何接口契约、不加 proto 字段（`request_id` 已存在）。
  - 不做日志聚合/采集（Loki/ELK 均不在 Phase 3 范围）。
  - 不动 brpc 自身的日志配置（`-logtostderr` 等启动参数保持现状）。
  - 不做指标（TASK-019）与 trace（TASK-021）。
- 相关 ADR：无新增。日志格式属实现细节，不改服务边界与数据所有权。
- 涉及目录：`include/common/`、`src/common/`、`src/gateway/`、`src/match/`、
  `src/room/`、`tests/unit/`、`scripts/`、`CMakeLists.txt`、`docs/`。
- 接口变化：无（日志不是契约）。数据变化：无。
- 失败场景：
  - 日志函数本身失败（例如写 stderr 出错）→ 不抛异常、不影响业务路径，
    与今天的 `fprintf` 行为一致。
  - `request_id` 为空（老客户端不带）→ 用 `-` 占位，**不编造一个 id**。
  - 单行过长 → 不截断业务字段（宁可行长，也不丢排障信息）。
- 验收命令（在 WSL 中执行）：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug --output-on-failure
  bash scripts/verify-observability.sh --logs
  bash scripts/verify-all.sh            # 回归：既有 7 个脚本不受影响
  ```
- 测试要求：单元测试覆盖结构化输出的转义与空字段占位（含值里带空格、
  引号、换行的情况）；端到端覆盖"一条错误可以按 request_id 串起三个服务"。
- 回退方式：`git revert` 本任务提交。日志改动不影响任何行为路径，
  去掉后退回分散的 `fprintf`。
- 负责人：执行者（写入权）— 本轮由当前会话代理承担，项目所有者审阅与验收。

### TASK-019：指标暴露（`/metrics`）

- 状态：**已完成**（2026-10-03）。模块、三个服务的端点与全部指标（含 brpc 调用数与
  快照写入成功/失败数）、`verify-observability.sh --metrics` 验收、`verify-all.sh`
  门禁均已落地并实测通过。完整经过与实测数字见 `docs/devlog.md` 的
  「TASK-019 指标暴露」，其中包含一次**验收门禁被改坏**的事故记录。
  （2026-10-03 状态更正：本条此前长期停留在"待确认"，与 git 记录不符——
  TASK-019 已由 PR #18 合并、TASK-020 已由 PR #19 合并。）
- 依赖：无（可与 TASK-018 并行）
- 背景问题：`docs/04-quality-and-observability.md` 第 4 节列了 8 类指标的清单，
  但至今**一个都没有暴露**。`StreamHub` 已经有可断言的计数器
  （`BackfilledFrameCount` / `ResetEventCount`），却没有出口；其余指标
  （连接数、QPS、延迟、错误率、房间数）连计数都没有。
- 本次目标：三个服务各暴露一个 Prometheus 文本格式的 `/metrics` 端点，
  指标项与第 4 节清单对齐。
- 范围：
  - `include/common/metrics.hpp` + `src/common/metrics.cpp`：计数器/直方图的
    最小实现 + 文本暴露（**不引入 prometheus-cpp**：它会带来新依赖，
    而文本格式本身就是协议，几十行可覆盖本项目需要的指标类型）。
  - Gateway：HTTP 请求数（按路径与状态码）、SSE 连接数、推送补发帧数与
    `stream.reset` 次数（按原因）、brpc 调用数与失败数。
  - Match：队列长度、入队/配对/取消计数、快照写入成功/失败数。
  - Room：房间数（按阶段）、帧推进计数、断线判负计数、结果落库重试计数、
    快照写入成功/失败数。
  - `brpc` 的 `/metrics` 与内置 `/status` 的关系写清楚（不要两个入口语义重叠）。
  - `scripts/verify-observability.sh --metrics`：断言三个端点可访问、
    指标可解析、且**关键计数确实随一次真实对局增长**（不能只断言"端点 200"）。
- 非范围：
  - 不引入 Prometheus 服务端与 Grafana（TASK-020）。
  - 不做标签基数控制之外的优化（Phase 3 只记录瓶颈，不立即优化）。
  - 不暴露任何敏感信息（Token、密码不得成为标签）。
- 相关 ADR：无新增（ADR-0001 已包含 Prometheus 作为技术方向）。
- 涉及目录：`include/common/`、`src/common/`、三个服务目录、`tests/unit/`、
  `scripts/`、`CMakeLists.txt`、`docs/`。
- 接口变化：新增三个 `/metrics` HTTP 端点（只读、无鉴权，仅监听本机）。
- 数据变化：无。
- 失败场景：`/metrics` 不可用时不影响业务端点；指标未注册时输出空集而不是报错；
  高基数标签（例如把 `room_id` 当标签）必须避免。
- 验收命令：`bash scripts/verify-observability.sh --metrics` + 既有全套回归。
- 测试要求：单元测试覆盖文本格式与计数语义；端到端断言计数与真实事件一一对应。
- 回退方式：`git revert`；指标是旁路，去掉后退回无指标状态。
- 已完成（2026-10-03）：
  - `include/common/metrics.*`（counter/gauge/histogram + 文本导出）、
    `api/proto/metrics.proto` + `MetricsService`（三个服务各自端口注册）；
  - Gateway：`rgbt_http_requests_total{path,status}`、`rgbt_sse_connections`、
    `rgbt_push_backfilled_frames`、`rgbt_push_reset_total{reason}`；
  - Match：`rgbt_match_queue_length`、`rgbt_match_events_total{event}`；
  - Room：`rgbt_rooms{phase}`（6 个阶段）、`rgbt_room_frames_advanced_total`；
  - `scripts/verify-observability.sh --metrics` 与 `verify-all.sh` 门禁；
  - 实测：构建 0 error/0 warning；ctest 269/269；格式 82 文件通过；
    `--logs` 与 `--metrics` 均退出码 0；`verify-all.sh` **8/8 通过**。
- 全部指标已落地（2026-10-03 收尾）：
  - **brpc 调用数与失败数**：`rgbt_rpc_calls_total{target,outcome}`。记账点选在
    `brpc_room_client.cpp` / `brpc_match_client.cpp` 里，包装**全部 23 个**
    `return XxxCallStatus::...`——这是真正的 RPC 层，每个方法都以一个 CallStatus
    收敛，包装 return 能零遗漏地覆盖全部调用。没有 request_id 标签（高基数）。
  - **快照写入成功/失败数**：`rgbt_queue_snapshot_total{outcome}`（Match）与
    `rgbt_snapshot_write_total{outcome}`（Room）。Room 原本就在计数，本次只是暴露。

### TASK-020：Prometheus + Grafana 接入

- 状态：**已完成**（2026-10-03）。compose、抓取配置、数据源与 8 块面板、
  启停脚本、`verify-observability.sh --scrape`（20 条断言）均已落地并实测通过。
  完整经过（含 5 个踩坑）与实测数字见 `docs/devlog.md` 的
  「TASK-020 Prometheus + Grafana 接入」。
  已知限制：延迟面板暂无数据（HTTP 耗时直方图尚未接入，见该节说明）。
- 依赖：TASK-019
- 背景问题：有指标端点但没有人采集、没有面板，等于"数据可查"这条退出标准
  仍然没有兑现。
- 本次目标：一条命令拉起采集与面板，并且**面板上的数字能与验收脚本对得上**。
- 范围：`deploy/compose/docker-compose.observability.yml`（独立文件）、
  Prometheus 抓取配置（三个服务的 `/metrics`）、Grafana 数据源与首块面板
  （连接数、QPS、延迟、错误率、房间数）、`scripts/observability-up.sh` /
  `-down.sh`、`scripts/verify-observability.sh --scrape`。
- 非范围：不做告警规则与 Alertmanager（没有值班对象，属过度设计）；
  不把监控栈接进 `dev-up.sh`；不做长期存储与远程写。
- 失败场景（**本任务最需要先验证的**）：镜像拉不动是已知风险（文档记录过
  GitHub 被限速、需走加速通道）。因此**第一步是限时拉取实测**，
  而不是先写配置——这与 Backlog 里"应用服务容器化"那条的教训一致。
  端口冲突、宿主机内存不足（Docker Desktop 仅 11 GiB）同样要预检。
- 验收命令：`bash scripts/observability-up.sh && bash scripts/verify-observability.sh --scrape`。
- 回退方式：`git revert` + `observability-down.sh`；不影响业务 compose。

### TASK-021：关键路径 trace id 贯通

- 状态：**已实现并实测通过**（2026-10-03）。
  完整经过（含修掉的真实缺陷与验收脚本自己坏掉的三次）见 `docs/devlog.md` 的
  「TASK-021 实施记录」。
- 依赖：TASK-018
- 背景问题：日志有 id 了，但"一次登录/匹配/进房/重连/结算"跨三个服务的完整
  路径仍然要人脑拼。
- 本次目标：五条关键路径的每一个环节都带上同一个 trace id，并且能在日志里
  按它取出一条完整的调用序。
- 范围：Gateway 在入口生成 trace id（复用/兼容 `request_id`）、经 brpc 传递
  （**不新增 proto 字段**：用已有的 `request_id` 字段承载，避免契约变更）、
  Room 的内部线程用 room_id 关联；`scripts/verify-observability.sh --trace`
  按 trace id 断言"五个服务内环节齐全且顺序单调"。
- 非范围：不引入 OpenTelemetry SDK 与 OTLP collector（已确认）；
  不做采样策略与 span 可视化；不改 proto。
- 失败场景：id 在跨进程边界丢失（实测踩过同类问题：brpc 不会把 HTTP 头映射进
  protobuf 字段，必须显式读取）→ 由验收脚本逐跳断言覆盖。
- 验收命令：`bash scripts/verify-trace.sh`（见下方"实施结果"关于脚本归属的说明）。
- 回退方式：`git revert`。
- **实施结果（2026-10-03）**：
  - **修掉一个真实缺陷**：`BrpcRoomAllocator` 此前把 `match_id` 当成 `request_id`
    发给 Room，于是 Room 的 `room_created` 记下一个在 Gateway 与 Match 日志里
    都不存在的 id —— "匹配 → 建房间"这条跨服务链路**本来就是断的**。
    `RoomAllocator::Allocate` 增加 `request_id` 形参，由 `MatchQueue` 填入
    **该组队首玩家**的 request_id（配对是异步的，取队首才确定、可复现；
    幂等键仍是 `match_id`，两者不再混用）。
  - **Gateway 的收敛点**：`ApplyHttpStatusAndRecord` 增加 `request_id` 形参，
    在所有 HTTP 响应（含成功路径）处输出一条 `request_done`，字段为
    `op`（由 restful 路径推导，11 条映射，找不到记 `unknown` 而不编造）、
    `status`、`trace`；级别随状态码（5xx→error，4xx→warn）。51 处调用点补齐。
    此前 15 个接口里只有 2 个有结构化日志。
  - **Room 补齐结算路径**：`GetMatchResult` 新增 `match_result_queried`
    （ok / pending / store_unavailable / not_found 四条出口）。
  - `scripts/verify-trace.sh`（新增）：五条关键路径的 trace 贯通验收，
    含"未带 request_id 时不编造 trace"与"结构化行未被字段值断行"两条不变量。
    **取代**任务单里"给 `verify-observability.sh` 加 `--trace`"的写法：
    那边已有三个互相独立的模式，再加一个会放大"条件块没闭合就吞掉整段断言"的
    历史风险；且本脚本需要真的打完一整局（实测 17~20 秒），混进去会让只验日志的
    人白等。
  - 新增 6 个单元测试（`ctest` 269 → 275），其中
    `EveryMappedPathEmitsRequestDoneWithItsOperation` **逐条覆盖全部 11 个映射
    路径**——本任务修的就是覆盖面问题，抽查证明不了覆盖面。
    用例通过重定向并捕获进程级 stderr 来断言真实输出。
  - 实测：构建 0 error / 0 warning；`ctest` **275/275**；
    `check-format.sh` 通过（82 个文件）；`verify-trace.sh` **连跑 2 次均退出码 0**；
    `verify-all.sh` **9/9 通过（含新脚本，实测 236 秒）**。

### TASK-022：容量基线与首份报告

- 状态：**已完成**（2026-10-04）。6 档压测（1/10/50/100/500/1000 连接）实测通过，
  首份容量报告见 `docs/benchmarks/README.md`，原始数据见
  `docs/benchmarks/raw/20261004-140614/`。
- 依赖：TASK-020
- 背景问题：`CLAUDE.md` 第 6 条要求"引入复杂度必须有容量证据"，而 Phase 2 的
  三条已知限制（ticker 线程写快照、队列快照在入队请求路径、presence 不落库）
  都因为"没有证据"被推迟。没有容量基线，Phase 4 的故障注入也无从判断
  "这个延迟算不算异常"。
- 本次目标：建立 1 / 10 / 50 / 100 / 500 / 1000 连接档位的可复现压测，
  产出首份容量报告与至少一个明确瓶颈结论。
- 范围：`scripts/bench.sh`（起服务、按档位施加负载、采集指标、落原始数据）、
  `docs/benchmarks/` 首份报告（环境、版本、命令、原始结果、瓶颈结论）、
  与 `docs/04-quality-and-observability.md` 的 SLO 一节对齐。
- 非范围：不为了数字好看做优化（Phase 3 的纪律是"只记录瓶颈"）；
  不做多实例与集群压测（ADR-0003 非目标）。
- **实施结果（2026-10-04）**——完整经过见 `docs/devlog.md` 的「TASK-022 实施记录」：
  - **压测客户端形态（已确认）**：`wrk`/`hey`/`ab`/`vegeta`/`k6`/`locust` **一个都没有**，
    `sudo -n` 不可用（装不了）。选择**自研 `bench/loadgen.py`（Python asyncio）**，
    理由不是"装不上"，而是现成 HTTP 工具只能打静态请求，恰好绕开本系统真正的
    瓶颈面（SSE 扇出、房间推进、每秒快照写入、匹配队列写入）。
    配套 `bench/histogram_quantiles.py`（按 `path` 分组算分位数）。
  - **1000 连接可行性（已实测）**：可行。`file_max`/`nr_open` 实际无上限，
    瓶颈是 shell `ulimit -n`（默认 10240，脚本显式提高）；1000 档位实测
    1000/1000 登录与配对成功、500 个并发房间、0 异常。
  - **同时补齐了 TASK-020 遗留的延迟直方图**（项目所有者确认并进本轮）：
    `MetricsRegistry::Observe` 新增自定义桶边界与标签重载；Gateway 11 个处理函数
    入口计时；桶边界取 SLO 线（50/100/250 ms），因此 P95 < 100 ms 这条判据可读。
  - **一个必须记录的方法学修正**：第一版压测复用种子账号（内置启用身份只有 3 个），
    1000 个机器人只产生 3 个 `player_id`，于是绝大多数入队被判 `kAlreadyQueued`、
    而同一玩家的重复进房是**幂等成功**——最终得到"667 次 join 挤进 1 个房间"
    这种自相矛盾的数字。修正为每个机器人独立身份后，同一档位从
    "配对 667 / 建房间 1 / 攻击 694" 变为 **"配对 1000 / 建房间 2979 / 攻击 25218"**。
    为此新增 Gateway 开关 `-enable_bench_accounts`（**默认关闭**，只识认严格的
    `bench-<5 位数字>`），并有 3 个单元测试锁住默认值与严格匹配规则。
  - **瓶颈结论**：Match 的队列快照写入在请求路径上，并被全局
    `snapshot_order_mutex_` 串行化（`MatchQueue::PersistSnapshot`）。
    实测入队端点 P95 从 100 档位的 88 ms 涨到 500 档位的 **842 ms**、1000 档位的
    **956 ms**（SLO 是 100 ms）；轮询端点从 5.8 ms 涨到 859 ms（次生效应：
    清算也走快照写入）。这是 Backlog 里「把匹配队列的快照写入移出请求路径」
    那条已知限制的**量化确认**。
  - **阶段决定**：遵循 Phase 3 的"只记录瓶颈、不立即优化"纪律，
    **本阶段不优化**；把该 Backlog 项升级为有证据的任务并补上量化目标
    （500 档位入队 P95 < 100 ms）作为 Phase 4 的输入。
  - 实测：构建 0 error / 0 warning；`ctest` **284/284**；
    `check-format.sh` 通过（82 个文件）；`bench.sh` 6 档全部执行完毕
    （唯一 "未通过" 是 1 连接档的 `stream_opened=0`——两人一局，单人永远配不上，
    属**语义必然**而非缺陷，报告第 4.1 节已说明）。
- 验收命令：`bash scripts/bench.sh --level 100`（单档位可独立运行）+ 报告。
- 回退方式：`git revert`；报告是文档，压测脚本与合成账号开关都是旁路
  （开关默认关闭，去掉它不影响任何既有行为）。


> **编号说明**：本任务在编号上排在 Phase 1 计划之后，但**实际执行时间早于
> TASK-008 起的所有待办任务**。原因是它不实现任何功能，只是把已经确认的范围
> 裁剪写进文档；若强行插入编号，会导致 `api/proto/` 与 `src/` 中大量指向
> TASK-008/009 的注释被无意义地改写，扩大文档提交的 Diff。

- 状态：已完成（文档改动已合并到 main：`097d8f4`）
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

## Phase 4 任务拆分（2026-10-04，**等待项目所有者逐个确认**）

Phase 3 已全部完成并合并（TASK-018 ~ TASK-022，`main` 含至 PR #21）。Phase 4 的
范围与退出标准见 `docs/02-roadmap.md` 第 7 节。

拆分原则与前三个阶段一致：**每个任务都要有可独立运行的验收命令，且不引入下一个
任务的组件。** 本阶段与前三个阶段的差别是：**交付物是"故障下的实测事实"，而不是
功能**——判定标准是"每个场景都有实测的检测时间、恢复时间和数据丢失边界"，
而不是"接口能不能调通"。

| 顺序 | 编号 | 任务 | 依赖 |
|---|---|---|---|
| 1 | TASK-023 | 统一故障注入器与依赖不可用的恢复时间测量 | 无 |
| 2 | TASK-024 | 进程崩溃与恢复边界（`kill -9`） | TASK-023 |
| 3 | TASK-025 | 连接风暴与限流边界 | TASK-023 |
| 4 | TASK-026 | 优雅退出与排空 | TASK-023 |
| 5 | TASK-027 | 长稳运行：内存与 FD 稳定性 | TASK-024 ~ TASK-026 |

**三项已裁决的决策（2026-10-04，项目所有者）**：

1. **应用服务容器化不作为 Phase 4 的前置条件**，任务单一律按"本机进程 + PID"写。
   依据：TASK-022 的 `scripts/bench.sh` 已实测证明本机进程就能完整地起停、注入、
   采集与清理（`stop_services` 发 SIGTERM 并等退出），而 `kill -9`、停 Redis/MySQL、
   连接风暴都不依赖容器。容器化的真实价值在"可复现的现场演示"，归 **Phase 5**。
   这一步同时更正了 Backlog 里"容器化是故障注入前置条件"的旧判断。
2. **原 TASK-028（把匹配队列快照写入移出请求路径）不纳入 Phase 4**，退回 Backlog
   并保留 TASK-022 给的量化目标（500 档位入队 P95 842 ms → < 100 ms）。
   理由：本阶段先只交付"故障下的实测事实"；性能改动会改产品代码，与其余五个
   只加测试工具的任务性质不同，混在一个阶段里会让"阶段是否完成"的判据变模糊。
3. **本阶段分支为 `feat/phase-4`**（一阶段一分支 + 一任务一提交 + 每任务合并回
   `main`，**不 squash**），见 `docs/03-development-workflow.md` 第 9 节。
   任务单本轮的文档改动是该分支的起始提交。

### TASK-023：统一故障注入器与依赖不可用的恢复时间测量

- 状态：**已完成并合并**（2026-10-04 实现并实测通过；提交 `ed92604`，经 PR #23
  合并到 `main`，merge commit `c6abac2`）
- 依赖：无
- 背景问题：Phase 1 ~ 3 的故障场景散落在各业务验收脚本里，每个脚本自己实现
  `docker stop/start` 与等待逻辑。结果是：① 同一类故障被重复测，等待时长各写一遍；
  ② **没有任何一处记录"恢复时间"**，而这是 Phase 4 退出标准的核心要求；
  ③ 现有断言大多只检查"返回了正确的错误码"，不检查"从注入到出现错误码用了多久"。
- 本次目标：建立统一的故障注入与测量夹具，并把**依赖不可用**这一类场景补全。
- 范围：
  - `chaos/lib.sh`：注入原语（停/启 Redis、停/启 MySQL、按**通道**注入）+ 测量
    （记录注入时刻、首次失败时刻、恢复时刻）+ 就绪等待。为什么是脚本而不是
    `chaos/` 下的 C++ 工具：注入动作全是进程与容器操作，C++ 只会多一层编译。
  - `chaos/verify-dependency-down.sh`：覆盖四个**通道**，每个都输出实测的
    `检测时间` 与 `恢复时间`：
    1. **Redis 停机** → 登录/鉴权 503 `session_store_unavailable`（会话不可用）；
    2. **Redis 停机** → 匹配仍可用（队列快照可丢弃，TASK-015 决策 B）；
    3. **MySQL 停机** → 登录 503 `player_store_unavailable`（玩家档案不可用）；
    4. **MySQL 停机** → 进行中的对局**正常打完**，结果停在 `FINISHING` 并按 1 秒
       重试；查询返回 503 `result_pending` 而**不是** 404、也不返回内存里的胜负；
       MySQL 恢复后**无需重启服务**结果自动落库。
  - 结果汇总表追加到 `docs/devlog.md` 的「恢复时间汇总」一节（Phase 2 已有该节，
    本任务扩展它而不是另开一处）。
- 非范围：不做进程崩溃（TASK-024）；不做连接风暴（TASK-025）；不引入新组件；
  不改任何服务代码。
- **一个必须显式处理的坑**（TASK-015 踩过）：Gateway 的会话也在**同一个 Redis** 上，
  直接 `docker stop rgbt-redis` 之后所有 HTTP 请求会先因鉴权失败返回 503，
  根本走不到被测的那条通道。因此"按通道注入"是必需的：注入方式要能只让**目标
  通道**失效（例如把某个服务的快照存储指向一个不可用端口），而不是把共享依赖整体
  停掉。验收脚本必须能证明"被注入的通道失败了，而其它通道没有失败"。
- 失败场景：注入超时未生效（应报错而不是继续测）；恢复后服务未自愈（必须报失败）；
  测到的"检测时间"为 0（说明断言在注入之前就已经满足，属测试错误）。
- 验收命令：`bash chaos/verify-dependency-down.sh`（四个通道全部实测并输出时间）。
- 测试要求：每个通道都要有"注入前正常 → 注入后失败且错误码正确 → 恢复后自愈"三段；
  并断言**其它通道未受影响**（这是"按通道注入"是否成立的自证）。
- 回退方式：`git revert`；全是新增的测试工具，删掉不影响任何构建与运行。
- 涉及目录：`chaos/`、`scripts/`、`docs/`。

- 实施结果（2026-10-04）：
  - 交付：`chaos/relay.py`（透明 TCP 中继，通道级注入的前提）、`chaos/lib.sh`
    （注入原语 + 检测/恢复时间测量 + 就绪等待 + 断言汇总）、
    `chaos/verify-dependency-down.sh`（四个通道的三段式验收）。
  - 验收命令：`bash chaos/verify-dependency-down.sh`，**实测退出码 0，0 项失败**。
  - 实测结果（本机 16 核 / 11 GiB，brpc-debug）：
    | 通道 | 检测时间 | 恢复时间 |
    |---|---|---|
    | 1 Redis / Gateway 会话存储 | 12 ms | 14 ms |
    | 2 Redis / Match 队列快照 | 142 ms（首次快照写入失败） | 118 ms |
    | 3 MySQL / Gateway 玩家档案 | 9 ms | 21 ms |
    | 4 MySQL / Room 快照与结果 | 帧照常前进（470 ms 推进 5 帧） | 723 ms（结果自动落库） |
  - 关键设计：**按通道注入必须做在网络层**。三个服务共用同一个 Redis 与 MySQL，
    直接 `docker stop` 会同时打掉会话存储与队列快照（TASK-015 已踩过），因此让被测
    服务连中继、杀掉中继即只切断那一个依赖。隔离在脚本里是**自证**的：每个通道注入后
    都断言其它通道仍然健康。
  - 实测发现（已写入 `docs/devlog.md`，**未改产品代码**）：
    ① Gateway 的依赖不可用路径（`FillError`）不写日志，只能从 `/metrics` 的 503 计数
    观测；② MySQL 不可用让同一局对局从基线 16.7 秒变成 755 秒（快照写失败各等一次
    3 秒连接超时）；③ 本机 `date +%s%3N` 输出"秒+字面量3+纳秒"，与
    `scripts/bench.sh` 的墙上时长不同源（首尾相减内部一致，既有数字不受影响）。
  - 单元测试/构建：无新增测试（本任务只加测试工具），`ctest`/构建未受影响；
    `src/`、`include/`、`api/`、`tests/` 与 `CMakeLists.txt` **无任何改动**。
  - 未做：进程崩溃（TASK-024）、连接风暴（TASK-025）、优雅退出排空（TASK-026）；
    `verify-all.sh` 未接入本脚本（任务单未要求，且它会停依赖、耗时以分钟计）。

### TASK-024：进程崩溃与恢复边界（`kill -9`）

- 状态：**待确认**
- 依赖：TASK-023
- 背景问题：TASK-014 已经定义了 Room 的恢复边界（"最多回退一个快照间隔"），
  但那是**在单测与一个脚本里验证的**，且只覆盖 Room。Phase 4 的退出标准要求每个
  场景都有**实测的数据丢失边界**；Gateway 与 Match 崩溃后的行为也没有被系统测量过。
- 本次目标：三个服务分别 `kill -9`，测量恢复时间与**数据丢失边界**；把已知无法精确
  恢复的部分明确写出来，而不是隐藏。
- 范围：
  - `chaos/verify-process-crash.sh`：对 Gateway / Match / Room 各做一次 `kill -9`
    并测量：
    - **Room**：恢复后仍在进行中的对局是否还在、回退了多少帧（对照 TASK-014 的
      "上界 10 帧"）、`FINISHING` 房间重启后是否被重新纳入落库重试（TASK-008 的
      已知限制：结果只在内存→重启会丢，需实测确认现在**不会**丢）；
    - **Match**：排队中的玩家是否仍在队列、重启期间流逝的时间是否被算进超时
      （TASK-015 的语义）；
    - **Gateway**：SSE 订阅全部丢失（Phase 2 已记的已知限制）→ 客户端需要重连；
      会话本身在 Redis，不受影响。测量"从崩溃到客户端可重新登录/订阅成功"的时间。
  - 每个场景记录：崩溃前的状态、崩溃时刻、恢复时刻、**丢失了什么**（逐项列出）。
- 非范围：不做多进程同时崩溃（那需要多副本，ADR-0003 非目标）；不做 K8s 探针式重启。
- 失败场景：崩溃后进程未自动重启（`chaos` 脚本负责重启并断言就绪）；恢复出的状态
  与快照不一致（报失败并打印两份状态）；结果落库出现重复行（幂等被破坏，报失败）。
- 验收命令：`bash chaos/verify-process-crash.sh`。
- 测试要求：三个服务各自的恢复时间与丢失边界都要有数字；重复行检查要对
  `match_results` 做一次实际查询。
- 回退方式：`git revert`；只新增测试脚本。
- 涉及目录：`chaos/`、`docs/`。

### TASK-025：连接风暴与限流边界

- 状态：**待确认**
- 依赖：TASK-023
- 背景问题：Phase 4 的退出标准要求验证"瞬时大量连接建立时的限流与拒绝行为，
  **已有连接不受影响**"。当前系统**没有针对连接数的显式限流**：Gateway 依赖
  `brpc` 的线程与 fd 上限，而 `docs/04-quality-and-observability.md` 也没有连接数
  上限的 SLO。因此本任务很可能得出"当前没有限流"这个结论——那本身就是一个必须
  记录的实测事实，而不是缺陷。
- 本次目标：测量"瞬时并发连接建立"时的行为，明确**有**还是**没有**限流，
  以及已建立的连接是否受影响。
- 范围：
  - 复用 `bench/loadgen.py` 作负载源（TASK-022 已交付），`chaos/` 下新增
    `verify-connection-storm.sh`：先建立基线连接（例如 50 条）并保持，再在短时间内
    猛增到目标值（例如 500/1000），观察：
    - 新连接的失败率与失败方式（拒绝 / 超时 / 502 / 503）；
    - **基线连接在风暴期间是否仍然收到推送**（这是"已有连接不受影响"的判据）；
    - Gateway 的 fd 用量、线程数、RSS 的变化（`/proc/<pid>/status` + `/metrics`）。
  - 明确 fd 上限的影响：`scripts/bench.sh` 已把 `ulimit -n` 提到 8192；本任务要
    **显式测出"不提高 ulimit 时先从哪里失败"**，因为那才是默认部署下的真实边界。
- 非范围：不实现新的限流中间件（若结论是"需要限流"，只写结论与建议，实现另立任务）。
- 失败场景：风暴导致进程退出（报失败并保留日志与 fd 快照）；基线连接被中断
  （这正是本任务要证明的"不受影响"，若被中断则是真实缺陷）。
- 验收命令：`bash chaos/verify-connection-storm.sh`。
- 测试要求：必须同时给出"风暴期间的失败方式"与"基线连接是否存活"两个方向的证据。
- 回退方式：`git revert`；只新增测试脚本。
- 涉及目录：`chaos/`、`bench/`、`docs/`。

### TASK-026：优雅退出与排空

- 状态：**待确认**
- 依赖：TASK-023
- 背景问题：当前三个服务收到 SIGTERM 后会**立即停止**：`room_main` 停 ticker 线程、
  `server.Stop(0)`（0 = 不等待）。既有验收只断言"退出码 0"。而 Phase 4 的退出标准
  要求的是**排空语义**：停止接收新房间、**等待活跃对局结束**、超时后补偿。
  当前**没有**这个语义，因此本任务同时是"测量"和"实现最小排空"。
- 本次目标：定义并实现最小排空语义，并实测"排空了多久、有没有对局被截断"。
- 范围：
  - 三个服务在 SIGTERM 后的行为定义：
    - **Room**：停止接受新房间（`CreateRoom` 返回 503 `shutting_down`），等待活跃
      对局结束，最多等 `kDrainTimeoutMs`（默认 30 秒，可配）；超时后对未结束的房间
      落一次终态快照并标 `ABORTED`（**不伪造胜负**）。
    - **Match**：停止接受新入队（返回 503），已配对的结果仍可领取。
    - **Gateway**：停止接受新连接，但让已建立的 SSE 连接**收到 `room.finished` 或
      显式关闭事件**后再关闭（而不是直接断开）。
  - `chaos/verify-drain.sh`：测量"活跃对局期间 SIGTERM → 进程退出"的耗时、
    被等待的对局是否打完并落库、超时场景下是否留下 `ABORTED` 而不是假胜负。
- 非范围：不做滚动升级与多副本排空（ADR-0003 非目标）；不做连接的优雅迁移。
- 失败场景：排空超时后仍有对局处于 `PLAYING`（报失败）；退出码非 0；
  排空期间新请求被受理（必须被拒绝）；对局被截断却写入胜负（**最严重**，必须报失败）。
- 验收命令：`bash chaos/verify-drain.sh`。
- 测试要求：覆盖"能在超时前排空完"与"必须超时截断"两种路径；后者断言落的是
  `ABORTED`、且 `match_results` 里**没有**这一局的胜负行。
- 回退方式：`git revert`。本任务**会改产品代码**（三个服务的退出路径），
  回退后恢复"立即退出"的现状。
- 涉及目录：`src/gateway/`、`src/match/`、`src/room/`、`tests/unit/`、`chaos/`、`docs/`。

### TASK-027：长稳运行：内存与 FD 稳定性

- 状态：**待确认**
- 依赖：TASK-024 ~ TASK-026
- 背景问题：Phase 4 的退出标准最后一条是"持续连接、断连、重连，观察内存与 FD 是否
  稳定"。前三个阶段的验收都是**短时**的（`verify-all.sh` 约 215 秒，容量压测每档
  60 秒），因此内存增长趋势与 fd 泄漏从未被观察过。这类问题只在长时间运行后暴露。
- 本次目标：在固定负载下持续运行**足够长**的时间，给出内存与 FD 的时间序列，
  并判断是否存在单调增长。
- 范围：
  - `chaos/verify-soak.sh`：支持 `--duration` 与 `--players`（默认 50）。
    **时长已定（2026-10-04 项目所有者裁决）：默认 30 分钟，上限 1 小时**；
    超过上限时报错而不是静默截断（更长的窗口会占用验收时间，且本机 11 GiB 同时
    跑服务与监控栈）。负载里**主动制造**周期性的断连与重连
    （每 N 秒断开一部分 SSE 并重连），因为"只连着不动"测不出订阅清理的泄漏。
  - 采样：每 10 秒记录三个服务的 RSS / 线程数 / fd 数（`/proc/<pid>/status` +
    `/proc/<pid>/fd` 计数）、Gateway 的 SSE 连接数与房间数（`/metrics`）。
  - 判定：给出**首尾对比**与**趋势**（是否单调），并按"有没有回到基线"给结论。
    不预设阈值——先拿到实测曲线，阈值与 SLO 的调整另立任务。
- 非范围：不做 24 小时以上长稳（本机资源与时间不允许，且先要一轮基线）；
  不做内存泄漏的修复（若发现，另立任务并附证据）。
- 失败场景：进程 OOM 被杀（报失败并保留采样）；fd 数单调增长不回落（报失败并给出
  增长速率）；SSE 连接数在断连后不下降（订阅未清理，**这是最可能命中的真实缺陷**）。
- 验收命令：`bash chaos/verify-soak.sh --duration 1800 --players 50`。
- 测试要求：必须包含"断连后连接数回落"这一断言（否则长稳就退化成"一直连着"）。
- 回退方式：`git revert`；只新增测试脚本。
- 涉及目录：`chaos/`、`docs/`、`docs/benchmarks/`（长稳结果作为容量报告的补充一节）。

### （原 TASK-028 已移出本阶段）

**「把匹配队列快照写入移出请求路径」不纳入 Phase 4**（2026-10-04 项目所有者裁决），
退回本文档的 Backlog，并保留 TASK-022 给出的量化目标。下面保留原任务单全文，
以便将来立项时直接取用——**它的状态仍是待确认，不在本阶段执行**。

### TASK-028（已移出 Phase 4，保留备查）：把匹配队列快照写入移出请求路径

- 状态：**已移出 Phase 4**（2026-10-04 裁决）。**不在本阶段执行**，保留任务单供将来
  立项；若启动，应另开阶段或按单独任务批准（它会改产品代码，与 Phase 4 其余任务
  性质不同）。
- 依赖：TASK-023（需要用故障注入验证"Redis 挂起"这条路径）
- 背景问题：TASK-022 的容量报告给出了量化证据——**入队端点的 P95 延迟**：
  100 档位 88 ms → 500 档位 **842 ms** → 1000 档位 **956 ms**，而 SLO 是
  P95 < 100 ms。根因是 `MatchQueue::PersistSnapshot()` 用全局
  `snapshot_order_mutex_` 把"取快照 + 写 Redis"整体串行化，于是每一次状态变化的
  入队/轮询都要排在前一次 Redis 写之后；轮询端点也因此从 5.8 ms 涨到 859 ms
  （次生效应：超时清算走同一条写入路径）。Backlog 里这条限制自 TASK-015 起就写着
  "先要有证据"，现在证据有了。
- 本次目标：让快照写入不再位于请求路径的关键路径上，并把入队 P95 拉回 SLO 以内。
- **量化目标（本任务的验收判据）**：在同一台机器、同一 `bench.sh` 配置下，
  **500 档位入队 P95 < 100 ms**（当前 842 ms）。
- 范围：
  - `src/match/`：把快照写入改为**合并 + 异步**（独立的写入线程或定时器）：
    请求路径只标记"队列已变化"，由后台按最小间隔合并写入。
  - 保留既有语义：整份重写、原子替换（`MULTI/DEL/RPUSH.../EXEC`）、失败只记日志
    不重试、Redis 不可用时降级为纯内存（TASK-015 的决策 B **不变**）。
  - 关闭时的行为要明确：进程退出前必须做一次最终写入（否则"最后一次变化"丢失）。
  - `scripts/bench.sh` 复跑 500/1000 档位，结果追加到
    `docs/benchmarks/README.md`（**新开一节记录优化前后对比**，不修改原有数据）。
- 非范围：不改配对规则、不改快照格式、不引入新的存储组件、不引入消息队列；
  不做多实例队列。
- 失败场景：异步写入让"重启后队列状态落后"超出定义的范围（必须明确写出新的偏差
  边界，并更新 TASK-015 的恢复语义）；关机时最后一次写入未执行；
  Redis 挂起时后台线程堆积任务（必须限流，不能无界堆积）。
- 验收命令：
  ```bash
  cmake --preset brpc-debug && cmake --build --preset brpc-debug
  ctest --test-dir build/brpc-debug --output-on-failure
  bash scripts/bench.sh --level 500 --duration 60
  bash scripts/verify-persistence.sh   # 回归：快照与队列恢复语义不能退化
  ```
- 测试要求：单测覆盖"合并写入"与"关机前最终写入"；端到端用 `bench.sh` 给出
  优化前后的 P95 对比（**同一环境、同一命令**，否则数字不可比）。
- 回退方式：`git revert` 单个提交；回退后退回"同步写快照"的现状（功能不丢，
  只是延迟回到实测的基线值）。
- 涉及目录：`src/match/`、`tests/unit/match/`、`scripts/`、`docs/`。

## Backlog：后续待办

- **把匹配队列的快照写入移出请求路径**（TASK-015 期间记录）。当前快照写入同步
  发生在入队请求里：Redis **拒绝连接**时几乎不花时间，但 Redis **挂起**时会多等一个
  Redis 超时（默认 500 ms），与 Gateway 的 `match_timeout_ms` 同量级，
  理论上可能让一次入队超时——这与"不阻塞业务"的降级目标不完全一致。
  彻底解决需要独立写入线程或定时器（引入并发），**先要有证据**。
  连带可考虑共享的"连接不可用冷却期"，避免每个请求都去撞一个已知挂掉的依赖。
- **让 TSan 真正可用**（TASK-014 期间已完成一半）。`brpc-tsan` 预设已经建好并能
  跑通，但 vcpkg 提供的 brpc 没有用 TSan 插桩，实测 21 条报告**全部**落在 brpc
  内部（`butil::Mutex`、`cpuwide_time_ns` 等），对本项目代码没有指向性。
  要让它成为可用的并发门禁，需要**用 TSan 重建 brpc 及其依赖链**
  （或改用能按模块过滤报告的方案）。代价不小：brpc 本体普通编译就要 17 分钟。
  在补上之前，并发缺陷只能靠 A/B 复现、ASan 与直接现象这类间接证据。
  参见 `docs/devlog.md` 的「TASK-014 实施记录」。
- **把快照写入从 ticker 线程移出去**（TASK-014 期间记录为已知限制）。当前
  `RoomManager::Tick` 在推进线程上直接写快照，因此 MySQL 不可用时推进会被连接
  超时阻塞、变成突发式。需要独立写入线程或异步队列，但**先要有容量证据**
  （Phase 3 的压测），否则属于提前引入复杂度。
  **（2026-10-04 TASK-023 补充实测数字）**：容量证据已经有了，而且比预想更严重——
  MySQL 不可用时同一局对局，基线一局 **4.1 ~ 16.7 秒**，注入后脚本观测到终态用了
  **755 秒**（同期帧号只到 48 帧）。量级与「快照每 1 秒调度一次 + 每次写失败等一次
  3 秒连接超时」吻合（755 次 × 1 秒 ≈ 755 秒）。正确性不受影响（终态正确、结果
  恢复后落库且无重复行），受影响的只有推进速度。
  **触发条件**：进入 Phase 5 的稳定性收口时重新评估；或任何一次实测出现
  「依赖不可用期间对局推进慢到影响验收」时立即处理。另有两条同源但独立、
  **读产品代码即可判断、不需要跑故障注入**的高速路：`kSnapshotIntervalMs` 的取值，
  以及 MySQL 连接失败后重复 `connect` 之前是否要加"已知不可用冷却期"
  （Backlog 第一条已提到同样的冷却期思路，两处可以一起做）。
- **Gateway 的依赖不可用路径不写日志**（TASK-023 期间实测发现）。`FillError()`
  只填错误体并返回状态码，既没有 `LogWarn` 也没有 `LogInfo`，因此
  `session_store_unavailable` / `player_store_unavailable`（以及 `room_unavailable`、
  `result_pending`、`result_store_unavailable`）这五类依赖失败在日志里**查不到**，
  唯一可观测的痕迹是 `/metrics` 的 `rgbt_http_requests_total{status="503"}` 计数
  与响应体里的 `reason`。这是真实的排障缺口：线上遇到 503 时，日志里没有任何一行
  说明是哪个依赖挂了。TASK-023 的验收脚本因此改用指标断言。
  **判断这件事不需要新增测试**——读 `src/gateway/gateway_service.cpp` 的
  `FillError` 与各失败分支即可确认。
  **触发条件**：Phase 3 已交付的 Grafana 面板与告警上线后，若「503 上升」这条告警
  无法从日志定位到具体依赖，就修；或 Phase 5 做 Runbook / 可观测性收口时一并处理。
  修法很轻：在这些失败分支上各加一条 `LogWarn`（带 `reason` 与 `request_id`），
  与既有 `match_enqueue_failed` 的写法一致。
- **`scripts/bench.sh` 的墙上时长与真实秒不同源**（TASK-023 期间实测发现）。
  本机 coreutils 9.x 的 `date +%s%3N` **不**把纳秒截断成 3 位，而是输出
  「epoch 秒 + 字面量 `3` + 纳秒」：实测 `date +%s%3N` = `179110103575678273`，
  而 `date +%s` = `1791101035`。`bench.sh` 用它做首尾相减，因此**内部一致**
  （既有容量报告的数字不受影响，那些数字是"同源相减"，量级正确）；但若把这些
  秒数与别处的真实秒并列比较就会错。
  **判断这件事不需要跑压测**——执行一次 `date +%s%3N; date +%s` 对比即可确认。
  **触发条件**：凡是**新增**基于墙钟的速率、超时或 SLO 断言的脚本，一律改用 bash
  自带的 `EPOCHREALTIME`（TASK-023 的 `chaos/lib.sh` 已这么做，可直接抄
  `now_us` / `now_ms` / `elapsed_ms_from_us`）；`bench.sh` 自身的替换等下一次
  动它时顺手做，不必单独开任务。
- **跨 Windows / WSL 的同步工具**（TASK-014 期间踩了两次）。临时用的 rsync 脚本
  同时踩到 mtime（Ninja 不重建 → 新旧目标文件混链 → 假崩溃）与权限位
  （DrvFs 全 755 → 154 个文件权限被改）两个坑。当前靠脚本里的 `touch` +
  按 git 恢复权限来兜底，但它是会话临时工具、不在仓库里。若后续继续双环境协作，
  应该把它固化成一个受版本控制的脚本并写进 `docs/06-operations.md`。
- **应用服务的容器化**（TASK-011 期间决定延后）。给三个 C++ 服务与前端写
  Dockerfile，并加进 `deploy/compose/docker-compose.yml`。**（2026-10-04 更正：
  原先记的是「故障注入的前置条件」，但 TASK-022 的 `scripts/bench.sh` 已证明
  本机进程 + PID 就能完整起停、注入、采集与清理，`kill -9` / 停依赖 / 连接风暴
  都不依赖容器。**项目所有者于 2026-10-04 裁决：不作为 Phase 4 的前置条件**，
  其真实价值在「现场演示可复现」，归 **Phase 5** 评估。）**
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
