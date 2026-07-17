import { defineStore } from 'pinia'
import { ref } from 'vue'

export interface AuditLog {
  id: number
  type: string
  action: string
  processName: string
  pid: number
  timestamp: string
  details: string
}

export interface AuditStats {
  totalEvents: number
  fileEvents: number
  networkEvents: number
  registryEvents: number
  blockedEvents: number
}

export const useAuditStore = defineStore('audit', () => {
  const logs = ref<AuditLog[]>([])
  const stats = ref<AuditStats>({
    totalEvents: 0,
    fileEvents: 0,
    networkEvents: 0,
    registryEvents: 0,
    blockedEvents: 0,
  })
  const loading = ref(false)

  async function loadLogs(filters?: { limit?: number; type?: string }, silent = false) {
    if (!silent) loading.value = true
    try {
      logs.value = await window.saferAPI.audit.getLogs(filters)
    } finally {
      if (!silent) loading.value = false
    }
  }

  async function loadStats() {
    stats.value = await window.saferAPI.audit.getStats()
  }

  async function clearLogs() {
    await window.saferAPI.audit.clearLogs()
    logs.value = []
  }

  function getTypeLabel(type: string): string {
    const labels: Record<string, string> = {
      file_create: 'File Create',
      file_write: 'File Write',
      file_delete: 'File Delete',
      file_rename: 'File Rename',
      network_connect: 'Network Connect',
      network_request: 'Network Request',
      http_request: 'HTTP Request',
      http_response: 'HTTP Response',
      ftp_command: 'FTP Command',
      sni_capture: 'HTTPS SNI',
      websocket_frame: 'WebSocket Frame',
      registry_change: 'Registry Change',
    }
    return labels[type] || type
  }

  function getActionBadge(action: string): string {
    const badges: Record<string, string> = {
      allowed: 'Allowed',
      blocked: 'Blocked',
      logged: 'Logged',
    }
    return badges[action] || action
  }

  return {
    logs,
    stats,
    loading,
    loadLogs,
    loadStats,
    clearLogs,
    getTypeLabel,
    getActionBadge,
  }
})
