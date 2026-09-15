# 验收清单

> 状态：已确认（2026-09-15）
>
> 用途：每个任务交付后，项目所有者按本清单逐条验证。清单只包含可以用命令或页面
> 复核的证据，不包含“看起来没问题”这类主观判断。

## 使用方式

1. 每条都有「命令」和「预期结果」，逐条执行并核对。
2. 任一条与预期不符，**停止验收**，把实际输出反馈给执行者，不要继续下一条。
3. 全部通过后，在「验收记录」里写明结论，再决定是否关闭任务。

## 第 0 步：确认本地与远程一致

**为什么**：如果本地不是远程的最新状态，后面所有验证都可能是在验证旧代码。

```bash
cd ~/workspace/realtime-game-backend
git fetch origin
git status --short
git log --oneline -n 3
git rev-list --left-right --count origin/main...main
```

预期结果：

- `git status --short` **无任何输出**（工作区干净）。
- `git log` 最新一条是本次任务的合并或提交。
- `git rev-list` 输出 `0	0`（左为远程独有、右为本地独有，都必须是 0）。

## 第 1 步：跑自动验收命令

**为什么**：这是唯一能证明“代码真的能构建、测试真的通过”的方式。文档和聊天都不能
替代它。

```bash
bash scripts/verify.sh
```

预期结果：

- 末尾出现 `===== 验收通过：全部预设构建与测试成功 =====`
- 每个预设都有 `100% tests passed, 0 tests failed`
- 出现 `格式检查通过`
- 退出码为 0（可用 `echo $?` 立即查看）

如需分步定位问题，可只跑单个预设：

```bash
bash scripts/verify.sh debug
```

## 第 2 步：确认没有不该提交的文件

**为什么**：构建产物、IDE 配置、密钥一旦入库，清理成本远高于预防成本。

```bash
git status --short --ignored | grep -E '^!!' | head -5   # 确认忽略规则生效
git ls-files | grep -E '\.idea/|^build/|\.o$|librgbt|_tests$' || echo "干净：无构建产物与 IDE 文件"
git ls-files | wc -l
```

预期结果：

- 第二条命令输出 `干净：无构建产物与 IDE 文件`。
- 文件总数与任务描述相符（不含突然暴增的文件）。

## 第 3 步：检查 CI 结果

**为什么**：本地能过不等于 CI 能过，环境差异只有 CI 能暴露。

浏览器打开：

```text
https://github.com/Archer11-q/realtime-game-backend/actions
```

预期结果：

- 最新一次运行对应本次合并的提交，且为绿色（`success`）。
- 若为红色：点进失败步骤，复制日志文本给执行者。

命令行替代方式（未安装 `gh` 时用第一条）：

```bash
curl -s "https://api.github.com/repos/Archer11-q/realtime-game-backend/actions/runs?per_page=3" \
  | grep -E '"head_branch"|"status"|"conclusion"'
```

## 第 4 步：人工审阅改动

**为什么**：自动检查验证“能不能跑”，人工审阅验证“该不该这样做”。

```bash
# 查看本次交付改了哪些文件
git diff --stat HEAD~1 HEAD

# 逐个查看关键文件的实际内容
git show HEAD:CMakeLists.txt
git show HEAD:.github/workflows/ci.yml
```

审阅时重点回答：

- 改动是否只覆盖任务声明的范围？有没有夹带未确认的功能。
- 是否违反 `docs/01-architecture.md` 的服务边界和数据所有权。
- 错误路径、边界情况和失败场景是否被处理或有记录。
- 是否有任务要求但没做的事。
- `docs/TASKS.md`、`docs/devlog.md` 是否已经反映本次改动。

## 第 5 步：核对文档与代码一致

**为什么**：本项目是“文档即真相”，文档与实现不一致会直接误导后续迭代。

```bash
grep -n "状态：" docs/TASKS.md | head -5
tail -40 docs/devlog.md
```

预期结果：

- `docs/TASKS.md` 中当前任务的状态与实际进度相符。
- `docs/devlog.md` 有本次任务的记录，包含真实的命令与结果。

## 第 6 步：给出验收结论

验收结束后，把结论写回 `docs/TASKS.md` 对应任务，格式如下：

```text
- 验收结果（YYYY-MM-DD）：通过 / 不通过
- 验收人：项目所有者
- 依据：第 1 步 verify.sh 输出、第 3 步 CI 运行编号
- 遗留问题：<无 / 具体问题>
```

## 第 7 步：关闭任务并合并

- 分支合并且 CI 绿色后，可以删除已合并的远程分支。
- `main` 保持始终可构建、可测试。
- 未解决的问题写入 Backlog 或 `docs/07-open-decisions.md`，不要塞进本次提交。

## 当前任务的验收速查（TASK-002）

| 步骤 | 命令 | 预期 |
|---|---|---|
| 0 | `git rev-list --left-right --count origin/main...main` | `0	0` |
| 1 | `bash scripts/verify.sh` | 三个预设全部通过，格式检查通过 |
| 2 | `git ls-files \| grep -E '\.idea/\|^build/'` | 无输出 |
| 3 | 打开 Actions 页面 | 最新运行为绿色 |
| 4 | `git diff --stat f685be0 HEAD` | 22 个文件，均为构建骨架与文档 |
| 5 | `grep -n 'TASK-002' docs/TASKS.md` | 状态与实施结果已更新 |
| 6 | 写入验收结论 | 见第 6 步模板 |

## 已知的常见误判

- **`docker info` 无响应不等于 Docker 未配置**：先确认 Docker Desktop 已启动。
  2026-09-15 曾因此产生一次误判，已更正。
- **`git status` 显示大量文件被修改**：多半是文件权限位被改（例如从 Windows 侧
  用 tar 覆盖），不是内容真被改动。用
  `git diff --ignore-cr-at-eol` 和 `ls -l` 确认权限后再处理。
- **CI 完全不运行**：GitHub 只从默认分支注册工作流，文件只在功能分支时不会触发。
