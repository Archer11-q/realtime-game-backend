/// @file stream.test.ts
/// @brief SSE 解析的单元测试。
///
/// 为什么这段逻辑值得单独测：它是整个前端里最容易出错、又最难靠肉眼发现的部分。
/// 解析错了的表现是「事件偶发丢失」或「JSON.parse 失败被静默吞掉」，
/// 在浏览器里看起来只是"有时候血条不更新"，几乎不可能稳定复现。
///
/// 这里覆盖的都是 SSE 规范里真实存在的边界，不是凑数：
/// 注释（心跳）、多行 data、冒号后无空格、CRLF、无 data 行。

import { describe, expect, it } from 'vitest'

import { decodeEnvelope, parseSseBlock } from './stream'

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
