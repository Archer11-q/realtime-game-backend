# 演示前端（TASK-010）

Vue 3 + TypeScript + Vite + Canvas。只负责**演示和操作**，不承载任何业务真相
（见 `docs/01-architecture.md` 第 1 节）。所有状态都来自 Gateway。

## 运行

```bash
# 1. 起依赖与三个后端服务（Room / Match / Gateway）
bash scripts/verify-stream.sh --keep      # 会启动服务并保留进程

# 2. 起前端
cd web
RGBT_GATEWAY_PORT=8080 npm run dev
```

浏览器打开 `http://127.0.0.1:5173`，在**两个窗口**里分别登录 `alice` 与 `bob`，
两边都点「开始匹配」，配对后会自动进入对战页。

## 命令

| 命令 | 作用 |
|---|---|
| `npm run dev` | 开发服务器（含 `/api` 代理） |
| `npm run test` | SSE 解析的单元测试（vitest） |
| `npm run type-check` | `vue-tsc --noEmit` |
| `npm run build` | 类型检查 + 生产构建到 `dist/` |
| `npm run preview` | 预览构建产物 |

一键验收：`bash scripts/verify-web.sh`（仓库根目录执行）。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `RGBT_GATEWAY_PORT` | `8080` | 代理目标端口。Gateway 换了端口时必须跟着改 |
| `RGBT_WEB_PORT` | `5173` | Vite 监听端口 |

## 两个刻意的取舍

**不用 `EventSource`，改用 `fetch` + `ReadableStream` 手动解析 SSE。**
`EventSource` 无法设置请求头，token 只能放进查询字符串，于是会出现在访问日志、
浏览器历史与 `Referer` 里。手动解析多约 60 行代码，换来 token 始终走
`Authorization` 头。解析逻辑有单元测试（`src/api/stream.test.ts`）。

**不引 `vue-router` / Pinia / UI 组件库 / axios。**
四个视图是**同一个会话的四个阶段**，不是可独立寻址的页面，用一个 `stage`
状态切换就够；跨组件共享的状态只有会话那几项，模块作用域的 `ref` 足够。
这符合 `CLAUDE.md` 第 6 条：不以技术先进为理由增加组件。

## 目录

```
src/
├── api/
│   ├── types.ts        # 与 api/proto/gateway.proto 一一对应（保持 snake_case）
│   ├── client.ts       # HTTP 封装；按 error.reason 分支，不按状态码
│   ├── stream.ts       # SSE 解析与订阅
│   └── stream.test.ts  # SSE 解析的边界测试
├── composables/
│   └── useGameSession.ts   # 会话状态机：login -> lobby -> battle -> result
├── views/                  # 四个视图
├── components/
│   └── BattleCanvas.vue    # Canvas 对战渲染（画面 = 服务端帧快照的重放）
├── App.vue
├── main.ts
└── styles.css
```

## 已知限制

- **不做断线重连**（Phase 2）。流断开时界面显示「推送已断开」，需要用户重新操作。
- **匹配状态用轮询而不是推送**。理由见 `docs/05-api-and-data.md` 第 2 节：
  为每个在线连接轮询 Match 的代价不值那份实时性。
- **不做本地乐观更新**。点攻击后血量不会立即变化，要等服务端下一帧的推送。
  这是刻意的：本地先减血会制造第二种真相，与服务端判定冲突时无从分辨。
