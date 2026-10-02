/// @file stream.ts
/// @brief 消费 Gateway 的 SSE 推送。
///
/// **为什么不用浏览器原生的 `EventSource`**：它无法设置请求头，token 只能放进
/// 查询字符串，于是 token 会出现在访问日志、浏览器历史与 Referer 里。
/// 这里改用 `fetch` + `ReadableStream` 手动解析 SSE——多约 60 行代码，
/// 换来 token 始终走 `Authorization` 头。后端两种方式都支持，选这一种是安全取舍。
///
/// 断线重连不在这里实现（Phase 2）。当前行为是：流被服务端关闭或网络断开时，
/// `streamRoom` 正常返回，由调用方决定界面提示与下一步动作。

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
