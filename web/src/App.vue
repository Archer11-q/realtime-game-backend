<script setup lang="ts">
import { computed } from 'vue'

import BattleView from './views/BattleView.vue'
import LobbyView from './views/LobbyView.vue'
import LoginView from './views/LoginView.vue'
import ResultView from './views/ResultView.vue'
import { kMaxReconnectAttempts } from './api/stream'
import { useGameSession } from './composables/useGameSession'

const session = useGameSession()

// 四个视图用一个 computed 切换，不用 vue-router。
// 理由：这是单页演示，四个阶段是**同一个会话的状态**，不是可独立寻址的页面。
// 用路由会引入 history 管理、路由守卫与「刷新后落到哪个页面」这些无收益的问题。
const currentView = computed(() => {
  switch (session.stage.value) {
    case 'lobby':
      return LobbyView
    case 'battle':
      return BattleView
    case 'result':
      return ResultView
    case 'login':
    default:
      return LoginView
  }
})

const playerLabel = computed(() => {
  const profile = session.player.value
  if (profile === null) {
    return ''
  }
  // PlayerInfo 里没有 account 字段（见 api/proto/gateway.proto），
  // 因此只可能用 display_name 兜底到 player_id。
  return `${profile.display_name ?? '玩家'}（${profile.player_id ?? '?'}）`
})

/**
 * 顶栏的推送状态文案。
 *
 * 三种情况必须分开写，不能都叫「推送已断开」：
 * 「重连中（第 2/5 次）」玩家什么都不用做，而「已断开放弃」意味着
 * 30 秒宽限期已经用完、这一局按判负处理——玩家必须知道，也有重试按钮。
 */
const pushLabel = computed(() => {
  switch (session.streamStatus.value) {
    case 'connected':
      return '推送已连接'
    case 'connecting':
      return '推送连接中…'
    case 'reconnecting':
      return `推送重连中（第 ${session.streamAttempt.value}/${kMaxReconnectAttempts} 次）`
    case 'exhausted':
      return '推送已断开放弃'
    case 'stopped':
    default:
      return '推送已断开'
  }
})

/** 顶栏徽标的配色档位：live（正常） / warn（还会自己好） / down（不会了）。 */
const pushTone = computed(() => {
  switch (session.streamStatus.value) {
    case 'connected':
      return 'live'
    case 'connecting':
    case 'reconnecting':
      return 'warn'
    case 'exhausted':
      return 'down'
    case 'stopped':
    default:
      return 'down'
  }
})
</script>

<template>
  <header class="app-header">
    <div>
      <h1>Realtime Game Backend 演示</h1>
      <div class="sub">Gateway · Match · Room/Battle ｜ 推送走 SSE（ADR-0004）</div>
    </div>
    <div class="row">
      <span v-if="playerLabel !== ''" class="badge">{{ playerLabel }}</span>
      <span v-if="session.stage.value === 'battle'" class="badge" :class="pushTone">
        {{ pushLabel }}
      </span>
      <button
        v-if="session.stage.value === 'battle' && session.streamStatus.value === 'exhausted'"
        @click="session.retryStream()"
      >
        重试连接
      </button>
      <button v-if="session.stage.value !== 'login'" @click="session.signOut()">退出登录</button>
    </div>
  </header>

  <component :is="currentView" />
</template>
