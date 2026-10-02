<script setup lang="ts">
import { computed } from 'vue'

import BattleView from './views/BattleView.vue'
import LobbyView from './views/LobbyView.vue'
import LoginView from './views/LoginView.vue'
import ResultView from './views/ResultView.vue'
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
</script>

<template>
  <header class="app-header">
    <div>
      <h1>Realtime Game Backend 演示</h1>
      <div class="sub">Gateway · Match · Room/Battle ｜ 推送走 SSE（ADR-0004）</div>
    </div>
    <div class="row">
      <span v-if="playerLabel !== ''" class="badge">{{ playerLabel }}</span>
      <span
        v-if="session.stage.value === 'battle'"
        class="badge"
        :class="session.streamConnected.value ? 'live' : 'down'"
      >
        {{ session.streamConnected.value ? '推送已连接' : '推送已断开' }}
      </span>
      <button v-if="session.stage.value !== 'login'" @click="session.signOut()">退出登录</button>
    </div>
  </header>

  <component :is="currentView" />
</template>
