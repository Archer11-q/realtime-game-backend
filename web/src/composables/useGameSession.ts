/// @file useGameSession.ts
/// @brief 演示用的会话状态机：登录 -> 大厅（匹配）-> 对战 -> 结算。
///
/// 状态放在模块作用域，也就是一个「不用 Pinia 的极简 store」。
/// 理由：整个演示只有一个会话、四个阶段，跨组件共享的也只有这几项。
/// 为一个 20 行的状态引入 Pinia 与它的 devtools 集成，属于 CLAUDE.md 第 6 条
/// 明确反对的「以技术先进为理由增加组件」。

import { computed, ref } from 'vue'

import * as api from '../api/client'
import { ApiFailure, NetworkFailure } from '../api/client'
import {
  connectRoomPush,
  decodeEnvelope,
  kMaxReconnectAttempts,
  streamRoom,
} from '../api/stream'
import type { PushConnection, PushStatus } from '../api/stream'
import type {
  MatchResultInfo,
  MatchStatusInfo,
  PlayerInfo,
  RoomPlayerInfo,
  RoomStateInfo,
  SessionReadyPayload,
  StreamEnvelope,
} from '../api/types'

export type Stage = 'login' | 'lobby' | 'battle' | 'result'

/** 匹配轮询间隔。匹配状态变化是低频事件，不需要更密。 */
const kMatchPollIntervalMs = 800
/** 结果落库的兜底重试次数与间隔（正常路径上一次就能拿到）。 */
const kResultRetryLimit = 5
const kResultRetryIntervalMs = 400

const token = ref('')
const player = ref<PlayerInfo | null>(null)
const stage = ref<Stage>('login')
const match = ref<MatchStatusInfo | null>(null)
const room = ref<RoomStateInfo | null>(null)
const result = ref<MatchResultInfo | null>(null)
const resultPending = ref(false)
const notice = ref('')
const errorText = ref('')
/** 推送连接状态。见 api/stream.ts 的 PushStatus：重连中与已放弃必须能分辨。 */
const streamStatus = ref<PushStatus>('stopped')
/** 当前已经尝试到第几次重连，供界面显示「重连中（第 N/5 次）」。 */
const streamAttempt = ref(0)
const lastSequence = ref(-1)

let matchPollTimer: ReturnType<typeof setTimeout> | undefined
let push: PushConnection | undefined
let pushRoomId = ''

const selfPlayerId = computed(() => player.value?.player_id ?? '')

const selfPlayer = computed<RoomPlayerInfo | undefined>(() =>
  room.value?.players?.find((item) => item.player_id === selfPlayerId.value),
)

const opponent = computed<RoomPlayerInfo | undefined>(() =>
  room.value?.players?.find((item) => item.player_id !== selfPlayerId.value),
)

function clearNotice(): void {
  notice.value = ''
  errorText.value = ''
}

function describeFailure(cause: unknown): string {
  if (cause instanceof ApiFailure) {
    // 按 reason 而不是按状态码分支：reason 是稳定标识，状态码会随分类调整而变。
    switch (cause.reason) {
      case 'token_required':
      case 'invalid_token':
        return '会话已失效，请重新登录。'
      case 'room_unavailable':
        return '房间服务当前不可用（Room 未启动或已重启）。'
      case 'match_unavailable':
        return '匹配服务当前不可用（Match 未启动或已重启）。'
      case 'result_pending':
        return '对局已结束，但结果尚未落库，正在重试。'
      case 'not_a_member':
        return '你不是这一局的成员，无法订阅它的状态。'
      case 'room_already_finished':
        return '这一局已经结束了。'
      case 'room_not_playing':
        return '对局尚未开始，暂时不能提交攻击。'
      case 'session_store_unavailable':
        return '会话存储（Redis）当前不可用。'
      default:
        return `${cause.message}（${cause.reason}）`
    }
  }
  if (cause instanceof NetworkFailure) {
    return cause.message
  }
  return cause instanceof Error ? cause.message : String(cause)
}

function stopMatchPolling(): void {
  if (matchPollTimer !== undefined) {
    clearTimeout(matchPollTimer)
    matchPollTimer = undefined
  }
}

function stopStream(): void {
  if (push !== undefined) {
    push.stop()
    push = undefined
  }
  pushRoomId = ''
  streamAttempt.value = 0
  streamStatus.value = 'stopped'
}

export function useGameSession() {
  async function signIn(account: string, password: string): Promise<void> {
    clearNotice()
    try {
      const response = await api.login({ account, password })
      token.value = response.token ?? ''
      player.value = response.player ?? null
      if (token.value === '' || player.value === null) {
        errorText.value = '登录成功但响应里缺少 Token 或玩家信息。'
        return
      }
      stage.value = 'lobby'
    } catch (cause) {
      errorText.value = describeFailure(cause)
    }
  }

  async function signOut(): Promise<void> {
    stopMatchPolling()
    stopStream()
    const current = token.value
    token.value = ''
    player.value = null
    match.value = null
    room.value = null
    result.value = null
    resultPending.value = false
    lastSequence.value = -1
    stage.value = 'login'
    clearNotice()
    if (current !== '') {
      // 登出失败不影响本地已经清掉的会话状态。
      await api.logout(current).catch(() => undefined)
    }
  }

  /** 匹配轮询。命中 matched 后自动进房并订阅推送。 */
  function schedulePoll(): void {
    stopMatchPolling()
    matchPollTimer = setTimeout(() => {
      void pollOnce()
    }, kMatchPollIntervalMs)
  }

  async function pollOnce(): Promise<void> {
    if (token.value === '' || stage.value !== 'lobby') {
      return
    }
    try {
      const response = await api.getMatchStatus(token.value)
      match.value = response.match ?? null
      const state = match.value?.state
      if (state === 'matched') {
        const roomId = match.value?.room_id ?? ''
        if (roomId !== '') {
          await enterBattle(roomId)
          return
        }
      }
      if (state === 'timeout') {
        notice.value = '排队超时，已回到未排队状态。可以再点一次开始匹配。'
        stopMatchPolling()
        return
      }
      schedulePoll()
    } catch (cause) {
      errorText.value = describeFailure(cause)
      // 依赖抖动时继续轮询：Match 恢复后应当自动接上，不需要用户重开页面。
      schedulePoll()
    }
  }

  async function startMatching(): Promise<void> {
    clearNotice()
    try {
      const response = await api.enqueueMatch(token.value)
      match.value = response.match ?? null
      if (match.value?.state === 'matched') {
        const roomId = match.value.room_id ?? ''
        if (roomId !== '') {
          await enterBattle(roomId)
          return
        }
      }
      schedulePoll()
    } catch (cause) {
      errorText.value = describeFailure(cause)
    }
  }

  async function cancelMatching(): Promise<void> {
    clearNotice()
    stopMatchPolling()
    try {
      const response = await api.cancelMatch(token.value)
      match.value = response.match ?? null
    } catch (cause) {
      errorText.value = describeFailure(cause)
    }
  }

  /** 进入对战：先加入房间（幂等），再订阅 SSE。 */
  async function enterBattle(roomId: string): Promise<void> {
    stopMatchPolling()
    clearNotice()
    try {
      const response = await api.joinRoom(token.value, roomId)
      room.value = response.room ?? null
    } catch (cause) {
      errorText.value = describeFailure(cause)
      return
    }
    stage.value = 'battle'
    startStream(roomId)
  }

  /**
   * 订阅房间推送，并带上自动重连（TASK-016 前端部分）。
   *
   * 重连窗口就是 Gateway 的 30 秒宽限期：期内重连成功，对局接着打；
   * 超过宽限期判断线方负。因此这里的重试预算（1/2/4/5/5 秒）刻意压在 17 秒内，
   * 给「每次尝试本身的连接耗时」留出余量。
   *
   * 状态与重连的关系：
   *   * session.ready 只说明「流通了」，不重置退避——真正重置的是收到带 payload
   *     的事件（room.state / room.finished），由 push.resetBudget() 表达。
   *   * room.finished 是终局：立刻 stop()，不再重连，也不会留下定时器。
   */
  function startStream(roomId: string): void {
    stopStream()
    pushRoomId = roomId
    lastSequence.value = -1
    streamAttempt.value = 0
    streamStatus.value = 'connecting'

    const connection = connectRoomPush({
      run: (signal) =>
        streamRoom({
          roomId,
          token: token.value,
          signal,
          onEvent: (event) => {
            if (event.event === 'session.ready') {
              streamStatus.value = 'connected'
              return
            }
            const envelope = decodeEnvelope<StreamEnvelope<RoomStateInfo>>(event)
            if (envelope?.payload === undefined) {
              return
            }
            room.value = envelope.payload
            if (typeof envelope.sequence === 'number') {
              lastSequence.value = envelope.sequence
            }
            // 收到真实数据：链路确实通了，退避预算回到起点。
            connection.resetBudget()
            streamStatus.value = 'connected'
            if (event.event === 'room.finished') {
              // 对局结束：先停掉重连，再切结算页。顺序不能反——
              // finishBattle() 是异步的，期间若还有重连在跑，会重新订阅一个
              // 已经结束的房间。
              connection.stop()
              void finishBattle()
            }
          },
        }),
      onStatus: (status) => {
        streamStatus.value = status
        // 重试次数是状态的派生值，不单独维护一个可能跑偏的计数器：
        // 状态机的 'reconnecting' 每次等待开始时上报一次，递增即可；
        // 而 'exhausted' 一定等于上限，直接对齐，避免显示「第 4/5 次」却已经放弃。
        switch (status) {
          case 'connecting':
          case 'connected':
            streamAttempt.value = 0
            break
          case 'reconnecting':
            streamAttempt.value = Math.min(streamAttempt.value + 1, kMaxReconnectAttempts)
            break
          case 'exhausted':
            streamAttempt.value = kMaxReconnectAttempts
            break
          case 'stopped':
          default:
            break
        }
      },
      onError: (cause) => {
        // 只在还处于对战阶段时提示：切到结算页之后的失败是收尾噪音。
        if (stage.value === 'battle') {
          errorText.value = describeFailure(cause)
        }
      },
    })
    push = connection
  }

  /** 界面上「重试连接」按钮的动作：预算耗尽后用户显式要求再试。 */
  function retryStream(): void {
    const roomId = pushRoomId !== '' ? pushRoomId : (room.value?.room_id ?? '')
    if (roomId === '') {
      return
    }
    errorText.value = ''
    startStream(roomId)
  }

  /**
   * 查询落库结果。
   *
   * `result_pending`（503）表示「对局已结束但结果还没写进数据库」，是**可重试**的
   * 状态，因此这里按间隔重试几次；其他失败（MySQL 不可用、404 等）不重试。
   * 正常路径上结果在 room.finished 推送之前就已同步写入，一次就能拿到。
   */
  async function fetchResult(matchId: string): Promise<void> {
    if (matchId === '' || token.value === '') {
      return
    }
    for (let attempt = 0; attempt < kResultRetryLimit; attempt += 1) {
      try {
        const response = await api.getMatchResult(token.value, matchId)
        result.value = response.result ?? null
        resultPending.value = false
        return
      } catch (cause) {
        if (cause instanceof ApiFailure && cause.reason === 'result_pending') {
          resultPending.value = true
          await new Promise((resolve) => setTimeout(resolve, kResultRetryIntervalMs))
          continue
        }
        // 其他失败不再重试：界面仍能显示 SSE 推送里的权威胜负，
        // 只是「已落库」这一项会显示为否。
        errorText.value = describeFailure(cause)
        resultPending.value = false
        return
      }
    }
    // 重试用尽仍未落库：保留 pending 提示，由用户手动重查。
    resultPending.value = true
  }

  /** 对局结束：进入结算页并查询落库结果。 */
  async function finishBattle(): Promise<void> {
    stage.value = 'result'
    resultPending.value = false
    const matchId = match.value?.match_id ?? room.value?.match_id ?? ''
    await fetchResult(matchId)
  }

  /** 结算页的手动重查。落库失败时 Room 会按 1 秒间隔重试，因此重查是有意义的。 */
  async function reloadResult(matchId: string): Promise<void> {
    errorText.value = ''
    await fetchResult(matchId)
  }

  async function attack(): Promise<void> {
    clearNotice()
    const roomId = room.value?.room_id ?? ''
    if (roomId === '') {
      errorText.value = '还没有房间，无法提交攻击。'
      return
    }
    try {
      await api.submitAttack(token.value, roomId)
      // 攻击结果不在这里落地：界面显示的一切状态都来自 SSE 推送的权威快照，
      // 避免「本地乐观更新」与服务端判定不一致时出现两种真相。
    } catch (cause) {
      errorText.value = describeFailure(cause)
    }
  }

  function backToLobby(): void {
    stopStream()
    room.value = null
    result.value = null
    resultPending.value = false
    match.value = null
    notice.value = ''
    errorText.value = ''
    stage.value = 'lobby'
  }

  return {
    // 状态
    token,
    player,
    stage,
    match,
    room,
    result,
    resultPending,
    notice,
    errorText,
    streamStatus,
    streamAttempt,
    lastSequence,
    selfPlayerId,
    selfPlayer,
    opponent,
    // 动作
    signIn,
    signOut,
    startMatching,
    cancelMatching,
    enterBattle,
    attack,
    reloadResult,
    retryStream,
    backToLobby,
  }
}

/** 供组件之外使用（例如调试时清空状态）。 */
export function resetSession(): void {
  stopMatchPolling()
  stopStream()
  token.value = ''
  player.value = null
  match.value = null
  room.value = null
  result.value = null
  stage.value = 'login'
  lastSequence.value = -1
}

export type { SessionReadyPayload }
