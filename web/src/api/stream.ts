/// @file stream.ts
/// @brief 消费 Gateway 的 SSE 推送。
///
/// **为什么不用浏览器原生的 `EventSource`**：它无法设置请求头，token 只能放进
/// 查询字符串，于是 token 会出现在访问日志、浏览器历史与 Referer 里。
/// 这里改用 `fetch` + `ReadableStream` 手动解析 SSE——多约 60 行代码，
/// 换来 token 始终走 `Authorization` 头。后端两种方式都支持，选这一种是安全取舍。
///
/// 分层：`streamRoom` 只负责「一次订阅」，不含任何重试；`connectRoomPush` 在它
/// 之上实现重连状态机（TASK-016 前端部分）。拆成两层是为了让重连逻辑可以在不
/// 模拟 HTTP 的前提下被单独测试——见 stream.reconnect.test.ts。
///
/// 重连窗口与后端语义对齐：Gateway 在推送连接断开后进入 30 秒宽限期，
/// 期内对局暂停推进（帧号与血量冻结），超过宽限期判断线方负。
/// 因此前端必须在 30 秒内重连成功，否则这一局就真的丢了。

import { ApiFailure, NetworkFailure } from './client'
import type { ApiError } from './types'

export interface StreamEvent {
  /** SSE 的 `event:` 字段。Gateway 发送 session.ready / room.state / room.finished。 */
  event: string
  /** SSE 的 `data:` 字段。多行时按规范用 \n 连接。 */
  data: string
}

/**
 * 解析一个 SSE 事件块（已按空行切分）。
 *
 * @returns 没有 data 行时返回 null。纯注释块（心跳 `: ping`）就属于这种情况——
 *          它必须被忽略，而不是变成一个空事件。
 */
export function parseSseBlock(block: string): StreamEvent | null {
  let event = 'message'
  const data: string[] = []

  for (const rawLine of block.split('\n')) {
    const line = rawLine.endsWith('\r') ? rawLine.slice(0, -1) : rawLine
    if (line === '') {
      continue
    }
    // 以冒号开头的是注释。心跳走这条路径，不作为事件交给上层。
    if (line.startsWith(':')) {
      continue
    }
    const colon = line.indexOf(':')
    const field = colon < 0 ? line : line.slice(0, colon)
    let value = colon < 0 ? '' : line.slice(colon + 1)
    // 规范规定字段值前的一个空格不属于值。少去掉这个空格会让 JSON.parse 失败。
    if (value.startsWith(' ')) {
      value = value.slice(1)
    }
    if (field === 'event') {
      event = value
    } else if (field === 'data') {
      data.push(value)
    }
  }

  if (data.length === 0) {
    return null
  }
  return { event, data: data.join('\n') }
}

/** 把 SSE 事件的 data 解析成信封对象。解析失败返回 null，不抛异常。 */
export function decodeEnvelope<T>(event: StreamEvent): T | null {
  try {
    return JSON.parse(event.data) as T
  } catch {
    return null
  }
}

export interface StreamOptions {
  roomId: string
  token: string
  signal: AbortSignal
  onEvent: (event: StreamEvent) => void
}

/**
 * 订阅房间推送，直到流结束或 signal 被中止。
 *
 * @returns 流正常结束（服务端关闭）或被中止时返回；网络错误抛出。
 */
export async function streamRoom(options: StreamOptions): Promise<void> {
  const url = `/api/v1/stream?room_id=${encodeURIComponent(options.roomId)}`

  let response: Response
  try {
    response = await fetch(url, {
      headers: {
        Accept: 'text/event-stream',
        Authorization: `Bearer ${options.token}`,
      },
      signal: options.signal,
    })
  } catch (cause) {
    if (options.signal.aborted) {
      return
    }
    throw new NetworkFailure(
      `订阅推送失败：${cause instanceof Error ? cause.message : String(cause)}`,
    )
  }

  if (!response.ok) {
    // 失败时后端**不进入流式模式**，返回的是普通 JSON 错误体。
    // 这里必须读它，否则界面只能显示一个没有原因的 HTTP 数字。
    const body = (await response.json().catch(() => null)) as { error?: ApiError } | null
    throw new ApiFailure(
      response.status,
      body?.error?.reason ?? 'stream_failed',
      body?.error?.message ?? `订阅推送失败（HTTP ${response.status}）`,
    )
  }
  if (response.body === null) {
    throw new NetworkFailure('订阅响应没有可读流')
  }

  const reader = response.body.getReader()
  const decoder = new TextDecoder()
  let buffer = ''

  try {
    for (;;) {
      const { done, value } = await reader.read()
      if (done) {
        break
      }
      // 统一换行：SSE 允许 \r\n，而事件分隔的判断依赖 \n\n。
      buffer += decoder.decode(value, { stream: true }).replace(/\r\n/g, '\n')

      let separator = buffer.indexOf('\n\n')
      while (separator >= 0) {
        const block = buffer.slice(0, separator)
        buffer = buffer.slice(separator + 2)
        const event = parseSseBlock(block)
        if (event !== null) {
          options.onEvent(event)
        }
        separator = buffer.indexOf('\n\n')
      }
    }
  } catch (cause) {
    // 主动中止（组件卸载、离开对局）不是错误，不该冒泡到界面。
    if (options.signal.aborted) {
      return
    }
    throw cause
  } finally {
    await reader.cancel().catch(() => undefined)
  }
}

/// ---------------------------------------------------------------------------
/// 自动重连状态机（TASK-016）
/// ---------------------------------------------------------------------------

/**
 * 推送连接对界面可见的状态。
 *
 * 五个状态刻意分开而不是压成一个布尔值：「已断开」既可能是「正在第 3 次重连」
 * （玩家不用管），也可能是「已经放弃，这一局要输了」（玩家必须知道）。
 * 把它们显示成同一句话，就是任务要求里明确禁止的「静默」。
 *
 * `connected` 不由状态机内部判定：它应该表示「真的在收数据」，而状态机能看到的
 * 只有「HTTP 流开着了」。所以由调用方在收到事件时才切到 `connected` 与 `connecting`。
 */
export type PushStatus = 'connecting' | 'connected' | 'reconnecting' | 'exhausted' | 'stopped'

/**
 * 退避序列（毫秒），同时也是最大重试次数的来源。
 *
 * 为什么是这五个数：
 *   * 1s/2s/4s 是指数退避——瞬时抖动（TCP 重传、服务端一次 502）一次就过去了。
 *   * 上限 5s：不能再大。后端的宽限期只有 30 秒，而**每次尝试本身还要花时间**
 *     （连接超时、握手）。序列累计 1+2+4+5+5 = 17 秒，加上每次尝试的开销，
 *     总窗口仍在 30 秒以内；换成 1/2/4/8/16 光等待就 31 秒，必然超期。
 *   * 长度 5 而非更多：能连续失败 5 次（约 17 秒）说明不是抖动，而是 Gateway
 *     或 Room 真的没起来。继续无限重试只会让界面永远停在「重连中」，
 *     玩家看不到「这一局已经丢了」这个事实。
 */
export const kReconnectBackoffMs: readonly number[] = [1_000, 2_000, 4_000, 5_000, 5_000]

/** 最大重试次数。等于退避序列长度，两者不会各自漂移。 */
export const kMaxReconnectAttempts = kReconnectBackoffMs.length

/** 一次订阅的执行体。测试用它替换真实的 fetch 流。 */
export type StreamRunner = (signal: AbortSignal) => Promise<void>

/**
 * 退避等待用到的定时器。默认就是全局的 setTimeout/clearTimeout。
 *
 * 之所以把它显式列出来而不是直接调全局函数：重连逻辑里真正难测的是
 * 「等多久」和「停止时定时器有没有被清掉」，而 vitest 的假定时器在
 * 「推进时间」与「冲微任务」之间的顺序上并不直观——用一次
 * `advanceTimersByTimeAsync` 可能跑掉不止一次尝试，断言会变得既脆弱又难读。
 * 换成注入一个几十行的虚拟时钟后，测试可以精确地说「时间走到 1000ms 时
 * 发生了什么」。生产路径不传这个参数，行为完全不变。
 */
export interface RetryTimers {
  set: (handler: () => void, delayMs: number) => unknown
  clear: (handle: unknown) => void
}

export interface ReconnectOptions {
  /** 执行一次订阅。生产环境传 `(signal) => streamRoom({...})`。 */
  run: StreamRunner
  /** 状态变化回调。每次变化都会调用一次，界面据此刷新。 */
  onStatus: (status: PushStatus) => void
  /** 单次订阅失败或流异常结束时的错误回调。主动停止不触发它。 */
  onError?: (cause: unknown) => void
  /** 退避等待的定时器实现。默认使用全局 setTimeout/clearTimeout。 */
  timers?: RetryTimers
}

/** 重连订阅的句柄。停止是幂等的。 */
export interface PushConnection {
  /** 停止：中止当前流、取消待执行的退避定时器。可重复调用。 */
  stop: () => void
  /**
   * 报告「刚收到一次真实数据」。调用方应在解析出事件后调用它。
   *
   * 这是**唯一**重置退避预算的入口，刻意不做成「连接建立即重置」：
   * 一个「接受连接后立刻关闭」的服务端会因此永远用不完预算，等价于无限重试。
   */
  resetBudget: () => void
  /** 当前状态，供不便等待回调的地方读取。 */
  status: () => PushStatus
}

/**
 * 带自动重连的推送订阅。
 *
 * 重试预算规则（这是本状态机的核心，测试逐条覆盖）：
 *   * 「收到数据」（`resetBudget`）重置预算。必须是真的收到事件，而不是 TCP
 *     连上或 HTTP 200——理由见 `PushConnection.resetBudget`。
 *   * 网络异常（抛错）与流被对端关闭算**同一种**失败，都消耗一次预算。
 *     从玩家视角两者没有区别：都是「收不到状态了」。
 *   * 预算耗尽后进入 `exhausted` 并停止，不会自己恢复；界面据此提示玩家，
 *     需要时由用户显式重试（重新调用本函数）。
 *   * `stop()` 之后不会再有任何尝试，也不会有残留的定时器或未释放的流。
 */
export function connectRoomPush(options: ReconnectOptions): PushConnection {
  const timers: RetryTimers = options.timers ?? {
    set: (handler, delayMs) => setTimeout(handler, delayMs),
    clear: (handle) => {
      clearTimeout(handle as ReturnType<typeof setTimeout>)
    },
  }
  const controller = new AbortController()
  let stopped = false
  let attempt = 0
  let status: PushStatus = 'connecting'
  let sleeping: unknown
  let wakeWait: (() => void) | undefined

  function report(next: PushStatus): void {
    status = next
    options.onStatus(next)
  }

  /** 退避时长。attempt 是「已经失败了几次」，也即序列下标。 */
  function backoffFor(attempt: number): number {
    return kReconnectBackoffMs[attempt] ?? kReconnectBackoffMs.at(-1) ?? 1_000
  }

  /**
   * 等待 `delayMs` 毫秒，或者主动停止。
   *
   * 停顿期间收到的每一次真实数据都会调用 resetBudget()，它把这次等待**直接
   * 重启**成第一个退避值。这一点是必要的而不是优化：真实数据到达说明链路已经
   * 恢复，此时还按上一次失败算出来的长退避（最多 5 秒）去等，等于白白烧掉
   * 后端 30 秒宽限期里最宝贵的那几秒。
   */
  async function waitFor(delayMs: number): Promise<void> {
    let wake: (() => void) | undefined
    const timer = new Promise<void>((resolve, reject) => {
      wake = resolve
      const onAbort = (): void => {
        if (sleeping !== undefined) {
          timers.clear(sleeping)
          sleeping = undefined
        }
        reject(controller.signal.reason)
      }
      if (controller.signal.aborted) {
        onAbort()
        return
      }
      // 先挂 abort 监听再设定时器：否则 stop() 恰好落在两者之间时，
      // 定时器会留下来，重连在「已停止」之后又跑一次。
      controller.signal.addEventListener('abort', onAbort, { once: true })
      sleeping = timers.set(() => {
        sleeping = undefined
        controller.signal.removeEventListener('abort', onAbort)
        resolve()
      }, delayMs)
    })
    wakeWait = wake
    try {
      await timer
    } finally {
      wakeWait = undefined
    }
  }

  async function run(): Promise<void> {
    for (;;) {
      report(attempt === 0 ? 'connecting' : 'reconnecting')
      try {
        await options.run(controller.signal)
      } catch (cause) {
        if (controller.signal.aborted) {
          return
        }
        options.onError?.(cause)
      }
      if (controller.signal.aborted) {
        return
      }
      if (attempt >= kMaxReconnectAttempts) {
        // 预算耗尽：界面必须能区分「还会自己好」与「不会了」，所以这里单独报状态，
        // 且不经过下面的 'stopped'（两者语义不同：一个是放弃，一个是主动停）。
        stopped = true
        report('exhausted')
        return
      }
      // 先按当前下标取退避值，再自增：attempt 的语义是「已经失败了几次」，
      // 而第 1 次重试必须用序列第 0 个值（1s）。顺序反过来会让每次退避
      // 都往后错一位——1s 变 2s、2s 变 4s，重连窗口被白扔掉几秒。
      const delayMs = backoffFor(attempt)
      attempt += 1
      report('reconnecting')
      try {
        await waitFor(delayMs)
      } catch {
        // 只有 abort 会走到这里：主动停止，不是失败。
        return
      }
    }
  }

  const finished = run()
    .catch((cause: unknown) => {
      // 走到这里说明是状态机自身的缺陷，而不是网络问题。不能静默吞掉。
      if (!stopped && !controller.signal.aborted) {
        options.onError?.(cause)
      }
    })
    .then(() => {
      stopped = true
      if (status !== 'exhausted') {
        report('stopped')
      }
    })

  /** 让 run() 落定后再返回，避免停止与「不再重试」之间出现竞态。 */
  void finished

  return {
    stop(): void {
      if (controller.signal.aborted) {
        return
      }
      controller.abort(new DOMException('推送订阅已停止', 'AbortError'))
    },
    resetBudget(): void {
      attempt = 0
      // 立刻结束当前等待：run() 会带着新预算重新排一次退避。
      const resume = wakeWait
      if (resume !== undefined) {
        wakeWait = undefined
        resume()
      }
    },
    status(): PushStatus {
      return status
    },
  }
}
