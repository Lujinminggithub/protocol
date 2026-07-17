import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import electron from 'vite-plugin-electron'
import renderer from 'vite-plugin-electron-renderer'
import { resolve } from 'path'

export default defineConfig({
  root: 'renderer',
  plugins: [
    vue(),
    electron([
      {
        entry: '../main/index.ts',
        vite: {
          build: {
            outDir: '../dist-electron/main',
            rollupOptions: {
              external: ['electron']
            }
          }
        }
      },
      {
        entry: '../preload/index.ts',
        vite: {
          build: {
            outDir: '../dist-electron/preload'
          }
        }
      }
    ]),
    renderer()
  ],
  resolve: {
    alias: {
      '@': resolve(__dirname, 'renderer/src')
    }
  },
  build: {
    outDir: '../dist',
    emptyOutDir: true
  },
  base: './'
})
