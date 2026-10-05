# 运行手册（Runbook）：故障处置与排查

> 状态：草案（2026-10-05 初稿，等待项目所有者验收）
>
> **与 [环境与运维](06-operations.md) 的分工**：那一份讲**怎么起停、怎么配置**
> （WSL、Docker、端口、Compose、备份、发布）。这一份讲**坏了怎么办**——
> 怎么发现、怎么处置、恢复到什么程度、哪些情况**不恢复**。
> 需要启动命令时看 `06-operations.md`，需要处置步骤时看这里。
>
> 本文的每一条都指到实测出处（任务单的「实施结果」、`docs/devlog.md` 的
> 「实施记录」，或 `docs/benchmarks/raw/`）。**没有出处的数字不许写进本文**；
> **"设计意图"与"实测行为"分开写**，未实测的地方明确标注。

## 0. 怎么用这份手册

### 0.1 三条前提（不先读会把数字读错）

1. **全部数字来自本机实测**：WSL2 Ubuntu 26.04、16 vCPU、11 GiB、
   `brpc-debug` 预设（**Debug，未开优化**）、压载端与被测服务**同机**、端口固定
   8080/8082/8083。换机器、换构建类型、换部署形态，数字不可比。
   **Release 构建未测**：要用于容量规划必须重跑 `release` 预设。
2. **恢复时间不含"重启依赖本身"的耗时**。口径定义在 `chaos/lib.sh` 第 21~25 行：
   - **检测时间** = 注入依赖不可用 / 杀进程 → 首次观测到预期失败；
   - **恢复时间** = **发起重启（或依赖恢复）** → 业务重新可用（含 brpc 长连接重建与
     首次成功调用）。
   时刻一律用 bash 的 `EPOCHREALTIME`：本机 `date +%s%3N` **不是**真实毫秒
   （coreutils 9.x 的行为，见 `docs/TASKS.md` 的 Backlog 条目）。
3. **三个场景按语义没有"恢复时间"这个量**（连接风暴、排空、长稳）——硬凑数字就是
   编造。它们各自的**等价量**写在对应小节里并标注"该量不适用"。
   完整论证见 `docs/devlog.md`「三个场景为什么没有恢复时间」。

### 0.2 一条命令重现：`scripts/demo.sh` 的九个步骤

本文里"怎么重现"一律指向这九个步骤，不另写一套命令。
`bash scripts/demo.sh --list` 打印它们；**实测 7/7 自动环节通过、总耗时 321 s**
（上限 900 s = 15 分钟。第一轮曾 1138 s 超限，唯一主因是步骤 7 占了 793 s）。

| 步骤 | 环节 | 演示里实际执行 | 本手册的相关小节 |
|---:|---|---|---|
| 1 | 登录与会话 | `bash scripts/verify-login.sh` | §1 通道 A |
| 2 | 匹配与配对 | `bash scripts/verify-match.sh` | §4、§11 |
| 3 | 进房、对战与结算 | `bash scripts/verify-room.sh` | §2 通道 B、§3 |
| 4 | 服务端推送（SSE） | `bash scripts/verify-stream.sh` | §5、§10 |
| 5 | 持久化与恢复 | `bash scripts/verify-persistence.sh` | §3、§4 |
| 6 | 断线重连与补发 | `bash scripts/verify-reconnect.sh` | §6 |
| 7 | 依赖不可用与恢复 | **默认只打印已实测结论摘要**；`bash scripts/demo.sh --full` 才真跑 `chaos/verify-dependency-down.sh`（单跑实测 **793 s**，会把演示顶到 19 分钟、超过 15 分钟上限） | §1、§2 |
| 8 | 优雅退出与排空 | `bash chaos/verify-drain.sh` | §8 |
| 9 | 人工核对 | `bash scripts/dev-up.sh` + 浏览器（Canvas 渲染 / 按钮状态 / 视图切换**无法自动判定**） | — |

演示之外的命令（占标准端口或耗时以十分钟计）。**改动 `src/` 之后两条都必须跑**
（这是 TASK-029 / TASK-035 各留下一次的教训）：

```bash
bash scripts/verify-all.sh                               # 快速门禁 9 个（实测 230 ~ 256 s）
bash scripts/verify-chaos.sh                             # 故障类 5 个（实测 ≈1300 s）
bash scripts/verify-chaos.sh --only dependency-down,process-crash
bash scripts/verify-chaos.sh --include-soak               # 再加 30 分钟长稳
bash chaos/verify-soak.sh --duration 1800 --players 50      # 长稳 30 分钟（上限 1 小时）
bash scripts/bench.sh --level 500 --duration 60             # 容量
```

**各脚本接受的参数（不要编造开关）**：

| 脚本 | 参数 |
|---|---|
| `chaos/verify-dependency-down.sh` | 只有 `--keep` |
| `chaos/verify-process-crash.sh` | 只有 `--keep` |
| `chaos/verify-drain.sh` | 只有 `--keep` |
| `chaos/verify-connection-storm.sh` | `--keep`、`--players`（默认 400）、`--duration`（默认 20） |
| `chaos/verify-soak.sh` | `--duration`（默认 1800 / 上限 3600）、`--players`（默认 50）、`--churn-every`（60）、`--churn-fraction`（0.2）、`--interval`（10）、`--settle`（60）、`--keep` |
| `scripts/verify-reconnect.sh` | `--keep`、`--no-docker` |
| `scripts/verify-chaos.sh` | `--include-soak`、`--only <名字,逗号分隔>`、`--list`、`--fail-fast` |
| `scripts/verify-all.sh` | `--only <名字>`、`--list`、`--fail-fast` |
| `scripts/demo.sh` | `--fast`、`--full`、`--list` |

要**只跑某一个故障场景**，用统一入口的 `--only`（脚本名：`dependency-down` /
`process-crash` / `drain` / `connection-storm` / `reconnect` / `soak`）。

### 0.3 证据从哪来

| 来源 | 位置 |
|---|---|
| 指标 | 三个服务各自的 `GET /metrics`（Prometheus 文本格式）：Gateway `:8080`、Match `:8082`、Room `:8083`。brpc 内置 `GET /health`（探活）与 `GET /status`。监控栈 `:9090` / Grafana `:3000`（`bash scripts/observability-up.sh` / `-down.sh`） |
| 日志 | 结构化单行 `key=value`，恒定字段 `ts/service/level/event/trace`；`dev-up.sh` 起的服务写在 `.run/<service>.log`，chaos 脚本的写在 `.run/chaos-<name>.log`，演示的写在 `.run/demo-<时间戳>/` |
| 原始数据 | 容量与长稳有归档：`docs/benchmarks/raw/<目录>/`。**四类故障注入（依赖不可用 / 进程崩溃 / 连接风暴 / 排空）没有 raw 归档**，数字只在任务单与 devlog 里——引用时不要虚构 `docs/benchmarks/raw/<name>/` 路径（见 §13） |

**日志字段要点**：`event=` 是事件名；`trace=` 就是 `request_id`（没有 id 时**不输出该
字段，也不编造**）。TASK-030 之后所有错误路径收敛到一条
`event=request_failed http_status=<N> error_code=<...> reason=<...>`，
于是"返回了 503"在日志里查得到——**在此之前（TASK-023 时期）查不到**，
旧文档里"503 无日志"的描述已过时。

### 0.4 分诊表（先看现象）

| 现象 | 最可能的原因 | 跳到 |
|---|---|---|
| 登录 `503 session_store_unavailable` | Redis 不可用（Gateway 会话通道） | §1 A |
| 登录 `503 player_store_unavailable` | MySQL 不可用（Gateway 档案通道） | §2 A |
| 匹配照常成功，但日志有 `queue_snapshot_write_failed` | Redis 不可用（Match 快照通道，**按设计降级**） | §1 B |
| 入队 `503 match_unavailable` | Match 进程没起 / 端口不通 / **brpc 通道未预热** | §4 |
| 入队 `429 match_queue_full` | 队列已达上限（限流类，退避重试） | §4 |
| 房间接口 `503 room_unavailable` | Room 进程没起 / 正在重启 | §3 |
| 查询结果 `503 result_pending` | 结果未落库（**不是 404**；MySQL 不可用或正在重试） | §2 B |
| 查询结果 `503 result_store_unavailable` | MySQL 不可用，且**这一局本次启动没被恢复** | §2 B、§12 |
| 对局卡住不推进、SSE 停在最后一帧 | 有人断线，**宽限期内对局暂停**（预期行为） | §6 |
| 客户端顶栏「推送已断开」、`rgbt_sse_connections` 归零 | Gateway 崩溃或重启（订阅表在内存里） | §5 |
| `rgbt_sse_connections` 静默期不回落 / `skipped_no_write_total` 增长 | SSE 订阅生命周期缺陷（TASK-029 **已修**，先确认版本） | §10 |
| 新连接大量**超时**（不是 HTTP 错误），日志 `Too many open files` | 达到进程 fd 上限（**当前没有显式限流**） | §7 |
| SIGTERM 后进程不立刻退出 | 正在排空（**预期行为**） | §8 |
| 500 档位入队 p95 ≈ 230 ms 而 Match handler 只有 ~6 ms | 成因在 brpc 接入/传输层，**七个方向已被否证** | §11 |

### 0.5 指标与事件名清单（速查）

| 服务 | 指标（`/metrics`） |
|---|---|
| Gateway | `rgbt_http_requests_total{path,status}`、`rgbt_http_request_seconds{path}`、`rgbt_sse_connections`、`rgbt_sse_subscriptions_created_total`、`rgbt_sse_subscriptions_closed_write_failed_total`、`rgbt_sse_subscriptions_closed_other_total`、`rgbt_sse_subscriptions_skipped_no_write_total`、`rgbt_sse_oldest_subscription_age_ms`、`rgbt_push_backfilled_frames`、`rgbt_push_reset_total{reason}`、`rgbt_rpc_calls_total{target,outcome}`、`rgbt_gateway_match_rpc_max_ms{op}` |
| Match | `rgbt_match_queue_length`、`rgbt_match_events_total{event}`、`rgbt_match_stage_max_ms{op,stage}`、`rgbt_match_room_allocate_total{outcome}`、`rgbt_match_room_allocate_ms`、`rgbt_match_room_allocate_max_ms`、`rgbt_queue_snapshot_total{outcome}`、`rgbt_match_queue_snapshot_pending`、`rgbt_match_queue_snapshot_lag_ms`、`rgbt_match_queue_snapshot_write_ms`、`rgbt_match_queue_snapshot_merged_total`、`rgbt_match_queue_snapshot_ticks_total` |
| Room | `rgbt_rooms{phase}`、`rgbt_room_frames_advanced_total`、`rgbt_snapshot_write_total{outcome}` |

> **两个指标不要依赖（实测发现）**：
> 1. `rgbt_result_persist_total`（结果落库重试）与 `rgbt_room_events_total` 这两个名字在
>    `include/common/metrics.hpp` 第 263/266 行**已定义，但没有登记**（`src/` 下没有使用
>    点）。后果：**结果落库重试没有指标出口**，只能用 `rgbt_rooms{phase="finishing"}`
>    与日志 `result_persist_failed` 代替。
> 2. `rgbt_match_events_total{event="paired"}` 在压载下读数为 **0**，而同一次压载里
>    `rgbt_match_room_allocate_total{outcome="ok"}=250`、压载端自报 `paired=500`
>    （原始数据 `docs/benchmarks/raw/20261005-212008/`）。原因是 TASK-035 方案 A 把配对
>    移进分配 worker 之后，只有"入队这一次调用内当场配对"才计数
>    （`src/match/match_service.cpp` 第 215~224 行）；**低负载走空闲快路径时仍会计数**，
>    所以单次验收（`verify-observability.sh`）看不出来。**不要用它做负载下的配对判据**，
>    用 `rgbt_match_room_allocate_total`。两条都已记入 `docs/TASKS.md` 的 Backlog。

事件名（`event=`）常用值：`request_done`、`request_failed`、`match_enqueued`、
`match_enqueue_ok`、`match_enqueue_failed`、`match_enqueue_rejected`、
`match_allocation_worker_started` / `_stopped`、`queue_snapshot_write_failed`、
`queue_snapshot_load_failed`、`queue_snapshot_entry_skipped`、`queue_snapshot_final_flush`、
`room_created`、`room_create_rejected`、`room_joined`、`presence_reported`、
`presence_report_failed`、`room_restored`、`restore_load_failed`、`room_snapshot_rejected`、
`room_snapshot_write_failed`、`result_persist_failed`、`match_result_queried`、
`subscribe_ready`、`subscribe_rejected`、`stream_reset_sent`、`backfill_done`、
`backfill_unavailable`、`drain_started`、`drain_finished`、`drain_timeout_abort`、
`drain_timeout_close`、`mysql_connect_failed`、`mysql_connection_lost`、
`player_reader_failed`、`create_room_call_failed`、`create_room_rejected`、
`service_start_failed`、`service_register_failed`、`metrics_register_failed`。

---

## 1. Redis 不可用

三个服务里有两个把 Redis 当作会话存储或旁路快照，因此"Redis 挂了"是**两条性质完全
不同的通道**；混在一起看会把"降级"误判成"故障"。注入必须**按通道**做。

> **绝对不要用 `docker stop rgbt-redis` 来验证单条通道**：三个服务共用同一个 Redis，
> 停掉之后所有 HTTP 请求先因鉴权失败返回 503，根本走不到被测通道。隔离只能做在
> **网络层**：让被测服务连 `chaos/relay.py`，杀掉中继即只切断那一条
> （TASK-015 与 TASK-023 各踩过一次）。

### 通道 A：Gateway 的会话存储（中继 `16379 → 6379`）

**症状（怎么发现）**
- 登录 `POST /api/v1/login` 返回 **503**，响应体 `reason=session_store_unavailable`；
- 日志 `event=request_failed http_status=503 reason=session_store_unavailable`
  （TASK-030 之后才有）；
- 指标 `rgbt_http_requests_total{path="/api/v1/login",status="503"}` 增长；
- 脚本断言：`通道 1：注入后 12 秒内登录未返回 503（注入可能未生效）`、
  `通道 1：Gateway /metrics 的 503 计数没有增加`、
  `隔离：Match 或 Room 在通道 1 注入期间不健康`——**隔离是自证的**，不是只在文档里声称。

**影响范围**：登录与一切依赖会话解析的请求（匹配、进房、订阅）。已建立的 SSE 长连接
**不受影响**（订阅表在 Gateway 内存里），对局在 Room 里独立推进。

**处置**
```bash
bash scripts/dev-down.sh                       # 清残留（占 8080/8082/8083）
bash chaos/verify-dependency-down.sh           # 四通道三段式实测（约 13 分钟）
bash chaos/verify-dependency-down.sh --keep    # 保留服务与中继便于排查
# 手动确认依赖本身：
redis-cli -h 127.0.0.1 -p 6379 PING
docker ps --filter name=rgbt-redis
docker start rgbt-redis                        # 起回来即自愈，**不需要重启 Gateway**
```

**恢复到什么程度**
- **实测检测 11 ~ 13 ms、恢复 14 ~ 15 ms**（多轮区间；单轮值 12 / 14 ms）；
- **完全自愈，不重启服务**：依赖恢复后第一次登录即成功；
- **无数据丢失**：会话不因依赖抖动而被删除（键有 TTL，但那与本次故障无关）。

**已知边界**
- **Redis "挂起"（接受连接但不回包）从未被单独实测**：实测注入是**切断中继**（连接被
  拒/重置）。`docs/TASKS.md` 只写"挂起时会多等一个 Redis 超时（默认 **500 ms**），与
  Gateway 的 `-match_timeout_ms`（也是 500 ms）同量级，**理论上**可能让一次入队超时"。
  **这是设计推断，不是实测行为**（TASK-015 记录，未修，见 Backlog 的连接不可用冷却期）。
- 会话存储不可用期间**不写假成功**：返回 503，不返回空会话。

**相关指标与日志字段**
`rgbt_http_requests_total{path,status="503"}`、`rgbt_rpc_calls_total{outcome="failed"}`、
`event=request_failed`、`event=match_enqueue_failed`、`event=subscribe_rejected`。

**出处**：`chaos/verify-dependency-down.sh`（断言在第 553/557/571/576 行）、
`docs/devlog.md`「TASK-023 实施记录」「推送连续性与恢复时间汇总」、
`docs/TASKS.md` 的 TASK-023/TASK-015 任务单。**无 raw 归档。**

### 通道 B：Match 的队列快照（中继 `16380 → 6379`）

**症状（怎么发现）**
- **业务照常成功**：入队、配对、领取匹配结果全部正常（这是设计，不是缺陷）；
- 日志 `event=queue_snapshot_write_failed`；
- 指标 `rgbt_queue_snapshot_total{outcome="failed"}` 增长、`rgbt_match_queue_snapshot_lag_ms`
  变大、`rgbt_match_queue_snapshot_pending` 停在 1；
- 可观测的对照现象是"**内存队列非空而 Redis 快照为空**"；
- **没有"业务失败"可观测**——所以这一通道**"检测时间"这个量不适用**：硬凑一个
  "从注入到业务失败"的时间是错误的度量（脚本头部对此有明确说明）。

**影响范围**：仅"Match 重启后能否恢复排队状态"这一项能力。队列的**权威状态在 Match
进程内存里**，Redis 只是旁路快照（`CLAUDE.md`：Redis 不作为唯一真相）。

**处置**
```bash
bash chaos/verify-dependency-down.sh           # 通道 2 随四通道一起跑（无单通道开关）
bash scripts/verify-chaos.sh --only dependency-down
docker start rgbt-redis                        # 恢复后下一次队列变化自然写回，无需重启 Match
```
**不要用 `docker stop rgbt-redis` 测这一通道**（理由见本节开头）。TASK-015 期间的第一版
验收就是这么测的，结果"所有请求在鉴权那一步 503"，**测错了对象**，后来改成只让 Match 的
快照存储指向一个不可用端口。

**恢复到什么程度**
- 首次快照写失败 **137 ~ 141 ms**（单轮 142 ms）、依赖恢复后 **100 ~ 113 ms**（单轮
  118 ms）内重新写成功；
- 期间"匹配仍可用"另测 **116 ~ 122 ms** 内完成；
- 快照是**整份重写 + 原子替换**（`MULTI/DEL/RPUSH.../EXEC`），不会写出半个队列。

**已知边界**
- **快照允许丢**：写入失败不重试、不阻塞（TASK-015 决策 B）。Redis 不可用期间的
  最后一次队列变化不会进快照，**若此时 Match 重启，该变化丢失**。
- Match **重启时**若 Redis 不可用 → **一个条目都不恢复**，日志
  `event=queue_snapshot_load_failed` 并如实报告"本次未恢复任何排队状态"——
  不假装"恢复了一个空队列"。此时所有排队玩家对外表现为 `idle`。
- **TASK-028 之后是新语义**（与 TASK-023 的实测不是同一版代码）：快照改为**合并 +
  异步刷写**（默认合并窗口 100 ms，`-snapshot_merge_interval_ms`），因此崩溃最多丢
  **一个合并窗口内的队列变化**；正常 SIGTERM 由 `FlushSnapshotNow` 保证排空返回前最后
  一次变化已落盘（日志 `event=queue_snapshot_final_flush`）。

**相关指标与日志字段**
`rgbt_queue_snapshot_total{outcome}`、
`rgbt_match_queue_snapshot_{pending,lag_ms,write_ms,merged_total,ticks_total}`、
`event=queue_snapshot_write_failed`、`event=queue_snapshot_load_failed`、
`event=queue_snapshot_entry_skipped`、`event=queue_snapshot_encode_failed`。

**出处**：`chaos/verify-dependency-down.sh`、`docs/devlog.md`「TASK-023 实施记录」
「TASK-015 实施记录」「TASK-028 实施记录」、`docs/TASKS.md` 的 TASK-015/TASK-028 任务单。

---

## 2. MySQL 不可用

同样是两条通道，性质不同：Gateway 的档案读取是**硬失败**，Room 的快照与结果是
**延迟**。注入按通道做（中继 `13307 → 3306` 与 `13306 → 3306`）。

### 通道 A：Gateway 的玩家档案（中继 `13307 → 3306`）

**症状（怎么发现）**
- 登录 **503**，`reason=player_store_unavailable`；
- 日志 `event=request_failed ... reason=player_store_unavailable`、
  `event=player_reader_failed`、`event=mysql_connect_failed`；
- 指标 `rgbt_http_requests_total{path="/api/v1/login",status="503"}` 增长；
- 断言：`通道 3：登录错误码`、`通道 3：注入后 12 秒内登录未返回 503`、
  `通道 3：Gateway /metrics 的 503 计数没有增加`。

**影响范围**：只有登录（及一切需要读档案的路径）。匹配、房间、推送不受影响。

**处置**
```bash
bash scripts/verify-chaos.sh --only dependency-down     # 含通道 3
docker ps --filter name=rgbt-mysql && docker start rgbt-mysql
# 恢复后**不需要重启 Gateway**
```
**恢复到什么程度**：实测**检测 9 ~ 11 ms、恢复 18 ~ 20 ms**（单轮 9 / 21 ms）；
完全自愈；**无数据丢失**（读取失败不产生写入）。

**已知边界**：MySQL 不可用期间**不写假成功**——不会用"账号不存在"掩盖依赖故障。
`/health` 只探活（brpc 内置），**不代表依赖可用**；依赖可用性要看 `/metrics` 的 503 计数
与日志。区分存活与就绪的 `/health/ready` 在 Backlog 里（尚无实现）。

### 通道 B：Room 的快照与对局结果（中继 `13306 → 3306`）

**症状（怎么发现）**
- 运行中查询对局结果返回 **503 `result_pending`**——**不是 404，也不返回内存里的胜负**；
- **对局照常推进并打完**（脚本有反证断言：`通道 4：注入后对局停住了` 若命中即为失败）；
- 日志 `event=room_snapshot_write_failed`（快照，可丢）、`event=result_persist_failed`
  （结果，不可丢，按 1 秒重试）、启动期 `event=restore_load_failed`；
- 指标 `rgbt_snapshot_write_total{outcome="failed"}` 增长、
  `rgbt_rooms{phase="finishing"}` 持续 > 0（**这是"结果停在 FINISHING"唯一可用的指标**，
  见 §0.5）。

**影响范围**：结果落库被推迟；对局推进速度被拖慢（见下）。

**处置**
```bash
bash scripts/verify-chaos.sh --only dependency-down
docker start rgbt-mysql                        # 恢复后结果在下一个重试周期（1 秒）内写入
# 核对幂等：同一个 match_id 不应产生第二行
mysql -h 127.0.0.1 -u"$MYSQL_USER" -p"$MYSQL_PASSWORD" realtime_game \
  -e 'SELECT match_id, COUNT(*) c FROM match_results GROUP BY match_id HAVING c > 1;'
```

**恢复到什么程度**
- 查询在恢复后 **563 ~ 962 ms** 内变成 200（区间取决于"依赖恢复"与"Room 下一次 1 秒
  重试"的相位，单轮实测 723 ms）；
- **对局结果不丢**：结果停在 `FINISHING` 并**无限重试**（不设放弃上限）；
- **不产生重复行**：`match_id` 是幂等业务键（脚本断言期望 1 行 / 0 个重复组）；
- 注入期间帧照常前进（实测 **435 ~ 470 ms 推进 5 帧**）。

**已知边界（三条重要的）**
1. **对局推进会被拖慢一个数量级**：基线一局 **4.1 ~ 16.7 秒**，注入后脚本观测到终态用了
   **755 秒**（同期帧号只到 48 帧）。量级与"快照每 1 秒调度一次 + 每次写失败等一次
   **3 秒** MySQL 连接超时"吻合（755 次 × 1 秒 ≈ 755 秒）。成因是
   **快照写入在 Room 的 ticker 线程上**（`RoomManager::Tick` → `FlushSnapshots`），
   该期间推进是**突发式**而不是严格 10 Hz（"停一次、再连续推几十帧"）。
   **正确性不受影响，只影响推进速度**；修复方向是独立写入线程，属 Backlog。
2. **`FINISHING` 房间无限重试、无放弃上限**：MySQL 长期不可用会让 `FINISHING` 房间
   持续累积（有意取舍：不可再生的数据优于内存占用）。
3. **Room 重启时 MySQL 不可用 → 本次启动一个房间都不恢复**（`restore_load_failed`），
   此时查询返回 **503 `result_store_unavailable`** 而非 404——服务不假装"没有这条结果"。
   依赖恢复后**再重启一次**，`finishing` 快照会被恢复并落库（实测恢复点 `frame=600`、
   结果恰好 1 行）。准确说法是"**这次启动没有恢复**"，不是"数据丢了"。

**相关指标与日志字段**
`rgbt_rooms{phase="finishing"}`、`rgbt_snapshot_write_total{outcome}`、
`rgbt_room_frames_advanced_total`（判断推进是否被拖慢）、
`event=result_persist_failed`、`event=room_snapshot_write_failed`、
`event=restore_load_failed`、`event=mysql_connect_failed`、`event=mysql_connection_lost`。

**出处**：`chaos/verify-dependency-down.sh`（通道 4 断言在 763/771/808/813/839/847/854 行）、
`docs/devlog.md`「TASK-023 实施记录」「推送连续性与恢复时间汇总」、
`docs/01-architecture.md` 第 5 节的已知限制、`docs/TASKS.md` 的 Backlog
「把快照写入从 ticker 线程移出去」。**无 raw 归档。**

---

## 3. Room/Battle 崩溃与恢复（`kill -9`）

**症状（怎么发现）**
- 检测：房间查询从 200 变 **503**（Gateway 日志 `op=get_room_state status=503`）
  ——实测 **22 ~ 29 ms** 内首次观测到失败；
- 重启后：日志 `event=room_restored frame=<N>`；
- 若启动时存储不可用：`event=restore_load_failed` + `consequence=本次启动不恢复任何房间`；
- 脚本断言（看到这些就是没恢复好）：`恢复点不等于最后快照`、`血量与快照不一致`、
  `回退帧数超出预期`、`丢失窗口异常`、`match_results 行数/重复异常（期望 1 / 0）`。

**影响范围**：该 Room 进程下**所有**房间。Gateway 与 Match 不受影响；会话仍在 Redis 里，
因此客户端不需要重新登录。

**处置**
```bash
bash chaos/verify-process-crash.sh            # 场景 1/2/2b 与 3a/3b/4 依次全跑（单轮 50~57 s）
bash chaos/verify-process-crash.sh --keep
bash scripts/verify-chaos.sh --only process-crash
bash scripts/dev-up.sh                        # 或单独重启 Room
# 启动日志是"恢复了几局"的唯一入口：
grep -E "room_restored|restore_load_failed|room_snapshot_rejected" .run/room.log
# 核对库里没有残留的 playing/finishing：
mysql ... -e "SELECT state, COUNT(*) FROM rooms GROUP BY state;"
```
恢复动作是**启动同步路径**（在开始接受请求之前完成）——不要改成异步：否则同一客户端会
先查到"房间不存在"、几十毫秒后又查到房间，它无从判断哪次为真。

**恢复到什么程度（4 轮区间）**

| 项 | 实测 |
|---|---|
| 检测时间（对局中崩溃） | **22 ~ 29 ms** |
| 恢复时间（发起重启 → 可用） | **2910 ~ 3000 ms**，其中进程重启本身 **522 ~ 532 ms** |
| 恢复点精度 | **精确等于最后一次快照**（读启动日志 `room_restored` 的 `frame`） |
| 丢失的帧 | **1 ~ 4 帧**（观测），**上界 10 帧** = 一个快照间隔（1 秒） |
| 丢失窗口（最后快照 → 崩溃） | 171 ~ 439 ms |
| 未丢失的字段 | 房间、成员、`match_id`、双方 HP、`room_id`（逐项与快照比对一致） |
| 恢复后能否打完 | 能，继续推进直到产生真实胜负 |
| `match_results` 行数 | **恰好 1 行**（崩溃+重启不破坏幂等） |

更早一轮（TASK-014）的独立实测：3 次丢失 **0 / 0 / 1 帧**，与"上界 10 帧"一致。

**场景 2：`FINISHING`（结果待落库）崩溃 —— 无丢失。** 恢复时间 **544 ~ 552 ms**
（进程重启 524 ~ 527 ms）；恢复后 `frame=600`，结果被重新纳入重试并落库，恰好 1 行。
这是 TASK-008 "已结束未落库的对局重启会丢"那条已知限制的**正解**。

**场景 2b：重启时 MySQL 不可用 —— 一个房间都不恢复**，如实报 `load_failed`（详见 §2 通道 B
的边界第 3 条）；存储恢复后再重启的恢复时间 **831 ~ 863 ms**。

**已知边界**
1. **一个未定位的差异（如实记录，未修）**：对局中崩溃恢复 **~3.0 s**，而同为重启 Room 的
   `FINISHING` 场景只要 **~0.55 s**。日志显示 Room 本身约 **0.5 s** 就恢复好了
   （`room_restored`），其余约 **2.5 s** 是 **Gateway 侧通道重建**，且是**快速失败**
   （每次约 64 ms）而不是等超时。代码事实：`src/gateway/brpc_room_client.cpp` **没有**设
   `health_check_interval`，取 brpc 默认 **3 s**，与观测周期吻合——**但这是推断，不是已证实
   的因果**（A/B 实验"给场景 2 补一次停机期失败调用"**没有复现**）。已记入 Backlog。
2. **presence 与宽限计时不落库**：Room 重启后所有玩家被当作**在线**、宽限计时清零。
   代价是"**Room 重启期间到期的对局不会被判负**"，而是回到正常推进。
3. **快照可以丢弃**：写入失败不重试，下一个周期自然覆盖 → "可用的最后一版"可能落后
   实时状态一个间隔（上界 1 秒 = 10 帧；每次攻击 10 点血、满血 100，即最多少算或多算
   一次攻击，占满血 **10%**）。
4. **停机期间的帧被丢弃**，不在启动瞬间补齐（避免启动 CPU 尖峰；代价是对局在墙钟上被
   拉长，与 `kMaxCatchUpFrames` 同理）。
5. **损坏的快照行不静默丢弃**：血量越界、双方都没加入、状态非法等一律记
   `event=room_snapshot_rejected` 并把该行**改写为 `aborted`**（否则每次启动都会被重新
   扫出来、重新被拒绝，而事后核对时看不出这一局怎么了）。
6. **恢复失败的三种情况必须可区分**：①存储不可用 → 一个房间都不恢复（`load_failed`）；
   ②某一行损坏 → 改写成 `aborted`；③与内存中已有房间冲突 → 保留内存里的那份并跳过。
7. **多进程同时崩溃未测**：需要多副本，属 ADR-0003 非目标。

**相关指标与日志字段**
`rgbt_rooms{phase}`、`rgbt_room_frames_advanced_total`、`rgbt_snapshot_write_total{outcome}`、
`event=room_restored`、`event=restore_load_failed`、`event=room_snapshot_rejected`、
`event=reject_writeback_failed`、`event=room_snapshot_write_failed`、`event=result_persist_failed`。

**出处**：`chaos/verify-process-crash.sh`（断言在 660/709/717/728/739/782 行）、
`docs/devlog.md`「TASK-024 实施记录」「TASK-014 实施记录」「推送连续性与恢复时间汇总」、
`docs/01-architecture.md` 第 5 节「房间状态的恢复边界」、`docs/TASKS.md` 的 TASK-024 任务单。
**无 raw 归档。**

**重现**：不在 `demo.sh` 里（占标准端口），用上面的命令。

---

## 4. Match 崩溃与恢复（`kill -9`）

**症状（怎么发现）**
- 入队返回 **503 `match_unavailable`**；轮询 `/api/v1/matches/current` 也失败；
- 重启后日志会打印"恢复了几人 / 是否 `load_failed`"；
- 排队中的玩家在恢复后仍是 `queued`（3a），或已回到 `idle`（3b）；
- 脚本断言：`场景 3a：Redis 队列快照为空，重启无从恢复（前置不成立）`、
  `场景 3a：重启后 12 秒内排队状态未恢复`、`场景 3b：预期不再是 queued`、
  `场景 3b：启动日志没有报告超时未恢复`（后者的日志串是"已超时未恢复 1 人"）。

**影响范围**：匹配链路。房间与已开始的**对局不受影响**（Room 独立推进），
已登录会话不受影响。已完成的配对结果也不丢（在内存 + Redis 快照）。

**处置**
```bash
bash chaos/verify-process-crash.sh            # 含 3a / 3b
bash scripts/verify-chaos.sh --only process-crash
bash scripts/dev-up.sh                        # 或单独重启 Match
grep -E "queue_snapshot|match_allocation_worker" .run/match.log
```
**注意**：Match 每次重启后 **Gateway→Match 的 brpc 通道需要预热**——不预热时紧随的第一次
入队会拿到 503 `match_unavailable`（服务其实是好的）。chaos 脚本会显式预热；手工排查时
先打一次轻量请求再下结论。

**恢复到什么程度**

| 场景 | 恢复时间（发起重启 → 可用） | 丢失 |
|---|---|---|
| 3a 排队中崩溃 | **607 ~ 655 ms**（其中进程重启 523 ~ 532 ms） | **无**：排队状态从 Redis 快照恢复，`queued_at` 仍是原入队时刻 |
| 3b 停机时长**超过**排队超时（脚本构造为 3 秒） | 检测时间 **5528 ~ 5535 ms**（停机时长） | **该条目按设计不再恢复**：日志"已超时未恢复 1 人"，查询回到 `idle`，**没有假装还在排队** |

配套的既有验收（TASK-013/015）：入队后 Redis 有 **1 条**快照；`kill -9` Match 后重启玩家
仍为 `queued`；Match 指向不可用端口时**匹配照常成功**且日志明确记录降级。

**已知边界**
- **排队超时默认 30 秒**（`-match_timeout_seconds`）。停机超过它，排队位置就没了——
  这是**设计语义，不是缺陷**（重启期间的时间要算进去，恢复时会重算超时）。
- **快照是旁路**：Redis 不可用即降级为纯内存（§1 通道 B）；TASK-028 之后崩溃最多丢一个
  合并窗口（默认 100 ms）。
- **配对结果也持久化**（`matched` 条目）：否则"已配对但客户端还没领取"的那一局会随重启
  消失，而那一局的房间在 Room 里是**真实存在**的。
- **分配中（`allocating`）的条目在重启后退回队列**（不丢弃、也不当成已配对）：重启后无法
  知道 `CreateRoom` 是否已成功，而 Room 以 `match_id` 保证幂等，重新分配会拿回同一个
  `room_id`。代价是可能出现一个无人加入的孤儿房间，由 Room 的等待超时（30 秒）自行回收。
- **不做多实例**，因此没有"跨节点迁移"这条路径（ADR-0003）。

**相关指标与日志字段**
`rgbt_match_queue_length`、`rgbt_match_events_total{event}`、
`rgbt_queue_snapshot_total{outcome}`、`rgbt_match_room_allocate_total{outcome}`、
`event=match_enqueued`、`event=queue_snapshot_load_failed`、
`event=match_allocation_worker_started` / `_stopped`。

**出处**：`chaos/verify-process-crash.sh`（断言在 1021/1051/1056/1110/1115 行）、
`docs/devlog.md`「TASK-024 实施记录」「TASK-015 实施记录」、`docs/01-architecture.md`
第 7 节失败模型、`docs/TASKS.md` 的 TASK-024 任务单。**无 raw 归档。**

---

## 5. Gateway 崩溃与恢复

**症状（怎么发现）**
- 浏览器顶栏显示"推送已断开"；所有 SSE 连接在 **13 ~ 29 ms** 内断开
  （断言：`场景 4：Gateway 崩溃后 SSE 长连接未断开`）；
- 所有 HTTP 接口不可达；`rgbt_sse_connections` 归零；
- 客户端**必须重连**（服务端无法把订阅"还"给它）。

**影响范围**：HTTP 入口与全部推送。**会话不受影响**（在 Redis，断言"旧 token 重启后仍
应 200"）、**对局不受影响**（Room 独立推进，实测 Gateway 停机期间帧从 3 前进到 12）。

**处置**
```bash
bash chaos/verify-process-crash.sh            # 场景 4
bash scripts/verify-chaos.sh --only process-crash,reconnect
bash scripts/dev-up.sh                        # 或单独重启 Gateway
# 手工验证补发：带上最后收到的帧号重新订阅，观察 id: 行是否连续
curl -sN --max-time 60 -H "Authorization: Bearer $TOKEN" \
  -H "Last-Event-ID: $frame_checkpoint" \
  "http://127.0.0.1:8080/api/v1/stream?room_id=$ROOM_ID" | grep -o '^id: [0-9]*'
```

**恢复到什么程度**
- **恢复时间 823 ~ 837 ms**（发起重启 → 重新订阅成功并收到 `session.ready`；其中进程重启
  523 ~ 524 ms）；
- **补发**：带 `Last-Event-ID` 时窗口内缺失的帧按序补发，每个事件带 `id`。实测缺口
  **20 帧（frame 20 → 40）被完整补齐（id 21 → 40）、未发 `stream.reset`**；
- **对局不中断**：宽限期按"**最后一位**断线者 + 30 秒"计算，因此**一次 Gateway 重启不会
  立刻作废进行中的对局**（单测 `BothOfflineWaitsForTheLastOneToExpire` 锁定）。

**已知边界**
1. **全部 SSE 订阅丢失，不可恢复**（订阅表在 Gateway 进程内存里）——客户端必须重连。
2. **补发不跨进程重启**：新进程没有旧的环形缓冲，超出窗口一律 `stream.reset`。
   窗口是 Room 的快照环形缓冲 **`kMaxSnapshotHistory = 128` 帧 ≈ 12.8 秒**。
3. `Last-Event-ID` 的五种情形各有独立原因，**按原因分开计数**
   （`rgbt_push_reset_total{reason}`），处置方式不同：

   | 情况 | 行为 | 含义 |
   |---|---|---|
   | 头**缺失**（第一次订阅） | **不补发、也不发 `stream.reset`** | 正常路径，由第一次 Tick 推当前状态 |
   | 头存在但**无法解析** | `stream.reset`（`id_malformed`） | **客户端的 bug**，不能静默当成首次订阅 |
   | 早于缓冲最早一帧 | `stream.reset`（`id_out_of_window`） | 客户端落后超过 12.8 秒 |
   | 晚于房间当前帧 | `stream.reset`（`id_ahead`） | 帧号由服务端单调生成，出现即状态不一致 |
   | 等于当前帧 | `stream.reset`（`id_current`） | 给客户端一次明确确认 |
   | **落在窗口内** | 按帧号递增补发 `room.state`（带 `id`），之后进入实时推送 | 补发的事件与实时推送共用同一套事件类型 |

4. **补发只保证帧连续，不保证重放每一帧的中间状态**（同一帧可能有多次变化，
   例如对方断线）。
5. **什么时候真的补不上**（要点，不是免责声明）：客户端在断线**之前**就已落后超过
   12.8 秒；同一房间多条订阅（多标签页）互相追赶超过 12.8 秒。
   **不包含"断线期间"**：宽限期内对局暂停推进，帧号根本不前进，因此断线 30 秒后回来
   缺口是 **0 帧**——这也是 128 帧够用的原因。若哪天把"暂停推进"改成"继续推进"，
   这个窗口立刻就不够了。
6. **Gateway 的排空可能为"其实没人在听"的订阅等到上限**：服务端只有在**写**的时候才会
   发现客户端没了，最长要等一个心跳周期（15 秒）。这是 `verify-stream.sh` 里
   "Gateway 未在 10 秒内退出"那条历史现象的真实原因（该脚本已改为
   `-drain_timeout_ms 1000`）。
7. **不要按"帧号"判断是否需要推送**：判据是完整状态签名（帧号 + 血量 + online + 阶段），
   否则整个宽限期里客户端收不到任何事件，而"对方断线了"恰恰是那一刻最该知道的事。
8. **未覆盖**：`ParseLastEventId` 没有单元测试（它直接读 brpc 的 HTTP 头，
   真实链路由 `verify-reconnect.sh` 第 7 节覆盖）；前端 `useGameSession` 里"重连时带上
   `lastSequence`"没有自动化覆盖（项目无组件测试环境）。

**相关指标与日志字段**
`rgbt_sse_connections`、`rgbt_sse_subscriptions_created_total`、
`rgbt_sse_subscriptions_closed_write_failed_total`、`rgbt_sse_subscriptions_closed_other_total`、
`rgbt_sse_subscriptions_skipped_no_write_total`、`rgbt_sse_oldest_subscription_age_ms`、
`rgbt_push_backfilled_frames`、`rgbt_push_reset_total{reason}`、
`event=subscribe_ready`、`event=subscribe_rejected`、`event=backfill_done`、
`event=backfill_unavailable`、`event=stream_reset_sent`、`event=presence_report_failed`。
SSE 事件名：`session.ready` / `room.state` / `room.finished` / `stream.closed` / `stream.reset`。

**出处**：`chaos/verify-process-crash.sh`（场景 4 断言在 1178~1229 行）、
`scripts/verify-reconnect.sh`（第 7 节补发断言在 537/543 行）、
`docs/devlog.md`「TASK-024 实施记录」「推送连续性与恢复时间汇总」、
`docs/01-architecture.md`「推送连续性与补发边界」。**无 raw 归档。**

---

## 6. 客户端断线、宽限期与补发

**症状（怎么发现）**
- 对局**卡住不推进**：帧号与血量全部冻结——这是**宽限期内的预期行为**，不是卡死；
- 客户端侧 SSE 断开；`rgbt_sse_connections` 减少；Room 日志 `event=presence_reported`
  （离线一次、重连后在线一次）；
- 超期未归：断线判负，结果里能看到 `finish_reason=disconnect`。

**影响范围**：单局。宽限期内**对局暂停**（这正是"回来接着打"能精确成立的原因）；
双方都断线则整局作废。

**处置**
```bash
bash scripts/verify-reconnect.sh                     # 断线 / 重连 / 补发 / 超期判负 / 双方断线作废
bash scripts/verify-reconnect.sh --keep --no-docker
bash scripts/verify-chaos.sh --only reconnect
# 判断是谁的问题：先看 Room 的 presence 上报，再看客户端网络
grep -E "presence_reported|presence_report_failed" .run/room.log .run/gateway.log
```

**恢复到什么程度**

| 情形 | 行为 | 实测 |
|---|---|---|
| 宽限期内（30 秒）重连 | 清除断线标记，**从冻结处继续**，无需任何补偿或重放 | 重连时延由客户端退避决定：1/2/4/5/5 秒，累计 17 秒内 |
| 宽限期内对局 | **暂停推进**，因此缺口是 **0 帧** | —— |
| 宽限期到期，**只有一方**断线 | 判断线方负（`finish_reason=disconnect`），**产生胜负**并写 `match_results` | 固定 30 秒（`kReconnectGraceMs`） |
| 宽限期到期，**双方都**断线 | 本局**作废**（`ABORTED`），**不写结果、不造假胜负** | 判据是**最晚的断线时刻 + 宽限期** |
| 补发 | 窗口内按序补 `room.state`（带 `id`）；窗口外 `stream.reset` | 实测缺口 20 帧（id 21→40）完整补齐 |

**为什么"双方都断线"要等最后一位**：Gateway 重启会让所有订阅同时断开。若按第一位到期就
作废，任何一次 Gateway 重启都会立刻毁掉所有进行中的对局。

**已知边界**
1. **`presence` 与宽限计时不落库**：Room 重启后双方视为在线、计时清零 → **Room 重启期间
   到期的对局不会被判负**（见 §3 边界 2）。
2. **宽限期是固定常量**（30 秒），不按对手等待意愿或对局进度调整。
3. **SSE 断开只能靠写失败发现**（最长一个心跳周期 15 秒）：因此"玩家其实已经走了"这件事
   有最多 15 秒的感知延迟。
4. **不做跨设备接管、不做排队中的断线保位、不做 `Last-Event-ID` 之外的补帧**（任务单非范围）。

**相关指标与日志字段**
同 §5；另有 `event=presence_reported`（在线/离线两次）与 `rgbt_sse_connections` 的变化曲线。

**重现**：`demo.sh` 步骤 6。

---

## 7. 连接风暴与连接被拒

**症状（怎么发现）**
- **当前没有任何针对连接数的显式限流**（没有 429、没有连接数中间件）：**边界是进程 fd
  上限，失败点在 `accept()`**。这是任务单预告的实测事实，**不是缺陷**。
- **fd 耗尽时客户端看到的是"超时"，不是 HTTP 错误码**：登录/订阅报 `TimeoutError`、
  响应 `status=0`。服务端日志才有直接证据：
  `acceptor.cpp: Fail to accept ... Too many open files [24]`，且 fd **恰好顶在上限**。
  **否则会去客户端找原因**——这是本条最值得记住的一句。
- 断言：`配置 $tag：Gateway 在风暴中退出`、`配置 $tag：基线连接被中断`、
  `配置 $tag：风暴期间基线没有持续收到推送`、
  `配置 $tag：构造的 $soft_limit fd 上限没有触发任何连接失败，本对照不成立`。

**影响范围**：**新连接**被拒；**已有连接不受影响**（这是本场景最重要的断言）。

**处置**
```bash
bash chaos/verify-connection-storm.sh                       # 默认：基线 10 条 + 400 条风暴 / 20 秒
bash chaos/verify-connection-storm.sh --players 200 --duration 15
bash chaos/verify-connection-storm.sh --keep
bash scripts/verify-chaos.sh --only connection-storm
# 提高服务端 fd 上限（上限只对服务进程生效；客户端要单独提高，否则两类失败混在一起）
ulimit -n 8192 && bash scripts/dev-up.sh
ls /proc/$(cat .run/gateway.pid)/fd | wc -l
```
构造方式：显式构造两组**服务端** soft fd 上限——`low = 256`（定位"从哪里开始失败"）、
`high = 8192`（证明失败确实来自这个上限）。**这两个值是构造的，不是本机默认值**：
本机默认 soft 实测 **10240**（hard 1048576），已高于 8192。基线身份与风暴身份刻意错开
（`bench-00990` 起 vs `bench-00000` 起）。

**恢复到什么程度（两轮区间）**

| 观测项 | low（服务端 fd = 256） | high（服务端 fd = 8192） |
|---|---|---|
| 风暴登录成功 / 失败 | **223 / 177**（失败全为 `TimeoutError`，status=0） | **400 / 0** |
| 风暴开流成功 / 失败 | **0 / 444**（全 `stream_header:TimeoutError`） | **400 / 0** |
| 连接层失败计数 | ~800 | **0** |
| Gateway fd 峰值 | **256**（顶到上限） | **834** |
| Gateway 线程数 | 21 不变 | 21 不变 |
| Gateway RSS | 约 40 → 41 MB | 39268 → 58856 kB（+19 MB） |
| `Too many open files` | 约 **50 次**（`acceptor.cpp`） | 0 次 |
| **基线连接存活** | **10/10** | **10/10** |
| 基线窗口内事件 / 静默秒数 | 1330 次 / **0** | 656 次 / **0** |
| 进程是否存活 | 三个服务全存活 | 全存活 |

业务层噪声（**不是**连接层失败，未计入断言）：high 配置里 `matches:503` 约 49 次
（loadgen 重试后 400/400 全部配对成功）、`rooms/input:400/409` 约 97 次（对局已结束后的
输入，属业务拒绝）。

**一条口径异常（如实记录，不构成矛盾）**：low 配置里采样到 Gateway 的
`rgbt_sse_connections` 风暴后是 **57**（大于基线 10），而同一次风暴客户端侧
`stream_opened=0`。两边口径不同——服务端计的是已登记的订阅，客户端计的是拿到响应头的流。
要精确归因需要一次单独观测。

**已知边界**
- **不实现限流**（任务单非范围）。若将来要限流，应先定**连接数 SLO**，再单独立项。
  当前 `docs/04-quality-and-observability.md` 里**没有连接数上限的 SLO**。
- **"恢复时间"这个量不适用**：压力是外部施加的，服务端不需要"恢复"，压载停止即回到稳态。
  等价量是"拒绝边界 / 支持规模 / 既有连接不受影响"三个方向。
- 只测"**瞬时建立**"，**不测长时间高水位**下的稳定性（那是长稳的范围）。

**相关指标与日志字段**
`rgbt_sse_connections`、`rgbt_http_requests_total{status="503"}`、
`rgbt_rpc_calls_total{outcome="failed"}`、`/proc/<pid>/fd` 计数、日志 `Too many open files`。

**出处**：`chaos/verify-connection-storm.sh`（断言在 493/549/562/598 行）、
`chaos/storm_baseline.py`、`docs/devlog.md`「TASK-025 实施记录」与「Phase 4 退出标准对照表」、
`docs/TASKS.md` 的 TASK-025 任务单。**无 raw 归档。**

---

## 8. 优雅退出与排空（SIGTERM）

**症状（怎么发现）**
- **SIGTERM 之后进程不立刻退出——这是预期行为**（它在排空活跃对局）；
- 排空期间新请求被拒：写请求 **503 `shutting_down`**，**读请求仍可用**；
- 已建立的 SSE 会先收到一条显式 `stream.closed`（`reason=server_shutdown`）再被关闭，
  **不是被直接切断**（断言 `SSE 客户端没有收到 stream.closed（连接被直接切断了）`）；
- 到点仍未结束的：Room 日志 `event=drain_timeout_abort`；Gateway 日志
  `event=drain_timeout_close`；
- **一个必须认得出的失败信号**：若库里出现"被截断的局写进了 `match_results`"
  （断言原文："这是最严重的失败"），说明在伪造胜负。

**影响范围**：从收到信号开始拒绝新工作；已在进行的对局被等待完成。

**处置**
```bash
bash scripts/dev-down.sh                      # 正常停法（发 SIGTERM，等排空）
bash chaos/verify-drain.sh                    # 四条排空路径（单轮约 45 秒）
bash scripts/verify-chaos.sh --only drain
tail -f .run/room.log | grep -E "drain_"      # 看是排空完还是超时截断
```
脚本用的**构造值**（"验证的是语义，不是'默认 30 秒'这个数字"）：Room 排空上限
`15000 ms`、超时截断上限 `3000 ms`、Match 宽限 `2000 ms`、Gateway 上限 `3000 ms`。

三个服务各自的排空语义（**读这一行就能判断行为是否正常**）：

| 服务 | 默认上限 | 挡什么 | 不挡什么 |
|---|---|---|---|
| Room | `-drain_timeout_ms` = **30000** | `CreateRoom` 对新 `match_id` 返回 `ROOM_SHUTTING_DOWN`；**已存在的 `match_id` 仍走幂等分支返回同一个房间**（不能省：否则 Match 的一次重试会丢掉已建好的房间、玩家被退回队列） | 已建立房间的**推进与落库**；等待条件是"没有 CREATED/WAITING/PLAYING **且**没有 FINISHING" |
| Match | `-shutdown_grace_ms` = **1000** | 只挡 `Enqueue`（`MATCH_SHUTTING_DOWN`） | `GetStatus` 照常 → "已配对的结果仍可领取"这句真的成立 |
| Gateway | `-drain_timeout_ms` = **30000** | 登录/入队/取消/进房/输入/新订阅一律 503 `shutting_down` | **读请求不受影响**；已建立的 SSE 走到终点（房间打完收 `room.finished`，到点先写 `stream.closed`） |

**恢复到什么程度（两轮区间）**

| 场景 | SIGTERM → 退出 | 关键断言 |
|---|---|---|
| Room 能排空完（上限 **15 s**） | **8178 ~ 8180 ms** | 退出码 0、`drain_finished`；对局**用攻击打完（8.2 秒）并落库真实胜者**（`rooms.state=finished`）；排空期间新配对拿不到房间（客户端停在 queued，Room 记 `room_create_rejected reason=shutting_down`） |
| Room 必须超时截断（上限 **3 s**） | **3109 ~ 3114 ms** | 退出码 0、`drain_timeout_abort`；快照 `state=aborted`、`finish_reason=aborted`、`winner=NULL`；**`match_results` 里没有这一局**；库里无残留 playing/finishing |
| Match 排空（宽限 **2 s**） | **2073 ~ 2077 ms** | 退出码 0；新入队 503 `shutting_down`；**已配对的结果仍可领取**（200 + `state=matched` + `room_id`） |
| Gateway 排空（上限 **3 s**） | **3112 ~ 3115 ms** | 退出码 0；新请求 503 `shutting_down`；已建立 SSE 收到 `stream.closed` + `server_shutdown` |

**已知边界**
- **"恢复时间"不适用**（排空本身就是受控终止）。真正的量是**终止耗时**与**是否丢数据**。
- **超时截断只处理 CREATED/WAITING/PLAYING，不动 FINISHING**：`FINISHING` 的房间已经有
  真实胜负、只是在等落库，把它标成 `ABORTED` 等于丢掉一个真实结果，而它其实还有救
  （`finishing` 快照在库里，下次启动由恢复逻辑重新纳入落库重试）——**排空对它是"推迟"，
  不是"丢弃"**（单测 `AbortUnfinishedGamesLeavesFinishingRoomAlone` 钉住）。
- **等待条件包含"没有待落库的结果"**：只等"没有未结束的对局"会在对局刚结束那一刻就退出，
  而结果还停在内存里——那正是 TASK-008/014 那条已知限制的形状。
- **产品默认的 30 秒上限本身没有被单独压测过**（验收用的是构造值 15 s / 3 s）。
- **滚动升级、多副本排空、连接的优雅迁移都是非目标**（ADR-0003：本项目不做多实例）。
- **`shutting_down` 与 `*_unavailable` 必须分开**：前者重试没有意义，后者应当退避重试。
  第一版曾漏了 Gateway 的错误码映射，把 503 表现成 **500 `match_internal`**
  （实测 `HTTP 500 reason=[match_internal]`）——**看到 500 而不是 503 时先查这里**。
- **两处曾经踩过的实现坑（症状对照）**：①排空期间推进线程必须继续跑（第一版信号一到就
  退出，于是"等活跃对局结束"等的是一个**冻结**的对局：上限必然用满、对局被误标 `ABORTED`、
  `match_results` 什么都没有）；②排空判据满足后**还要再推一次 Tick**（终态快照是在下一次
  Tick 才写的，少了它进程会带着一条 `state=playing` 的旧快照退出）。
- **5 个既有验收脚本自己起服务时会传 `-drain_timeout_ms 1000`**（它们的 SIGTERM 只是收尾，
  不需要等排空；**产品默认值不变**）。**看到"未在 10 秒内退出"先确认是不是这个原因，
  别当成回归**——TASK-026 首轮就是这么误报的，而 `verify-reconnect.sh` 当时漏改、
  躺了整整一个任务周期才被补跑抓到（这正是 `verify-chaos.sh` 存在的理由）。

**相关指标与日志字段**
`rgbt_rooms`、`rgbt_room_frames_advanced_total`、`rgbt_match_events_total`、
`event=drain_started`、`event=drain_finished`、`event=drain_timeout_abort`（Room）、
`event=drain_timeout_close`（Gateway）、`event=room_create_rejected reason=shutting_down`、
`event=match_enqueue_rejected reason=shutting_down`、`event=queue_snapshot_final_flush`。

**出处**：`chaos/verify-drain.sh`（断言在 523/529/589/606/611/616/665/674/693/757/759 行）、
`docs/devlog.md`「TASK-026 实施记录」与「Phase 4 退出标准对照表」、
`docs/TASKS.md` 的 TASK-026 任务单。**无 raw 归档。**

**重现**：`demo.sh` 步骤 8。

---

## 9. 长稳资源趋势（内存 / FD / 订阅数）

**症状（怎么发现）** —— 判定与观测口径是同一份，脚本会打印 `VERDICT|ok|` / `VERDICT|fail|`：
- **订阅累积**：`SSE 连接数峰值 N 超过上界 M：疑似订阅随断连周期累积（未清理）`。
  上界在脚本里写死为 `players * (1 + churn_fraction) + 4`（50 玩家 / 20% → **64**）；
  脚本注释说明这不是拍脑袋的 SLO："只要不随周期累积，这个上界就成立"。
- **静默期不回落**：`静默期结束时 SSE 连接数仍为 N（客户端已全部离开）：订阅没有被清理`
  ——静默期**最后一条采样必须为 0**。
- **fd 泄漏**：`<服务> 的 fd 数后半段每个采样都是新高（单调增长不回落）`。
- **压载有效性（TASK-029 补的）**：`压载已退化：N/M 个采样里客户端仍持有连接但服务端
  订阅数不足其一半——订阅被关闭而客户端收不到 FIN，此时压载没有在压任何东西`。
- **RSS 只给结论、不判失败**（任务单明确"不预设阈值"）：输出"出现过回落"或
  "窗口内单调不回落……本轮**不判定为泄漏**"。
- 采样：**每 10 秒一行**记录三个服务的 fd / 线程数 / RSS 与 Gateway 的 SSE 连接数、房间数；
  压载结束后进入 **60 秒静默期**继续采样。

**影响范围**：稳态资源占用。泄漏时 Gateway 的 fd 随运行时间增长，**长跑必然撞上 fd 上限**，
因此这是必须当故障处理的。

**处置**
```bash
bash chaos/verify-soak.sh --duration 1800 --players 50            # 30 分钟标准跑法（上限 1 小时）
bash chaos/verify-soak.sh --duration 480 --players 50             # 8 分钟回归（旧泄漏从第 6 分钟就明显）
bash chaos/verify-soak.sh --duration 480 --players 50 --churn-every 30
bash scripts/verify-chaos.sh --include-soak
# 临时缓解（旧版本）：重启 Gateway —— 代价是丢掉全部 SSE 订阅（§5）
```
参数边界（实测）：默认 1800 秒、上限 **3600 秒**，**超上限报错退出而不是静默截断**；
四个越界参数（超上限 / 玩家数为奇数 / 时长为 0 / 时长非数字）都实测为**非 0 退出**。

**恢复到什么程度**

修复前（TASK-027，30 分钟 / 50 玩家 / 每 60 秒断开 20%，**脚本退出码 1**）：

| 指标 | 首 | 末 | 峰值 | 结论 |
|---|---|---|---|---|
| Gateway fd | 114 | 164 | **254** | 与订阅数同涨；静默期只回落到 164 |
| Match / Room fd | 12 | 13 | 13 | 稳定 |
| 三服务线程数 | 21 / 20 / 21 | 21 / 20 / 21 | 不变 | **无线程增长** |
| Gateway RSS | 44516 kB | 50360 kB | 50552 kB | +5844 kB（折合 **+11.7 MB/小时**），窗口内出现过回落 |
| Match RSS / Room RSS | 38400 / 40200 kB | 39160 / 43680 kB | —— | +760 / +3480 kB |
| Gateway SSE 连接数 | 50 | **150** | **160** | **单调增长，不回落** |
| 房间数 | 25 | 20 | 25 | 稳定（对局 60 秒超时结束后重新配对） |

两条失败判定原文：
`VERDICT|fail|SSE 连接数峰值 160 超过上界 64：疑似订阅随断连周期累积（未清理）`、
`VERDICT|fail|静默期结束时 SSE 连接数仍为 150（客户端已全部离开）：订阅没有被清理`。
压载有效性（证明不是空转）：**29 个断连周期、累计主动断开 290 条 SSE、重连 290 次**、
`streams_opened=340`、结束时仍连着 50 条、窗口内推送事件 **183742** 条。
内存侧结论：**没有证据表明内存泄漏**（三个服务 RSS 在窗口内都出现过回落、线程数恒定、
进程未被 OOM）；**不稳定的是订阅与 fd**。

修复后（TASK-029，同一命令族，**0 项失败**）：

| 指标 | 修复前 | 修复后（且探针/判定补齐后） |
|---|---|---|
| SSE 连接数 | 峰值 73、静默期结束 **53** | **全程 50**、静默期结束 **0** |
| `skipped_no_write`（累计） | **146,374** | **0** |
| 最老订阅年龄 | 315,000 ms 且持续增长 | 峰值 84 s（对局换房所致），静默期归 0 |
| Gateway fd | 末值 67 | 末值 **14**（后半段新高 0/93） |
| 换局 / 重连 | 只断不换局（`requeued=0`） | 断开并重连 150 次、**换局 308 次** |
| 判定 | 失败 2 项 | **0 项失败** |

30 分钟最终验收读数：SSE 峰值 **50**、静默期结束 **0**、`skipped` 累计 **0**、最老订阅年龄
峰值 **82001 ms**、断连/重连 **29** 个周期、换局 **1450** 次、判定 **0 项失败**。
原始数据：`docs/benchmarks/raw/soak-20261004-task029-final/`（含 `soak-30min-analysis.txt`、
`soak-30min.json`、`soak-30min-samples.tsv`）；修复前那轮在
`docs/benchmarks/raw/soak-20261004-1810/`。

**已知边界**
- **泄漏的上界未知**：修复后只验到 **30 分钟**（脚本上限 **1 小时未用满**），
  **作为已知限制带入 Phase 5**（项目所有者 2026-10-04 裁决）。"通过"不等于"永不泄漏"。
- **不做 24 小时以上长稳**；**不预设阈值**（阈值与 SLO 的调整另立任务）。
- **"恢复时间"不适用**：稳态运行没有"恢复"动作，判据是"是否回到基线"（静默期连接数归零、
  fd 是否回落）。**"检测时间"部分适用**：若把"泄漏"当故障，它的检测量就是**最老订阅年龄**
  开始单调增长的时刻（TASK-029 已把它做成 `rgbt_sse_oldest_subscription_age_ms`）。
- **负载画像不是严格恒定**：对局会在 600 帧（60 秒）后按超时结束、服务端随即关闭该流，
  玩家重新配对进房再开流需要时间。因此正确的结论是"**在持续连接/断连/重连下：内存与线程
  稳定，但订阅与 fd 不稳定**"，而不是"50 条连接恒定不变地压了 30 分钟"。
- **一条未被脚本判定、也不得自行解释的读数**：30 分钟那轮 `Gateway 房间数` 首 25 / 末 125 /
  最大 151、后半段新高 2/93。脚本未对它下结论，引用时不要替它解释。
- **判定的质量约定（TASK-029 的教训）**：长稳 / 压测类的判定**必须包含"负载真的在压"这一条**，
  否则它会安静地退化成一台只打印"通过"的机器。
- `verify-all.sh` **未接入本脚本**（占标准端口、默认 30 分钟）；要跑用
  `scripts/verify-chaos.sh --include-soak`。

**相关指标与日志字段**
`rgbt_sse_connections`、`rgbt_sse_oldest_subscription_age_ms`、
`rgbt_sse_subscriptions_{created,closed_write_failed,closed_other,skipped_no_write}_total`、
`/proc/<pid>/fd` 计数、`/proc/<pid>/status` 的 `Threads:` 与 RSS。

**出处**：`chaos/verify-soak.sh`（判定在 529~638 行）、`chaos/soak_churn.py`、
`docs/devlog.md`「TASK-027 实施记录」「TASK-029 实施记录」「Phase 4 退出标准对照表」、
`docs/TASKS.md` 的 TASK-027/TASK-029 任务单、`docs/benchmarks/README.md` 的「长稳补充」一节。

---

## 10. SSE 订阅生命周期异常（TASK-027 发现 → TASK-029 修复）

**症状（怎么发现）** —— 三个指纹，看到任意一个就该怀疑这条：
1. `rgbt_sse_connections` 在长稳中**单调增长且不回落**（客户端已全部离开仍 > 0）；
2. **`rgbt_sse_subscriptions_skipped_no_write_total` 累计不为 0**（这是根因的指纹，
   TASK-029 专门为此新增）；
3. `rgbt_sse_oldest_subscription_age_ms` 持续增长（健康时应接近心跳间隔）。

**影响范围**：Gateway 的 fd 与内存随运行时间增长；**表现为真实缺陷而不是缓慢劣化**
（长跑必然撞上 fd 上限）。

**处置**
```bash
git log --oneline -1        # 确认版本包含 TASK-029 的修复（在 8261b06 之后）
curl -s http://127.0.0.1:8080/metrics | grep -E "skipped_no_write|oldest_subscription"
bash chaos/verify-soak.sh --duration 480 --players 50     # 8 分钟回归，必须退出码 0
bash chaos/verify-soak.sh --duration 1800 --players 50    # 30 分钟最终验收
bash scripts/verify-stream.sh                             # SSE 既有语义回归
```

**根因（已定位、已修）**
1. `ParseLastEventId`（`src/gateway/gateway_service.cpp`）在**没有** `Last-Event-ID` 头时返回
   `value = 0`；它自己的注释写的语义是"头不存在 → 这是**第一次订阅**，不是错误"。
2. 服务层把这个 `std::int64_t` 直接赋给 `std::optional<std::int64_t>
   subscribe_options.last_event_id`——**`optional` 被 engage 了 0**。于是"没有头"与
   "头就是 0"在 `StreamHub` 里再也分不出来，**每一个首次订阅都走了补发路径**。
3. 在**取不到房间状态**的房间上（对局 60 秒结束、房间被回收），`BackfillSubscription`
   返回 pending → 下一轮重试 → **永远 pending**；而原实现在这个分支里是**无条件
   `continue`**：这一轮**既不推状态、也不发心跳**。
4. SSE 下服务端收不到显式断开通知，**只能靠写失败发现对端走了**。一条永远不写的订阅等于
   对断连免疫：**订阅和它的 socket 一起留在表里，直到进程退出**。

修复（三处产品改动）：① `LastEventIdHeader` 增加 **`present`** 字段，**只有头真的出现过才
engage `optional`**（根因修复）；② `StreamHub::Tick` 的补发 pending 分支**不再完全跳过
写入**，到达心跳间隔时**照发心跳**（**不发 `session.ready`**——那会打乱"补发 → 实时"的边界）；
③ 新订阅创建时把心跳基准设为"现在"（初值 0 表示"从未"，直接比较会让新订阅在第一轮就被判成
心跳到期）。

**恢复到什么程度**：修复后 8 分钟与 30 分钟长稳均 **0 项失败**，
`skipped_no_write` 从 **146,374 → 0**，静默期订阅数回到 **0**，Gateway fd 末值 67 → **14**
（完整对比见 §9）。修复前的版本**只能靠重启 Gateway 缓解**（代价是丢全部订阅）。

**回归用例（并已证明它在修复前失败）**：
`StreamHubTest.PendingBackfillStillGetsHeartbeatSoDeadPeersAreDetected`——构造一个"房间状态
永远取不到"的订阅，断言①第一轮确实零写入且被计数；②到达心跳间隔必须发生一次写；
③此后写失败必须回收订阅。**把补发分支的心跳判断临时改成 `if (false)`（模拟修复前）后重跑，
该用例 3 处断言失败**（零写入、未回收、未关闭），恢复后通过。

**已知边界**
- **根因所在的"服务层拼装"没有单元测试**：`ParseLastEventId` 在匿名命名空间里，而单测驱动
  的是 `StreamHub`。目前由长稳回归锁定（`skipped` 必须为 0）；要单元级覆盖需要把它提成
  可测单元（后续重构）。
- **首次订阅（无 `Last-Event-ID`）不补发、也不发 `stream.reset`** 是**有意**的裁决结果：
  任务单里"范围第 4 条"与"失败场景"两处曾相互矛盾，项目所有者裁决按后者实现——头缺失
  （第一次订阅）不补发也不 reset，由第一次 Tick 正常推当前状态；头**存在但非法**仍发
  `stream.reset`（`id_malformed`），因为那是客户端的 bug，静默当成首次订阅会让它以为自己
  拿到了连续的事件。
- **评审这条缺陷时最容易搞错的一点**：单条订阅、以及"同一房间反复断连 + 立即重订阅"
  **都复现不出来**（负例一：1 房间 1 订阅，5 秒内从 1 回落到 0；负例二：15 轮压完仍停在 1，
  之后 15 秒内回落到 0）。触发条件必须包含**多房间 + 对局结束 + 玩家换房 + 断连宽限期**。
- **两个一起堵上的"验收侧的洞"（识别假通过的对照）**：①探针的洞——
  `chaos/soak_churn.py` 的 `on_event` 把事件名丢掉了，`room.finished` 到了也不结束读取，
  于是对局 60 秒一结束后客户端抱着一条再无事件的连接，**50 条"自以为连着"、服务端订阅数
  为 0、整段压载空转**（修好后 `requeued` 从 0 变成 308）；②判定的洞——旧判定只有"没有
  累积"和"静默期回落到 0"两条，**压载空转时两条都会通过**，会给出一个假的"长稳验收通过"
  （新判定在空转那轮确实报失败：6/18 个采样）。这个洞**不是修复引入的**：修复前订阅会泄漏、
  服务端订阅数一直不减，两个错误刚好互相掩盖。
- **泄漏的上界未知**（只验到 30 分钟，见 §9）。

**相关指标与日志字段**
四个订阅生命周期计数 + `rgbt_sse_oldest_subscription_age_ms`（完整名见 §0.5）、
`event=backfill_unavailable`、`event=backfill_done`、`event=subscribe_ready`。

**出处**：`src/gateway/gateway_service.cpp`、`src/gateway/stream_hub.*`、
`chaos/verify-soak.sh`、`docs/devlog.md`「TASK-029 实施记录」、`docs/TASKS.md` 的 TASK-027/
TASK-029 任务单、`docs/benchmarks/raw/soak-20261004-1810/` 与 `…-task029-final/`。

---

## 11. 入队延迟：TASK-035 的七次否证

**症状（怎么发现）**
- `POST /api/v1/matches` 在 **500 档位**下 p95 ≈ **230 ms**，而 SLO 是 **p95 < 100 ms**；
- 反差：**Match 的处理函数只要 6 ~ 11 ms**（`rgbt_match_stage_max_ms{op="enqueue",stage="total"}`）
  ——时间**不在 handler 里**；
- 旁证是"瓶颈搬家"而不是回归：同期 `rooms/join` p95 23 → 163 ms、`/stream` 46 → 95 ms，
  而帧数 615 → 741/s、攻击 4753 → 4822——同样时间里系统做了更多事。

**处置（按这个顺序，别重复已否证的路）**
1. 先看**分段指标**，它们就是为这条路径长期保留的：
   `rgbt_match_stage_max_ms{op,stage}`、`rgbt_gateway_match_rpc_max_ms{op}`、
   `rgbt_match_room_allocate_{ms,max_ms,total}`、
   `rgbt_match_queue_snapshot_{pending,lag_ms,write_ms,merged_total,ticks_total}`。
2. 判断"是不是宿主机在忙"：压载期间 `vmstat 1` 看 `r`（运行队列）与 `id`。
3. 判断"是不是 Redis"：压载中抓 `INFO commandstats` / `INFO latencystats` /
   `redis-cli --latency`。
4. **不要**再单独去试下面这七个方向——都已用同一台机器、同一条命令测过并否证：

| # | 假设 | 实验 | 实测结果 | 处理 |
|---:|---|---|---|---|
| 1 | Match 处理路径慢（配对同步等 Room） | 方案 A：房间分配移出请求线程 | handler `total` 最大 **80 → 6 ms**、配对段 81 → 3 ms，**端到端 p95 不动**（229.86 → 230.52 ms） | **保留**（无回归） |
| 2 | 宿主机 CPU 竞争 | 压载期间 `vmstat 1`（60 s / 76 样本） | **平均 r=1.3、us=3.2%、sy=6.7%、id=79.6%**（最大运行队列采样 7/6/5/5/4） | 只观测 |
| 3 | 监控栈抢占 | 停掉 Prometheus + Grafana 后复测 | p95 **232.01 ms（不变）**；p50/p90/p99 = 28.00/214.03/246.40 | 只观测 |
| 4 | brpc 分发器/线程数不足 | 两个服务都加 `-event_dispatcher_num=4 -bthread_concurrency=16`（默认 1 / 9） | p95 **244.62 ms（更差）**、p99 **439.02 ms** | **已回退** |
| 5 | Redis 会话查询慢 | `INFO commandstats` / `latencystats` / `--latency` | `hmget` **p99.9 = 44 µs**（`usec_per_call=4.40`）；`exec` p99.9 = 1146.879 µs；`--latency` 平均 0.15 / 最大 1 ms | 只观测 |
| 6 | 结构化日志写 I/O | 三个服务输出改 `/dev/null` 后复测 | p95 **232.23 ms（不变）**；p99 246.45 | 夹具改动已恢复 |
| 7 | Gateway→Match 单连接 | 两个客户端加 `channel_options.connection_type="pooled"` | p95 **234.82 ms（噪声内）**；p99 246.96 | **已回退** |

七次对照中端到端入队 p95 **稳定在 229.86 ~ 234.82 ms**（该区间**不含**上面第 4 步被回退的
244.62，也不含下面第 3 条那个未合并回归方案的 393.75）；唯一稳定的大项是
**Gateway→Match RPC 段**，而对端处理只要 6 ~ 11 ms——这段耗时在 **brpc 的接入/传输层**。

**恢复到什么程度**
- TASK-028 已把 **840.68 → 230.08 ms（−73%）**，入队 503 从 **179 → 0**，
  快照写入从"每次变化一次"变成"整轮 2 次、单次 ~1 ms、待写滞后 0"；
- TASK-035 的七次对照说明：这 **230 ms 在当前的测量条件下是一条可复现的基线**，
  **不是任何一处可改的业务逻辑或配置**；
- 因此 TASK-035 的目标已由项目所有者**重新界定**为"在明确写出的测量条件下，给出可复现的
  入队延迟基线与瓶颈排序，并说明哪些方向已被实测排除"（2026-10-05 裁决）。
  **原目标 p95 < 100 ms 仍然未达成，作为已知欠账记录，不放宽、不隐藏。**

**已知边界**
1. **成因在 brpc 接入/传输层**：Gateway 自己的事件分发器延迟
   `event_dispatcher_read_latency` **p80 就有 275 ms**（p99 1.16 s），而 CPU **80% 空闲**。
   handler 内部计时不含这一段，所以"处理函数 6 ms、端到端 230 ms"并不矛盾——
   **请求与响应都在事件循环里排队**。
2. **下一步必须改变测量方法或部署形态，而不是继续改业务代码**：压载端与服务分离部署、
   给服务独占核心、或改用长连接压测模型，并把"测量方法的影响"本身作为结论的一部分。
3. **一个已回退的旁支要记住**：方案 1 第一版（"取人后再回退"）造成重试放大，
   实测把 p95 推到 **393.75 ms**、p99 **897.76 ms**、入队 503 **38 次**、
   完成对局从 500 掉到 182、`room_allocate_max_ms` **11902 ms**、`/matches/current` 503
   达 **14757 次**。代码**从未合并**，但"看起来像优化"的改动必须用同一命令对照后才算数
   （原始数据 `docs/benchmarks/raw/20261005-165720/`）。
4. **一个被这次改动带出来的真实回归**：方案 A 让 `Enqueue` 返回与"配对结果可见"之间出现了
   一个短暂的 `queued` 窗口，`scripts/verify-match.sh` **连续两轮失败**
   （`bob 入队异常：HTTP 200 state=queued`、`room_id 不一致`）。原因是 TASK-035 合并时只跑了
   `ctest` 与压载，**没跑 `verify-all.sh` / `verify-chaos.sh`**。已通过给分配 worker 加
   **空闲快路径**修好（worker 队列为空时就地内联分配，约 3 ms），可见行为恢复到与改动前
   一致，同时保留高负载下"分配不阻塞请求线程"的收益。
5. **语义变更（跨任务，必须记住）**：队列快照改为合并 + 异步刷写后，
   **崩溃最多丢一个合并窗口（默认 100 ms）内的队列变化**；正常关机由 `FlushSnapshotNow`
   保证"排空返回前最后一次变化已落盘"。
6. **数字要带版本**：上述数字分别测于 TASK-028 之前/之后与 TASK-035 期间的不同提交；
   引用时**必须写明是哪一版**。特别是 `feat/phase-5` 上"分配 worker 空闲快路径"那次修订
   之后**没有再跑过 500 档位容量**——最新的容量数字属于修订前的代码版本。
7. **两处引用时要知道的偏差**：文字口径里的"RPC 段 142 ~ 240 ms / 对端 6 ~ 11 ms"是
   **prose 合成区间**，逐轮原始值里 `enqueue` 段最小是 **141**、对端最大是 **81**；
   第 2 步的 80% 空闲/运行队列 1.3 **只有 devlog 文字、无 raw 归档**（`.run/` 不入库）。

**相关指标与日志字段**
`rgbt_match_stage_max_ms{op,stage}`、`rgbt_gateway_match_rpc_max_ms{op}`、
`rgbt_match_room_allocate_{ms,max_ms,total}`、`rgbt_match_queue_snapshot_*`、
`rgbt_http_request_seconds{path="/api/v1/matches"}`、
`rgbt_http_requests_total{path="/api/v1/matches",status="503"}`。

**重现**
```bash
bash scripts/bench.sh --level 500 --duration 60     # 官方压制口径
bash scripts/bench.sh --level 1000 --duration 60    # 不退化（记录前后）
# 原始数据落 docs/benchmarks/raw/<时间戳>/；各轮目录与出处见 docs/benchmarks/README.md
```
`demo.sh` 的步骤 2 只是功能验收，**不产生容量数字**。

**出处**：`docs/devlog.md`「TASK-035 第一步」~「TASK-035 第八步」「TASK-035 方案 1 的第一版
实现：回归，已回退」「TASK-035 重新界定并收口」「TASK-028 开工第一步」「TASK-028 实施记录」、
`docs/TASKS.md` 的 TASK-028/TASK-035 任务单、`docs/benchmarks/README.md` 的 TASK-028 一节，
原始数据 `docs/benchmarks/raw/20261005-145750/`、`…-152502/`、`…-164445/`、`…-165720/`、
`…-180911/`、`…-202107/`、`…-202632/`、`…-204622/`、`…-205958/`、`…-210341/`、`…-212008/`。

---

## 12. 已知不恢复的场景（汇总，别在这里期待"一定能恢复"）

| # | 场景 | 会发生什么 | 依据 |
|---:|---|---|---|
| 1 | **Match 停机超过排队超时**（默认 30 秒） | 该排队条目**按设计不再恢复**，玩家回到 `idle`；日志"已超时未恢复 N 人" | §4、TASK-024 场景 3b |
| 2 | **Room 重启时 MySQL 不可用** | **本次启动一个房间都不恢复**（`restore_load_failed`），查询返回 503 `result_store_unavailable` 而非 404；**数据没丢**，依赖恢复后再重启即可恢复（实测 `frame=600` 落库、无重复行） | §2 B、§3 场景 2b |
| 3 | **Gateway 崩溃** | **全部 SSE 订阅丢失**，客户端必须重连；会话与对局不受影响 | §5、TASK-024 场景 4 |
| 4 | **补发窗口之外**（早于 128 帧 ≈ 12.8 秒，或跨进程重启） | 发 `stream.reset` + 当前完整状态，**中间缺失的帧不可恢复**（诚实告知，不假装补上） | §5、TASK-017 |
| 5 | **presence 与宽限计时不落库** | Room 重启后双方视为在线、计时清零 → **Room 重启期间到期的对局不会被判负** | §3 边界 2、§6 边界 1 |
| 6 | **停机期间本应推进的帧** | 被丢弃，不在启动瞬间一次性补上（有意选择）；上界 1 秒 = 10 帧 | §3 边界 4 |
| 7 | **MySQL 长期不可用** | `FINISHING` 房间**无限重试**、无放弃上限，持续累积；对局推进被拖慢到分钟级（755 s vs 基线 4.1~16.7 s）**但正确性不受影响** | §2 B |
| 8 | **连接风暴** | **没有显式限流**，拒绝边界就是进程 fd 上限；客户端看到的是**超时**而不是错误码 | §7 |
| 9 | **长稳泄漏上界** | 只验到 **30 分钟**（脚本上限 1 小时未用满）——"通过"不等于"永不泄漏" | §9 |
| 10 | **结果落库重试的可观测性** | `rgbt_result_persist_total` **未登记**，没有指标出口；只能用 `rgbt_rooms{phase="finishing"}` 与日志 `result_persist_failed` | §0.5 |
| 11 | **负载下的配对计数** | `rgbt_match_events_total{event="paired"}` 读数为 0（TASK-035 之后）；不要用它做判据，用 `rgbt_match_room_allocate_total` | §0.5 |
| 12 | **跨节点故障迁移 / 多副本排空 / 事件持久化重放** | **非目标**（ADR-0003），不是"暂未实现" | [ADR-0003](adr/0003-scope-reduction.md) |
| 13 | **并发缺陷门禁** | TSan 实测 **21 条报告全部落在 brpc 内部**（vcpkg 的 brpc 未用 TSan 插桩），对本项目代码**不具指向性**；并发缺陷目前只能靠 A/B 复现、ASan 与直接现象 | `docs/TASKS.md` Backlog |
| 14 | **共享依赖的释放顺序** | 三个服务共用同一个 Redis 与同一个 MySQL：**不要用 `docker stop` 来验证单条依赖通道**（会先打掉鉴权，测错对象），必须走 `chaos/relay.py` 的网络层隔离 | §1、§2；TASK-015/TASK-023 各踩一次 |

---

## 13. 出处索引与可追溯性的真实缺口

| 内容 | 出处 |
|---|---|
| 依赖不可用四通道的检测/恢复时间与断言 | `docs/devlog.md`「TASK-023 实施记录」、`docs/TASKS.md` 的 TASK-023 任务单、`chaos/verify-dependency-down.sh` |
| 进程崩溃的恢复时间与丢失边界（四个场景 + 2b） | `docs/devlog.md`「TASK-024 实施记录」、`docs/TASKS.md` 的 TASK-024 任务单、`chaos/verify-process-crash.sh` |
| 连接风暴的拒绝边界与基线连接存活 | `docs/devlog.md`「TASK-025 实施记录」、`docs/TASKS.md` 的 TASK-025 任务单 |
| 排空四条路径的终止耗时与数据边界 | `docs/devlog.md`「TASK-026 实施记录」、`docs/TASKS.md` 的 TASK-026 任务单 |
| 断线重连、宽限期与补发 | `docs/devlog.md`「推送连续性与恢复时间汇总」、`docs/01-architecture.md` 第 5 节、`scripts/verify-reconnect.sh` |
| 长稳首轮泄漏与修复后读数 | `chaos/verify-soak.sh`、`docs/benchmarks/raw/soak-20261004-1810/`、`…-task029-final/`、`docs/benchmarks/README.md` 的「长稳补充」 |
| SSE 泄漏根因、修复与回归用例 | `docs/devlog.md`「TASK-029 实施记录」、`docs/TASKS.md` 的 TASK-029 任务单 |
| 四个故障场景的"三元组"总表与三个"该量不适用" | `docs/devlog.md`「Phase 4 退出标准对照表」 |
| 容量基线六档（1 ~ 1000 连接） | `docs/benchmarks/README.md`、`docs/benchmarks/raw/20261004-140614/` |
| TASK-028 前后对比（840.68 → 230.08 ms） | `docs/devlog.md`「TASK-028 开工第一步：复跑基线」「TASK-028 实施记录」、`raw/20261005-145750/`、`raw/20261005-152502/` |
| TASK-035 七次否证的逐条原始数据 | `docs/devlog.md`「TASK-035 第一步」~「第八步」、`docs/benchmarks/raw/20261005-164445/`、`…-165720/`、`…-180911/`、`…-202107/`、`…-202632/`、`…-204622/`、`…-205958/`、`…-210341/`、`…-212008/` |
| 演示九个步骤与计时 | `scripts/demo.sh`、`docs/devlog.md`「TASK-031 第一步」「快路径修复的重跑结论」 |
| 起停与配置（不属于本文） | [环境与运维](06-operations.md) |
| 接口、错误码与幂等键 | [接口、数据与协议](05-api-and-data.md) |
| 恢复边界、失败模型、断线宽限期、补发边界 | [架构设计](01-architecture.md) 第 5 ~ 7 节 |
| 不做哪些事（Kafka/etcd/多实例等） | [ADR-0003 范围裁剪](adr/0003-scope-reduction.md) |

**可追溯性的真实缺口（如实记录，不要用虚构路径糊过去）**

1. **四类故障注入没有原始数据归档**：依赖不可用（TASK-023）、进程崩溃（TASK-024）、
   连接风暴（TASK-025）、排空（TASK-026）的数字**只存在于**任务单的「实施结果」与
   `docs/devlog.md` 的实施记录里。引用时出处只能写到"哪个任务单 / devlog 的哪一节"，
   **不能写 `docs/benchmarks/raw/<name>/`**。有归档的只有容量（TASK-022 与 TASK-028 前后）
   与长稳两轮。
2. **几处只有文字、没有 raw 的数字**：TASK-035 第 2 步的"80% 空闲 / 运行队列 1.3"
   （`vmstat` 输出在 `.run/`，不入库）；TASK-031 的演示计时与快路径修复后的重跑
   （输出在 `.run/demo-*.log`、`.run/va-after-fp.log`、`.run/vc-after-fp.log`）。
3. **几处口径差异，引用时必须注明轮次**：任务单里的"检测/恢复时间"是**首轮单轮值**，
   devlog 里的是**多轮区间**（例如通道 2 的恢复时间：单轮 118 ms vs 区间 100 ~ 113 ms）；
   TASK-029 的最老订阅年龄在两处分别是 315,000 ms（修复前）与 84 s / 82001 ms（修复后，
   不同轮次）；`verify-all.sh` 的耗时在 230 ~ 256 s 之间浮动。
4. **`scripts/bench.sh` 的墙上时长与真实秒不同源**：本机 coreutils 9.x 的 `date +%s%3N`
   **不**把纳秒截断成 3 位，而是输出"epoch 秒 + 字面量 `3` + 纳秒"。脚本首尾同源相减因此
   **内部一致**（既有容量数字不受影响）；但**新增**基于墙钟的速率/超时/SLO 断言一律要用
   bash 自带的 `EPOCHREALTIME`（`chaos/lib.sh` 的 `now_us` / `now_ms` 可直接抄）。