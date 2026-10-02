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

### 补充（2026-09-22）：验收与合并

- 项目所有者实际执行了 `cmake --build --preset brpc-debug`、`ctest`（46/46）与
  `bash scripts/verify-login.sh`（48/48），全部通过。
- 已通过 **PR #3** 合并到 `main`，合并提交 `0e58a2f`。CI 对功能分支与 `main` 均为
  `success`。TASK-006 状态更新为**已完成**。
- 合并前修正的两处判断，记录如下：
  1. **端口处理方式纠正**：第一版把网关默认端口改成 18080 以避开本机冲突，属修错
     层次。默认值已回到约定值 8080，改为由 `scripts/verify-login.sh` 预检并自动挑选
     空闲端口。项目所有者指出「8080 只是被临时占用」，复验时确认该端口仍被无关的
     `uvicorn` 进程监听——这恰好证明了「冲突留给环境、选择交给脚本」是对的。
  2. **新增文件的权限位**：首次提交与第一次 amend 时新文件被记为 `100755`，原因是
     权限归一化脚本只遍历 `git ls-files`（已跟踪文件）。改为同时覆盖未跟踪文件后，
     提交内权限为 87 个 `100644` + 6 个 `100755`（仅 `scripts/*.sh`）。
- 新增本地约定：每个任务完成后额外输出一份 `interview-notes/<任务号>.md`
  （面试复习材料，已在 `.gitignore` 中，不作为交付物）。约定同时写入 `CLAUDE.md`。
- 下一步：TASK-007（Match Service）。

## TASK-007 实施记录（2026-09-22）

### 背景

Phase 1 的最小闭环要求「两个客户端能匹配进同一房间」。此前项目只有 Gateway 的登录
切片，而且**从未发起过一次服务间 RPC**——brpc 一直被用来把 Gateway 自己暴露成 HTTP
服务。因此本任务真正的难点不是匹配算法，而是把第一条跨服务链路（Gateway -> Match）
的契约、错误语义和故障行为建起来。

### 完成

- `api/proto/match.proto`：`MatchService` 契约（`EnqueueMatch`、`GetMatchStatus`、
  `CancelMatch`），含 `MatchState`（idle / queued / matched / timeout）与
  `MatchErrorCode`。这是项目第一个服务间契约。
- `src/match/match_queue.{hpp,cpp}`：匹配队列与配对逻辑，不依赖 brpc，可独立测试。
- `src/match/room_allocator.{hpp,cpp}`：房间分配接口 + Phase 1 占位实现
  （`room_id` 由 `match_id` 派生）。
- `src/match/match_service.{hpp,cpp}`：brpc 服务实现，只做协议转换与错误码映射。
- `src/match/match_main.cpp`、`src/match/CMakeLists.txt`：独立进程 `rgbt_match`。
- `api/proto/gateway.proto`：新增 `MatchStatusInfo` 与三个匹配接口，
  `ErrorCode` 追加 `RESOURCE_EXHAUSTED = 6`。
- `src/gateway/match_client.hpp` + `brpc_match_client.{hpp,cpp}`：Gateway 侧调用接口
  与 brpc 实现，错误码映射为 400 / 429 / 500 / 503。
- `src/gateway/gateway_service.{hpp,cpp}`：三个匹配接口；抽出 `ResolvePlayerId`
  统一鉴权，`GetCurrentPlayer` 一并改用它。
- `src/gateway/error.cpp`：`RESOURCE_EXHAUSTED -> 429`，并把限流纳入可重试。
- `tests/unit/match/match_queue_test.cpp`：22 个新用例。
- `tests/unit/gateway/gateway_service_test.cpp`：新增假 `MatchClient` 与 14 个匹配
  接口用例。
- `scripts/verify-match.sh`：端到端验收入口（30 项）。
- 文档：`docs/05-api-and-data.md`（接口表、匹配错误语义表、服务间契约约定）、
  `docs/01-architecture.md`（4.2 匹配数据流补充落地差异、状态归属表）、
  `docs/06-operations.md` 与 `deploy/compose/.env.example`（`MATCH_HTTP_PORT`）。

### 决策（项目所有者于 2026-09-22 全部选择 A，已写入 docs/TASKS.md）

1. **队列放 Match 进程内存**。与架构文档把队列所有者记为 Match 一致；若改为放
   Redis 并让 Gateway 直读，属于数据所有权变化，必须先写 ADR。
2. **配对只做 FIFO 两人一局**。当前没有任何分数体系，先实现分差放宽等于把未验证的
   评分模型固化进契约。
3. **房间分配抽象为 `RoomAllocator`**。Room 的契约应由拥有它的 TASK-008 定型，
   提前冻结会逼 TASK-008 迁就一个没有房间语义的接口。
4. **客户端轮询获取匹配结果**。WebSocket 属 TASK-009；轮询接口在 WebSocket 落地后
   仍作为兜底保留。
5. **`MATCH_HTTP_PORT=8082`**。

实现层面的决策：

- **单个互斥锁保护全部队列状态**。队列与结果表都以 `player_id` 为键，所有状态变更
  在同一把锁内完成，于是架构要求的「同一玩家不重复进入多个有效队列或匹配结果」
  由数据结构本身保证，而不是靠额外检查。**已知限制**：这把锁只在单进程内有效，
  多实例部署时不再成立，属 Phase 4。
- **时间由调用方注入**（`now_ms` 参数）。超时与结果过期因此可以用单元测试精确验证，
  不需要 sleep，也不会偶发失败。
- **惰性淘汰**：不创建定时器或后台线程，超时与过期只在每次调用时顺带结算。
- **重复入队不覆盖已有的匹配结果**。否则「配对成功」与「客户端下一次轮询」之间的
  竞态会让玩家丢掉已经配好的局。
- **`timeout` 与 `idle` 是两个状态**。前者提示「重新匹配」，后者是「可以开始匹配」，
  合并会让前端无法给出正确提示。
- **不自动重试跨服务调用**。失败按类型映射（503 / 429 / 400 / 500），由调用方决定
  是否重发；在传输层无脑重放会掩盖真实的容量问题。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`cmake --preset brpc-debug && cmake --build --preset brpc-debug`
- 结果：配置与构建成功，退出码 0，**无新增编译警告**（`-Werror` 生效）。
- 命令：`ctest --test-dir build/brpc-debug`
- 结果：**87/87 通过**（TASK-006 为 46 个，本次新增 41 个：match 22 个 +
  gateway 匹配接口 14 个 + 错误码映射与其它 5 个）。
- 命令：`bash scripts/check-format.sh`
- 结果：通过，检查 40 个文件。
- 命令：`bash scripts/verify.sh`
- 结果：`debug`/`release`/`asan` 三预设各 4/4 通过；首次因格式检查失败，
  格式化后复跑通过。
- 命令：`bash scripts/verify-match.sh`（需 Docker；首次因 Docker 未启动而中止，
  项目所有者启动 Docker Desktop 后复跑）
- 结果：**30 项全部通过，退出码 0**。覆盖：未入队为 `idle`、缺少 Token 返回 400、
  alice 入队为 `queued`、bob 入队后双方立即配对且 `match_id`/`room_id` 一致、
  `player_ids` 恰好两人、重复入队幂等返回同一 `match_id`、结果保留期过后回到
  `idle`、第二次匹配得到新的 `match_id` 与 `room_id`、取消幂等、超时状态为
  `timeout` 且过期后回到 `idle`、Match 停机时入队与查询均返回 503
  `match_unavailable`、Match 恢复后无需重启 Gateway 即可匹配、Gateway 全程存活、
  两个进程收到 SIGTERM 后退出码均为 0。

### 问题与风险（本轮实际踩到并解决的）

- **`response->mutable_error()` 会提前创建 error 子消息**。三个用例
  （`GetCurrentPlayerSucceedsWithValidToken`、`GetMatchStatusReturnsIdleForNewPlayer`、
  `CancelMatchIsIdempotentWhenNotQueued`）失败，原因是断言「成功响应不应带 error」。
  根因：把 `response->mutable_error()` 直接交给辅助函数，即使函数不写入，
  **protobuf 的 `mutable_*` 也会实例化该子消息**，于是成功响应里带上一个空 error，
  客户端会据此误判为失败。修复：先写入局部 `v1::Error`，只在失败时拷贝回响应。
  这是本轮最值得记住的一个坑，因为它**只在单元测试断言 `has_error()` 时才会暴露**，
  端到端脚本看 HTTP 状态码和 `state` 字段是发现不了的。
- **brpc 的 restful 映射按路径分派，不支持按 HTTP 方法分派**。文档原先把取消匹配
  设计为 `DELETE /api/v1/matches/current`，与查询共用路径，这在 brpc 下不可实现。
  改为 `POST /api/v1/matches/current/cancel` 并同步更新 `docs/05-api-and-data.md`。
  这与 TASK-005 中「brpc 不存在 `brpc::HttpService`」属于同一类问题：
  **框架行为必须实测，不能按常识假设**。
- **端到端脚本第一版用 carol 作为「第三个玩家」，结果 10 项失败**。carol 在种子数据
  里是 `disabled`，专门用于覆盖「禁用账号被拒绝」，无法登录。这暴露了一个覆盖缺口：
  **种子数据只有两个可用身份，而「第三个玩家不会被并入已配满的局」需要三个**。
  当前处理：端到端改为验证强度接近的「已完成的局不会被后续请求复用」，
  该不变量由单元测试 `CompletedMatchDoesNotAbsorbLaterPlayers` 覆盖，
  脚本结尾显式打印这个未覆盖项，不假装已覆盖。是否新增第四个启用身份待项目所有者决定。
- **`std::signal` 不是 `std::` 的成员**：`<signal.h>` 只提供 `::signal`。
  改为 `::signal` 与既有 `::usleep` 保持一致。
- **格式化**：5 个新文件未过 `clang-format --dry-run --Werror`，已在 WSL 格式化后
  把结果写回 Windows 副本（Windows 侧是编辑来源，只改一边会在下次同步时丢失）。
- **`verify.sh` 会 `rm -rf build`**，因此它会清掉 brpc 预设的构建产物；后续需要
  `brpc-debug` 的命令要重新构建。本轮把它放在端到端验收之前执行以避开这个影响。
- **辅助文件第三次混进提交**：本轮把 `scripts/_commit7.txt`（提交信息文件）提交了
  进去。前两次是 `scripts/_*.sh`，因此 `.gitignore` 的规则写成了只匹配 `.sh`——
  而这次的 `.txt` 不在范围内。修复：规则改为 `scripts/_*`（匹配全部扩展名），
  并把该文件从提交中删除。**这条经验值得记住：忽略规则要按「意图」写，而不是按
  当时那一个文件的扩展名写**；只要意图是「以下划线开头的都是本机临时文件」，
  模式就应该覆盖全部扩展名。
- 与之相关的一个脚本 bug：剔除文件的断言最初用 `git diff --cached --name-only`
  判断，会把**期望中的删除**也当成违规而报错。改为按 `--name-status` 只看
  `A/M/R`，允许 `D`。

### 未做 / 留给后续

- 不实现 Room/Battle Service；`room_id` 是占位号，不含任何房间状态（TASK-008）。
- 不实现 WebSocket 推送（TASK-009）。
- 不实现分差放宽、等待时间放宽、多实例分片。
- 不实现队列的 Redis 快照与进程重启恢复（Phase 2）。**Match 重启即丢失排队状态**，
  这是当前明确的已知限制。
- `src/match/CMakeLists.txt` 与 `src/gateway/CMakeLists.txt` 的 Protobuf 代码生成
  写法**已经重复**（第二个 proto 出现），本轮故意不抽取以保持提交可审阅，
  已记入 `docs/TASKS.md` 的 Backlog。
- 端到端的「第三个玩家」覆盖缺口（见上）。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-match.sh`；确认后开 PR。
- TASK-008（Room/Battle Service）在 TASK-007 验收通过后开单。

## 2026-10-02：范围裁剪（TASK-012）

### 背景

项目所有者在范围复盘中确认：原路线图的 Phase 4（等 etcd、多实例）与 Phase 5
（Kafka、Settlement Worker），以及独立的 Player/State 服务，与本项目真正要证明的
能力不匹配——没有真实的异步消费者，也没有真实的多实例服务发现需求。
**这些内容此前只写在对话里，文档中仍表述为"后续阶段"，因此后续 AI 仍会按计划实现。**

### 决策

新增 [ADR-0003：范围裁剪](adr/0003-scope-reduction.md)，把下列内容从"延后"改为
**"不实现"**：

- Kafka 及任何消息队列、领域事件、事件回放、异步结算 Worker
- etcd 及任何服务注册、发现、租约机制
- 多实例部署与水平扩展
- Kubernetes / k3s / 容器编排
- 独立的 Player/State 服务
- 独立的 Settlement 服务
- 排行榜
- 匹配分差放宽 / MMR / 评分体系

连带决定：

1. **服务集合固定为三个**：Gateway、Match、Room/Battle。
2. **`players` 表所有者改为 Gateway**（正式归属）。ADR-0002 的"临时例外"退出条件
   由 ADR-0003 取消；实现上 `PlayerReader` 接口不变。
3. **`match_results` 表所有者改为 Room/Battle**，对局结束时**同步幂等**写入
   （以 `match_id` 为幂等业务键），不经过任何异步链路。Settlement 服务不实现。
4. **路线图重排**：原 Phase 4/5 取消，原 Phase 6 的可靠性验证内容提为新的
   Phase 4，Phase 5 为工程收口。
5. **产品目标调整**：保留登录、匹配、进房、最小对战、断线重连、对局结果写入与查询；
   移出排行榜、事件回放、异步结算。

### 同步修改的文档

`CLAUDE.md`（规则、技术方向、服务边界、数据所有权、准入条件、当前阶段，并新增
「范围裁剪」硬性约束一节）、`README.md`、`docs/00-charter.md`、
`docs/01-architecture.md`、`docs/02-roadmap.md`、
`docs/04-quality-and-observability.md`、`docs/05-api-and-data.md`、
`docs/06-operations.md`、`docs/07-open-decisions.md`（D-004、D-005 改为已关闭）、
`docs/README.md`、`docs/TASKS.md`、`docs/adr/0001`（标注部分替代）、
`docs/adr/0002`（取消退出条件）、`deploy/compose/.env.example`（移除
`KAFKA_BROKERS` 与 `ETCD_ENDPOINTS`）、`migrations/002`、`003` 的头部说明，
以及 `CMakeLists.txt` 与 `src/` 中 5 处指向已取消阶段的注释。

### 未做的事（有意保留）

- **没有删除 `migrations/002`、`003`**：迁移只追加，删除属破坏性变更。
  `match_results` 保留表定义，只是所有者与写入者变更。
- **没有改 SQL 的 DDL 与列 `COMMENT`**：只改文件头部注释。改 DDL 会让新库与已有
  数据卷产生 schema 漂移，而 `CREATE TABLE IF NOT EXISTS` 不会修正已存在的表。
- **没有重编号 TASK-008~011**：它们只是计划，但重编号会迫使 `api/proto/` 与 `src/`
  中大量指向 TASK-008/009 的注释一起改动，把一次纯文档提交变成跨模块改动。
  因此本任务取编号 TASK-012，并在 `docs/TASKS.md` 中注明它实际先于 TASK-008 执行。

### 验证

- 命令：全仓库检索 `Kafka`、`etcd`、`Settlement`、`Player/State`、`多实例`。
- 结果：剩余出现处均为「非目标」说明、ADR 的历史论证，或 `docs/devlog.md` 中
  2026-09-22 及以前的历史记录；**不存在实现指引**。
- 结果：`deploy/compose/.env.example` 已无 Kafka 与 etcd 变量。
- 结果：服务列表在 `CLAUDE.md`、`README.md`、`docs/01-architecture.md`、
  `docs/05-api-and-data.md` 四处口径一致，均为三个服务。
- 本任务**只改文档与注释，不改任何行为代码**，因此未运行构建与测试；
  合并前由项目所有者运行一次 `bash scripts/verify.sh` 确认无意外影响。

### 下一步

- 由项目所有者审阅 Diff 并确认合并。
- 合并后按 `docs/TASKS.md` 的 Phase 1 拆分进入 TASK-008（Room/Battle Service），
  其范围已包含「对局结束时同步幂等写入 `match_results`」。

## 2026-10-02：取消面试复习笔记约定

### 决策

取消 `CLAUDE.md` 中「每完成一个任务额外输出一份 `interview-notes/<任务号>.md`」的
约定（该约定于 2026-09-22 的 TASK-006 收尾时引入）。迭代流程简化为
「按迭代计划逐步实现 -> 项目所有者确认」，不再附带面试准备材料。

理由由项目所有者给出：面试准备材料不属于迭代交付物，把生成它们嵌入每轮任务
流程会稀释迭代本身的产出。

### 完成

- 删除 `interview-notes/` 整个目录（`README.md`、`00-overview.md`、
  `TASK-006.md`、`TASK-007.md`），Windows 备份副本与 WSL 正式目录同时删除。
- `CLAUDE.md`：移除原「面试复习笔记」一节，替换为「迭代交付物范围（2026-10-02 起）」，
  明确禁止再生成此类文件。
- `.gitignore`：保留 `interview-notes/` 规则并改写注释为"防御性保留"，防止目录被
  重建后误提交。

### 验证

- 结果：`git ls-files interview-notes/` 为空——该目录**从未被 git 跟踪**，
  因此也从未出现在任何 PR 或远程分支中。
- 结果：删除后 `Test-Path` 与 WSL 侧 `test -e` 均确认目录不存在。

## TASK-008 实施记录（2026-10-02）

### 背景

TASK-007 之后「匹配成功」只得到一个由 `match_id` 派生的占位 `room_id`，
**不产生任何房间状态**。Phase 1 要求「两个客户端完成一场最小对战」，
所以必须有一个服务真正持有房间的权威状态、推进对局并产出对局结果。

### 对局规则（项目所有者确认，决策 A）

两人一局；服务端 10 Hz 推进（100 ms/帧）；输入只有「攻击」，每次扣对手 10；
HP 初值 100；一方归零即结束；600 帧（60 秒）后按 HP 判定，相同为平局。
数值集中在 `src/room/room_types.hpp`，并由单元测试锁定。

### 完成

- `api/proto/room.proto`：`CreateRoom` / `JoinRoom` / `SubmitInput` /
  `GetRoomState` / `GetMatchResult`，以及 `RoomState`、`FinishReason`、`RoomErrorCode`。
- `src/room/`：`room_types`、`battle_room`（生命周期与帧推进）、`room_manager`
  （注册表、结果落库、回收）、`match_result_writer` + `mysql_match_result_writer`、
  `room_service`（brpc 服务）、`room_main`（进程入口 + 50 ms 推进线程）。
- `src/match/brpc_room_allocator.*`：真实房间分配。`RoomAllocator::Allocate` 增加
  `player_ids` 参数——房间需要知道本局都有谁，才能校验加入者的身份。
- `MatchQueue` 改为**两阶段配对**：锁内取人并标记分配中 → 锁外 brpc 分配 →
  锁内提交。分配失败时玩家退回队首。
- `src/gateway/`：`room_client.hpp` + `brpc_room_client.*`；四个 HTTP 接口。
- `include/common/mysql_connection.*`：原在 `src/gateway/`，现被两个服务使用，
  按放置规则移入公共目录，单独成目标 `rgbt_common_mysql`（它依赖 vcpkg，
  不能并入默认预设也会构建的 `rgbt_common`）。
- `cmake/generate_service_proto.cmake`：`room.proto` 是第三个 proto，触发了
  Backlog 里记录的抽取条件，三个服务现在共用同一份代码生成逻辑。
- `tests/unit/room/`（24 个用例）、match 与 gateway 的新用例。
- `scripts/verify-room.sh`：端到端验收入口。
- 删除 `src/player/.gitkeep` 与 `src/settlement/.gitkeep`，与 ADR-0003 对齐。

### 决策

- **`FINISHING` 是独立状态**。已分出胜负但结果未落库时停在这里并按 1 秒间隔重试，
  查询返回 `result_pending`（503）。写成功才进 `FINISHED`。理由：若直接用内存里的
  胜负回给客户端，进程重启后客户端会拿到一个数据库里查不到的结论。
- **`ABORTED` 不写结果**。等待玩家加入超时的房间写一行 winner 为空的记录，
  等于把「没打成」伪装成「打平了」。
- **结果写入失败时无限重试，不设放弃上限**。不可再生的数据优于内存占用；
  代价是 MySQL 长期不可用会让 FINISHING 房间累积，已在 TASKS.md 记为已知限制。
- **持锁期间绝不调用 MySQL**。写结果与读结果都在锁外完成，只在读取/更新房间状态
  时短暂持锁。否则一次 MySQL 超时会把所有房间的 tick 一起卡住。
- **单次 Tick 限制最大补偿帧数**（`kMaxCatchUpFrames`）。进程被挂起后一次性补完
  几百帧会造成 CPU 尖峰，而这段时间的输入本来也已失去意义。
- **房间推进由 Room 自己的线程驱动**，不依赖客户端请求。否则双方都不请求时对局
  会永远停在原地。
- **`connected` 表达「是否在房间内」，不是「网络是否连通」**。TASK-008 没有断线
  重连（属 Phase 2），PLAYING 之后恒为 true；WAITING 期间未加入的玩家为 false。
- **`BattleRoom` 允许移动但禁止拷贝**。允许移动是为了测试能有一个「构造并返回」的
  工厂函数；禁止拷贝是因为房间状态被悄悄复制成两份比编译错误严重得多。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`cmake --preset brpc-debug && cmake --build --preset brpc-debug`
- 结果：配置与构建成功，无新增编译警告（`-Werror` 全程生效）。
- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：**144/144 通过**（TASK-007 时为 89 个，本次新增 55 个）。
- 命令：`bash scripts/check-format.sh`
- 结果：通过，检查 58 个文件。新文件首次提交时 13 个未过格式检查，
  已在 WSL 格式化后回写 Windows 副本。
- 命令：`bash scripts/verify-room.sh --no-docker`
- 结果：**全部通过，退出码 0**。实测覆盖：双方拿到相同 match_id/room_id；
  房间的 match_id 与匹配结果一致（幂等键生效）；alice 进房 `waiting`、
  bob 进房 `playing`；重复加入幂等；缺 Token 返回 400；
  一次攻击使对手 HP 100 → 90 且不继续下降；提交 10 次攻击后 `state=finished`、
  `winner_id=p-0001`、`finish_reason=hp_zero`；结束后提交输入返回
  **409 room_already_finished**；结果可查询且 `match_results` 只有 **1 行**，
  重复查询后仍为 1 行；不存在的 match_id 返回 **404 result_not_found**；
  Room 停机时查询返回 **503 room_unavailable** 且分配失败时两人留在队列
  （alice=queued bob=queued，**未产生半成品配对**）；Room 重启后**仅靠轮询**
  即重新匹配成功；MySQL 停机期间对局正常结束，结果查询返回
  **503 result_pending** 且响应体不含 result 字段；MySQL 恢复后
  **无需重启 Room** 即自动落库且 winner 正确；三个进程 SIGTERM 退出码均为 0。

### 问题与风险（本轮实际踩到并解决的）

- **brpc 也不把 query string 映射进 protobuf 字段**。这与此前记录的「brpc 不映射
  HTTP 头」是同一类问题，但这次踩在查询参数上：`GET /api/v1/results?match_id=...`
  返回 `400 match_id_required`，因为 `request->match_id()` 永远是空字符串。
  更隐蔽的是——**它在单元测试里完全测不出来**，因为单元测试直接调用服务、
  没有 HTTP 上下文。是端到端验收脚本第一次真实跑 HTTP 才暴露出来的。
  修复：新增 `ExtractQueryParam`，与 `ExtractToken` 同一套取值顺序
  （请求体优先、其次 query string）。教训：凡是来自 HTTP 而**不在 body JSON 里**
  的输入，都必须显式读取，不能指望框架映射。
- **brpc 的 restful 映射不支持 `{name}` 路径参数**。查证 brpc 官方文档
  （`docs/cn/http_service.md` 的 Restful URL 一节）确认：只支持 `*` 通配符，
  匹配部分通过 `unresolved_path()` 取回。而 `/api/v1/rooms/*` 会与
  `/api/v1/rooms/join`、`/api/v1/rooms/input` 这类固定子路径产生歧义。
  因此 room_id 与 match_id 最终走**查询参数**。这不是偏好，是框架约束，
  与 TASK-007 取消匹配路径的调整性质相同。
- **配对失败后玩家会卡在队列里**。由验收脚本第 11 步暴露：Room 重启后
  brpc channel 惰性重连，第一次 `CreateRoom` 必然报
  `[E112]Not connected to 127.0.0.1:8083 yet`，玩家退回队列；而配对原本只在
  「有人入队」时触发，于是这两人要等到下一个新玩家出现才可能被配上。
  在有真实流量的环境里它会自愈，所以很容易被忽略——**是端到端脚本把它逼出来的**。
  修复：新增 `retry_pairing_` 标记 + `RetryPairingIfNeeded`，由客户端持续轮询的
  `GetStatus` 顺带重试一次，并且只在真的有待重试配对时才发起远程调用
  （有单元测试锁定「无待重试时不发 RPC」）。
- **`<mysql/mysql.h>` 是靠传递依赖碰巧编译过的**。把 `mysql_connection` 移到
  `include/common/` 后单独成目标，失去了 gateway 链接 brpc/protobuf 带来的
  `${prefix}/include` 搜索路径，于是编译失败。vcpkg 的 `unofficial::libmariadb`
  暴露的是 `${prefix}/include/mysql`，正确写法是 `<mysql.h>`。原写法属于隐患，
  换个链接组合就会断。
- **全仓库 `chmod 644` 会把 `build/` 下的可执行文件一起改掉**。这个坑 devlog 里
  记录过，本轮又踩了一次：`rgbt_common_tests` 是 9 月 18 日的陈旧二进制，
  源码未变所以没有重新链接，执行位被改掉后 `ctest` 报 `BAD_COMMAND`。
  权限归一化必须限定在纳入版本管理的源文件上，绝不能遍历整个仓库。
- **`GatewayServiceImpl` 新增依赖后，构造函数签名变化会波及所有测试夹具**。
  这是签名变更的正常代价，本轮一次性改到位（`FakeRoomClient` 同时补上了
  10 个 Gateway 侧用例）。
- **测试里把 `Tick` 当成「一步跳到第 600 帧」是错的**。单次 Tick 有
  `kMaxCatchUpFrames` 上限，超过的部分被有意丢弃。最初写的 11 个用例因此失败；
  改为按帧推进的辅助函数后通过。测试必须按与生产一致的粒度调用被测代码。

### 未做 / 留给后续

- 不实现 WebSocket 推送（TASK-009）；客户端通过轮询获取房间状态。
- 不实现断线重连与宽限期（Phase 2）。
- 房间快照只存在内存的环形缓冲里，**Room 进程重启即丢失**（Phase 2）。
- 已结束但未落库的对局只存在内存里，进程重启会丢失（Phase 2）。
- 匹配队列与房间都在进程内存，单进程内有效（ADR-0003 已确认不做多实例）。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-room.sh`；确认后开 PR。
- 通过后进入 TASK-009（Gateway WebSocket 路由）。

## 2026-10-02：补上第三个启用测试身份（dave / p-0004）

### 背景

TASK-007 的记录里留了一个待决问题（见本文档 2026-09-22 的「问题与风险」一节）：

> **种子数据只有两个可用身份，而「第三个玩家不会被并入已配满的局」需要三个**……
> 是否新增第四个启用身份待项目所有者决定。

这个决定一直没有结论，因此 TASK-008 的端到端脚本里仍然把两条不变量打印成
「未在端到端覆盖」：匹配的「第三个玩家不会被并入已配满的局」、
房间的「非本局成员无法加入」。项目所有者指出该决定悬空，本轮补上。

### 完成

- `migrations/004_seed_test_players.sql`：新增 `dave / p-0004 / Dave / active`。
- `src/gateway/test_credentials.cpp`：新增 `dave / dave_dev_pw`。
- `scripts/verify-login.sh`：种子行数断言 3 -> 4；新增 dave 状态为 active 的断言。
- `scripts/verify-match.sh`：新增第 7b 节，用 dave 真正验证
  「第三个玩家不会被并入已配满的局」；头部注释与结尾的「未覆盖项」一并更新。
- `scripts/verify-room.sh`：用 dave 真正验证「非本局成员无法加入」返回
  `400 not_a_member`；结尾的「未覆盖项」一并更新。
- `tests/unit/gateway/player_directory_test.cpp`：把「档案有、代码里没有凭据」
  那个用例的账号从 `dave / p-0004` 改成 `erin / p-0005`。**不改就测不出原意**：
  dave 从本轮起是真实存在的测试身份，用它构造不出「配置不一致」这个场景。
- `api/proto/room.proto`：新增 `ROOM_NOT_A_MEMBER` 与 `ROOM_NOT_PLAYING` 两个错误码；
  `room_service.cpp` 改用它们；`room_client.hpp` / `brpc_room_client.cpp` /
  `gateway_service.cpp` 同步映射。

### 问题与风险（本轮实际踩到并解决的）

- **`verify-match.sh` 被 TASK-008 改坏了，而且不是立刻能看出来的那种坏**。
  TASK-007 时房间分配是 Match 进程内的占位实现，所以那个脚本只启动 Match 与
  Gateway；TASK-008 把分配换成真实的 brpc 调用后，**脚本没起 Room**，
  于是所有配对都失败，表现为「玩家一直 queued」。重新跑它才发现，
  这也说明**新增服务依赖时必须回头检查既有验收脚本**，不能只验新脚本。
  修复：脚本增加 Room 的端口预检、启动、就绪等待与优雅退出检查。
- **调用方只映射错误码，写在 `reason` 里的区别跨进程后必然丢失**。
  端到端断言「非本局成员返回 400 not_a_member」失败，实际返回的是
  `room_invalid_argument`：Room 侧确实把 `reason` 设成了 `not_a_member`，
  但 `BrpcRoomClient` 按**错误码**映射，而这两个场景共用
  `ROOM_INVALID_ARGUMENT`，区别只存在于自由文本里，于是被丢掉了。
  修复：在契约里给它们**独立的错误码**（`ROOM_NOT_A_MEMBER` / `ROOM_NOT_PLAYING`），
  而不是继续依赖 `reason`。教训：需要调用方区分的语义必须体现在**结构化字段**上；
  放在自由文本里等于指望每一层都恰好把它透传下去。
- **`--no-docker` 会跳过迁移应用，因此种子数据的新增行不会生效**。
  第一次跑 `verify-match.sh --no-docker` 时 dave 登录失败，一度以为是凭据没配对，
  实际是那一轮没有重新应用 `004_seed_test_players.sql`。已在脚本头部写明这个前提。

### 决策

- **扩展现有的 `004_seed_test_players.sql`，而不是新增 `005_…`**。理由：该文件是
  **开发用种子数据**，不是 schema 迁移；它的头部注释本来就是「账号 ↔ player_id」
  的唯一对照表，把测试身份集合拆到两个文件会让这张表在两边都不完整。脚本每次运行
  都会重新应用 `migrations/*.sql`（幂等），因此已有数据卷也会拿到 dave 这一行。
- **不把 carol 改成可用**。它承担「账号被禁用」这条失败路径的覆盖，改成 active
  会丢掉一个已经有效的用例。

### 验证

- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：通过（`player_directory_test` 改用 erin 后仍覆盖原场景）。
- 命令：`bash scripts/verify-login.sh`、`bash scripts/verify-match.sh`、
  `bash scripts/verify-room.sh`
- 结果：见各脚本输出的真实结论；匹配与房间的脚本不再打印「未覆盖项」。

## TASK-009 实施记录（2026-10-02）

### 背景与范围变更

原定范围是「Gateway WebSocket 路由与房间消息」。按项目纪律在编码前先核实框架能力，
结论是这条路在当前技术栈下走不通：

- **brpc 1.16.0 完全不支持 WebSocket**。在 `docs/`、`include/brpc/`、`src/` 三处
  检索 `websocket` 全部为空；唯一的命中来自 Thrift（`TThriftWebSocketServer`），
  那是 brpc 的传递依赖，与 brpc 无关。
- **在 brpc 之上自实现 WebSocket 也不成立**。brpc 只提供
  `Controller::CreateProgressiveAttachment()`，它能持续写响应体，但无法接管底层
  socket。WebSocket 要求握手后把同一连接切成帧协议并**读取**客户端帧，而在 brpc 下
  这些字节会被当作新的 HTTP 请求解析。
- **没有现成的 WebSocket 库**。vcpkg 只装了 brpc 依赖所需的 boost 子集，没有 beast；
  websocketpp / uwebsockets / libwebsockets 均未安装。

同时确认 brpc **官方支持 SSE**：`docs/cn/http_service.md` 第 336–350 行明确写了
「利用该特性可以轻松实现 Server-Sent Events(SSE) 服务」，且源码包内的官方示例
`HttpSSEServiceImpl` 就是一个普通的 protobuf service 方法——与现有
`GatewayServiceImpl` 形态完全一致。

据此，项目所有者确认改用 SSE，新增 **ADR-0004** 记录这次方向变更与备选方案的取舍。

### 完成

- `docs/adr/0004-sse-instead-of-websocket.md`：方向变更的完整论证。
- `api/proto/gateway.proto`：新增 `StreamEvents` RPC 与请求/响应消息。
- `src/gateway/event_sink.hpp`：事件出口接口（为了能脱离 HTTP 上下文做单元测试）。
- `src/gateway/stream_hub.hpp/.cpp`：订阅表与推送驱动。只对有订阅者的房间轮询、
  帧号变化才推送、同房间多订阅者扇入成一次调用、写失败即清理订阅。
- `src/gateway/gateway_service.cpp`：`StreamEvents` 处理函数 +
  `ProgressiveAttachmentSink`（brpc 的实现方）。
- `src/gateway/gateway_main.cpp`：接线订阅表与推进线程，新增
  `-stream_poll_interval_ms`、`-stream_heartbeat_interval_ms`；
  优雅退出顺序改为「停线程 → 关所有长连接 → 停服务器」。
- `tests/unit/gateway/stream_hub_test.cpp`：13 个用例；`gateway_service_test.cpp`
  新增 6 个 SSE 失败路径用例。
- `scripts/verify-stream.sh`：端到端验收。
- `deploy/compose/.env.example`：删除 `GATEWAY_WS_PORT`（从未被任何进程读取）。
- 全仓库 47 处 WebSocket 表述同步为 SSE。

### 决策

- **只在帧号变化时推送**。每 100 ms 推一条完全相同的快照是纯粹的空转，且会让
  客户端误以为有更新。但「进入结束态」是必须送达的事件，即使帧号没变。
- **结束事件之后主动关闭流**。`finished` 与 `aborted` 都是终点，之后这条流没有
  内容了；不关闭会让客户端一直等。
- **Room 抖动时保持连接、也不推 error 事件**。房间查不到可能只是暂时不可用，
  关掉连接会强迫客户端重连；而每次轮询都推一条 error 会刷屏。恢复后继续推。
- **`session.ready` 在第一次 Tick 里发，而不是在 Subscribe 里发**。
  Subscribe 运行在 brpc 的请求处理中，那时响应还没提交；brpc 文档说明只有
  `done` 之后写入的数据才会立刻以 chunked 形式发出。
- **订阅要校验成员身份**。`room_id` 由客户端提供，不校验的话任何登录用户只要
  拿到（或猜到）room_id 就能长期订阅别人的房间、看到对方的血量。
  随机生成的 ID「难猜」不是访问控制。
- **`match.updated` 不做推送**。它需要 Gateway 为每个在线连接轮询 Match，
  而匹配状态变化频率很低；客户端继续轮询 `/api/v1/matches/current`。
  把这条件为「未实现」写进文档，而不是留一个含糊的"计划中"。
- **保留全部轮询接口**。断线或代理不支持 SSE 时它们是唯一可用的路径。
- **删除而不是保留 `GATEWAY_WS_PORT`**。它从未被任何进程读取；在改用 SSE 之后
  更没有任何用途。留着只会让下一个人以为有个 WebSocket 端口可以用。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`cmake --preset brpc-debug && cmake --build --preset brpc-debug`
- 结果：构建成功，无新增编译警告（`-Werror` 全程生效）。
- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：**163/163 通过**（TASK-008 时为 144 个，本次新增 19 个）。
- 命令：`cmake --preset brpc-debug -B build/brpc-asan -DRGBT_ENABLE_SANITIZER=ON`
  然后运行 `rgbt_gateway_tests`
- 结果：**91/91 通过，ASan 与泄漏检测均无报告**。
  必须单独配这个目录：`asan` 预设不启用 vcpkg，因此根本不构建 Gateway 与它的测试。
- 命令：`bash scripts/verify-stream.sh`
- 结果：**全部通过，退出码 0**。实测覆盖：订阅后收到 `session.ready`（含
  `player_id` 与 `room_id`）；帧推进产生 **13 次** `room.state` 推送且 `sequence`
  为 `0..12` **严格单调递增**；攻击造成的血量变化在推送中可见（`p-0002` hp=90）；
  空闲时收到心跳注释 `: ping` 且**未污染事件流**；结束后收到 `room.finished`
  且服务端**主动关闭连接**（curl 自行退出）；非本局成员被拒绝
  `400 not_a_member`；缺 Token / 缺 room_id 各 400；房间不存在 404；
  轮询接口仍返回 200；**持有一条打开的 SSE 长连接时，Gateway 仍在 10 秒内以
  退出码 0 退出**。
- 命令：`bash scripts/verify-room.sh`、`bash scripts/verify-match.sh`
- 结果：全部通过（回归：推送上线没有破坏轮询与匹配链路）。

### 问题与风险（本轮实际踩到并解决的）

- **测试自己抓到了 use-after-free**。写 `StreamHub` 的单元测试时，我把可观测状态
  放在 `FakeSink` 内部，而对局结束与写失败这两条路径会**销毁 sink**。结果那两个
  用例直接 SegFault。这类问题在普通构建下常常"看起来通过"（内存还没被复用），
  只在 ASan 或恰好被覆盖时才暴露。修复：把可观测状态移到 sink 之外的共享对象里，
  并在文件头写明为什么必须这样做。
- **长连接会拖住优雅退出，且这一点必须被验证而不是假设**。
  `brpc::Server::Stop()` 要等响应结束，而 SSE 响应不会自己结束。因此退出顺序
  必须是「停推进线程 → 关闭所有订阅 → 停服务器」。验收脚本专门开一条保持打开的
  长连接再发 SIGTERM，确认仍在 10 秒内退出——如果只测「没有连接时的退出」，
  这个问题永远不会被发现。
- **第一次写验收脚本时，我把 SIGTERM 测试建在了一个已经结束的房间上**。
  订阅一个 `finished` 房间会立刻收到 `room.finished` 并被关闭，于是"长连接未能
  保持打开"，那条断言什么都没验证。修复：先等双方回到 idle，再开一局新的。
  **测试里"连接没保持住"可能是测试搭错了场景，而不是产品有问题**。
- **`asan` 预设覆盖不到 Gateway**。它不使用 vcpkg，而 Gateway 只在 vcpkg 工具链下
  构建（因为它依赖 brpc）。也就是说 **TASK-005 以来的 Gateway 代码从来没有跑过
  ASan**。本轮用 `-DRGBT_ENABLE_SANITIZER=ON` 单独配了 `build/brpc-asan` 才跑上。
  这是一个真实的覆盖缺口，应该固化成一条命令，而不是每次临时拼参数。
- **测试目标的告警策略与产品代码不同，且这是有意为之**。`rgbt_set_test_warnings`
  不加 `-Werror`，原因是 GTest 的 `TEST_F` 宏展开出的静态函数会被
  `-Wunused-function` 误报（devlog 2026-09-18 已记录）。副作用是测试里忽略
  `[[nodiscard]]` 返回值只会产生警告、不会失败——`room_manager_test.cpp` 里有
  约 20 处 setup 调用就是这样。它们不影响断言的有效性，因此本轮**没有**顺手修改
  另一个任务的测试文件（一个 PR 只解决一个问题），仅在此记录。

### 未做 / 留给后续

- 不实现 `match.updated` 推送（理由见决策）。
- 不做帧级高频上行；上行仍是「一次动作一个 HTTP 请求」。
- 不做断线重连补帧（Phase 2）。SSE 规范自带 `Last-Event-ID` 语义，Phase 2
  实现「补缺失状态」时可以直接利用。
- 推送只有进程内的订阅表，**Gateway 重启即丢失订阅**，客户端需重连（Phase 2）。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-stream.sh`；确认后开 PR。
- 通过后进入 TASK-010（Vue 演示页面：登录、大厅、对战、结算）。

## TASK-010 实施记录（2026-10-02）

### 背景

到 TASK-009 为止服务端链路已经完整，但没有任何东西能把它演示出来。
Phase 1 的退出标准是「双浏览器完成匹配、进房和对战」，没有前端就无法验证。

动手前先核对了前端要对接的字段名：`api/proto/gateway.proto` 里
`LoginResponse.player`、`EnqueueMatchResponse.match`、`JoinRoomResponse.room`、
`GetMatchResultResponse.result` 都是**无前缀直接嵌套**，且字段名是 snake_case。
前端因此不做 camelCase 转换——多一层映射只多一处「读出来是 undefined」的错误。

### 环境问题（先于编码解决）

**WSL 里没有 Node。** `command -v node` 为空；PATH 里出现的 `npm`/`pnpm` 指向
`/mnt/c/Users/.../AILauncher/node/`，那是 Windows 侧的 Node 被 WSL 互操作暴露出来的，
路径与文件权限都不适用于 `~/workspace` 下的仓库。

两条常见路径都不通，实测结论：

- **nvm 装不上**：安装脚本在 `raw.githubusercontent.com`，本机访问被重置
  （`curl: (35) Recv failure: Connection reset by peer`）。探测结果：
  `raw.githubusercontent.com` 000，而 `nodejs.org` 与 `registry.npmjs.org` 都是 200。
- **apt 装不了**：源里有 `nodejs 22.22.1`，但本机没有免密 sudo。

最终用官方 tarball 解压到 `~/tools/node`（与 `~/tools/vcpkg` 同级），
`node v22.22.2` / `npm 10.9.7`。安装步骤、被否决的两种方式与原因写进了
`docs/06-operations.md` 第 1 节，`scripts/verify-web.sh` 自己把该目录加进 PATH。

### 完成

- `web/`：Vue 3 + TypeScript + Vite 应用，四个视图 + Canvas 对战渲染。
- `web/src/api/stream.ts`：用 `fetch` + `ReadableStream` 手动消费 SSE。
- `web/src/api/stream.test.ts`：SSE 解析的 12 个边界用例（vitest）。
- `web/src/api/client.ts`：HTTP 封装，**按 `error.reason` 分支而不是按状态码**。
- `web/vite.config.ts`：`/api` 代理到 Gateway，浏览器侧同源，无需 CORS。
- `scripts/verify-web.sh`：端到端验收（单测 + 类型检查 + 构建 + 代理 + 全流程）。
- `docs/06-operations.md`：Node 安装方式与前端运行方式。

### 决策

- **不用 `EventSource`，改用 `fetch` + `ReadableStream` 手动解析 SSE。**
  `EventSource` 无法设置请求头，token 只能进查询字符串，于是会出现在访问日志、
  浏览器历史与 `Referer` 里。多约 60 行代码换取 token 始终走 `Authorization` 头。
  这段解析逻辑因此成为前端最值得测的部分，单独写了单元测试。
- **不引 `vue-router`**：四个视图是**同一个会话的四个阶段**，不是可独立寻址的
  页面。用路由会引入 history 管理、路由守卫与「刷新后落到哪一页」这些无收益的问题。
- **不引 Pinia**：跨组件共享的只有会话那几项，模块作用域的 `ref` 足够。
- **不引 UI 组件库与 axios**：整个界面只有四个视图，写 CSS 比引组件库便宜。
  依据 CLAUDE.md 第 6 条。
- **不做本地乐观更新**：点攻击后血量要等服务端下一帧推送才变。
  本地先减血会制造第二种真相，与服务端判定冲突时无从分辨。
- **结算页把「胜负」与「结果已落库」分开显示**。胜负来自 SSE 推送的权威快照，
  「已落库」来自另一次 `GET /api/v1/results`。两者分开是为了把
  「对局已结束但结果还没写进数据库」这个真实状态暴露出来，而不是含糊过去。
- **代理不改写响应头**。SSE 需要的 `Cache-Control` / `X-Accel-Buffering` 由
  Gateway 自己设置；在代理里再写一遍等于把同一份缓存策略维护在两个地方。
- **本轮不加前端 CI job**。本地可运行性已由 `verify-web.sh` 覆盖，
  前端稳定后再纳入。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`cd web && npm run test`
- 结果：**12/12 通过**。覆盖的边界包括：心跳注释块必须返回 null（不能变成空事件）、
  多行 `data` 按换行连接、`data:` 后**只有一个**空格被去掉、CRLF 行尾、
  空 `data` 与「没有 data 行」必须区分。
- 命令：`cd web && npm run type-check`
- 结果：无错误（`vue-tsc --noEmit`）。
- 命令：`cd web && npm run build`
- 结果：构建成功，产物 `index.html` + `assets/index-*.js`（82.75 kB，gzip 32.30 kB）
  + CSS（2.35 kB）。
- 命令：`bash scripts/verify-web.sh`
- 结果：**全部通过，退出码 0**。实测覆盖：`GET /` 返回含 `#app` 的页面；
  空请求体的 `POST /api/v1/login` **经代理**得到 `400 account_required`
  （证明 `/api` 代理生效，没配就是 404）；alice/bob 登录、匹配、进房全程经代理；
  **SSE 经代理增量到达**——`session.ready`、`room.state`、攻击造成的
  `p-0002 hp=90`、心跳注释都在**连接仍然打开时**被文件轮询观察到，
  这条断言的意义是证明 Vite 代理没有把响应缓冲到最后一起吐；
  非本局成员订阅经代理被拒（400 `not_a_member`）；打完一局后经代理收到
  `room.finished` 且流被服务端关闭；结果查询经代理返回 200 且 winner 正确；
  轮询兜底经代理仍可用；Vite 与三个后端进程都能优雅退出。

### 问题与风险（本轮实际踩到并解决的）

- **npm 默认解析到 TypeScript 7，而 `vue-tsc` 与它不兼容**。
  报错是 `ERR_PACKAGE_PATH_NOT_EXPORTED: Package subpath './lib/tsc' is not defined`——
  TS 7（Go 版编译器）不再暴露该子路径，而 `vue-tsc` 正是通过它加载编译器。
  修复：显式装 `typescript@^5.9.0`。**注意这是依赖解析的结果，不是版本偏好**；
  将来 `vue-tsc` 支持 TS 7 后可以松开。
- **验收脚本里的心跳断言原本是竞态**。第一次跑 `verify-web.sh` 时
  「经代理未观察到心跳」失败，但产品是对的：心跳按固定间隔发送，
  前一条断言（血量变化）可能在第一次心跳之前就返回了。
  修复：改成 `wait_for_file_pattern` 等待，并**同时修掉了 `verify-stream.sh`
  里同样的写法**——它在 TASK-009 那次恰好通过，只是时序上运气好。
  这类断言的危险在于它会在无关改动后随机变红，让人误以为是产品回归。
- **`vite.config.ts` 里用 `process.env` 需要 `@types/node`**，
  否则类型检查报 `Cannot find name 'process'`。代价是应用代码也能看到 Node 类型，
  换取不必拆分 `tsconfig.app` / `tsconfig.node` 两个项目引用。
- **原本想在 Vite 代理里改写 `Cache-Control`，但那会造成策略两处维护**，
  而且类型上 `proxy.on` 在 Vite 8 的类型定义里不存在。删掉这段之后既少一处重复，
  也顺带解决了类型错误——原来的写法是「以为需要」而不是「验证过需要」。
- **`verify-web.sh` 覆盖不到渲染。** 它用 curl 验证的是网络路径与代理行为
  （前端发出的每个 URL、方法、请求头，以及 SSE 经过代理是否增量到达），
  但 Canvas 画得对不对、按钮可用状态对不对，curl 验证不了。
  脚本结尾显式打印人工检查步骤，**不假装已覆盖**。

### 未做 / 留给后续

- 不做断线重连 UI（Phase 2）。SSE 规范自带 `Last-Event-ID`，届时可直接利用。
- 匹配状态仍靠轮询感知，不推送。
- 前端不进 CI。`verify-web.sh` 需要 Docker 与三个服务，不适合当前的 CI 形态；
  接入方式在 TASK-011（集成验收）里一并考虑。
- 不做移动端适配与多语言。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-web.sh`；确认后开 PR。
- 通过后进入 TASK-011（集成验收：一条命令启动并双客户端完成对局）。

## TASK-011 实施记录（2026-10-02）

### 背景

Phase 1 的其余退出标准都已经满足（`scripts/verify-web.sh` 已证明两个身份能登录、
匹配、进同一房间并打完整局），但**启动流程散落在四个脚本文档与各处命令里**，
没有人能照着一条命令把整套环境跑起来。退出标准里的最后一条「能用一条命令启动
集成环境」还没有兑现。

### 先解决的一个文档冲突

`docs/06-operations.md` 第 2 节写的是：

> 集成环境：**所有 C++ 服务、Web**、Redis、MySQL 通过 Docker Compose 启动。

而 `README.md` 写的是「Redis 与 MySQL 通过 Docker Compose 启动」「compose/
（**仅 Redis + MySQL**）」，实际的 `docker-compose.yml` 里也确实只有
`redis` 与 `mysql` 两个服务。也就是说 `06-operations.md` 那条描述对应的是一个
**从未实现过的目标**，两个文档口径不一致。

按 `CLAUDE.md`「发现文档与实现冲突时停止编码并先报告冲突，不要自行选择方案」，
先报告冲突并请项目所有者决定，而不是自己挑一个口径做。

项目所有者选择：**当前不把应用服务容器化**，先把「一条命令启动」做出来。
理由记录在 `docs/06-operations.md` 第 2 节，容器化记入 Backlog。

### 完成

- `scripts/dev-up.sh`：一条命令起齐 Redis/MySQL（容器）+ 三个 C++ 服务 + 前端。
  含前置检查（docker / cmake / node / 端口）、迁移应用、上一轮会话清理、
  逐服务就绪等待、端到端探活，最后打印人工验证步骤。
- `scripts/dev-down.sh`：按**进程组**停止全部进程，按端口兜底核对并断言释放；
  `--with-docker` 可一并停容器（数据卷保留）。
- `scripts/verify-all.sh`：按顺序跑 6 个验收脚本，输出汇总表与失败项摘要；
  支持 `--list`、`--only`、`--fail-fast`。
- `.gitignore`：新增 `.run/`（dev-up 的 pid 与日志、verify-all 的验收日志）。
- `docs/06-operations.md`：改写「集成环境」一节，消除与 README 的冲突。
- `docs/TASKS.md`：新增 TASK-011 任务单；Backlog 补上「应用服务容器化」及其
  **第一步应该是限时构建实测**这一前提。
- `README.md`：更新「当前状态」与新增「快速开始」，此前那节还停留在 TASK-005 时代。

### 决策

- **`verify-all.sh` 存在的理由不是"方便"，而是承载一条纪律。**
  TASK-008 改坏 `verify-match.sh` 后十天无人发现，说明光写"改变依赖的任务必须
  重跑既有脚本"没有用——重跑必须是一条命令，否则一定会被跳过。
- **不做 fail-fast（默认）**。按「登录 → 匹配 → 房间 → 推送 → 前端」由下而上排序，
  前一个失败通常会让后一个也失败；一次看到全部结果比逐个修更有用。
  需要时用 `--fail-fast`。
- **端口固定而不是自动挑选**。TASK-010 期间我让人去 `verify-stream.sh` 的输出里
  找自动挑中的端口，那一步没有存在价值。固定端口让人工步骤可以直接照抄。
  并发多套环境时用环境变量覆盖。
- **端口被占用时明确报错并指出占用进程，不静默换端口**。换端口会让前端的代理
  目标与用户手上的 URL 对不上，产生更难定位的问题。
- **`dev-down.sh` 按进程组停，并按端口兜底**。TASK-010 已经踩过一次：
  `npm run dev` 的进程树是 npm → sh → vite，只 kill 第一层会留下后两层占用端口。
- **停容器时不带 `-v`**。开发数据卷不该被一次 `dev-down` 抹掉。

### 验证（均在 WSL 的 `~/workspace/realtime-game-backend` 执行）

- 命令：`bash scripts/dev-up.sh --no-build`
- 结果：退出码 0。四个端口占用预检通过；Redis/MySQL 达到 healthy；
  4 个迁移脚本应用成功；Room(8083) / Match(8082) / Gateway(8080) / 前端(5173)
  依次就绪；经代理探活 `GET / -> 200`、空登录体 `POST /api/v1/login -> 400`
  （证明 `/api` 代理生效）。
- **`dev-up.sh` 退出后服务仍然存活**（单独一次调用确认）：四个端口均在监听，
  三个后端进程与 vite 都在，`.run/*.pid` 与 `ps` 对得上。
  这一条是关键——脚本退出后服务跟着死掉的话，用户打开浏览器只会看到空白。
- 命令：经 Vite 代理跑完整一局（`dev-up.sh` 之后手动驱动 alice/bob）
- 结果：alice/bob 登录成功、配对成功（`room_id=r-gr5L-BcfBmJ2GNsOLPXDkH0e`）、
  双方进房后 `state=playing`。
- 命令：`bash scripts/dev-down.sh`
- 结果：四个进程全部停止；端口兜底核对全部空闲；重复执行安全（幂等）；
  `--with-docker` 也安全。
- 命令：`bash scripts/verify-all.sh`
- 结果：**6/6 全部通过**，退出码 0。逐项耗时：verify 30s、verify-login 23s、
  verify-match 31s、verify-room 28s、verify-stream 10s、verify-web 20s，
  **合计 142 秒**。
- 命令：`bash scripts/check-format.sh`
- 结果：通过（C++ 侧未改动，仅确认无回归）。

### Phase 1 退出标准逐条核对

引自 `docs/02-roadmap.md` 第 4 节：

| 退出标准 | 状态 | 证据 |
|---|---|---|
| 两个客户端可以登录、匹配、进入同一房间并完成对局 | 满足 | `scripts/verify-web.sh`（经 Vite 代理驱动两个身份打完整局）；`scripts/verify-room.sh` |
| 正常路径和主要异常路径有测试 | 满足 | 163 个 C++ 单元测试 + 12 个前端单元测试；六个验收脚本各自覆盖依赖不可用、超时、幂等、权限等异常路径 |
| 页面能显示连接、匹配和房间状态 | 满足 | `web/` 四个视图；顶栏显示 SSE 连接状态；对战页显示帧号与推送序号 |
| 能用一条命令启动集成环境 | 满足 | `bash scripts/dev-up.sh` |

**渲染部分（Canvas 画面、按钮状态、视图切换）需要人工确认**，
验收脚本覆盖的是网络路径与服务行为。人工步骤由 `dev-up.sh` 启动后打印。

### 问题与风险（本轮实际踩到并解决的）

- **我写了一个没有实测过的耗时数字。** 在 `verify-all.sh` 的注释与 TASK-011 的
  验收命令里，我写了「约 15-25 分钟」——那是我凭印象填的。实测是 **142 秒**，
  差了约十倍。CLAUDE.md 第 7 条禁止编造数字，这类"看起来无害的估计"同样属于
  编造：它会让人误判要不要跑，从而**降低验收被执行的概率**，正好破坏这个脚本
  存在的意义。已改为实测值，并在注释里写明测量条件（本机 16 核 / 11 GiB、
  ccache 已预热）。
- **文档冲突必须先报告而不是自己选**。README 与 `06-operations.md` 对"集成环境"
  的定义不一致，而 TASK-011 的产出完全取决于按哪个口径做。若我自行选一个，
  无论选哪个都会让另一份文档变成误导。报告 + 请所有者决定，成本只有一轮对话。
- **`dev-up.sh` 退出后服务必须存活，这一点必须单独验证。**
  "脚本自己报告就绪"和"脚本退出后服务还在"是两件事。我在一次调用里跑
  `dev-up.sh`，在**另一次**调用里查端口与进程——中间隔着调用边界。
  TASK-010 期间我用 `setsid nohup` 起的演示进程就曾经在某个时刻消失，
  因此这条不能假设。

### 未做 / 留给后续

- 应用服务容器化（Backlog，见上文；第一步是限时构建实测）。
- 不做故障注入（Phase 4）。
- `verify-all.sh` 不进 CI：它需要 Docker 与四个本机进程，当前 CI 形态不适合。
- `dev-up.sh` 不做热重载编排（改 C++ 要重跑）。

### 下一步

- 由项目所有者审阅 Diff、运行 `bash scripts/dev-up.sh` 与 `bash scripts/verify-all.sh`，
  并按打印的步骤在浏览器里确认渲染；确认后开 PR 合并。
- 合并后 Phase 1 结束，进入 Phase 2（持久化和恢复：房间快照、断线重连、
  Gateway 重启后的订阅恢复）。

## TASK-013 实施记录（2026-10-02）

### 背景

`docs/01-architecture.md` 第 5 节已经把「房间权威状态」的恢复策略写成
**「从最近快照恢复」**，但房间状态至今只存在于进程内存的环形缓冲里。
Room 一重启，进行中的对局就无声消失，连"这里曾经有一局"都查不到。

本任务只做**写入路径**（读取属 TASK-014）：一次引入两处未验证的复杂度
不符合本项目的拆分原则。

### 完成

- `migrations/005_create_rooms.sql`：房间记录表，15 列全部离散。
- `src/room/room_snapshot_writer.hpp` + `mysql_room_snapshot_writer.*`。
- `RoomManager`：快照调度（首发 / 按间隔 / 终态）+ 锁外写入 + 两个计数器。
- `src/common/mysql_connection.cpp`：`kMaxBindParams` 8 -> 32；错误信息带上
  实际值与上限。
- `scripts/verify-persistence.sh`：端到端验收（已加入 `verify-all.sh`）。
- 7 个新的 `RoomManagerTest` 用例。

### 决策

- **单表 + 离散列，不用 JSON 快照列**。schema 本身就是"能恢复什么"的文档；
  把它藏进不透明字段等于删掉这份文档，并立刻引入格式版本管理问题。
  `p1_*`/`p2_*` 不是未经验证的假设：`kPlayersPerRoom = 2` 是 TASK-008 已确认的规则。
  被否决的备选是 `rooms` + `room_players` 子表：不把"2"写进 schema，但恢复要
  join、写入要事务，而人数在 ADR-0003 下不会变——收益不成立、成本立刻发生。
- **快照间隔 1 秒（10 帧）**。重启后最多回退 1 秒进度（最多一次攻击、10% 血量）。
- **快照写入失败不重试、不阻塞，下一次覆盖**。这是本任务最重要的区分：
  对局结果**不可丢弃**（所以 FINISHING 无限重试），房间快照**可以丢弃**。
  把快照也做成"必须成功"只会让一次 MySQL 抖动在内存里堆起一批过期快照，
  而收益为零。因此用两个独立接口，而不是合成一个 "RoomRepository"。
- **第一个快照立刻写，不等间隔**。`rooms` 表的用途之一是"事后能查到这里曾经
  有一局"；等一秒再写会让一个刚创建就异常退出的房间完全消失。
- **表里只保留最新一份快照，不是审计日志**。需要历史的消费者还不存在。

### 验证

- 命令：`ctest --test-dir build/brpc-debug --output-on-failure`
- 结果：**170/170 通过**（TASK-011 时为 163，本次新增 7 个）。
- 命令：`bash scripts/verify-persistence.sh`
- 结果：**全部通过，退出码 0**。实测覆盖：迁移 005 幂等且 `rooms` 表 15 列齐全；
  房间创建后立刻落一条（状态 `created`）；状态随对局推进更新为 `playing`；
  `frame` 随服务端推进增长且**不超前**（快照 19 / 实时 25）；攻击后 `p2_hp=90`
  落库；结束后落终态（`finished` + `winner_id=p-0001` + `finish_reason=hp_zero`）；
  **MySQL 停机期间对局继续推进（frame 15 -> 35）并能打完**；恢复后快照自动
  补上终态，无需任何补写逻辑。
- 命令：`bash scripts/check-format.sh`
- 结果：通过，65 个文件（新文件首次未过，已在 WSL 格式化后回写）。
- 回归：`verify-room.sh` 与 `verify-stream.sh` 均退出码 0——TASK-013 改了 Room 的
  依赖（新增快照写入），按 `docs/TASKS.md` 的任务完成定义必须重跑它们。

### 问题与风险（本轮实际踩到并解决的）

- **`MysqlConnection` 有一个 8 参数的硬上限，而错误信息没说上限是多少、也没说
  来自哪里。** 快照要绑 15 个参数，于是每一条写入都失败，日志里只有
  "too many params"。我为此先怀疑 `AS new` 行别名（换成 `VALUES()` 无效），
  又手工在命令行执行同一条 SQL（完全正常），最后才 grep 到 `kMaxBindParams = 8`
  ——注释还写着"Room 写 match_results 时需要 6 个参数"，即这个值按当时的用量定，
  TASK-013 一超出就变成静默失败。两处修复：上限提到 32（覆盖 15 列并留余量，
  仍是定长栈数组，无动态分配），以及**错误信息带上实际值与上限**。
  教训：**一个上限值如果不出现在它的错误信息里，它就会变成一个排查黑洞**。
- **失败日志没带原因，等于没有日志。** 第一版 `FlushSnapshots` 只打印
  "快照写入失败"，看不到 MySQL 的错误。补上 `last_error()` 之后一次就定位到了。
  最终把"记原因"放在 writer 层（只有它知道 MySQL 的错误），调用方只计数。
- **两次断言写错，产品都是对的。** 一处以为终态快照会显示 `FINISHED`，实际那次
  Tick 里房间还是 `FINISHING`（结果正在落库），下一次才观察得到；另一处以为房间
  一开始就显示 `playing`，实际首个快照的 `created` 是**准确的**——那一刻确实
  还没人加入。两次都是时序假设错了，不是实现错了。
- **我并行跑了两次验收脚本，互相抢端口和服务，得到一堆假失败。**
  看到"alice 登录失败"时就该意识到是环境冲突而不是代码问题。

### 未做 / 留给后续

- 读取路径（启动时恢复）属 TASK-014。
- 快照表不做清理策略（没有容量证据，见 CLAUDE.md 第 6 条）。
- 不恢复 `FINISHING` 房间的落库重试（属 TASK-014）。

### 下一步

- 由项目所有者审阅并运行 `bash scripts/verify-persistence.sh`；确认后合并。
- 通过后进入 TASK-014（房间重启恢复与恢复边界）。

## TASK-014 实施记录（2026-10-02）

### 起点：一个自带"崩溃待查"的 WIP 提交

接手时仓库里已有 `wip/task-014-partial`（`a35b3ef`），提交信息自己写着
「读取路径 + 启动恢复，**存在崩溃待查**」。恢复逻辑（快照读取器、校验、`Restore`、
`FINISHING` 重新纳管）基本成形，但 `scripts/verify-persistence.sh` 全红：
Room 一启动就 abort。本任务的工作因此是「把首尾收完」，而收尾过程中挖出了三个
**既有缺陷**，其中两个与 TASK-014 自身无关。

### 崩溃一：同一条 MySQL 连接被多个线程共用（并发协议错乱）

**现象**：只要 `rooms` 表里有一条未结束的快照，Room 启动就 abort：

```text
启动恢复：扫描 1 个未结束房间，恢复 1 个，标记 ABORTED 0 个
I… server.cpp:1262] Server[rgbt::room::RoomServiceImpl] is serving on port=18099
../src/nssl-3.6.4-…/ssl/ssl_lib.c:4362: OpenSSL internal error: refcount error
../src/nssl-3.6.4-…/ssl/ssl_lib.c:1432: OpenSSL internal error: refcount error
→ SIGABRT（退出码 134）
```

**根因**：`room_main.cpp` 只创建**一条** `MysqlConnection`，交给
`result_writer` / `snapshot_writer` / `snapshot_reader` 三者共用；而 ticker 线程
（第 137 行启动）会立刻为刚恢复的房间写第一份快照，主线程同一时刻还在做启动探活
（第 145 行 `IsHealthy()`）。一股 `MYSQL*` 字节流被两个线程同时写，协议被搅乱，
下一个语句直接报 `Received malformed packet`，走到"连接已失效"的重连分支，
`mysql_close()` 拆 TLS 时 OpenSSL 引用计数校验失败，进程 abort。
`MysqlConnection` 当时**没有任何互斥**。

为什么偏偏 TASK-014 撞上：此前 Room 启动时 `rooms` 里没有未结束行（"恢复 0 个"），
ticker 那一瞬间无快照可写。TASK-014 第一次让"启动时就有房间"成为常态。

**A/B 证据**（`rooms` 表放一条未结束行，各启动 10 次）：

| 版本 | 存活 | 崩溃 | 出现 malformed packet |
|---|---|---|---|
| 修复前 | 3/10 | **7/10**（139×5、134×2） | 3/10 |
| 修复后 | **10/10** | 0/10 | 0/10 |

**修复**：`MysqlConnection` 内部加一把互斥锁，`Query`/`Ping` 各自加锁，把原逻辑
抽成不持锁的 `QueryLocked`（`Ping` 若直接调 `Query` 会对同一把非递归锁加锁两次，
那是死锁）；`last_error()` 改为**按值返回**，否则调用方会在另一个线程正写该字符串
时读它（真实数据竞争）。锁覆盖整个查询（含重连重试），因为"回复配对正确"依赖
"发出到取回之间没有别的线程插队"。

**这不是"顺手加的保险"，而是 Gateway 一并受益的既有缺陷修复**：
`gateway_main.cpp` 同样只创建一条连接交给所有 brpc worker 线程。

### 崩溃二：同一条 Redis 连接被多个线程共用（回复张冠李戴）

修好 MySQL 后，`verify-persistence.sh` 新增的 7e（12 个并发登录）立刻暴露出
**Gateway 的另一个同类缺陷**：`RedisSessionStore` 也只创建一条 hiredis 连接，
被所有 brpc worker 线程共用，同样没有互斥。

**最直接的证据**是并发登录的响应体：

```json
{"status_code":200,"token":"OK","expires_in_seconds":604800,...}
```

`"OK"` 是 `SET … NX` 的 `+OK` 状态回复——它被另一个线程的 `GET` 当成 Token 读走了。
同批 12 个请求里另有 3 个返回 503（`session_store_unavailable` /
`player_store_unavailable`），并且出现过一次段错误。

**修复**：与 MySQL 同一套做法——加锁覆盖 public 方法的**整个**命令序列
（`GET → HSET → EXPIRE → SET NX → 可能的 GET/DEL` 必须一次性持锁，否则回复仍会错配）。

修复后 Token 不再是 `OK`，也不再崩溃；只剩 1/12 返回 `player_store_unavailable`。
补上 `MysqlPlayerReader` 的失败日志后，那一例的真面目清楚了：

```text
[gateway] 玩家档案查询失败：prepare failed: TLS/SSL error: unexpected eof while reading
[mysql] connection lost, reconnecting and retrying once
```

即：这一节前面停过 MySQL，Gateway 手上那条连接已经死了，**第一个**碰到它的请求
负责发现断连并重连。这属于"依赖刚恢复时的首次探测"，本身就是设计上允许的可重试
503，不该算进"并发共用连接"这一项。因此 7e 改为**先预热一次**（顺序登录，
结果无论 200 还是 503 都只记录、不判失败），再并发打 12 个并断言 12/12 成功。
预热之后连续 3 次运行均为 12/12。

### 崩溃三（假警报）：构建陈旧 —— `rsync -a` 的 mtime 与 `-p`

排查上面两个缺陷期间出现过两组**看起来很严重、实际不存在**的崩溃：

- Room 在 MySQL 停机时 6/6 SIGSEGV（栈落在 `pthread_mutex_lock`，
  `this` 是个垃圾值 0x1bcc0）；
- Gateway 在并发登录时 `free(): invalid pointer`。

**根因在工具链，不在产品代码**：会话的写入位置是 Windows 副本，构建位置在 WSL，
中间用 `rsync -a` 同步。`-a` 含 `-t`（保留 mtime），把文件同步进构建树后目标
mtime 可能**早于**上次构建的 `.o`，Ninja 便认为"不需要重建"，于是把**新头文件**
和**旧目标文件**混着链接。给 `MysqlConnection` 加 mutex 成员改变了类的
`sizeof`：`room_main.cpp.o` / `gateway_main.cpp.o` 仍按旧尺寸分配对象，而
`mysql_connection.cpp.o` 已是新布局，互斥锁落在 malloc 的 chunk 头上 →
表现为 `free(): invalid pointer` 与 `pthread_mutex_lock` 崩溃。

**验证**：清理重建后，同一复现脚本从 6/6 崩溃变成 0/3 崩溃（gdb 下 60 秒不崩）。

**同时修掉第二个同步陷阱**：`rsync -a` 含 `-p`，而 DrvFs（`/mnt/d`）上所有文件都
呈现为 755，于是 WSL 仓库里 154 个文件被改成 `100755`，`git status` 看起来
"几乎全仓库都被改了"（内容未变，只有权限位）。现在同步脚本
**不用 `-p/-o/-g`、对每个真正传输的文件 `touch`、并按 git 记录恢复权限位**。

**教训**：跨 Windows/WSL 的同步必须让构建系统看见"文件变了"。mtime 与权限位
都能伪装成产品缺陷，而且伪装得很像。

### 修掉验收脚本自身的三处错误（原来的断言不可能通过）

7 节这些用例是 WIP 里新写的，**从未真正通过过**（一跑到那里就崩），因此其中的
错误断言一直没被发现：

1. **7a「恢复位置精确等于最后一次快照」测错了时刻**。房间一恢复就继续按 10 Hz
   推进，而脚本是重启后 1~3 秒才通过 Gateway 查询，帧号早已前进——实测
   `kill 前 30，恢复后 44`，于是"丢失量"算成 **-14 帧**。
   修法：丢失量的定义改为「kill 时的实时帧 − 最后一次落库的帧」（= 还没来得及
   写进快照的进度，上界即一个快照间隔）；恢复点则**从 Room 启动日志读**。
   为此给 `RoomManager::Restore` 增加一行按房间打印的恢复日志
   （`已恢复房间：… frame=N phase=…`）——这是唯一能在外部观测到"恢复点是哪一帧"
   的途径，对排障本身也有用。日志在**锁外**打印，沿用"持锁不做 I/O"的规则。
2. **7b 的前提自相矛盾**。它先停掉 MySQL、把对局打到 `finishing`，然后在 MySQL
   **仍停机**时重启 Room，再断言"FINISHING 房间被恢复"。但恢复本身就要读 MySQL，
   而存储不可用时 `Restore` 按设计**一个房间都不恢复**。等于要求"在没有 MySQL 的
   情况下从 MySQL 恢复"。修法：改为在 **MySQL 正常时**直接写入一条合法的
   `finishing` 行（正是"进程被杀时结果尚未落库"在库里的样子），重启 Room，
   断言它被恢复并**把结果补写进 `match_results`**——这才是 TASK-008 那条已知限制的正解。
3. **第 6 节「MySQL 停机期间对局继续推进」的 2 秒观察窗太窄**。快照写入是在
   **ticker 线程**上做的（`Tick → FlushSnapshots`），所以一次连不上的写入会让这个
   线程阻塞在 MySQL 连接超时上（默认 3 秒），停机期间推进是**突发式**的。
   固定 2 秒窗口可能整段落在一次停顿里，产生误报（实测到过一次 `frame 14 -> 14`）。
   修法：改成观察"12 秒内是否推进过"，并把这条机制写进
   `docs/01-architecture.md` 的已知限制——它是真实的设计边界，不是测试问题。

### `verify-all.sh` 里有一行把验收脚本悄悄吃掉了

跑「一条命令」时发现它只跑 6 个脚本，**没有 `verify-persistence.sh`**。
读脚本发现 `all_scripts=(...)` 写了**两行**，第二行漏掉 `verify-persistence`，
把第一行整个覆盖：

```bash
all_scripts=(verify verify-login verify-match verify-room verify-stream verify-web verify-persistence)
all_scripts=(verify verify-login verify-match verify-room verify-stream verify-web)   # ← 覆盖掉上一行
```

`git log -S` 显示两行都来自 TASK-013 的提交 `5642b28`。也就是说 TASK-013 的
devlog 里"已加入 `verify-all.sh`"实际没有生效，它自己的验收脚本从没被这条命令跑到
——正是 `verify-all.sh` 开头那段理由警告的情况，只是这次坑在脚本自己身上。
已删除重复行并更新耗时记录。

### 完成

- `include/common/mysql_connection.hpp` + `src/common/mysql_connection.cpp`：
  内部互斥、`QueryLocked` 拆分、`last_error()` 按值返回。
- `src/gateway/redis_session_store.hpp` + `.cpp`：内部互斥，锁覆盖整个命令序列。
- `src/room/room_manager.cpp`：恢复点日志（锁外打印）。
- `src/gateway/mysql_player_reader.cpp`：失败时打印 MySQL 的真实错误。
  （调用方只能看到一个 503，具体是断连、权限还是语句被拒只有这一层知道——
  排查那一例并发 503 时正卡在这里。）
- `CMakePresets.json`：新增 `brpc-asan` 与 `brpc-tsan` 预设
  （vcpkg + ASan / TSan，configure/build/test 三处）。
- `CMakeLists.txt`：`rgbt_apply_sanitizer` 改为按开关选择 ASan 或 TSan，
  两者互斥（同时打开直接 FATAL_ERROR），并在 TSan 构建里加 `-Wno-tsan`
  （该告警由第三方头文件里的 `atomic_thread_fence` 触发，不是本项目代码）。
- `src/gateway/CMakeLists.txt`、`src/match/CMakeLists.txt`、`src/room/CMakeLists.txt`：
  给三个**服务可执行文件**补上 `rgbt_apply_sanitizer`——此前只有 `*_lib` 与测试
  目标加了它，服务的 sanitizer 构建根本无法链接，等于 sanitizer 从未覆盖过服务进程。
- `docs/TASKS.md` + `docs/02-roadmap.md`：清理过期状态标注（TASK-001/002/003/007
  的「进行中」与 TASK-008 ~ TASK-013 的「待验收」全部更正为已完成并附合并点；
  roadmap 的「Phase 1 进行中」更正为已完成）。这些标注此前与 git 状态不符，
  会让人误以为还有一堆任务挂着。
- `scripts/verify-persistence.sh`：修正 7a 的度量方式、重写 7b 的前提、放宽第 6 节
  的观察窗、新增 7d（存在未结束房间时连续 3 次重启）与 7e（并发共用一条连接）。
- `scripts/verify-all.sh`：删掉覆盖 `all_scripts` 的重复行。
- `docs/01-architecture.md`：新增「房间状态的恢复边界」（精确字段、近似字段与
  偏差上界、三种失败情形的区分、明确不恢复什么、已知限制）。
- `docs/05-api-and-data.md`：补上 `rooms` 表（TASK-013 建了表但表清单一直没更新，
  文档与实现不一致）；表清单标题的"截至 TASK-006"改为"截至 TASK-014"。
- `tests/unit/room/room_manager_test.cpp`：抽出 `OpenPlayingRoom` helper 并对
  `Create`/`Join` 断言成功，消掉约 40 处 `[[nodiscard]]` 忽略告警。
- `tests/unit/match/match_queue_test.cpp`：显式丢弃一处 `GetStatus` 返回值
  （既有告警，非本任务引入；为了让"构建无告警"这句话成立而顺手修掉）。

### 决策

- **在 `MysqlConnection` / `RedisSessionStore` 内部加锁，而不是"每个线程一条连接"**：
  这里修的是"同一对象被并发使用"这一确定性错误，最小改动就是互斥。连接的**数量**
  是另一件事（连接池属 Phase 3，当前没有容量证据，见 CLAUDE.md 第 6 条）。
  已知代价：锁覆盖整个调用，慢依赖会串行化这条连接上的请求——但一条连接本来
  就无法并发执行两条命令，真正的扩容手段是连接池，不是去掉锁。
- **给 `Restore` 加一行日志而不是让验收脚本"大概对得上"**：恢复点帧号在外部
  不可观测（房间立刻继续推进），要么加日志、要么放弃断言。选择加日志，它对排障
  本身也有价值。
- **7b 改为构造 `finishing` 行，而不是保留一个不可能通过的前提**：宁可承认原断言
  写错了，也不把它改成"看起来能过"的弱断言。
- **不修"快照写入阻塞 ticker 线程"**：它是真实边界（MySQL 不可用时推进变突发式），
  但把快照写入移到独立线程属于 Phase 3 的解耦工作，当前没有容量证据，
  只在 `01-architecture.md` 记录清楚。

### 验证

均在本机 WSL（16 核 / 11 GiB）执行。

- `cmake --build --preset brpc-debug --clean-first`：退出码 0，
  **全量重建后 0 条 error/warning**（此前遗留的 `match_queue_test.cpp` 一处
  `[[nodiscard]]` 告警已一并修掉）。
- `ctest --test-dir build/brpc-debug`：**185/185 通过**。
- `bash scripts/check-format.sh`：通过（69 个文件）。
- `bash scripts/verify-all.sh`：**7 个脚本全部通过（228 秒）**；修正 `all_scripts`
  之后 `verify-persistence` 才真正被这条命令跑到
  （verify 29s / verify-login 53s / verify-match 38s / verify-room 32s /
  verify-stream 11s / verify-web 21s / verify-persistence 44s）。
- 崩溃回归 harness（`rooms` 里放一条未结束行后反复启动 Room）：
  **修复前 7/10 崩溃（139×5、134×2）→ 修复后 10/10 存活**，
  `malformed packet` 从 3/10 降到 0/10。
- 清理重建前后的对照（证明第三个"崩溃"是构建陈旧）：MySQL 停机复现
  **清理前 6/6 SIGSEGV → 清理后 0/3 崩溃**。

`bash scripts/verify-persistence.sh` 连跑 3 次（这是本任务的验收入口）：

| 运行 | 退出码 | 恢复点 = 最后快照 | 实测进度丢失 | 未结束房间下重启 | 12 并发登录 |
|---|---|---|---|---|---|
| 1 | 0 | 是（30） | 0 帧 | 3/3 存活 | 12/12 |
| 2 | 0 | 是（30） | 0 帧 | 3/3 存活 | 12/12 |
| 3 | 0 | 是（29） | 1 帧 | 3/3 存活 | 12/12 |

进度丢失量的上界由快照间隔决定（10 帧 = 1 秒），实测 0~1 帧。

**TSan 也已运行，但当前配置下不具指向性**——这也是一个需要记录的事实，不是"跑过了"就完事：

- 为了跑 TSan 补了两处：新增 `brpc-tsan` 预设，以及修掉两个**既有**阻塞点：
  1. `rgbt_apply_sanitizer` 只作用于 `*_lib` 与测试目标，**三个服务可执行文件从未
     应用 sanitizer**（ASan 版服务直接链接失败，等于 sanitizer 从未覆盖过服务进程）；
  2. GCC 的 `-Wtsan`（"TSan 无法插桩 `atomic_thread_fence`"）在 `-Werror` 下直接
     编译失败，而该构造只出现在 brpc / libstdc++ 头文件里（`grep atomic_thread_fence`
     在本仓库为空），因此在 TSan 构建里用 `-Wno-tsan` 屏蔽这一条。
- 实测结果：Room 启动恢复 3 次全部存活、Gateway 12 个并发登录 12/12 成功，
  但产生 **21 条 TSan 报告**（Room 每次 5 条，Gateway 6 条）。
- **这 21 条全部指向 brpc 内部**：按栈帧统计，186 帧来自 brpc 源码，
  `butil::Mutex::lock`、`butil::cpuwide_time_ns`、`butil::IOPortal::pappend_from_file_descriptor`
  等；来自本项目源码的只有 2 帧，且都是**对象构造点**
  （`BrpcMatchClient::BrpcMatchClient`、`main`），不是竞争访问。
- 原因是 vcpkg 提供的 brpc **没有用 TSan 插桩**：brpc 自己的锁与每 CPU 时间缓存
  在 TSan 看来就是"无同步的裸访问"。要让它成为可用的门禁，必须先用 TSan 重建
  brpc 及其依赖链（brpc 本体普通编译就要 17 分钟，插桩后更久）。
  **结论：TSan 预设保留（它是那一步的前置条件），但当前不能当作本项目的并发门禁；
  并发缺陷的证据仍然来自 A/B 复现（7/10 → 0/10）、ASan 与"废弃 Token `OK`"这类直接现象。**
  这件事记入 `docs/TASKS.md` 的 Backlog。

**ASan 的运行结果**（`build/brpc-asan`，`detect_leaks=0`：brpc 自身的静态对象会被报成泄漏，
开着它只会把真正的内存错误淹掉）：

| 场景 | 结果 |
|---|---|
| Room 启动恢复 + 立刻写第一份快照，5 次 | 存活 5/5，**ASan/UBSan 报告 0/5** |
| Gateway 12 个并发登录 | **12/12 成功**，无 ASan/UBSan 报告，进程存活 |

### 问题与风险

- 上面第三个"崩溃"是**我自己造成的假象**（构建陈旧），前后花了两轮排查。
  记在这里是因为它比产品缺陷更容易重犯：同步目录 + 增量构建这个组合会骗人。
- 并发缺陷的修复是"让同一条连接串行化"，**没有**消除依赖本身成为瓶颈的可能。
  锁的代价只在一处被观测到（Redis/MySQL 超时会串行化该连接上的请求），
  没有做压测，因此**不能**说"性能没有影响"。
- `verify-persistence.sh` 的 7d/7e 依赖"表里存在未结束快照"这个前置条件，
  脚本会自己构造并在结束时清理。若前一次运行异常中断，可能留下
  `m-restart-014` / `m-finishing-014` 这类测试行，需手工删除。

### 下一步

- 按 [03-development-workflow.md](03-development-workflow.md) 第 9 节的分支粒度，
  本任务是 `feat/phase-2` 上的**单个提交**（提交信息带任务号），不单独建分支。
  审阅时看该提交的 Diff，或直接跑验收命令。
- 由项目所有者审阅 Diff，并运行：
  `bash scripts/verify-persistence.sh`（约 45 秒）与 `bash scripts/verify-all.sh`
  （实测 212 秒）。
- TASK-015（匹配队列的 Redis 快照与重启恢复）需要项目所有者先决定
  「Redis 不可用时是明确报错还是降级为纯内存」，见任务单。

## TASK-015 实施记录（2026-10-02）

### 背景

TASK-007 留下的已知限制原文是「**Match 重启即丢失排队状态**」：排队中的玩家会
突然从 `queued` 变成 `idle`，而且没有任何解释。TASK-015 把队列状态写进 Redis，
启动时重建，并把停机期间流逝的时间算进超时判定。

项目所有者在本轮确认了任务单里唯一悬着的那项决策：
**Redis 不可用时降级为纯内存，不阻塞匹配**。

### 完成

- `src/match/match_queue_store.hpp`：快照类型、**纯函数**编解码、存储接口。
- `src/match/match_queue_store.cpp`：长度前缀文本格式（显式版本号 `1`）。
- `src/match/redis_match_queue_store.hpp/.cpp`：Redis LIST + 管道化的
  `MULTI/EXEC` 整份替换，内部互斥（沿用 TASK-014 的教训：共享连接必须加锁）。
- `src/match/match_queue.{hpp,cpp}`：`Restore`、`MakeSnapshotLocked`、
  `PersistSnapshot`；清算函数改为返回"是否变化"，只有真的变了才写快照。
- `src/match/match_main.cpp`：`-redis_host/-redis_port/-redis_timeout_ms/-env_prefix`，
  恢复在**开始接受请求之前**执行，结果打进启动日志。
- `tests/unit/match/match_queue_store_test.cpp`：18 个用例。
- `scripts/verify-persistence.sh`：新增 7f 一节（队列快照、重启恢复、降级）。
- 文档：本记录 + `docs/TASKS.md` 的 TASK-015 任务单与结果。

### 决策

- **整份重写而不是增量更新**。队列条目之间有 FIFO 顺序，增量更新要自己维护顺序，
  而整份重写天然给出"要么旧的完整队列、要么新的完整队列"。上限 1000 条，
  重写开销可以忽略。
- **`MULTI/EXEC` 包住 `DEL` + 若干 `RPUSH`**。没有这条保证时，进程在改写中途被杀
  会留下"只剩后半截"的队列——那比丢队列更糟，因为它是**看起来正常**的错误状态。
  用管道（`redisAppendCommand`）而不是逐条 `redisCommand`：队列上千行时逐条会变成
  上千次往返，而快照写入发生在入队路径上。**所有管道回复必须全部取走**，
  否则连接会与回复错位——这正是 TASK-014 实测过的那类缺陷。
- **编解码是纯函数，且带显式版本号**。本仓库没有 JSON 库，自己写解析器属于引入
  未验证的复杂度；长度前缀对内容无要求、不需要转义。版本号让"以后改格式"能被
  **识别并拒绝**，而不是解析出一支乱七八糟的队列。解析玩家数时**先卡上界再循环**，
  损坏的计数不能变成一次空转。
- **快照里不存 `request_id`**。它只用于入队幂等，而去重以 `player_id` 为键：
  重启后同一 `request_id` 重试会被判为 `kAlreadyQueued`，语义不变。
  附带好处是快照里**不含任何客户端可控的字符串**。
- **`allocating` 条目按"排队中"写入，重启后退回队列**。重启后无法知道
  `CreateRoom` 是否已经成功；Room 侧以 `match_id` 幂等，重新分配会拿回同一个
  `room_id`，因此退回是安全的，丢弃则会让玩家无缘无故消失。
  已经登记取消意图的条目**不写**：客户端的取消已答复成功，重启后让它重新出现
  会与那个答复矛盾。
- **只有状态真的变化才写快照**。轮询是高频率路径，若每次查询都写，每个客户端的
  每次轮询都会变成一次 Redis 写。清算函数因此返回"是否变化"。
- **快照与写入成对串行化**（`snapshot_order_mutex_ → mutex_`）。只做"锁内取、
  锁外写"会引入新问题：两个线程可能各自取到较早的快照 S1 与较晚的 S2，
  却按 S2 → S1 的顺序写入，把队列**写回退**。先拿前者再取队列锁，
  既保证顺序，又**不在持有队列锁时做 I/O**。

### 验证（WSL，16 核 / 11 GiB）

- `ctest --test-dir build/brpc-debug`：**203/203 通过**（185 → 203，新增 18 个）。
- `cmake --build --preset brpc-debug`：0 error / 0 warning。
- `bash scripts/check-format.sh`：通过（74 个文件）。
- `bash scripts/verify-persistence.sh`：**退出码 0**。7f 一节实测：
  - 入队后 `dev:match:queue` 有 1 条快照；
  - `kill -9` Match 后重启，`GET /api/v1/matches/current` 仍报 `queued`，
    启动日志打印「队列恢复：扫描 1 行，重建排队 1 人」；
  - 让 Match 指向一个不可用端口后：启动日志如实报告「队列快照读取失败」，
    **匹配照常成功**（降级为纯内存），且日志明确记录「队列快照写入失败」。

### 问题与修正（本轮实际踩到的）

- **第一版 7f 测错了对象**。它用 `docker stop rgbt-redis` 来测 Match 的降级，
  但 **Gateway 的会话也在同一个 Redis 上**：停掉 Redis 后所有 HTTP 请求会先因
  鉴权失败返回 503，请求根本走不到 Match。失败项指向的其实是 Gateway 的会话存储，
  与 TASK-015 要验证的行为无关。改为让 **Match 的快照存储**指向一个不可用端口，
  链路其余部分保持完好，降级行为才被真正隔离验证到。
  教训：**故障注入要注入到被验证的那个组件上**，否则测到的是链路上游的行为。
- **一条测试的前提写错了**。`AllocatingEntriesAreSnapshottedAsQueued` 一开始检查
  "最后一次快照"，但后续操作会覆盖它，于是断言失败。改为在分配回调里捕获
  "分配期间产生的那个快照"，断言才指向真正想验证的时刻。

### 队列快照跨运行残留：一个被 `verify-match.sh` 抓到的真回归

自己写的新功能第一次跑全套验收就撞上了：`verify-match.sh` 失败于
`未入队状态异常：HTTP 200 state=timeout`——它断言"没入过队的玩家应当是 idle"，
但那个玩家报的是 `timeout`。

原因不是断言写错，而是**队列快照把上一轮运行的状态带进了这一轮**：
Match 启动时会恢复 `dev:match:queue`，而验收脚本此前只清理 `dev:gateway:*`
（会话与登录幂等），从来没有清过队列快照。上一轮 7f 留下的排队玩家，
在新一轮里被恢复成内存条目，于是"从没排过队的人"有了状态。

两处修正，分别针对测试隔离与产品语义：

1. **测试隔离**：所有会启动 Match 的脚本（`dev-up.sh`、`verify-login/match/
   room/stream/web/persistence.sh`）原本就有"清理上一轮 Key"的动作，
   把 pattern 从 `dev:gateway:*` 放宽到 `dev:*`，队列快照一并清掉。
   这与它们清理会话的理由完全相同：**验收必须从干净状态开始**。
2. **产品语义**：`Restore` 不再把"已经超过排队超时"的条目装进内存变成
   `timeout` 状态，而是**直接不恢复**并计入报告（新增 `expired_results` 计数）。
   理由是快照可能来自很久以前的一次启动：把这些条目装进内存，会让一个"在这个
   进程里从没排过队的人"立刻报告 `timeout`，并占着结果保留期不放。
   停机时间**仍然算在等待里**（这正是"重新判定超时"的含义），只是表达方式从
   "恢复成已超时"改成"干脆不恢复"——玩家该做的就是重新入队，
   而"队列里没有他"是这个语义最直接的表达。

修正后 `verify-all.sh` **7/7 通过（195 秒）**。这条也说明 `verify-all.sh` 值回票价：
它是唯一能发现"新功能污染了别的验收脚本"的地方。

### 未做 / 遗留

- **快照写入仍在入队请求路径上**。Redis **拒绝连接**时几乎不花时间，但 Redis
  **挂起**（不回包）时会多等一个 Redis 超时（默认 500 ms），与 Gateway 的
  `match_timeout_ms`（也是 500 ms）同量级，理论上可能让一次入队超时。
  要彻底消除需要把快照写入移出请求路径（独立写入线程或定时器），
  那是引入并发的改动，先要有证据。记入 Backlog。
- 队列的**结果保留期**靠 `stamp_ms` 恢复，与房间快照一样是周期性快照，
  不构成"精确恢复"。
- 匹配队列快照只覆盖单实例（ADR-0003 非目标）。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-persistence.sh`
  与 `bash scripts/verify-all.sh`。
- 按第 9 节的分支粒度，本任务同样是 `feat/phase-2` 上的**单个提交**；
  TASK-014 与 TASK-015 各自一个提交，按顺序合并回 `main`
  （`git merge --no-ff feat/phase-2`），**不 squash 整条分支**。

## TASK-016 实施记录（2026-10-02）

### 背景

TASK-008 起 `connected` 字段只表示"是否已加入房间"，**网络断开根本不被感知**：
对局中刷新页面或断网，玩家永久缺席，而对局仍会推进到他输。本任务把「断线」变成
可观测、可恢复、有期限的状态。

项目所有者确认了四个决定：宽限期 **30 秒**；期内**暂停推进**；到期时
**只有一方断线则判断线方负**（新增 `finish_reason = disconnect`，产生胜负并落库）、
**双方都断线则作废**；双方断线时等**最后一位**到期。

### 完成

- `api/proto/room.proto`：`SetPlayerPresence` RPC、`PlayerState.online`、
  `FINISH_REASON_DISCONNECT`。
- `api/proto/gateway.proto`：`RoomPlayerInfo.online`（轮询兜底接口也要能看到）。
- `src/room/room_types.hpp`：`kReconnectGraceMs = 30s`、`FinishReason::kDisconnect`、
  `PlayerSnapshot.online`。
- `src/room/battle_room.{hpp,cpp}`：`SetPresence` / `IsWaitingForReconnect` /
  `ResolveReconnectGrace` / `Abort`；`Tick` 在有人断线时冻结推进。
- `src/room/room_manager.*`、`room_service.*`：新 RPC 的通路与错误码映射。
- `src/gateway/room_client.hpp`、`brpc_room_client.*`：`SetPresence` 接口与实现，
  以及 proto→内部结构的映射（`online`、`disconnect`）。
- `src/gateway/stream_hub.*`：订阅建立/移除/`CloseAll`/写失败四条路径都上报 presence；
  推送判据从"帧号"改为**状态签名**；推送 JSON 带上 `online`。
- `web/`：`connectRoomPush` 重连状态机（退避 1/2/4/5/5 秒、最多 5 次、累计 17 秒，
  留在 30 秒宽限期内）、4 种连接状态的界面提示与「重试连接」按钮。
- `scripts/verify-reconnect.sh`（新增）：真实验收。
- 测试：Room 宽限期状态机 8 个用例、StreamHub presence 上报 5 个用例、
  前端重连状态机 17 个用例。

### 决策

- **`online` 与 `connected` 必须是两个字段**。`connected` = "在房间里"，
  已被 `rooms.pN_joined` 与快照校验（"PLAYING 的房间必须双方都已加入"）依赖；
  改它的语义会破坏 TASK-014 的恢复逻辑。断线的玩家仍然在房间里。
- **presence 与宽限计时不落库**（`rooms` 表不加列）。Room 重启后把双方当作在线、
  计时清零，客户端重连后会重新上报。代价是"Room 重启期间到期的对局不会被判负"，
  记入 `docs/01-architecture.md` 的已知限制。**选择不加列**的理由是本任务的最小改动
  原则：加列就要改迁移、快照读写与校验，而收益只是恢复一个"重启前就已断线"的计时。
- **双方都断线时等最后一位到期**。Gateway 重启会让所有订阅同时断开；若按第一位
  到期就作废，任何一次 Gateway 重启都会立刻毁掉所有进行中的对局。
  由单测 `BothOfflineWaitsForTheLastOneToExpire` 锁定。
- **推送判据改为状态签名**（帧号 + 血量 + online + 阶段）。宽限期内帧号不动，
  只比帧号会让客户端在整个宽限期里**收不到任何事件**——而"对方断线了"恰恰是
  那一刻最该知道的事。
- **上报失败不重试**。`online` 是尽力而为的事实同步：下一次连接状态变化会覆盖它，
  为它做重试/补偿只会换来一套需要自己维护的状态机。

### 验证（WSL，16 核 / 11 GiB）

- `cmake --build --preset brpc-debug`：0 error / 0 warning。
- `ctest --test-dir build/brpc-debug`：**216/216 通过**（203 + 新增 13）。
- `bash scripts/check-format.sh`：通过（74 个文件）。
- `bash scripts/verify-reconnect.sh`：**退出码 0**。实测覆盖：
  SSE 订阅建立（`session.ready`）→ 杀掉流 → `players[0].online` 不再是 true →
  **帧号冻结**（23 → 23，3 秒内不动）→ 期内重连 → `online` 恢复 true、
  血量与断线时一致 → 继续推进（36 → 56）；
  第二次断开后真的等满 30 秒 → `state=finished`、`finish_reason=disconnect`、
  `winner_id=p-0002`、**结果已写入 `match_results`**；
  第三局双方都断线 → `state=aborted` 且 **`match_results` 里没有这一局**。
- `bash scripts/verify-web.sh`：**退出码 0（43 项通过）**；前端 `vitest` 29 通过
  （原 12 + 新增 17）、`vue-tsc --noEmit` 与 `vite build` 均通过。

### 问题与修正（本轮实际踩到的）

- **客户端断网这条路径原本漏上报**。`StreamHub` 清理写失败的订阅时是**直接 erase**，
  不经过 `Unsubscribe`，因此"客户端断网"——最常见的断线方式——从来不会上报 offline，
  Room 也就永远不会进入宽限期。是新写的单测
  `WriteFailureReportsPlayerOffline` 逼出来的：它第一次就失败了。
  修法是在锁外统一收集并上报（持锁不做 I/O）。
- **Gateway 的映射漏了两个新值**。`ToFinishReasonString` 没有 `FINISH_REASON_DISCONNECT`
  分支，会 fallthrough 返回 `"none"`；`ToSnapshot` 也没转 `online`。
  结果是"断线判负"在界面上显示成"没有结束原因"。**是验收脚本抓到的，不是单测**——
  因为单测只覆盖到 Room 内部，跨服务的字段映射只有端到端才会暴露。
- **proto3 的 JSON 会省略等于默认值的字段**，因此 `online=false` 时该字段
  根本不出现在响应里。验收脚本第一版按 `online == false` 断言，读到的永远是空串。
  判据改为"不是 true"，并**在同一段里保留一条反向检查**（重连后必须出现
  `online=true`），否则这条断言会退化成"永远成立"。
- **前端退避下标错位**（由前端子代理发现并修复）：初版在等待**之后**自增 `attempt`，
  而等待内部又读 `attempt` 取退避值，实际退避变成 2/4/5/5/5 秒，累计 31 秒
  ——**会吃掉整个 30 秒宽限期**。改为先取值再自增。

### 未做 / 遗留

- **前端没有做真实断线的端到端**：只验证到状态机与构建。真实断线由
  `scripts/verify-reconnect.sh` 在后端侧验证（用 curl 起真实 SSE 再杀掉它），
  浏览器的"拔网线"路径没有自动化验证。
- **`useGameSession` 的接线没有单测覆盖**：项目没有组件测试环境
  （无 `@vue/test-utils` / jsdom），加环境属于引入新依赖，本轮不引入。
- **不可重试的 HTTP 错误仍会耗完重试预算**（例如 `invalid_token` /
  `not_a_member` / `room_already_finished`）：现在会白重试约 17 秒才放弃。
  该由前端错误分类处理，记入 Backlog。
- **`Last-Event-ID` 补帧属 TASK-017**：本轮重连后直接拿最新快照；
  在"宽限期内暂停推进"的前提下不会丢帧，但这一点**没有独立验证**。

### 下一步

- 由项目所有者审阅 Diff 并运行 `bash scripts/verify-reconnect.sh`
  与 `bash scripts/verify-all.sh`。
- 按第 9 节的分支粒度，本任务是 `feat/phase-2` 上的**单个提交**；
  合并回 `main` 用 `git merge --no-ff`，**不 squash**。

## 分支粒度纠正记录（2026-10-02）

**问题**：本轮我按 Phase 1 的「一任务一分支」建了
`feat/task-014-room-recovery` 与 `feat/task-015-match-queue-snapshot` 两条分支，
把 TASK-014 拆成 4 个提交（含一个"存在崩溃待查"的 WIP 提交），
还向项目所有者建议 **squash 合并**。项目所有者指出这违反了已确认的约定。

**约定原文**（`docs/03-development-workflow.md` 第 9 节，2026-10-02 项目所有者确认）：

> **自 Phase 2 起改为「一阶段一分支 + 一任务一提交 + 每任务合并回 main」**
> ……**不要 squash 整条分支。** 一旦压成一个提交，就退化成「一次合并一堆任务」，
> 上面这条收益会全部消失。这是本约定唯一的硬性纪律。

**我的错误有三处**，而且第三处比前两处严重：

1. 建了任务级分支，而 Phase 2 只应保留 `feat/phase-2`。
2. 把 TASK-014 做成 4 个提交，而约定是"一任务一提交"。
3. **建议 squash 合并**——那恰好会把两个任务压成一次合并，
   正是约定里点名的"唯一硬性纪律"要防的事。

**根因**：`docs/03-development-workflow.md` 是必读顺序里的第 5 份文档，我漏读了它，
于是拿 Phase 1 的印象去推 Phase 2 的流程。项目文档已经把规则写清楚了，
这不是"规则不明确"的问题，是**没有读**。

**纠正**：

- 两个任务各压成**一个提交**放回 `feat/phase-2`（TASK-014 一个、TASK-015 一个）。
- 删除 `feat/task-014-room-recovery` 与 `feat/task-015-match-queue-snapshot`
  （本地与远端）。
- 本文档与 `docs/TASKS.md` 中所有指向任务级分支的记录一并改正。

**教训**：流程类约定和代码一样属于"必须查证的事实"。以后涉及分支、提交、合并、
发布这类流程动作前，先读 `03-development-workflow.md` 对应章节，
而不是沿用上一个阶段的习惯。

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
