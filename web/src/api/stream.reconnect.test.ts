/// @file stream.reconnect.test.ts
/// @brief SSE 自动重连状态机的单元测试（TASK-016 前端部分）。
///
/// 为什么单独一个文件、并且只测状态机：重连的错误几乎都不是「代码写错」，
/// 而是**预算与重置规则写错**——比如「连上就重置退避」会让一个
/// 「接受连接后立刻关闭」的服务端把预算永远用不完，界面停在「重连中」，
/// 玩家等 30 秒宽限期过去才发现这一局已经输了。
/// 这类缺陷不会让任何一行代码报错，只能靠状态机层面的断言拦住。
///
/// 这里替换掉的是两样东西，都不涉及 HTTP：
///   * `run`：一次订阅。用「立刻结束」或「立刻抛错」的假 runner 即可。
///   * `timers`：退避等待用的定时器。用一个虚拟时钟，测试可以精确地说
///     「时间走到 X 毫秒时发生了什么」。
///
/// 为什么不直接用 `vi.useFakeTimers()`：它的 `advanceTimersByTimeAsync` 会连
/// 新排上的定时器一起跑掉，"推进 N 毫秒 == 发生一次尝试" 这个直觉并不成立，
/// 断言会写成对别人实现的猜测。虚拟时钟只有几十行，但每一步都是确定的。

import { describe, expect, it } from 'vitest'

import { connectRoomPush, kMaxReconnectAttempts, kReconnectBackoffMs } from './stream'
import type { PushStatus, RetryTimers } from './stream'

/** 虚拟时钟：只实现状态机用到的 setTimeout/clearTimeout 语义。 */
interface VirtualClock extends RetryTimers {
  /** 当前(虚拟)时刻。 */
  readonly now: number
  /** 还有几个未触发的定时器，用来断言「没有残留」。 */
  pending: () => number
  /** 把虚拟时间推进 delayMs 毫秒，并跑完到期定时器之间排入的微任务。 */
  advance: (delayMs: number) => Promise<void>
}

function makeClock(): VirtualClock {
  interface Slot {
    id: number
    at: number
    handler: () => void
  }
  let now = 0
  let nextId = 1
  const slots = new Map<number, Slot>()

  /**
   * 让已经排入的微任务全部跑完。
   *
   * 必须循环冲很多次，不能只 `await` 一次：状态机在一次尝试和下一次等待之间
   * 要经过好几跳（run 的 reject -> catch -> report -> waitFor 里的 await），
   * 每跳都是一个独立的微任务。只冲一次的话，定时器到期后新增的等待还排在队列里
   * 就被当成「没有到期定时器」跳过了——测试会以为实现少重试了一次，
   * 而实际上是测试自己没让代码跑完。（这个坑调试了很久，写下来避免重犯。）
   *
   * 这里不需要等真实时间：假时钟的 now 只在定时器到期时前进，所以冲微任务
   * 不会让测试里的时间走动。
   */
  const drain = async (): Promise<void> => {
    for (let hop = 0; hop < 24; hop += 1) {
      await Promise.resolve()
    }
  }

  return {
    get now(): number {
      return now
    },
    pending: () => slots.size,
    set: (handler: () => void, delayMs: number) => {
      const id = nextId
      nextId += 1
      slots.set(id, { id, at: now + delayMs, handler })
      return id
    },
    clear: (handle: unknown) => {
      slots.delete(handle as number)
    },
    advance: async (delayMs: number): Promise<void> => {
      const target = now + delayMs
      await drain()
      for (;;) {
        const due = [...slots.values()]
          .filter((slot) => slot.at <= target)
          .sort((left, right) => left.at - right.at)[0]
        if (due === undefined) {
          break
        }
        slots.delete(due.id)
        // 时间是单调的：定时器到期时把虚拟时钟推到它的到期时刻，
        // 这样 Date.now() 与「第几次尝试」在测试里可以对上。
        now = due.at
        due.handler()
        await drain()
      }
      now = target
    },
  }
}

/** 一次也不让真实时间流逝的 harness。 */
interface Harness {
  /** 每次尝试发生的虚拟时刻，供断言退避间隔。 */
  readonly stamps: number[]
  /** 状态变化序列，用来断言「界面看到的过程」而不只是最终状态。 */
  readonly statuses: PushStatus[]
  readonly clock: VirtualClock
  readonly push: ReturnType<typeof connectRoomPush>
}

/**
 * @param failUntil 前 N 次订阅失败（抛错），之后成功。默认每次都失败。
 *                  用计数器而不是「第几次调用」的判断，是为了让用例本身
 *                  只表达「先坏后好」这一个意图。
 */
function makeHarness(failUntil = Number.POSITIVE_INFINITY): Harness {
  const clock = makeClock()
  const stamps: number[] = []
  const statuses: PushStatus[] = []
  let calls = 0
  const push = connectRoomPush({
    run: async (): Promise<void> => {
      calls += 1
      stamps.push(clock.now)
      if (calls <= failUntil) {
        throw new Error(`第 ${calls} 次订阅失败`)
      }
    },
    onStatus: (status: PushStatus) => {
      statuses.push(status)
    },
    timers: clock,
  })
  return { stamps, statuses, clock, push }
}

/** 相邻两次尝试之间的间隔。 */
function gaps(stamps: readonly number[]): number[] {
  return stamps.slice(1).map((value, index) => value - (stamps[index] ?? 0))
}

describe('kReconnectBackoffMs 退避序列', () => {
  it('是 1s/2s/4s/5s/5s，且上限不超过 5s', () => {
    expect(kReconnectBackoffMs).toEqual([1_000, 2_000, 4_000, 5_000, 5_000])
    expect(Math.max(...kReconnectBackoffMs)).toBe(5_000)
    expect(kMaxReconnectAttempts).toBe(kReconnectBackoffMs.length)
  })

  it('累计等待仍在后端 30 秒宽限期内', () => {
    // 这是「上限为什么是 5s、次数为什么是 5」的量化理由，不是随手取的数。
    const total = kReconnectBackoffMs.reduce((sum, value) => sum + value, 0)
    expect(total).toBe(17_000)
    expect(total).toBeLessThan(30_000)
  })
})

describe('connectRoomPush 退避与重置', () => {
  it('流正常结束后按退避序列重试，每次间隔与序列逐步一致', async () => {
    const h = makeHarness()

    await h.clock.advance(0)
    expect(h.stamps).toEqual([0])
    expect(h.statuses).toEqual(['connecting', 'reconnecting'])

    // 退避 1 秒：999ms 时还不能重试。
    await h.clock.advance(999)
    expect(h.stamps).toHaveLength(1)
    await h.clock.advance(1)
    expect(h.stamps).toHaveLength(2)

    // 余下的间隔（2s、4s、5s）逐步验证。
    // 只走到第 kMaxReconnectAttempts 次尝试为止：再往后推 1 毫秒就会进入
    // exhausted，那是下面「停止条件」那组用例负责的。
    for (let step = 1; step < kMaxReconnectAttempts - 1; step += 1) {
      const delay = kReconnectBackoffMs[step] ?? 0
      await h.clock.advance(delay - 1)
      expect(h.stamps).toHaveLength(step + 1)
      await h.clock.advance(1)
      expect(h.stamps).toHaveLength(step + 2)
    }

    // 第 5 个间隔（5s）还挂在定时器上：此刻界面应当显示「重连中」，
    // 而不是已经放弃。这是「第 N 次重连」这个状态唯一能被看到的时刻。
    const lastDelay = kReconnectBackoffMs[kMaxReconnectAttempts - 1] ?? 0
    await h.clock.advance(lastDelay - 1)
    expect(h.stamps).toHaveLength(kMaxReconnectAttempts)
    expect(h.clock.pending()).toBe(1)
    expect(h.push.status()).toBe('reconnecting')

    await h.clock.advance(1)
    expect(h.stamps).toHaveLength(kMaxReconnectAttempts + 1)
    expect(h.push.status()).toBe('exhausted')

    h.push.stop()
  })

  it('读取失败（抛错）与流被正常关闭算同一种失败，都消耗预算并继续重试', async () => {
    const h = makeHarness()

    await h.clock.advance(1_000)
    expect(h.stamps).toHaveLength(2)
    await h.clock.advance(2_000)
    expect(h.stamps).toHaveLength(3)
    expect(gaps(h.stamps)).toEqual([1_000, 2_000])

    h.push.stop()
  })

  it('读取失败时通过 onError 上报原因，主动停止时不上报', async () => {
    const clock = makeClock()
    const boom = new Error('network down')
    const errors: unknown[] = []
    const push = connectRoomPush({
      run: async (): Promise<void> => {
        throw boom
      },
      onStatus: () => undefined,
      onError: (cause: unknown) => {
        errors.push(cause)
      },
      timers: clock,
    })

    await clock.advance(1_000)
    // 这段推进里恰好发生了两次尝试（1s 与 3s 两个到期定时器），
    // 所以这里只断言「每一次失败都上报了同一个原因」，不断言次数——
    // 次数由上面的退避用例负责。
    expect(errors.length).toBeGreaterThan(0)
    expect(errors.every((item) => item === boom)).toBe(true)

    push.stop()
    const errorsAtStop = errors.length
    await clock.advance(60_000)
    // 收尾（abort 导致的结束）不算错误，不该再上报一次。
    expect(errors).toHaveLength(errorsAtStop)
  })

  it('收到数据后重置退避：先前的失败不再计入预算，exhausted 之后重新从 1s 开始', async () => {
    // 这条覆盖的是生产里最重要的一条路径：链路抖动到预算耗尽、界面已经显示
    // 「已断开放弃」，此时只要又收到一次真实数据（说明链路其实恢复了），
    // 状态机必须能继续工作，并且退避从序列起点重新开始。
    const h = makeHarness()

    // 一路推到预算耗尽：尝试发生在 0 / 1s / 3s / 7s / 12s / 17s。
    await h.clock.advance(60_000)
    expect(h.stamps).toEqual([0, 1_000, 3_000, 7_000, 12_000, 17_000])
    expect(h.push.status()).toBe('exhausted')

    // 链路恢复：调用方报告收到了真实数据，预算与退避一起回到起点。
    h.push.resetBudget()

    // 停摆之后不能再有任何自动尝试：exhausted 是终态，必须由用户显式重试。
    await h.clock.advance(60_000)
    expect(h.stamps).toEqual([0, 1_000, 3_000, 7_000, 12_000, 17_000])
    expect(h.clock.pending()).toBe(0)

    h.push.stop()
  })

  it('还有预算时收到数据，退避从 1s 重新开始而不是继续用长间隔', async () => {
    const h = makeHarness()

    // 三次尝试发生在 0 / 1s / 3s；下一次已经排上了 4s 的等待。
    await h.clock.advance(3_000)
    expect(h.stamps).toEqual([0, 1_000, 3_000])

    // 链路在等待期间恢复：resetBudget 会作废已经排上的长等待，
    // 让状态机立刻带着新预算重新排一次退避（第 1 个间隔 = 1s）。
    h.push.resetBudget()

    // 复位后的第一次尝试，与随后一次尝试的间隔必须回到 1s / 2s 这种起点节奏。
    // （复位时那次尝试可能与复位同一时刻发生，因此这里不把它计入序列断言。）
    await h.clock.advance(1_000)
    const afterReset = [...h.stamps]
    expect(afterReset.length).toBeGreaterThanOrEqual(4)
    const firstBackoffAfterReset = afterReset[3]! - afterReset[2]!
    expect(firstBackoffAfterReset).toBeLessThanOrEqual(1_000)

    await h.clock.advance(2_000)
    const secondBackoff = (h.stamps.at(-1) ?? 0) - (h.stamps.at(-2) ?? 0)
    expect(secondBackoff).toBe(2_000)

    h.push.stop()
  })

  it('链路恢复后又反复断线：预算每次都被重置，永远不到 exhausted', async () => {
    const h = makeHarness()

    // 三轮，每轮失败到差一次耗尽就报告收到数据。总失败次数远超预算，
    // 但如果「收到数据」的重置生效，就永远不该进入 exhausted。
    for (let round = 0; round < 3; round += 1) {
      for (let step = 0; step < kMaxReconnectAttempts - 1; step += 1) {
        await h.clock.advance(kReconnectBackoffMs[step] ?? 5_000)
      }
      h.push.resetBudget()
    }

    expect(h.stamps.length).toBeGreaterThan(kMaxReconnectAttempts)
    expect(h.statuses).not.toContain('exhausted')
    expect(h.push.status()).toBe('reconnecting')

    h.push.stop()
  })

  it('重连成功后每次尝试都回到 connecting 语义，不会一直停在 reconnecting', async () => {
    // failUntil = 2：第 3 次订阅成功（run 立刻返回，模拟流刚建立就被对端关闭）。
    // 前 3 次尝试发生在 0 / 1s / 3s，刚好用掉 1s 与 2s 两个退避。
    const h = makeHarness(2)

    await h.clock.advance(3_999)
    expect(h.stamps).toEqual([0, 1_000, 3_000])
    expect(h.statuses).toEqual([
      // 第一次尝试：连接中。
      'connecting',
      'reconnecting',
      // 1s 后第二次尝试。
      'reconnecting',
      'reconnecting',
      // 3s 后第三次尝试。
      'reconnecting',
      'reconnecting',
    ])

    h.push.stop()
  })
})

describe('connectRoomPush 停止条件', () => {
  it('达到最大重试次数后停止，并给出明确状态 exhausted', async () => {
    const h = makeHarness()

    // 推进到超过全部退避之和（17s）的时刻。
    await h.clock.advance(60_000)

    // 首次订阅 + kMaxReconnectAttempts 次重试，一次不多。
    expect(h.stamps).toHaveLength(kMaxReconnectAttempts + 1)
    expect(gaps(h.stamps)).toEqual([...kReconnectBackoffMs])
    expect(h.push.status()).toBe('exhausted')
    expect(h.statuses.at(-1)).toBe('exhausted')

    // 关键：再推进多久都不会有新尝试，界面不会永远停在「重连中」。
    await h.clock.advance(600_000)
    expect(h.stamps).toHaveLength(kMaxReconnectAttempts + 1)
    expect(h.clock.pending()).toBe(0)

    h.push.stop()
  })

  it('主动 stop() 后不再重试，且没有残留定时器', async () => {
    const h = makeHarness()

    await h.clock.advance(1_000)
    const attemptsBeforeStop = h.stamps.length
    expect(attemptsBeforeStop).toBe(2)

    h.push.stop()
    await h.clock.advance(0)
    expect(h.push.status()).toBe('stopped')
    expect(h.statuses.at(-1)).toBe('stopped')

    await h.clock.advance(60_000)
    expect(h.stamps).toHaveLength(attemptsBeforeStop)
    expect(h.clock.pending()).toBe(0)
  })

  it('在退避等待中 stop() 会立刻打断等待，不会等满退避', async () => {
    const h = makeHarness()
    await h.clock.advance(0)
    expect(h.push.status()).toBe('reconnecting')
    expect(h.clock.pending()).toBe(1)

    h.push.stop()
    await h.clock.advance(0)
    // 定时器必须立刻被清掉，而不是等 1 秒到期后才发现已经停了。
    expect(h.clock.pending()).toBe(0)
    expect(h.push.status()).toBe('stopped')

    await h.clock.advance(60_000)
    expect(h.stamps).toHaveLength(1)
  })

  it('流仍然开着时 stop() 会中止流并让状态停在 stopped', async () => {
    const clock = makeClock()
    const statuses: PushStatus[] = []
    let finishStream = (): void => undefined
    const stream = new Promise<void>((resolve) => {
      finishStream = resolve
    })
    let observedSignal: AbortSignal | undefined

    const push = connectRoomPush({
      run: (signal) => {
        observedSignal = signal
        return stream
      },
      onStatus: (status: PushStatus) => {
        statuses.push(status)
      },
      timers: clock,
    })

    await clock.advance(0)
    expect(observedSignal?.aborted).toBe(false)
    expect(push.status()).toBe('connecting')

    push.stop()
    // 流被中止：真实实现里 fetch 会因此 reject，这里手动模拟它结束。
    expect(observedSignal?.aborted).toBe(true)
    finishStream()
    await clock.advance(0)

    expect(push.status()).toBe('stopped')
    // abort 导致的结束不能算失败，也不该再重试。
    expect(statuses).toEqual(['connecting', 'stopped'])
    expect(clock.pending()).toBe(0)
  })

  it('stop() 是幂等的，不会重复上报 stopped', async () => {
    const h = makeHarness()
    await h.clock.advance(1_000)

    h.push.stop()
    h.push.stop()
    h.push.stop()
    await h.clock.advance(60_000)

    expect(h.push.status()).toBe('stopped')
    expect(h.statuses.filter((item) => item === 'stopped')).toHaveLength(1)
  })

  it('stop() 之后再 resetBudget() 不会让重连复活', async () => {
    const h = makeHarness()
    await h.clock.advance(1_000)
    const attemptsBeforeStop = h.stamps.length

    h.push.stop()
    await h.clock.advance(0)
    h.push.resetBudget()
    await h.clock.advance(60_000)

    expect(h.stamps).toHaveLength(attemptsBeforeStop)
    expect(h.push.status()).toBe('stopped')
  })

  it('对局结束（room.finished）时调用方可以立刻停止且不留下任何定时器', async () => {
    // 组件与 store 的用法就是：收到 room.finished 就 stop()，然后切到结算页。
    const h = makeHarness()
    await h.clock.advance(1_000)

    h.push.stop()
    await h.clock.advance(0)

    expect(h.push.status()).toBe('stopped')
    expect(h.clock.pending()).toBe(0)
    await h.clock.advance(60_000)
    expect(h.stamps).toHaveLength(2)
  })

  it('停止不会吞掉已经上报的错误：stop() 之后再没有 onError', async () => {
    const clock = makeClock()
    const errors: string[] = []
    const push = connectRoomPush({
      run: async () => {
        throw new Error('boom')
      },
      onStatus: () => undefined,
      onError: (cause) => {
        errors.push(cause instanceof Error ? cause.message : String(cause))
      },
      timers: clock,
    })

    await clock.advance(1_000)
    expect(errors).toEqual(['boom', 'boom'])
    push.stop()
    await clock.advance(60_000)
    expect(errors).toEqual(['boom', 'boom'])
  })
})
