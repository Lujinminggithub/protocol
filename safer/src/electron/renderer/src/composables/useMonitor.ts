import { onMounted, onUnmounted } from 'vue'
import { useMonitorStore } from '@/stores/monitor'

export function useMonitor() {
  const store = useMonitorStore()
  let timer: ReturnType<typeof setInterval> | null = null

  function startMonitoring(intervalMs: number = 2000) {
    if (timer) return

    store.refreshAll()
    timer = setInterval(() => {
      store.refreshAll()
    }, intervalMs)
  }

  function stopMonitoring() {
    if (timer) {
      clearInterval(timer)
      timer = null
    }
  }

  onUnmounted(() => {
    stopMonitoring()
  })

  return { store, startMonitoring, stopMonitoring }
}
