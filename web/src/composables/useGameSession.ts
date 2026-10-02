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
import { decodeEnvelope, streamRoom } from '../api/stream'
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
const streamConnected = ref(false)
const lastSequence = ref(-1)

let matchPollTimer: ReturnType<typeof setTimeout> | undefined
let streamAbort: AbortController | undefined

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
  if (streamAbort !== undefined) {
    streamAbort.abort()
    streamAbort = undefined
  }
  streamConnected.value = false
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

  function startStream(roomId: string): void {
    stopStream()
    const controller = new AbortController()
    streamAbort = controller
    lastSequence.value = -1

    void streamRoom({
      roomId,
      token: token.value,
      signal: controller.signal,
      onEvent: (event) => {
        if (event.event === 'session.ready') {
          streamConnected.value = true
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
        if (event.event === 'room.finished') {
          void finishBattle()
        }
      },
    })
      .then(() => {
        // 流结束：可能是对局结束（正常），也可能是服务端重启或网络断开。
        streamConnected.value = false
      })
      .catch((cause: unknown) => {
        streamConnected.value = false
        if (stage.value === 'battle') {
          errorText.value = describeFailure(cause)
        }
      })
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
    streamConnected,
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
