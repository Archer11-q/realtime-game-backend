import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发服务器把 /api 代理到 Gateway，浏览器侧因此是**同源**请求：
//   * 项目里没有任何 CORS 中间件，也不需要引入——同源就不存在预检
//   * 普通 HTTP 与 SSE 走同一条代理规则，前端不需要区分主机与端口
//
// 目标端口可用 RGBT_GATEWAY_PORT 覆盖，默认 8080（Gateway 的约定端口）。
// 端口冲突时改环境变量即可，不必改这个文件——理由与 .env.example 里写的一致。
const gatewayPort = process.env.RGBT_GATEWAY_PORT ?? '8080'

export default defineConfig({
  plugins: [vue()],
  server: {
    host: '127.0.0.1',
    port: Number(process.env.RGBT_WEB_PORT ?? 5173),
    strictPort: true,
    proxy: {
      '/api': {
        target: `http://127.0.0.1:${gatewayPort}`,
        changeOrigin: true,
        // 这里刻意**不**改写响应头。SSE 需要的 Cache-Control / X-Accel-Buffering
        // 由 Gateway 自己在响应里设置（见 src/gateway/gateway_service.cpp）。
        // 在代理再写一遍等于把同一份缓存策略维护在两个地方，
        // 任何一处改了另一处就会成为误导。
      },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
})
