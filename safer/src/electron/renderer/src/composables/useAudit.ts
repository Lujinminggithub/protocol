import { onUnmounted } from 'vue'
import { useAuditStore } from '@/stores/audit'

export function useAudit() {
  const store = useAuditStore()
  let refreshTimer: ReturnType<typeof setInterval> | null = null
  let refreshInFlight = false

  async function refresh(): Promise<void> {
    if (refreshInFlight) return
    refreshInFlight = true
    try {
      await Promise.all([store.loadLogs(undefined, true), store.loadStats()])
    } finally {
      refreshInFlight = false
    }
  }

  async function init() {
    await Promise.all([store.loadLogs(), store.loadStats()])
    if (!refreshTimer) {
      refreshTimer = setInterval(() => { void refresh() }, 2000)
    }
  }

  function onEvent(callback: (event: any) => void) {
    window.saferAPI.audit.onEvent(callback)
  }

  onUnmounted(() => {
    if (refreshTimer) {
      clearInterval(refreshTimer)
      refreshTimer = null
    }
  })

  return { store, init, onEvent }
}
