<script setup lang="ts">
import { ref } from 'vue'

import { useGameSession } from '../composables/useGameSession'

const session = useGameSession()

// 预填测试身份。种子数据里有 alice / bob / carol(disabled) / dave，
// 双人对局需要两个**启用**身份，因此默认填 alice 与 bob 各自的密码。
// 演示需要开两个浏览器窗口分别登录这两个账号。
const account = ref('alice')
const password = ref('alice_dev_pw')

async function submit(): Promise<void> {
  await session.signIn(account.value.trim(), password.value)
}

/** 一键切换到另一个测试身份，省掉演示时手输密码。 */
function useIdentity(next: 'alice' | 'bob' | 'dave'): void {
  account.value = next
  password.value = `${next}_dev_pw`
}
</script>

<template>
  <section class="panel">
    <h2>登录</h2>

    <div v-if="session.errorText.value" class="banner err">{{ session.errorText.value }}</div>

    <form @submit.prevent="submit">
      <label class="field">
        <span>账号</span>
        <input v-model="account" autocomplete="username" />
      </label>
      <label class="field">
        <span>密码</span>
        <input v-model="password" type="password" autocomplete="current-password" />
      </label>
      <div class="row">
        <button class="primary" type="submit">登录</button>
        <button type="button" @click="useIdentity('alice')">填 alice</button>
        <button type="button" @click="useIdentity('bob')">填 bob</button>
        <button type="button" @click="useIdentity('dave')">填 dave</button>
      </div>
    </form>

    <p class="hint">
      演示需要在<b>两个浏览器窗口</b>里分别登录 alice 与 bob，然后各自点「开始匹配」。
      密码由种子数据提供，见 <span class="mono">migrations/004_seed_test_players.sql</span>。
      页面通过 Vite 开发服务器代理访问 Gateway，因此不需要任何 CORS 配置。
    </p>
  </section>
</template>
