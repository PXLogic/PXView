import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// Tauri integration:
// - fixed port 3000 (must match tauri.conf.json devUrl)
// - strictPort so Vite never silently changes the port
// - clearScreen false so Tauri CLI output is preserved
// - HMR via WebSocket protocol for Tauri webview compatibility
const host = process.env.TAURI_DEV_HOST;

export default defineConfig({
  plugins: [react(), tailwindcss()],
  // Tauri uses a custom protocol in production; relative base is safest.
  base: './',
  server: {
    port: 3000,
    strictPort: true,
    host: host || '127.0.0.1',
    hmr: host
      ? { protocol: 'ws', host, port: 3001 }
      : undefined,
    clearScreen: false,
    // Ignore Rust build artifacts to prevent EBUSY errors during cargo compilation
    watch: {
      ignored: ['**/src-tauri/target/**'],
    },
  },
  // Suppress large chunk warnings — the agent UI is a single-page app
  build: {
    chunkSizeWarningLimit: 1500,
  },
})
