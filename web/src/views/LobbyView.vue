<script setup lang="ts">
import { computed } from 'vue'

import { useGameSession } from '../composables/useGameSession'

const session = useGameSession()

const matchState = computed(() => session.match.value?.state ?? 'idle')
const isQueued = computed(() => matchState.value === 'queued')

/** 把 Match 状态翻成中文。未知取值原样显示，避免界面把新状态说成「未知」。 */
const stateLabel = computed(() => {
  switch (matchState.value) {
    case 'idle':
      return '未排队'
    case 'queued':
      return '排队中'
    case 'matched':
      return '已配对'
    case 'timeout':
      return '排队超时'
    default:
      return matchState.value
  }
})
</script>

<template>
  <section class="panel">
    <h2>大厅</h2>

    <div v-if="session.errorText.value" class="banner err">{{ session.errorText.value }}</div>
    <div v-if="session.notice.value" class="banner warn">{{ session.notice.value }}</div>

    <dl class="kv">
      <dt>匹配状态</dt>
      <dd>{{ stateLabel }}</dd>
      <dt>当前队列长度</dt>
      <dd>{{ session.match.value?.queue_size ?? 0 }}</dd>
      <dt>同局玩家</dt>
      <dd>
        <template v-if="(session.match.value?.player_ids ?? []).length > 0">
          {{ (session.match.value?.player_ids ?? []).join('、') }}
        </template>
        <template v-else>—</template>
      </dd>
    </dl>

    <div class="row" style="margin-top: 16px">
      <button v-if="!isQueued" class="primary" @click="session.startMatching()">
        开始匹配
      </button>
      <button v-else class="danger" @click="session.cancelMatching()">取消匹配</button>
    </div>

    <p class="hint">
      两人配对成功后会<strong>自动进房</strong>并订阅 SSE 推送。
      需要在另一个窗口里用另一个账号同时点「开始匹配」，配对才会发生。
      这个界面靠轮询 <span class="mono">GET /api/v1/matches/current</span> 感知配对结果——
      匹配状态变化频率低，改推送需要服务端为每个在线连接轮询 Match，不划算
      （见 <span class="mono">docs/05-api-and-data.md</span> 第 2 节）。
    </p>
  </section>

  <section v-if="isQueued" class="panel">
    <div class="banner info">正在等待另一位玩家…请确认另一个窗口也已经点了「开始匹配」。</div>
  </section>
</template>
