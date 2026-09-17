# 开发日志

本文件只记录已经发生的事实、决策、问题和结果，不记录未经验证的设想。

## 2026-09-14

### 完成

- 建立项目根目录和文档基线。
- 明确项目定位、服务边界、技术栈和迭代方向。
- 建立 Claude 主开发、豆包辅助命令、DeepSeek 辅助解释、用户验收的协作方式。
- 项目所有者确认 TASK-000 和 ADR-0001。
- 确认正式项目目录迁移到 WSL 的 `~/workspace/realtime-game-backend`。
- 已把项目迁移到 WSL，排除了 `.idea`，并确认 Windows 副本与 WSL 副本内容一致。

### 决策

- 项目定位为分布式实时对战服务后端，不重复开发通用 RPC 框架。
- 服务间使用 brpc + Protobuf，浏览器使用 HTTP + WebSocket。
- 第一版使用 Docker Compose，不先引入 Kubernetes。
- Kafka 和 etcd 必须等待明确触发条件后引入。
- 采用当前 WSL2 Ubuntu 26.04 LTS 作为开发环境，不重装 Ubuntu 24.04。
- 当前工具链版本采用 GCC 15.2、CMake 4.2.3、Ninja 1.13.2、Git 2.53.0、
  Protobuf 3.21.12、clang-tidy 21.1.8、pkg-config 2.5.1 和 vcpkg 2026-07-27。
- Windows 目录仅保留为初始文档整理和迁移前备份，不用于正式构建。

### 验证

- 命令：只读检查 WSL 发行版、Linux 工具、Git 仓库和 GitHub CLI 状态。
- 结果：WSL2 Ubuntu 26.04 LTS；GCC 15.2.0；CMake 4.2.3；Git 2.53.0；
  Protobuf 3.21.12。
- 结果：已安装 Ninja 1.13.2、clang-tidy 21.1.8、pkg-config 2.5.1、
  redis-cli 8.0.5、mysql 8.4.11 和 vcpkg 2026-07-27。
- 结果：vcpkg 仓库提交为 `a1cae005c39be7b18ba319fced856b68d7276271`。
- 结果：Docker Desktop 4.74.0、Engine 29.4.3 和 Compose v5.1.3 已在 WSL 中
  验证可用。
- 结果：WSL 正式目录存在，项目文件对比无差异，未复制 `.idea`。
- 结果：WSL 正式目录已初始化 `main` 分支，`origin` 指向
  `git@github.com:Archer11-q/realtime-game-backend.git`，远程连接成功。
- 结果：当前仓库固定 Git `http.version=HTTP/1.1`，避免 GitHub HTTP/2 连接错误。
- 结果：WSL SSH 密钥已生成并由 GitHub 接受，`ssh -T git@github.com` 返回
  `Hi Archer11-q! You've successfully authenticated`。

### 待完成

- 完成首次提交和推送。（已于 2026-09-14 完成，见 2026-09-15 日志）
- 配置并验证 CLion WSL Toolchain。（已由项目所有者确认可用）
- 开始 TASK-001 和 TASK-002。

### 问题与风险

- 对局同步方式和登录方案仍为开放决策。

## 2026-09-15

### 完成

- 确认首次提交与推送的事实：`f685be0 chore: 初始化仓库基础框架`，作者
  Archer11-q，时间 2026-09-14 23:47:11 +0800，共 36 个文件，本地 `main` 与
  `origin/main` 差异为 `0 0`。
- 确认该提交不包含 `.idea/`、构建产物和密钥，内容可移植。
- 项目所有者确认 CLion WSL Toolchain 可用。
- 删除 `CLAUDE.md`、`README.md`、`docs/03-development-workflow.md` 中按厂商名称
  绑定职责的分工表述，改为不依赖厂商名的任务角色（执行者 / 辅助者），写入权按
  任务授予。

### 决策

- 采纳方案 A：WSL 目录为唯一正式开发环境，Windows 目录冻结为备份，不再作为
  构建和提交来源。
- 经实测，WSL 的 `~/workspace/realtime-game-backend` 是**无任何提交的空仓库**，
  因此采用“重新 clone 远程”而不是 `git pull`，避免无关联历史冲突。
- 职责划分不按 AI 产品名称绑定，避免同一产品的不同版本被混为一谈。

### 验证

- 命令：`git -C D:\CLion\realtime-game-backend log -1`、`git ls-tree -r --name-only
  f685be0`、`git rev-list --left-right --count origin/main...main`。
- 结果：提交存在且已推送；提交内无 `.idea/`；本地与远程无差异。
- 结果：仓库内存储与工作区均为纯 LF；`core.autocrlf=true` 未污染行尾。
- 结果：当前 Windows 会话无法访问 WSL（`wsl -l -v` 返回
  `Wsl/Service/E_ACCESSDENIED`），WSL 侧状态由项目所有者手动提供。
- 结果：本机无 cmake、ninja、g++、ctest；仅存在 CLion 自带
  `D:\CLion\CLion 2025.3.3\bin\cmake\win\x64\bin\cmake.exe` 与
  `bin\ninja\win\x64\ninja.exe`，可用于配置校验，但正式构建验收必须在 WSL 执行。
- 结果：Windows 命令行 Git 未配置提交身份，本次按仓库既有提交的作者身份设置
  仓库级 `user.name`/`user.email`，未使用 `--global`，未改写既有提交。
- 结果：文档修正已提交为 `d7541ce`，提交对象内行尾为纯 LF，工作区干净。
- 结果：从当前执行环境执行 `git push` 失败，报 `fatal error - couldn't create
  signal pipe, Win32 error 5`，属于执行环境的命名管道限制，不是密钥或权限问题。
  因此 `d7541ce` 尚未推送，需在 WSL 中完成推送。

### 问题与风险

- 在 WSL 完成 clone 之前，Windows 目录是唯一持有提交的工作树；此时不得删除。
- 双工作树并存会导致分叉，必须在 WSL clone 完成后冻结 Windows 目录。
- 对局同步方式和登录方案仍为开放决策。

### 下一步

- 在 WSL 完成空壳替换为正式 clone，并验证 `git log` 与 `git status`。
- 开始 TASK-002：CMake、Presets、格式与静态检查、最小测试和 CI 骨架。

### 补充（同日稍后）：WSL 恢复可访问并完成对齐

- 执行环境的文件策略放宽后，`wsl -l -v` 可正常返回（Ubuntu 为 Running），
  此前记录的 `Wsl/Service/E_ACCESSDENIED` 已不再出现，该条已过期。
- 结果：WSL 正式目录为 `~/workspace/realtime-game-backend`，`git init` 过、
  远程配置完整（含 `http.version=HTTP/1.1`）、但**没有任何提交**。
- 处理：为避免改写既有配置，未删除 `.git`，而是直接 `git fetch origin` 后
  `git reset --hard origin/main`。结果 `HEAD` 对齐到 `f685be0`，与远程差异
  `0 0`，`git status` 干净。
- 结果：`scripts/env-report.sh` 实测环境：Ubuntu 26.04 LTS、内核
  6.18.33.2-microsoft-standard-WSL2、16 逻辑核、11 GiB 内存；CMake 4.2.3、
  GCC 15.2.0、Ninja 1.13.2、Git 2.53.0、clang-format 21.1.8、clang-tidy 21.1.8、
  protoc、pkg-config、vcpkg 均存在；`ccache` 缺失。
- 结果：`/usr/include/openssl/ssl.h`、`/usr/include/gflags`、`/usr/include/glog`
  均缺失，属 TASK-004（brpc 基线）的前置条件。
- 结果（**已更正**）：先前在此记录“Docker Desktop 的 WSL Integration 未对本发行版
  启用”，该结论**错误**。真实原因是当时 Docker Desktop 未启动，因此 `docker info`
  无响应。经项目所有者启动 Docker Desktop 后复测：`docker version` 为
  `29.4.3 / server 29.4.3`（服务端可达）、存储驱动 `overlayfs`、
  `docker compose version` 为 `v5.1.3`、`docker` 解析到 `/usr/bin/docker`。
  结论回到 2026-09-14 的原始记录：Docker 在 WSL 中可用。
  教训：判断环境能力前必须确认被测服务已启动，否则会把“未启动”误判为“未配置”。
- 结果：同步工作树时发现跨文件系统权限陷阱——从 `/mnt/d`（NTFS）用 tar 读取时，
  所有文件被报为 755，导致 WSL 中每个已跟踪文件都被 git 标记为“已修改”。
  修复方式是对普通文件 `chmod 644`、对 `*.sh` 保留 `chmod 755`。这是“正式目录必须
  在 WSL 文件系统内、不要用 Windows 副本作为来源”的直接证据。

## TASK-002 实施记录（2026-09-15）

### 完成

- 顶层 `CMakeLists.txt`：C++20、`CMAKE_CXX_EXTENSIONS OFF`、默认 Debug、
  `compile_commands.json`、`-Werror` 开关、ASan 开关、CTest 接入。
- `CMakePresets.json`：`debug` / `release` / `asan` 三个 Preset，统一 Ninja，
  测试预设对 ASan 设置 `detect_leaks=1:abort_on_error=1`。
- `.clang-format`、`.clang-tidy`、`scripts/check-format.sh`（格式检查）。
- `include/common/version.hpp` + `src/common/version.cpp` + `rgbt_common` 静态库。
- `tests/unit/common/version_test.cpp`：4 个 GoogleTest 用例。
- `tests/CMakeLists.txt`：GoogleTest 通过 `FetchContent` 固定到 tag `v1.17.0`，
  并用 `RGBT_USE_VCPKG_TOOLCHAIN` 开关预留切换到 vcpkg 的路径。
- `scripts/verify.sh`：与 CI 等价的本地验收入口。
- `scripts/env-report.sh`：只读环境盘点脚本。
- `.github/workflows/ci.yml`：Ubuntu 24.04 上对三个 Preset 配置、构建、测试并
  执行格式检查，失败时上传测试日志。
- `.gitattributes`：强制文本文件 LF，防止 shell 脚本被检出为 CRLF。
- `docs/TASKS.md`：写入 TASK-002 完整任务单，并记录本轮写入权归属。

### 决策

- GoogleTest 采用 `FetchContent` 而非 vcpkg：本轮不需要 vcpkg baseline、镜像和
  缓存策略，避免扩大范围；切换点已用开关隔离，未来引入 vcpkg 时测试代码与
  目标名不变。
- 版本号通过编译期宏 `RGBT_VERSION_*` 注入，**不使用 `@VAR@` 模板头**。
- 预留 `RGBT_USE_VCPKG_TOOLCHAIN` 开关但默认关闭，不为未确认的依赖方式增加配置。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`bash scripts/verify.sh`（内部为 `rm -rf build` 后的全新配置与构建）。
- 结果：`debug`、`release`、`asan` 三个 Preset 全部配置成功、构建成功，
  `ctest` 各执行 4 个用例，均为 `100% tests passed, 0 tests failed out of 4`。
- 结果：`asan` 预设下编译与链接均带 `-fsanitize=address`，测试通过且无泄漏报告。
- 结果：全程 `-Werror` 生效，无任何编译警告导致失败，也无被忽略的警告。
- 结果：`clang-format --dry-run --Werror` 通过，检查 3 个文件。
- 结果：`clang-tidy` 退出码 0；对项目代码无告警，第三方头文件告警已抑制
  （`Suppressed 43167 warnings (43156 in non-user code, 11 NOLINT)`）。
- 结果：`build/` 未被 git 跟踪，未提交任何构建产物或 IDE 文件。
- 结果：CMake 4.2.3 + GoogleTest v1.17.0 组合**实测可用**，此前担心的
  CMake 4 与旧依赖不兼容问题在本组合下未出现。
- 结果：修复下述不稳定点后，连续两轮“删除 `build/` 后重新验收”均完整通过
  （`debug`、`release`、`asan` 各 4 个用例，加格式检查），`verify` 退出码为 0。
- 结果：共享依赖生效，整个 `build/` 下 `googletest-src` 只出现 1 次，
  三个预设复用同一份依赖，不再各下载一次。

### 问题与风险

- 首次实现存在两处真实缺陷，均由实际构建暴露并修复：`version.hpp.in` 缺少访问
  函数声明；`clang-format` 会把 `@PROJECT_VERSION_MAJOR@` 改写成
  `@PROJECT_VERSION_MAJOR @`，使 CMake 模板替换失效。后者是这类模板头的通用
  陷阱，已改用编译期宏并写入头文件注释。
- 验收过程中出现**间歇性配置失败**：连续多次全新构建里，总有一个预设（`debug`
  或 `asan`）配置失败，而单独重跑该预设立刻成功。定位结论是 `FetchContent`
  的默认行为对每个预设各下载一次依赖，三个预设即三次网络下载，放大了网络抖动。
  修复方式是在顶层 `CMakeLists.txt` 设定
  `FETCHCONTENT_BASE_DIR=${CMAKE_SOURCE_DIR}/build/_deps`，使三个预设共享一份
  下载与构建产物。修复后连续两轮全新验收均通过。
- 同步工作树时对全仓库执行 `chmod 644` 会把 `build/` 下的可执行文件也一并改掉，
  导致测试二进制报 `Permission denied`。权限修正应限定在纳入版本管理的文件上，
  `build/` 属于可删除的生成物，重新构建即可，不应参与权限处理。
- 提交过程中两次把本机临时辅助脚本（`scripts/_*.sh`）误提交：一次是被
  `git add -A` 卷入，一次是脚本自身执行时又执行了 `git add -A`。处理方式是用
  `git rm --cached` 精确剔除后 amend，并已在 `.gitignore` 加入 `scripts/_*.sh`
  规则防止复发。后续本机辅助脚本一律使用下划线前缀并放行忽略规则。
- WSL 侧的功能分支在仓库重置时丢失，提交一度落在 `main` 上；已用
  `git branch -f main origin/main` 复位 `main`，并把提交重建在
  `feat/task-002-build-skeleton` 上。`main` 现与 `origin/main` 完全一致。
- CI 状态（**已解决**）：合并前经 GitHub API 核实，工作流因只存在于功能分支而
  未被注册（`actions/workflows` 的 `total_count` 为 0）；原因是 GitHub 只从默认
  分支注册工作流。项目所有者于 2026-09-15 合并 PR #1 到 `main` 后，工作流被注册
  （`state: active`），CI 自动运行并**全部通过**：
  - run #1，分支 `feat/task-002-build-skeleton`，`completed / success`
  - run #2，分支 `main`，`completed / success`
  命令：`curl -s "https://api.github.com/repos/Archer11-q/realtime-game-backend/actions/runs?per_page=5"`。
  因此 TASK-002 的“CI 成功”验收项**已满足**。
- 结果：`main` 已快进拉取到 `eb67a6c Merge pull request #1 from
  Archer11-q/feat/task-002-build-skeleton`，与 `origin/main` 差异 `0 0`。
- TASK-003 依赖的 Docker **已确认可用**（见上文更正条目），TASK-003 无此阻塞。
- TASK-004 依赖的 `openssl`、`gflags`、`glog` 开发头文件在系统中缺失。这三项是
  brpc 的编译期依赖，不影响当前骨架；安装方式（系统 apt 还是交由 vcpkg 构建）
  属于 TASK-004 的范围，届时再决策。另注意 `VCPKG_ROOT` 当前未设置，
  vcpkg 位于 `/home/archer/tools/vcpkg`，版本 `2026-07-27`，与文档记录的基线一致。
- 已一并处理 `docs/07-open-decisions.md` 中按厂商绑定分工的旧表述。

### 下一步

- TASK-002 已具备完成条件，由项目所有者确认后标记为已完成。
- 输出并确认 TASK-003（Docker 开发依赖）任务单。

## TASK-003 实施记录（2026-09-15）

### 完成

- `deploy/compose/docker-compose.yml`：Redis 与 MySQL 两个服务，具名卷
  `redis-data`、`mysql-data`，healthcheck，`restart: unless-stopped`，
  端口仅绑定 `127.0.0.1`。
- `deploy/compose/README.md`：前置依赖、启动、健康检查、日志、停止、清理、
  备份与恢复、初始化脚本说明、常见问题。
- `.env.example`：按 `docs/06-operations.md` 补齐 Compose 所需变量，每项附
  取值范围说明；新增 `MYSQL_ROOT_PASSWORD`。
- `migrations/001_create_schema_migrations.sql`：最小建表脚本，使用
  `CREATE TABLE IF NOT EXISTS` 与 `ON DUPLICATE KEY UPDATE` 保证幂等。
- `scripts/verify-deps.sh`：依赖环境验收入口，支持 `--keep` 与 `--down-only`。

### 决策

- 镜像固定为 `redis:8.0` 与 `mysql:8.4`，与本机 redis-cli 8.0.5、
  mysql 客户端 8.4.11 的大版本一致。
- Redis 本轮使用默认 RDB 快照，未开启 AOF。理由是 `CLAUDE.md` 规定 Redis 不作为
  唯一真相，现在引入 AOF 属于提前引入能力，留到 Phase 2。
- Compose 变量使用 `${VAR:?提示}` 形式，缺少变量时立即失败并打印中文提示，
  避免用空密码启动一个看似正常的错误环境。
- 数据库端口只绑定 `127.0.0.1`，不暴露到局域网。
- `migrations/` 只读挂载到 `/docker-entrypoint-initdb.d`，且文档明确说明该目录下的
  脚本只在数据目录为空时执行一次。

### 验证

- 命令：`docker compose --env-file .env.example -f deploy/compose/docker-compose.yml config`
- 结果：退出码 0；确认 `migrations` 绑定挂载解析为仓库内绝对路径，且
  `read_only: true`。
- 结果：故意不传 `--env-file` 时退出码为 1，并输出 `缺少 REDIS_PORT`，
  快速失败行为符合设计。
- 命令：`bash scripts/verify-deps.sh --keep`
- 结果：一条命令启动成功；`redis` 与 `mysql` 均变为 `healthy`。
- 结果：`redis-cli ping` 返回 PONG；MySQL 业务账号连接成功，服务端版本 `8.4.11`。
- 结果：初始化脚本已执行，`schema_migrations` 可访问且迁移数为 1，证明
  `migrations` 挂载链路有效。
- 结果：写入测试数据后执行 `compose restart`（不删卷），Redis 键与 MySQL 行
  均保留，验证“重启后数据卷保留”。
- 结果：测试键与测试表已删除，未在数据库中留下验证残留。
- 命令：`bash scripts/verify-deps.sh --down-only`
- 结果：容器与网络已移除，两个数据卷仍存在（`realtime-game-backend_mysql-data`、
  `realtime-game-backend_redis-data`），验证“停止不等于删数据”。
- 备注：首次运行需要拉取镜像（Redis 约 40 MB、MySQL 约 200 MB），本次实测网络
  可用，拉取耗时较长但不影响结论。

### 问题与风险

- 修改 `.env` 中的 `MYSQL_PASSWORD` 不会更新已有数据卷中的密码，会导致认证失败。
  该场景已写入 `deploy/compose/README.md` 并给出两种处理方式。
- `down -v` 会永久删除数据，已在文档中以危险命令标注并加警告。
- 本任务未在 CI 中验证 Compose 配置。CI 当前只构建和测试 C++ 代码；是否增加
  `docker compose config` 校验留到后续按需决定，不扩大本次范围。
- 系统仍缺少 brpc 相关开发头文件（`openssl`、`gflags`、`glog`），属 TASK-004
  前置条件。
- `VCPKG_ROOT` 仍未设置。

### 下一步

- 由项目所有者审阅并合并本任务改动。
- 输出 TASK-004（brpc Gateway 基线）任务单，并在其中确定依赖获取方式：
  系统 apt 安装还是交由 vcpkg 构建。

### 补充（同日稍后）：.env 位置修正与 vcpkg 环境准备

#### 决策

- **`.env` 与 `.env.example` 从仓库根目录移到 `deploy/compose/`**。原设计放在根目录
  是错误判断：Docker Compose 的约定是从“compose 文件所在目录”自动读取 `.env`，
  放在根目录会强制每条命令都加 `--env-file`，多一个每次操作都可能漏掉的参数。
  修正后命令简化为
  `docker compose -f deploy/compose/docker-compose.yml up -d`。
- **TASK-004 的依赖获取方式选定为“先验证再全量”**：先用 vcpkg 只装 brpc，
  确认 CMake 4.2.3 + GCC 15.2.0 真能编译，再决定是否把 GTest 等一并迁入
  vcpkg manifest。理由是本轮已两次遇到“以为兼容、实际不兼容”的情况
  （CMake 4 与依赖、clang-format 与模板占位符），先花一次构建验证风险最低。

#### 完成

- `deploy/compose/docker-compose.yml`：移除对外部 `--env-file` 的依赖；
  修正 `migrations` 绑定挂载的路径说明。
- `deploy/compose/README.md`：同步修正全部命令；新增卷的实际名称
  （`realtime-game-backend_*`）、`docker volume inspect` 用法、备份文件不得入库的
  提醒，以及可执行的恢复演练步骤和第 11 节演练记录表。
- `.gitignore`：新增数据库导出文件忽略规则（`backup-*.sql`、`*_dump.sql`、
  `*.sql.gz`、`redis-dump-*.rdb`）。
- `docs/06-operations.md`：明确 Compose 相关 `.env` 必须与 compose 文件同目录。
- `docs/01-architecture.md`：补充宿主机地址与容器内服务名/端口的使用区别。

#### 验证

- 结果：不传 `--env-file` 时 `docker compose -f deploy/compose/docker-compose.yml
  config` 退出码为 **0**，确认 compose 能自动发现同目录的 `.env`。
- 结果：移走 `.env` 后同一命令退出码变为 1 并提示缺少变量，确认配置确实被读取。
- 结果：配置输出中 `migrations` 的 bind 源路径解析为
  `/home/archer/workspace/realtime-game-backend/migrations`，`read_only: true`。
- 结果：`.env`、`.env.example` 均已不在仓库根目录。

#### 项目所有者完成的恢复演练（真实记录）

- 项目所有者按 `deploy/compose/README.md` 第 7 节的步骤，实际执行了 MySQL
  备份与恢复演练：建表并写入 `before-backup` → `mysqldump` 备份 → 删表 →
  恢复 → 查询验证。
- 结果：恢复后 `SELECT k FROM _drill;` 返回 `before-backup`，**恢复链路可用**。
  这是 `docs/06-operations.md` 第 6 节“Docker 数据卷不能只创建不验证恢复”要求的
  首次实际验证。
- 备份产物 `backup-drill.sql`（2.9 KB，含真实数据）一度处于未被忽略状态，
  已通过 `.gitignore` 规则消除误提交风险。

#### vcpkg 环境准备

- 结果：`VCPKG_ROOT` 此前未设置。已建立单一来源的配置片段 `~/.vcpkg-env.sh`，
  由 `~/.profile` 与 `~/.bashrc` 共同引入，避免两处内容漂移。
- 设置内容：`VCPKG_ROOT=$HOME/tools/vcpkg`、
  `VCPKG_DEFAULT_BINARY_CACHE=$HOME/.cache/vcpkg/archives`（复用已编译 port，
  减少 brpc 这类重依赖的重复编译成本）、`VCPKG_DISABLE_METRICS=1`。
- 结果：登录 shell 与交互式 shell 中 `VCPKG_ROOT` 均正确，
  `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` 可达。
- **已知限制**：非交互式 shell（`bash -c`）不会读取 `.profile`/`.bashrc`，
  因此不会自动获得 `VCPKG_ROOT`。后续脚本如需该变量，必须显式
  `. "$HOME/.vcpkg-env.sh"` 或在命令前传 `-DCMAKE_TOOLCHAIN_FILE=...`。
- 变更前已备份：`~/.bashrc.backup-20260915`、`~/.profile.backup-20260915`。
  确认环境正常后可自行删除。所有相关文件权限保持 644。

#### 待处理

- 项目所有者的 shell 需重新登录或 `source ~/.profile` 后，`VCPKG_ROOT` 才会在
  其当前终端中生效（新开的终端自动生效）。

## TASK-004 实施记录（2026-09-16）

### 完成

- 用 vcpkg 经典模式安装 brpc 1.16.0 及其全部依赖，共 **73 个包**：
  protobuf 6.33.4、thrift 0.24.0、openssl 3.6.4、abseil、gflags、glog、leveldb、
  zlib、libevent、boost 1.92.0（40 个子库）等。
- 新增 `brpc-debug` 预设，通过 `toolchainFile` 指向 vcpkg，仅在需要 brpc 时使用。
- 新增 `src/gateway/smoke_main.cpp`：启用 brpc 内置服务的最小可运行服务。
- 新增 `src/gateway/CMakeLists.txt`、`scripts/verify-brpc.sh`。
- `src/CMakeLists.txt`：仅在 `RGBT_USE_VCPKG_TOOLCHAIN` 为真时加入 gateway 子目录，
  默认构建与 CI 不受影响。
- `tests/CMakeLists.txt`：vcpkg 模式下若未安装 gtest 则跳过测试并给出提示，
  而不是中断配置。

### 决策

- 确认采用方案 C：先用 vcpkg 只验证 brpc 可用，再决定是否全量迁移依赖。
  本轮不实现完整 Gateway，只交付「brpc 可用」的可验证证据。
- 本机 vcpkg 不是 git 克隆，**无法使用 manifest 的 `builtin-baseline`**，
  因此采用经典模式安装。若要固定版本，需把 vcpkg 重新克隆为 git 仓库。
- brpc 的 HTTP 能力使用其**内置运维服务**（`/health`、`/status`、`/version`），
  而不是自写 HTTP 服务类。

### 验证

- 命令：`bash scripts/verify-brpc.sh`，退出码 **0**。
- 结果：配置成功；构建成功并生成 `build/brpc-debug/bin/rgbt_brpc_smoke`。
- 结果：服务启动；`GET /health` 返回 **200**，响应体为 `OK`；brpc 内置 `/status`
  返回 200；未知路径返回 **404**，证明路由确实生效。
- 结果：发送 SIGTERM 后进程退出码 **0**，输出包含「已优雅退出」。
- 结果：**CMake 4.2.3 + GCC 15.2.0 能编译并链接 brpc 1.16.0**，这是本任务的核心
  验证目标，此前属未知风险。
- 结果：既有 `debug`/`release`/`asan` 三预设回归各 4/4 通过，格式检查通过，
  接入 vcpkg 未破坏原有构建。
- 结果：以上验收在 `VCPKG_ROOT` **未导出**的非交互式 shell 中同样通过，
  证明预设不依赖该环境变量。

### 问题与风险（本轮实际踩到并解决的）

- **GitHub 大文件被限速到 34 KB/s 且会中途停滞**，vcpkg 每次重试都从零开始，
  导致 openssl（53 MB）与 protobuf 下载卡死。解决方式：按 port 声明的 URL 与
  SHA512，用加速通道预下载并**逐个校验哈希**后放入 `$VCPKG_ROOT/downloads/`。
  共预置 5 个包（CMake 4.4.3、openssl、zlib、protobuf、libevent、thrift），
  全部哈希匹配。
- **vcpkg 要求自带 CMake 4.4.3**（高于本机 4.2.3），需先满足该前置。
- **vcpkg 不读取 git 的 `insteadOf` 加速配置**，它用自己的下载器，因此 git 层面的
  加速对 vcpkg 无效。
- **本地 CONNECT 代理方案对 302 重定向无效**：curl 跟随重定向到
  `codeload.github.com` 后走直连，绕过代理。该方案已放弃。
- **thrift 编译需要 flex/bison/autoconf/automake/libtool/m4**，本机最初全缺，
  且 `sudo` 需要密码。已由项目所有者安装后解决。
- **踩坑记录（首次出现，避免重犯）**：
  1. 用 `clang-format` 格式化 `CMakeLists.txt` 会破坏 CMake 语法（产生
     `Parse error`）。CMake 文件不要交给 clang-format。
  2. 创建脚本后必须确认已同步到 WSL 再执行，否则会以「文件不存在」（退出码 127）
     立即退出，表现为“任务在跑”实为“什么都没跑”。
- 代码层面的三处真实缺陷（均由实际编译暴露并修复）：
  1. 链接 `unofficial::brpc::brpc` 前缺少 `find_package(unofficial-brpc CONFIG REQUIRED)`。
  2. 未链接 `rgbt_common`，导致找不到 `common/version.hpp`。
  3. brpc **不存在 `brpc::HttpService` 类**；HTTP 服务通过 RPC 服务的
     restful 映射或内置服务提供，且 `AddBuiltinServices()` 是私有方法，
     内置服务实际由 `ServerOptions::has_builtin_services`（默认 true）控制。
- 未决：`api/proto/` 正式契约、完整 Gateway、GTest 迁入 vcpkg、
  CI 中增加 brpc 与 Compose 校验，均属后续任务。

### 下一步

- 由项目所有者审阅 `src/gateway/` 与 `CMakePresets.json` 的改动。
- 决定后续方向：进入完整 Gateway 实现（含 proto 契约与错误码），
  或先补齐 CI 覆盖。

## TASK-005 实施记录（2026-09-16）

### 背景修正

TASK-004 只交付了「brpc 工具链可用性验证」，其原始目标中的公共 Proto、错误码、
健康检查与优雅退出并未实现（`api/proto/` 当时只有 `.gitkeep`）。而 TASK-005 的
登录接口需要挂在 Gateway 上，Gateway 此前只有一个冒烟程序，属硬阻塞。
经项目所有者确认，将 TASK-004 剩余项并入 TASK-005，一次完成
「Gateway 最小可运行服务 + 登录切片」。

### 完成

- `api/proto/gateway.proto`：`GatewayService` 定义 `Login`、`GetCurrentPlayer`、
  `Logout` 三个 RPC，以及 `PlayerInfo`、`Error`、`ErrorCode` 公共结构。
  字段只追加，符合 `docs/05-api-and-data.md` 第 7 节的演进规则。
- `include/common/token.hpp` + `src/common/token.cpp`：Token 生成与格式校验。
- `src/gateway/error.{hpp,cpp}`：错误码到 HTTP 状态码的映射。
- `src/gateway/player_directory.{hpp,cpp}`：测试身份目录（内置 3 个账号，
  含一个 disabled 账号用于覆盖失败路径）。
- `src/gateway/session_store.hpp`：会话存储接口，便于测试注入。
- `src/gateway/redis_session_store.{hpp,cpp}`：基于 hiredis 的实现，含连接超时、
  断线重连与幂等写入。
- `src/gateway/gateway_service.{hpp,cpp}`：三个接口的服务实现。
- `src/gateway/gateway_main.cpp`：服务入口，用 restful 映射暴露 HTTP 路径。
- `src/gateway/unit/gateway_service_test.cpp`：25 个单元测试。
- `scripts/verify-login.sh`：端到端验收入口。

### 决策

- 落地 `docs/07-open-decisions.md` D-002（经项目所有者确认）：
  - Token 使用**不透明随机串**而非 JWT：第一版不需要自包含性，会话集中在
    Redis，便于统一吊销与过期控制。
  - **不把账号密码存入 MySQL**，使用内置测试身份。
  - **不实现 Token 刷新**，只做短期会话。
- HTTP 状态码必须在传输层体现。brpc 默认把 protobuf 响应序列化为 JSON body，
  但 HTTP 状态恒为 200，因此显式调用
  `cntl->http_response().set_status_code(...)`。
- Redis Key 带环境和服务前缀并设置 TTL，符合 `docs/05-api-and-data.md` 第 4 节。
- Redis 不可用时服务**不退出**，请求返回 503；恢复后无需重启。避免依赖抖动
  导致服务反复重启。
- 测试代码使用独立告警策略（`rgbt_set_test_warnings`）：GTest 的 `TEST_F`
  宏生成静态函数，在 `-Werror -Wunused-function` 下会误报。

### 验证

- 命令：`cmake --preset brpc-debug && cmake --build --preset brpc-debug`
- 结果：配置与构建均成功，生成 `bin/rgbt_gateway` 与 `rgbt_gateway_tests`。
- 结果：proto 代码由 vcpkg 的 `protoc 33.4.0` 生成成功。
- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：**29 个测试全部通过**（gateway 25 个 + common 4 个）。
- 命令：启动网关并对各 HTTP 路径发请求（此时 Redis 未启动）
- 结果：HTTP 状态码与设计一致：
  - 错误密码 -> **401**
  - 正确凭据但 Redis 不可用 -> **503**，`reason=session_store_unavailable`
  - 无 Token -> **400**
  - 非法 Token -> **400**
  - `/health` -> 200，`/status` -> 200（brpc 内置）
  - 不存在的路径 -> 404
- 结果：收到 SIGTERM 后退出码 0，输出「已优雅退出」。
- 结果：网关启动日志正确报告 Redis 当前不可用，且进程保持存活。

### 未验证（缺 Docker）

- 以下端到端路径需要在 Redis 可用时验证，本次因 Docker Desktop 未启动而**未运行**：
  正常登录、登录幂等（同 request_id 返回同一 Token）、不同 request_id 得到不同
  Token、有效 Token 查询玩家、登出后 Token 失效、Redis 停止时返回 503、
  Redis 恢复后无需重启即可登录。
- 结论：`scripts/verify-login.sh` 已具备覆盖以上全部场景的能力，
  但**必须实际运行通过后才能判定 TASK-005 完成**。

### 问题与风险（本轮实际踩到并解决的）

- `player_directory` 只有显式构造函数而无默认构造，导致测试夹具把它当成员声明时
  产生「默认构造被删除」并级联出 20 条报错。已补默认构造并注释原因。
- `error` 模块原本放在 `src/common/`，却需要 include gateway 的生成代码，
  造成公共库反向依赖网关契约。已移到 `src/gateway/`。
- 忘记把 `token.cpp` 加入 `src/common/CMakeLists.txt`，导致链接期
  `undefined reference`。
- `find_package(GTest)` 原本放在 `tests/` 子目录，而 `src/gateway` 在其之前被
  处理，且子目录间不共享普通变量作用域，导致 gateway 测试目标未生成。
  已把依赖解析提到顶层 `CMakeLists.txt`。
- protobuf 生成的枚举含 sentinel 值，`switch` 未覆盖时触发 `-Werror=switch`。
- `verify-login.sh` 最初用 `ctest -R gateway` 过滤，但测试名为
  `GatewayServiceTest.*`（不含小写 gateway），过滤不到，已改为跑完整套件。

### 下一步

- 项目所有者启动 Docker Desktop，运行 `bash scripts/verify-login.sh` 完成验收。
- 通过后形成 `v0.1-bootstrap` 里程碑。
- 将 brpc 预设与 Compose 纳入 CI（已记入 TASK-006 Backlog）。

### 补充（同日稍后）：项目所有者端到端验收发现的问题与修复

#### 问题（由项目所有者实际运行 `verify-login.sh` 发现，共 3 项）

- 有效 Token 查询 `/api/v1/players/me` 返回 **400**，原因 `token_required`。
- 不存在的 Token 查询返回 **400**，而预期为 401。
- 登出接口返回 **200**，但原 Token 仍能通过鉴权。

**根因是同一个，不是三个**：brpc 只把 HTTP body（JSON）映射到 protobuf 字段，
**不会**把 HTTP 头映射进任何字段（见 brpc 官方文档 `http_service.md` 中 headers
与 query string 的说明）。因此 `Authorization: Bearer <token>` 里的 Token 从未
进入 `request->token()`，该字段恒为空字符串：

- 查询接口因 Token 为空报 `token_required`；
- 登出接口拿到空 Token，删除的是一个不存在的 key（no-op），于是返回 200
  而会话**实际未被删除**，后续请求仍然通过。

**这属于「依赖未经验证的框架行为」这一类错误**，与 TASK-004 中误以为 brpc 存在
`brpc::HttpService` 是同一类问题。

#### 修复

- 新增 `ExtractToken()`：按「请求体字段 -> `Authorization` 头 -> query string」
  顺序取值，`Bearer` 前缀大小写不敏感。`GetCurrentPlayer` 与 `Logout` 改用它。
- 实测验证三种取 Token 方式均可用：`Authorization: Bearer` 头、`?token=` 查询
  参数、请求体 JSON。

#### 同时修复的验收脚本缺陷

`verify-login.sh` 运行前不清理 Redis 残留。脚本使用固定的 `env_prefix`（`dev`）
与固定 `request_id`，上次运行留下的幂等映射会让本次登录返回旧 Token，而该 Token
对应的会话可能已被登出，表现为「刚拿到的 Token 查询失败」这种难以定位的偶发失败。
第一次失败后重跑即通过，即由此引起。

修复：运行前清理 `dev:gateway:*`，并在有效 Token 查询失败时打印实际 Token 与
Redis 中存在的 session Key。实测清理时报告残留 9 个与 5 个 Key。

#### 同时修复的仓库污染

因同步脚本中的 `chmod` 处理不当，**68 个文件的权限位**被从 `100644` 改为
`100755`（整个仓库被标成可执行）并一并提交。已纠正为「脚本 755、其余 644」，
并 amend 到原提交，避免留下一次纯噪音提交。

#### 结构调整（经项目所有者确认）

按项目所有者意见，测试位置与构建耦合做了如下调整：

- `src/gateway/unit/gateway_service_test.cpp`
  → `tests/unit/gateway/gateway_service_test.cpp`（内容零改动，git 识别为重命名）。
- `src/gateway/CMakeLists.txt` 删除测试块。该文件此前需要判断
  `RGBT_TESTS_AVAILABLE`，使**服务目录耦合了测试基础设施**，而出问题的只是
  CMake 作用域组织方式。
- 根 `CMakeLists.txt` 删除 `RGBT_TESTS_AVAILABLE` 缓存变量及判定分支。
- `tests/CMakeLists.txt` 自行解析 GoogleTest；`tests/unit/CMakeLists.txt` 用
  `if(TARGET rgbt_gateway_lib)` 判断是否加入 gateway 测试，因为该目标只在启用
  vcpkg 时存在。
- 头文件位置**未改动**：服务内部头文件继续与实现同目录。

新增文档规则：

- `docs/01-architecture.md` 第 11 节「代码与测试的放置规则」（原第 11 节顺延为
  第 12 节）。核心判据是**依赖方向**而非「它是不是头文件」：只有被两个及以上
  服务使用的代码进 `include/common/`；只有一个服务使用的留在 `src/<service>/`。
- `README.md` 目录图补充放置规则说明与指引。

#### 验证

- 命令：`bash scripts/verify-login.sh`
- 结果：**连续两次 31/31 通过**，零失败。覆盖正常登录、登录幂等、不同
  `request_id` 得到不同 Token、无效输入（400/401）、有效 Token 查询、非法与
  不存在的 Token、登出失效、Redis 停止时返回 503、Redis 恢复后无需重启网关
  即可登录、网关进程全程存活、SIGTERM 后退出码 0。
- 结果：`ctest --test-dir build/brpc-debug` 为 **29/29 通过**，与结构调整前
  数量完全一致。
- 结果：测试可执行文件位于 `build/brpc-debug/tests/unit/{gateway,common}/`，
  `src/` 下已无任何测试文件。
- 结果：既有 `debug`/`release`/`asan` 三预设回归通过。
- 结果：`cmake --preset brpc-debug` 全新配置与构建通过；`grep` 确认
  `src/gateway/CMakeLists.txt` 已无任何 GTest 相关判断。

#### 提交

- `556c7e2` 实现 Gateway 登录切片
- `da7245f` 从 HTTP 头提取 Token，并隔离验收脚本状态
- `fac9798` 测试归位到 `tests/unit/gateway` 并消除服务目录对测试依赖的耦合

#### 待确认

- 方案 C（把 proto 代码生成从 `src/gateway/CMakeLists.txt` 移到顶层或
  `api/proto/`）本轮**未做**，理由是与测试归位混在一起会降低可审阅性，
  且当前只有一个 proto 文件，规则重复问题尚未出现。建议加入 TASK-006 Backlog，
  待出现第二个 proto 时再评估。

## TASK-006 实施记录（2026-09-17）

### 背景

TASK-005 的登录使用代码内的明文测试身份，`players` 表不存在。Phase 1 需要可恢复的
数据基线，且「数据模型是否合理」必须靠真实读写验证，而不是只建空表。本任务把玩家
档案的读取链路接到 MySQL，同时确认「Gateway 直接读 `players` 表」这一临时越界的
边界与退出条件。

### 完成

- `migrations/002_create_players.sql`：`players` 表，主键 `player_id`，
  `uk_players_account` 唯一约束，`status` 默认 `active`，**无 password 列**。
- `migrations/003_create_match_results.sql`：`match_results` 表，主键 `match_id`
  （幂等业务键），`winner_id` 可空（平局）。
- `migrations/004_seed_test_players.sql`：3 行种子数据（alice/p-0001/active、
  bob/p-0002/active、carol/p-0003/disabled），文件头标注**仅用于开发环境**。
- 三个脚本均幂等：`CREATE TABLE IF NOT EXISTS` + `INSERT ... ON DUPLICATE KEY
  UPDATE`，并各自写入 `schema_migrations` 一行。
- 新增 `libmariadb` vcpkg 依赖；`src/gateway/CMakeLists.txt` 增加
  `find_package(unofficial-libmariadb CONFIG REQUIRED)` 与 `OpenSSL`。
- `src/gateway/mysql_connection.{hpp,cpp}`：连接选项（含连接/读/写超时）、
  参数化查询、断线重试。
- `src/gateway/player_reader.hpp` + `mysql_player_reader.{hpp,cpp}`：`PlayerReader`
  接口与 MySQL 实现。
- `src/gateway/player_directory.{hpp,cpp}` 改为**接口**，并拆出
  `in_memory_player_directory.{hpp,cpp}`（原实现）与
  `database_player_directory.{hpp,cpp}`（新实现），结构对齐既有 `SessionStore`。
- `src/gateway/password_hash.{hpp,cpp}`：OpenSSL EVP SHA-256，输出 64 位小写十六
  进制；`ConstantTimeEquals()` 做定长比较。
- `src/gateway/test_credentials.{hpp,cpp}`：测试身份的单一来源。
- `src/gateway/gateway_service.{hpp,cpp}`：新增 `kUnavailable` 分支，返回
  503 `player_store_unavailable`。
- `src/gateway/gateway_main.cpp`：装配 MySQL 连接、新增 `-mysql_*` gflags。
- `tests/unit/gateway/player_directory_test.cpp`：17 个新用例（假 `PlayerReader`
  覆盖读取失败、禁用、不存在、哈希一致性等）。
- `docs/adr/0002-gateway-temporary-player-ownership.md`：记录临时越界、3 个备选
  方案与退出条件。
- `docs/05-api-and-data.md` 增补两表结构与访问约定；`docs/07-open-decisions.md`
  的 D-002、D-003 移入「已确认」。
- `scripts/verify-login.sh`：新增 `--schema-only`、自动选择空闲端口、迁移与表结构
  断言、MySQL 停机与自愈断言。

### 决策

- **D-003 定为最小状态同步**（服务端权威快照 + 广播）：第一版不需要帧同步级别的
  带宽与回放能力，快照同步可验证性更高，后续如需帧同步再走 ADR。
- **密码用 SHA-256 摘要比较，不引入 bcrypt/Argon2**：这是测试身份的防明文措施，
  不是真实密码体系。真实注册与口令哈希留到有真实用户体系时再决策，避免现在引入
  未验证的密码学方案。
- **建表范围只做 `players` + `match_results`**：`player_stats`、`room_records`、
  `processed_events` 没有 Phase 1 的写入者，提前建表等于建空壳。
- **`match_results` 只建表不写入**：所有者是 Settlement（Phase 5），Phase 1 没有
  实现它的理由。
- **接受 ADR-0002 的临时越界**：Phase 1 若先实现 Player/State 服务，会把范围从
  「最小闭环」扩大成「多一个服务」，代价高于收益。ADR 已写明退出条件。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`cmake --preset brpc-debug && cmake --build --preset brpc-debug`
- 结果：配置与构建成功，退出码 0，无新增编译警告（`-Werror` 全程生效）。
- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：**46/46 通过**（TASK-005 时为 29 个，本次新增 17 个）。
- 命令：`bash scripts/check-format.sh`
- 结果：通过，检查 29 个文件。
- 命令：`bash scripts/verify.sh`
- 结果：`debug`/`release`/`asan` 三预设全部配置、构建、测试通过，各 4/4，
  未破坏既有骨架。
- 命令：`bash scripts/verify-login.sh`（需 Docker 与 `deploy/compose/.env`）
- 结果：**48/48 通过，退出码 0**。覆盖：迁移可重复执行且幂等、`schema_migrations`
  记录 3 条、`players` 有 3 行种子且 carol 为 `disabled`、`players` 表确认无
  password 列、登录成功、登录幂等、有效/非法/不存在 Token、登出失效、账号禁用
  返回 401、Redis 停机与自愈、MySQL 停机返回 503 与自愈、网关进程全程存活、
  SIGTERM 后退出码 0。
- 结果：文件权限为 70 个 `100644` + 6 个 `100755`，无误改权限。
- 复验（端口处理方式纠正后重跑，2026-09-17）：重新构建通过，`ctest` 仍为
  **46/46**；`bash scripts/verify-login.sh` **48/48 通过，退出码 0**。本轮 8080 被
  无关进程占用，脚本按预期选用 **18080**，说明端口选择不依赖程序默认值。
- 注：首次提交（以及一次 amend）时新增文件被记为 `100755`，原因是权限归一化脚本
  只遍历了 `git ls-files`（已跟踪文件），未覆盖新文件。修正为
  `git ls-files --cached --others --exclude-standard` 后，提交内权限为
  87 个 `100644` + 6 个 `100755`（仅 `scripts/*.sh`）。

### 问题与风险（本轮实际踩到并解决的）

- **MySQL 重启后不自动恢复（5/5 次返回 503）**。日志显示 `[mysql] connected ...`
  之后紧跟 `Query failed: connect failed: ... (115)`：`mysql_ping` 对已被服务端关闭
  的连接仍返回成功，真正的失败要到第一条语句才暴露，而旧代码把它当作终止性错误。
  修复：把 `Query` 拆为 `QueryOnce` + 「死连接重试一次」（`IsConnectionLostError`
  匹配 2006/2013/2003/2002），重试前先 `Disconnect()`。修复后 MySQL 重启的**第一次**
  登录即返回 200。这是「自愈必须实测、不能靠推断」的又一例证。
- **端到端全部 404 `{"detail":"Not Found"}`**。真实原因是 8080 被另一个无关项目
  （`zsvirt-observability` 的 uvicorn）临时占用，网关启动失败（`Fail to listen
  0.0.0.0:8080`），请求打到了那个服务上。**未**终止他人进程。
  停顿点：第一反应是把网关默认端口改成 18080，这是**修错了地方**——端口被谁占用是
  环境状态，不是程序的默认值问题；把非约定端口硬编码进程序，会让代码与
  `.env.example` 的约定值不一致，且下次占用另外的端口还会复发。
  最终处理：进程默认端口与 `.env.example` 保持约定值 **8080**，改由验收脚本在启动前
  用 `ss` 显式预检，从候选列表（8080 起）挑选空闲端口并用 `-port` 传入；同时把
  「端口冲突会表现为 404」这一现象写进 `.env.example` 与脚本注释，避免下次再被
  同一个现象误导。这是本轮**唯一一次在错误层面修问题并被纠正**的记录。
  复验时的实测证据：8080 仍被 `uvicorn app.main:app`（另一个无关项目，
  `pid=70938`）监听，脚本按预期跳过 8080 并选用 18080，全程无需修改程序默认值，
  证明「把冲突留给环境、把选择交给脚本」这个做法成立。
- **`MYSQL_USER: unbound variable`（退出码 1，脚本在 1b 段中止）**：脚本在读取
  `MYSQL_*` 之前从未加载 `deploy/compose/.env`。修复：在第 0 段加入
  `set -a; . ./deploy/compose/.env; set +a`。
- **文档改动被静默回滚（两次）**：上次会话直接在 WSL 侧编辑 `docs/`，随后同步脚本
  从 Windows 侧 tar 覆盖，改动无声丢失。已确立约定：**Windows 侧为编辑来源**，
  改完再同步进 WSL；反之必丢。发现后已重新核对 TASKS、07-open-decisions、
  ADR-0002、01-architecture、02-roadmap、README、CLAUDE 的实际内容。
- **`mysql/mariadb_stmt.h` 重复包含导致枚举重定义**：`mysql.h` 已包含
  `mariadb_stmt.h`，只需 include 前者。
- **`clang-format` 陷阱复发**：`clang-format -i CMakeLists.txt`
  会破坏 CMake 语法（`Parse error. Expected a newline`）。CMake 文件永远不交给
  clang-format。
- 测试身份的账号/密码分布在两处，必须保持同步：`src/gateway/test_credentials.cpp`
  与 `migrations/004_seed_test_players.sql`。已核对一致（alice/p-0001、bob/p-0002、
  carol/p-0003 disabled）。这是当前设计的已知重复点，不是缺陷但需在改动时注意。
- 风险：ADR-0002 允许 Gateway 直接读 `players` 表，属**有期限的例外**。若 Phase 1
  结束时 Player/State 仍未落地，必须回到 ADR 重新决策，不能让例外变成默认架构。

### 未做 / 留给后续

- 未实现真实注册与口令哈希（bcrypt/Argon2），仍为测试身份。
- 未建 `player_stats`、`room_records`、`processed_events`。
- 未实现匹配、房间、WebSocket、前端、Settlement。
- 未增加 MySQL 连接池；每个请求当前走一次查询路径，Phase 1 的并发量下够用，
  是否引入连接池留到有实测瓶颈时决策。
- CI 仍不覆盖 brpc 预设与 Compose（CI 无 vcpkg），已记入 Backlog。

### 下一步

- 由项目所有者审阅 Diff 并运行验收命令；确认后提交到
  `feat/task-006-schema-and-migrations` 并开 PR。
- TASK-006 验收通过后再输出 TASK-007（Match Service）任务单。

## 日志模板

```markdown
## YYYY-MM-DD

### 完成

- ...

### 决策

- ...

### 验证

- 命令：
- 结果：

### 问题

- ...

### 下一步

- ...
```
