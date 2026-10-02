<script setup lang="ts">
/// @file BattleCanvas.vue
/// @brief 用 Canvas 渲染对战画面：双方角色、血条、帧号与服务端状态。
///
/// 为什么用 Canvas 而不是 DOM：对局画面对应「每帧重绘」的形态，用 DOM 表达会变成
/// 「改动一堆元素的样式」，反而失去了服务端帧推进的直观性。Canvas 每收到一次
/// `room.state` 推送就整帧重绘一次，画面与服务端帧号严格对应——这也是演示的重点：
/// **画面不是本地动画，而是服务端权威快照的重放**。
///
/// 因此本组件不做本地位置补间：坐标与数值全部直接来自推送。
/// 唯一的本地效果是「刚刚被打」的短暂闪烁，它只影响颜色。

import { onBeforeUnmount, onMounted, ref, watch } from 'vue'

import { useGameSession } from '../composables/useGameSession'

const session = useGameSession()

const kWidth = 880
const kHeight = 260
const kGroundY = 190
const kMaxHp = 100
/** 命中闪烁持续时间（毫秒）。 */
const kFlashMs = 200

const canvas = ref<HTMLCanvasElement | null>(null)

/** 上一次看到的血量，用来判断「刚刚被打」。初值 -1 表示还没有基准。 */
let lastSelfHp = -1
let lastOpponentHp = -1
/** 闪烁持续到哪个时刻（performance.now 的时间轴）。 */
let flashUntil = 0
let frameHandle: number | undefined

interface FighterOptions {
  x: number
  hp: number
  label: string
  flashing: boolean
  facing: 1 | -1
  isSelf: boolean
}

function drawFighter(context: CanvasRenderingContext2D, options: FighterOptions): void {
  const { x, hp, label, flashing, facing, isSelf } = options
  const bodyWidth = 54
  const bodyHeight = 86
  const top = kGroundY - bodyHeight
  const bodyColor = flashing ? '#ffffff' : isSelf ? '#4c8dff' : '#ff6b6b'

  context.fillStyle = bodyColor
  context.beginPath()
  context.roundRect(x - bodyWidth / 2, top, bodyWidth, bodyHeight, 10)
  context.fill()

  // 朝向指示：向对手方向伸出的小三角，让「谁面向谁」一眼可见。
  context.beginPath()
  context.moveTo(x + (facing * bodyWidth) / 2, top + 24)
  context.lineTo(x + (facing * (bodyWidth + 26)) / 2, top + 34)
  context.lineTo(x + (facing * bodyWidth) / 2, top + 44)
  context.closePath()
  context.fill()

  // 血条
  const barWidth = 150
  const barHeight = 12
  const barX = x - barWidth / 2
  const barY = top - 34
  const ratio = Math.max(0, Math.min(1, hp / kMaxHp))

  context.fillStyle = '#0b1120'
  context.fillRect(barX, barY, barWidth, barHeight)
  context.fillStyle = ratio > 0.5 ? '#3ddc97' : ratio > 0.2 ? '#ffb454' : '#ff6b6b'
  context.fillRect(barX, barY, barWidth * ratio, barHeight)
  context.strokeStyle = '#2c3a57'
  context.lineWidth = 1
  context.strokeRect(barX + 0.5, barY + 0.5, barWidth - 1, barHeight - 1)

  context.textAlign = 'center'
  context.fillStyle = '#e6ecf7'
  context.font = '13px system-ui, sans-serif'
  context.fillText(label, x, barY - 8)
  context.fillStyle = '#93a1bd'
  context.font = '12px ui-monospace, Consolas, monospace'
  context.fillText(`${hp} / ${kMaxHp}`, x, barY + barHeight + 16)
}

function draw(now: number): void {
  const element = canvas.value
  if (element === null) {
    return
  }
  const context = element.getContext('2d')
  if (context === null) {
    return
  }

  const selfHp = session.selfPlayer.value?.hp ?? kMaxHp
  const opponentHp = session.opponent.value?.hp ?? kMaxHp
  const state = session.room.value?.state ?? 'waiting'
  const frame = session.room.value?.frame ?? 0

  // 只在血量真的下降时触发闪烁。因此「单纯重绘」不会自己制造出命中效果。
  let hit = false
  if (lastSelfHp >= 0 && selfHp < lastSelfHp) {
    hit = true
  }
  if (lastOpponentHp >= 0 && opponentHp < lastOpponentHp) {
    hit = true
  }
  if (hit) {
    flashUntil = now + kFlashMs
  }
  lastSelfHp = selfHp
  lastOpponentHp = opponentHp

  const flashing = now < flashUntil

  context.fillStyle = '#131a29'
  context.fillRect(0, 0, kWidth, kHeight)

  context.strokeStyle = '#2c3a57'
  context.lineWidth = 2
  context.beginPath()
  context.moveTo(0, kGroundY)
  context.lineTo(kWidth, kGroundY)
  context.stroke()

  drawFighter(context, {
    x: 220,
    hp: selfHp,
    label: session.selfPlayer.value?.player_id ?? '我方',
    flashing,
    facing: 1,
    isSelf: true,
  })
  drawFighter(context, {
    x: kWidth - 220,
    hp: opponentHp,
    label: session.opponent.value?.player_id ?? '对手',
    flashing,
    facing: -1,
    isSelf: false,
  })

  context.fillStyle = '#93a1bd'
  context.font = '13px ui-monospace, Consolas, monospace'
  context.textAlign = 'left'
  context.fillText(`frame ${frame}`, 16, 26)
  context.textAlign = 'right'
  context.fillText(`state ${state}`, kWidth - 16, 26)

  context.textAlign = 'center'
  context.fillStyle = '#5c6b8a'
  context.fillText(
    session.lastSequence.value >= 0
      ? `最后一次服务端推送 sequence=${session.lastSequence.value}`
      : '等待服务端推送…',
    kWidth / 2,
    kHeight - 14,
  )
}

/// 重绘一次；如果正处于闪烁窗口，则继续下一帧直到闪烁结束。
function renderLoop(): void {
  const now = performance.now()
  draw(now)
  if (now < flashUntil) {
    frameHandle = requestAnimationFrame(renderLoop)
  } else {
    frameHandle = undefined
  }
}

/// 请求一次重绘。已在循环中时不重复登记，避免叠加多个 rAF 回调。
function requestRender(): void {
  if (frameHandle === undefined) {
    frameHandle = requestAnimationFrame(renderLoop)
  }
}

onMounted(() => {
  requestRender()
})

onBeforeUnmount(() => {
  if (frameHandle !== undefined) {
    cancelAnimationFrame(frameHandle)
    frameHandle = undefined
  }
})

// 每收到一次推送就重绘一次。这是画面与服务端帧号严格对应的原因。
watch(
  () => session.lastSequence.value,
  () => {
    requestRender()
  },
)
</script>

<template>
  <canvas
    ref="canvas"
    :width="kWidth"
    :height="kHeight"
    style="width: 100%; border-radius: 8px; display: block"
  />
</template>
