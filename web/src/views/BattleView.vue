<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'

import BattleCanvas from '../components/BattleCanvas.vue'
import { kMaxReconnectAttempts } from '../api/stream'
import { useGameSession } from '../composables/useGameSession'

const session = useGameSession()

const lastPushedAt = ref(0)
const now = ref(Date.now())
let ticker: ReturnType<typeof setInterval> | undefined

onMounted(() => {
  // 只用于显示「距上次推送多久」。这个计时器不产生任何网络请求，
  // 推送本身的到达时刻完全由 SSE 决定。
  ticker = setInterval(() => {
    now.value = Date.now()
  }, 500)
})

onBeforeUnmount(() => {
  if (ticker !== undefined) {
    clearInterval(ticker)
  }
})

// 序号变化 = 收到了新推送。用 sequence 而不是 payload 对象，
// 因为同一帧的 payload 内容可能完全相同，引用却不同，会导致误判。
watch(
  () => session.lastSequence.value,
  (sequence) => {
    if (sequence >= 0) {
      lastPushedAt.value = Date.now()
    }
  },
)

const roomState = computed(() => session.room.value?.state ?? 'waiting')
const frame = computed(() => session.room.value?.frame ?? 0)
const selfHp = computed(() => session.selfPlayer.value?.hp ?? 0)
const opponentHp = computed(() => session.opponent.value?.hp ?? 0)

/** 双方都已进房后才允许提交攻击：Room 在 WAITING 阶段会拒绝输入。 */
const canAttack = computed(() => roomState.value === 'playing')

const secondsSincePush = computed(() => {
  if (lastPushedAt.value === 0) {
    return -1
  }
  return Math.floor((now.value - lastPushedAt.value) / 1000)
})

/** 把房间状态翻成中文。未知取值原样显示，避免界面把新状态说成「未知」。 */
function stateLabel(state: string): string {
  switch (state) {
    case 'created':
      return '已创建'
    case 'waiting':
      return '等待对手加入'
    case 'playing':
      return '对战中'
    case 'finishing':
      return '结算写入中'
    case 'finished':
      return '已结束'
    case 'aborted':
      return '已废弃（无人加入）'
    default:
      return state
  }
}

/**
 * 推送连接状态文案。
 *
 * 这里刻意不写成「在线/离线」两个词：对局中「重连中（第 N/5 次）」和
 * 「已断开放弃」对玩家的含义完全不同——前者只要等，后者意味着后端 30 秒
 * 宽限期已经走完、这一局按判负处理，必须立刻让玩家看见并能手动重试。
 */
const pushLabel = computed(() => {
  switch (session.streamStatus.value) {
    case 'connected':
      return '已连接'
    case 'connecting':
      return '连接中'
    case 'reconnecting':
      return `重连中（第 ${session.streamAttempt.value}/${kMaxReconnectAttempts} 次）`
    case 'exhausted':
      return '已断开放弃'
    case 'stopped':
    default:
      return '未订阅'
  }
})

const pushTone = computed(() => {
  switch (session.streamStatus.value) {
    case 'connected':
      return 'live'
    case 'connecting':
    case 'reconnecting':
      return 'warn'
    case 'exhausted':
    case 'stopped':
    default:
      return 'down'
  }
})
</script>

<template>
  <section class="panel">
    <h2>对战</h2>

    <div v-if="session.errorText.value" class="banner err">{{ session.errorText.value }}</div>

    <div v-if="session.streamStatus.value === 'exhausted'" class="banner err">
      推送已断开放弃：已经连续重试 {{ kMaxReconnectAttempts }} 次仍未连上。后端宽限期是
      30 秒，超期会判断线方负——请确认 Gateway 是否还在运行，然后点「重试连接」。
    </div>
    <div v-else-if="session.streamStatus.value === 'reconnecting'" class="banner warn">
      推送中断，正在自动重连（第 {{ session.streamAttempt.value }}/{{ kMaxReconnectAttempts }} 次）。
      后端在断线后保留 30 秒宽限期，期内对局暂停推进；连上就会接着打。
    </div>

    <dl class="kv">
      <dt>房间号</dt>
      <dd class="mono">{{ session.room.value?.room_id ?? '—' }}</dd>
      <dt>对局号</dt>
      <dd class="mono">{{ session.room.value?.match_id ?? '—' }}</dd>
      <dt>状态</dt>
      <dd>{{ stateLabel(roomState) }}</dd>
      <dt>推送连接</dt>
      <dd>
        <span class="badge" :class="pushTone">{{ pushLabel }}</span>
        <button
          v-if="session.streamStatus.value === 'exhausted'"
          style="margin-left: 8px"
          @click="session.retryStream()"
        >
          重试连接
        </button>
      </dd>
      <dt>服务端帧号</dt>
      <dd>{{ frame }}</dd>
      <dt>最近推送序号</dt>
      <dd>
        {{ session.lastSequence.value }}
        <span v-if="secondsSincePush >= 0" class="badge">{{ secondsSincePush }}s 前</span>
      </dd>
    </dl>

    <div class="row" style="margin-top: 14px">
      <span class="badge">
        我方 {{ session.selfPlayer.value?.player_id ?? '?' }} HP {{ selfHp }}
      </span>
      <span class="badge">
        对手 {{ session.opponent.value?.player_id ?? '?' }} HP {{ opponentHp }}
      </span>
    </div>
  </section>

  <section class="panel">
    <BattleCanvas />
    <div class="row" style="margin-top: 14px">
      <button class="primary" :disabled="!canAttack" @click="session.attack()">攻击</button>
      <span v-if="!canAttack" class="hint">
        对局尚未开始（当前 {{ stateLabel(roomState) }}），服务端此时不接受输入。
      </span>
      <span v-else class="hint">
        每帧（100 ms）最多结算一次攻击，每次扣对手 10 点。连点不会更快。
      </span>
    </div>
  </section>
</template>
