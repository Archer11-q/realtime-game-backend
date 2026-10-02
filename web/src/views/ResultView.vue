<script setup lang="ts">
import { computed } from 'vue'

import { useGameSession } from '../composables/useGameSession'

const session = useGameSession()

const selfHp = computed(() => session.selfPlayer.value?.hp ?? 0)
const opponentHp = computed(() => session.opponent.value?.hp ?? 0)

const winnerId = computed(
  () => session.room.value?.winner_id ?? session.result.value?.winner_id ?? '',
)

const outcome = computed(() => {
  if (winnerId.value === '') {
    // 空 winner_id 是**合法的平局**，不是「结果缺失」。
    // 两者必须能区分：TASK-008 的 finish_reason 说明了是打平还是时间到。
    return '平局'
  }
  return winnerId.value === session.selfPlayerId.value ? '你赢了' : '你输了'
})

const finishReasonLabel = computed(() => {
  switch (session.room.value?.finish_reason ?? '') {
    case 'hp_zero':
      return '一方血量归零'
    case 'timeout':
      return '到达最大帧数，按血量判定'
    case 'waiting_timeout':
      return '房间无人加入，已废弃'
    case 'none':
    case '':
      return '—'
    default:
      return session.room.value?.finish_reason ?? '—'
  }
})

/** 结果是否已经落库。未落库时界面必须说清楚，不能假装结果已经确定。 */
const persisted = computed(() => session.result.value !== null)

const durationText = computed(() => {
  const started = session.room.value?.started_at_ms ?? 0
  const finished = session.room.value?.finished_at_ms ?? 0
  if (started <= 0 || finished <= 0) {
    return '—'
  }
  return `${((finished - started) / 1000).toFixed(1)} 秒`
})

function reload(): void {
  // 重新查询结果。落库失败时 Room 会按 1 秒间隔重试，
  // 因此这里手动重试是有意义的，而不是无意义的刷新。
  const matchId = session.room.value?.match_id ?? ''
  if (matchId !== '') {
    void session.reloadResult(matchId)
  }
}
</script>

<template>
  <section class="panel">
    <h2>结算</h2>

    <div v-if="session.errorText.value" class="banner err">{{ session.errorText.value }}</div>
    <div v-if="session.resultPending.value" class="banner warn">
      对局已结束，但结果<strong>尚未写入数据库</strong>。Room 正在按 1 秒间隔重试
      （通常是 MySQL 不可用）。下面的胜负来自推送的权威快照，不是数据库读回来的。
    </div>

    <div class="banner info" style="font-size: 18px; font-weight: 600">{{ outcome }}</div>

    <dl class="kv">
      <dt>对局号</dt>
      <dd class="mono">{{ session.room.value?.match_id ?? '—' }}</dd>
      <dt>房间号</dt>
      <dd class="mono">{{ session.room.value?.room_id ?? '—' }}</dd>
      <dt>胜者</dt>
      <dd>{{ winnerId === '' ? '（平局，无胜者）' : winnerId }}</dd>
      <dt>结束原因</dt>
      <dd>{{ finishReasonLabel }}</dd>
      <dt>最终帧号</dt>
      <dd>{{ session.room.value?.frame ?? 0 }}</dd>
      <dt>对局时长</dt>
      <dd>{{ durationText }}</dd>
      <dt>我方 HP</dt>
      <dd>{{ selfHp }}</dd>
      <dt>对手 HP</dt>
      <dd>{{ opponentHp }}</dd>
      <dt>结果已落库</dt>
      <dd>
        <template v-if="persisted">
          是（<span class="mono">GET /api/v1/results</span> 返回 player_count =
          {{ session.result.value?.player_count ?? '?' }}）
        </template>
        <template v-else>否</template>
      </dd>
    </dl>

    <div class="row" style="margin-top: 16px">
      <button class="primary" @click="session.backToLobby()">回到大厅</button>
      <button :disabled="persisted" @click="reload">重新查询结果</button>
    </div>

    <p class="hint">
      胜负以 SSE 推送的 <span class="mono">room.finished</span> 载荷为准；
      「结果已落库」是另外一次 <span class="mono">GET /api/v1/results</span> 查询的结果。
      两者分开显示，是为了暴露「对局已结束但结果还没写进数据库」这个真实状态。
    </p>
  </section>
</template>
