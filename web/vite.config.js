import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'
import path from 'path'

// Dev server proxies /api (REST + SSE) to the gateway or to the mock:
//   LINKRA_HOST=192.168.1.50 npm run dev     real device
//   npm run dev:mock                         mock (web/mock/server.js on localhost:8787)
const target = `http://${process.env.LINKRA_HOST || 'linkra-duo.local'}`

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
    },
  },
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target, changeOrigin: true,
        // The gateway rejects a foreign Origin (CSRF rule): present the proxy as same-origin
        configure: (proxy) => proxy.on('proxyReq', (req) => { if (req.getHeader('origin')) req.setHeader('origin', target) }),
      },
    },
  },
  build: {
    outDir: '../data',
    emptyOutDir: true,
    minify: 'esbuild',
    modulePreload: { polyfill: false },
    rollupOptions: {
      output: {
        // uPlot and uqr are not listed: they are loaded with import() only where needed
        manualChunks: {
          'vendor': ['preact', 'preact/hooks', '@preact/signals'],
          'ui': ['lucide-preact'],
        },
        entryFileNames: 'assets/[name]-[hash].js',
        chunkFileNames: 'assets/[name]-[hash].js',
        // everything under assets/: the firmware only serves /assets/* (review 07 N10)
        assetFileNames: 'assets/[name]-[hash][extname]',
      }
    },
    reportCompressedSize: true,
    chunkSizeWarningLimit: 500
  }
})
