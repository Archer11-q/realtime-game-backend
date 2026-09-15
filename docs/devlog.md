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
- 结果：WSL 中 `docker` 由 `/mnt/c/.../docker` 解析到 Windows 客户端，但执行
  `docker info` 报 `The command 'docker' could not be found in this WSL 2 distro`，
  即 Docker Desktop 的 WSL Integration 当前未对本发行版启用。这条与 2026-09-14
  日志中“Docker Desktop WSL Integration 已验证可用”的结论**不一致**，列入待确认。
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
- CI 状态：功能分支已推送为 `e17fdc1`（随后追加共享依赖修正），但 **CI 尚未运行**。
  经 GitHub API 核实，仓库为 public 且未禁用，`.github/workflows/ci.yml` 确实存在
  于远程功能分支，然而 `actions/workflows` 的 `total_count` 为 0，即工作流尚未被
  GitHub 注册。原因是 GitHub 只从默认分支注册工作流，仅存在于功能分支的 `push`
  事件不会触发。因此本任务未取得任何 CI 运行结果，`TASKS.md` 中“CI 成功”这一
  验收项仍待验证。
- `ubuntu-24.04` 上的 CMake 3.28 与 Preset 的 `cmakeMinimumRequired 3.25` 兼容，
  但 CI 结果仍需以真实运行数据为准。
- TASK-003 依赖的 Docker 在 WSL 中当前不可用（见上文），需要在 TASK-003 前解决。
- TASK-004 依赖的 `openssl`、`gflags`、`glog` 开发头文件缺失。
- `docs/07-open-decisions.md` 第 1 节“AI 分工”和第 3 节风险表中仍保留按厂商名称
  绑定的旧表述，与已修改的 `CLAUDE.md`、`README.md`、
  `docs/03-development-workflow.md` 不一致，待项目所有者确认后统一。

### 下一步

- 由项目所有者审阅 Diff 与验收命令后合并到 `main`。
- 合并到 `main` 后确认 CI 首次运行结果，这是 TASK-002 最后一项未完成的验收。
- 处理 `docs/07-open-decisions.md` 中仍未更新的按厂商绑定分工表述。
- 恢复 Docker WSL Integration，为 TASK-003 做准备。

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
