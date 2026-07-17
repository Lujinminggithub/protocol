import { onUnmounted } from 'vue'
import { useDlpStore } from '@/stores/dlp'

export function useDlp() {
  const store = useDlpStore()

  async function init() {
    await Promise.all([store.loadStatus(), store.loadPolicy()])
    // 启动定时刷新，外部加载/卸载驱动也能实时反映到界面
    store.startAutoRefresh(3000)
  }

  // 监听 DLP 告警
  function onAlert(callback: (alert: any) => void) {
    window.saferAPI.dlp.onAlert(callback)
  }

  onUnmounted(() => {
    store.stopAutoRefresh()
  })

  return { store, init, onAlert }
}
