/// @file stream.test.ts
/// @brief SSE 解析与请求头的单元测试。
///
/// 为什么这段逻辑值得单独测：它是整个前端里最容易出错、又最难靠肉眼发现的部分。
/// 解析错了的表现是「事件偶发丢失」或「JSON.parse 失败被静默吞掉」，
/// 在浏览器里看起来只是"有时候血条不更新"，几乎不可能稳定复现。
///
/// 这里覆盖的都是 SSE 规范里真实存在的边界，不是凑数：
/// 注释（心跳）、多行 data、冒号后无空格、CRLF、无 data 行、id 字段（TASK-017）。

import { afterEach, describe, expect, it, vi } from 'vitest'

import { decodeEnvelope, parseSseBlock, streamRoom } from './stream'

describe('parseSseBlock', () => {
  it('解析带 event 与 data 的普通事件', () => {
    const event = parseSseBlock('event: room.state\ndata: {"version":1}')
    expect(event).toEqual({ event: 'room.state', data: '{"version":1}' })
  })

  it('没有 event 字段时按规范使用 message', () => {
    const event = parseSseBlock('data: hello')
    expect(event).toEqual({ event: 'message', data: 'hello' })
  })

  it('心跳注释块返回 null，不能变成空事件', () => {
    // Gateway 每 15 秒发一次 ": ping"。若把它当成事件，界面会不停收到无意义事件。
    expect(parseSseBlock(': ping')).toBeNull()
  })

  it('注释与 data 混在同一块时只取 data', () => {
    const event = parseSseBlock(': keep-alive\nevent: room.state\ndata: {}')
    expect(event).toEqual({ event: 'room.state', data: '{}' })
  })

  it('多行 data 按换行连接', () => {
    const event = parseSseBlock('event: x\ndata: line1\ndata: line2')
    expect(event?.data).toBe('line1\nline2')
  })

  it('字段名与冒号之间可以有空格，值前的单个空格要去掉', () => {
    // 规范规定"冒号后的第一个空格不属于值"。少去掉它会让 JSON.parse 直接失败。
    const event = parseSseBlock('event:room.state\ndata:  {"a":1}')
    expect(event).toEqual({ event: 'room.state', data: ' {"a":1}' })
  })

  it('容忍 CRLF 行尾', () => {
    const event = parseSseBlock('event: room.state\r\ndata: {"a":1}\r')
    expect(event).toEqual({ event: 'room.state', data: '{"a":1}' })
  })

  it('只有注释没有 data 时返回 null', () => {
    expect(parseSseBlock(': ping\n: ping')).toBeNull()
  })

  it('空块返回 null', () => {
    expect(parseSseBlock('')).toBeNull()
  })

  it('保留 data 里的空字符串', () => {
    // 与"没有 data 行"不同：这是服务端确实发了一个空载荷。
    // 两者都返回 null 会让调用方无法区分"没数据"与"心跳"。
    const event = parseSseBlock('event: x\ndata:')
    expect(event).toEqual({ event: 'x', data: '' })
  })

  // --- TASK-017：id 字段（补发的依据） ------------------------------------

  it('解析 id 行成数字（帧号）', () => {
    const event = parseSseBlock('id: 42\nevent: room.state\ndata: {"frame":42}')
    expect(event).toEqual({ event: 'room.state', data: '{"frame":42}', id: 42 })
  })

  it('id 可以出现在 event 之前', () => {
    // 服务端固定把 id 写在第一行，但解析不应依赖顺序。
    const event = parseSseBlock('event: stream.reset\nid: 7\ndata: {}')
    expect(event?.id).toBe(7)
  })

  it('没有 id 行时不产生 id 字段', () => {
    // session.ready 与心跳没有 id。若默认填 0，客户端会把 0 当成
    // "我收到了第 0 帧"，下次重连时回传一个假的 Last-Event-ID。
    const event = parseSseBlock('event: session.ready\ndata: {}')
    expect(event).toEqual({ event: 'session.ready', data: '{}' })
    expect(event?.id).toBeUndefined()
  })

  it('id 不是合法数字时忽略它', () => {
    const event = parseSseBlock('id: abc\nevent: x\ndata: {}')
    expect(event?.id).toBeUndefined()
  })

  it('负数 id 被忽略', () => {
    // 帧号由服务端从 0 单调生成，不存在负数。
    const event = parseSseBlock('id: -5\nevent: x\ndata: {}')
    expect(event?.id).toBeUndefined()
  })

  it('id 为 0 时保留（0 是合法的帧号）', () => {
    const event = parseSseBlock('id: 0\nevent: x\ndata: {}')
    expect(event?.id).toBe(0)
  })
})

describe('decodeEnvelope', () => {
  it('解析合法 JSON', () => {
    expect(decodeEnvelope({ event: 'room.state', data: '{"type":"room.state"}' })).toEqual({
      type: 'room.state',
    })
  })

  it('非法 JSON 返回 null 而不是抛异常', () => {
    // 一条坏事件不应该让整个流断掉。
    expect(decodeEnvelope({ event: 'room.state', data: '{坏' })).toBeNull()
  })
})

// ---------------------------------------------------------------------------
// Last-Event-ID 请求头（TASK-017）
// ---------------------------------------------------------------------------

/** 让 fetch 返回一条立刻结束的 SSE 流，并记录它收到的 headers。 */
function stubFetch(): { headers: Record<string, string> } {
  const captured: { headers: Record<string, string> } = { headers: {} }
  const body = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.close()
    },
  })
  // 参数名加下划线：`noUnusedParameters` 会拦下未使用的形参，而这个桩只关心 headers。
  vi.stubGlobal('fetch', (_url: string | URL, init?: RequestInit) => {
    const headers = (init?.headers ?? {}) as Record<string, string>
    captured.headers = headers
    return Promise.resolve(
      new Response(body, { status: 200, headers: { 'Content-Type': 'text/event-stream' } }),
    )
  })
  return captured
}

describe('streamRoom 的 Last-Event-ID', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('带 lastEventId 时发送 Last-Event-ID 请求头', async () => {
    const captured = stubFetch()
    await streamRoom({
      roomId: 'r-1',
      token: 't-1',
      signal: new AbortController().signal,
      lastEventId: 42,
      onEvent: () => undefined,
    })

    expect(captured.headers['Last-Event-ID']).toBe('42')
    // token 仍然走 Authorization 头：补发功能不能把凭据挤到查询串里。
    expect(captured.headers.Authorization).toBe('Bearer t-1')
  })

  it('不带 lastEventId 时不发送这个头（首次订阅）', async () => {
    const captured = stubFetch()
    await streamRoom({
      roomId: 'r-1',
      token: 't-1',
      signal: new AbortController().signal,
      onEvent: () => undefined,
    })

    expect(captured.headers['Last-Event-ID']).toBeUndefined()
  })

  it('lastEventId 为 0 时也要发送（0 是合法帧号）', async () => {
    const captured = stubFetch()
    await streamRoom({
      roomId: 'r-1',
      token: 't-1',
      signal: new AbortController().signal,
      lastEventId: 0,
      onEvent: () => undefined,
    })

    expect(captured.headers['Last-Event-ID']).toBe('0')
  })
})
