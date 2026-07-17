import { defineStore } from 'pinia'
import { ref } from 'vue'

export interface DlpStatus {
  driverLoaded: boolean
  fileFilterActive: boolean
  networkFilterActive: boolean
  lastError: string | number | null
}

export interface DlpPolicy {
  fileFilterEnabled: boolean
  networkFilterEnabled: boolean
  auditEnabled: boolean
  fileActions: Record<string, string>
  networkActions: Record<string, string>
  processWhitelist: string[]
  processBlacklist: string[]
  blockedPorts: number[]
  blockedDomains: string[]
  blockedUrls: string[]
  blockedFtpCommands: string[]
  blockedFtpPaths: string[]
  blockedFtpContentPatterns: string[]
  blockedHttpHeaders: string[]
  blockedHttpTrailers: string[]
  blockedHttpBodyPatterns: string[]
  blockedJsonKeys: string[]
  blockedJsonPaths: string[]
  blockedJsonValues: string[]
  fileExtensions: string[]
}

export const useDlpStore = defineStore('dlp', () => {
  const status = ref<DlpStatus>({
    driverLoaded: false,
    fileFilterActive: false,
    networkFilterActive: false,
    lastError: null
  })
  const policy = ref<DlpPolicy>({
    fileFilterEnabled: true,
    networkFilterEnabled: true,
    auditEnabled: true,
    fileActions: { create: 'log', write: 'log', read: 'none', delete: 'log', rename: 'log' },
    networkActions: { http: 'log', https: 'log', ftp: 'log' },
    processWhitelist: [],
    processBlacklist: [],
    blockedPorts: [],
    blockedDomains: [],
    blockedUrls: [],
    blockedFtpCommands: [],
    blockedFtpPaths: [],
    blockedFtpContentPatterns: [],
    blockedHttpHeaders: [],
    blockedHttpTrailers: [],
    blockedHttpBodyPatterns: [],
    blockedJsonKeys: [],
    blockedJsonPaths: [],
    blockedJsonValues: [],
    fileExtensions: ['.exe', '.dll', '.docx', '.xlsx', '.pdf', '.zip']
  })
  const loading = ref(false)

  const statusText = ref('未连接')

  async function loadStatus() {
    loading.value = true
    try {
      const s = await window.saferAPI.dlp.getStatus()
      status.value = {
        driverLoaded: !!s?.driverLoaded,
        fileFilterActive: !!s?.fileFilterActive,
        networkFilterActive: !!s?.networkFilterActive,
        lastError: s?.lastError || null
      }
      updateStatusText()
    } catch (error: any) {
      status.value = {
        driverLoaded: false,
        fileFilterActive: false,
        networkFilterActive: false,
        lastError: error?.message || '无法读取驱动状态'
      }
      updateStatusText()
    } finally {
      loading.value = false
    }
  }

  async function loadPolicy() {
    const loaded = await window.saferAPI.dlp.getPolicy()
    applyLoadedPolicy(loaded)
  }

  function applyLoadedPolicy(loaded: any) {
    if (!loaded || typeof loaded !== 'object') return
    policy.value = {
      ...policy.value,
      ...loaded,
      fileActions: { ...policy.value.fileActions, ...(loaded.fileActions || {}) },
      networkActions: { ...policy.value.networkActions, ...(loaded.networkActions || {}) },
      processWhitelist: Array.isArray(loaded.processWhitelist) ? loaded.processWhitelist : [],
      processBlacklist: Array.isArray(loaded.processBlacklist) ? loaded.processBlacklist : [],
      blockedPorts: Array.isArray(loaded.blockedPorts) ? loaded.blockedPorts : [],
      blockedDomains: Array.isArray(loaded.blockedDomains) ? loaded.blockedDomains : [],
      blockedUrls: Array.isArray(loaded.blockedUrls) ? loaded.blockedUrls : [],
      blockedFtpCommands: Array.isArray(loaded.blockedFtpCommands) ? loaded.blockedFtpCommands : [],
      blockedFtpPaths: Array.isArray(loaded.blockedFtpPaths) ? loaded.blockedFtpPaths : [],
      blockedFtpContentPatterns: Array.isArray(loaded.blockedFtpContentPatterns) ? loaded.blockedFtpContentPatterns : [],
      blockedHttpHeaders: Array.isArray(loaded.blockedHttpHeaders) ? loaded.blockedHttpHeaders : [],
      blockedHttpTrailers: Array.isArray(loaded.blockedHttpTrailers) ? loaded.blockedHttpTrailers : [],
      blockedHttpBodyPatterns: Array.isArray(loaded.blockedHttpBodyPatterns) ? loaded.blockedHttpBodyPatterns : [],
      blockedJsonKeys: Array.isArray(loaded.blockedJsonKeys) ? loaded.blockedJsonKeys : [],
      blockedJsonPaths: Array.isArray(loaded.blockedJsonPaths) ? loaded.blockedJsonPaths : [],
      blockedJsonValues: Array.isArray(loaded.blockedJsonValues) ? loaded.blockedJsonValues : [],
      fileExtensions: Array.isArray(loaded.fileExtensions) ? loaded.fileExtensions : policy.value.fileExtensions
    }
  }

  // 定时刷新驱动状态：即使驱动是被外部（如 fltmc）加载/卸载的，UI 也能实时反映
  let pollTimer: ReturnType<typeof setInterval> | null = null
  function startAutoRefresh(intervalMs = 3000) {
    if (pollTimer) return
    void loadStatus()
    pollTimer = setInterval(() => { void loadStatus() }, intervalMs)
  }
  function stopAutoRefresh() {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null }
  }

  async function loadDriver() {
    loading.value = true
    try {
      const result = await window.saferAPI.dlp.loadDriver()
      if (result.success) {
        await loadStatus()
      }
      return result
    } finally {
      loading.value = false
    }
  }

  async function unloadDriver() {
    await window.saferAPI.dlp.unloadDriver()
    await loadStatus()
  }

  async function savePolicy(newPolicy: DlpPolicy) {
    // Pinia makes nested policy collections reactive proxies. Electron IPC
    // cannot structured-clone those proxies, so construct a plain DTO here.
    const policyDto: DlpPolicy = {
      fileFilterEnabled: newPolicy.fileFilterEnabled !== false,
      networkFilterEnabled: newPolicy.networkFilterEnabled !== false,
      auditEnabled: newPolicy.auditEnabled !== false,
      fileActions: { ...newPolicy.fileActions },
      networkActions: { ...newPolicy.networkActions },
      processWhitelist: [...newPolicy.processWhitelist],
      processBlacklist: [...newPolicy.processBlacklist],
      blockedPorts: [...newPolicy.blockedPorts],
      blockedDomains: [...newPolicy.blockedDomains],
      blockedUrls: [...newPolicy.blockedUrls],
      blockedFtpCommands: [...newPolicy.blockedFtpCommands],
      blockedFtpPaths: [...newPolicy.blockedFtpPaths],
      blockedFtpContentPatterns: [...newPolicy.blockedFtpContentPatterns],
      blockedHttpHeaders: [...newPolicy.blockedHttpHeaders],
      blockedHttpTrailers: [...newPolicy.blockedHttpTrailers],
      blockedHttpBodyPatterns: [...newPolicy.blockedHttpBodyPatterns],
      blockedJsonKeys: [...newPolicy.blockedJsonKeys],
      blockedJsonPaths: [...newPolicy.blockedJsonPaths],
      blockedJsonValues: [...newPolicy.blockedJsonValues],
      fileExtensions: [...newPolicy.fileExtensions],
    }
    const result = await window.saferAPI.dlp.setPolicy(policyDto)
    if (result?.success) {
      applyLoadedPolicy(result.policy || policyDto)
    }
    return result
  }

  function updateStatusText() {
    if (!status.value.driverLoaded) {
      statusText.value = '驱动未加载'
    } else if (status.value.lastError) {
      statusText.value = `错误: ${status.value.lastError}`
    } else if (status.value.fileFilterActive && status.value.networkFilterActive) {
      statusText.value = '全部防护中'
    } else if (status.value.fileFilterActive) {
      statusText.value = '文件防护中'
    } else if (status.value.networkFilterActive) {
      statusText.value = '网络防护中'
    } else {
      statusText.value = '防护已停止'
    }
  }

  return {
    status, policy, loading, statusText,
    loadStatus, loadPolicy, loadDriver, unloadDriver, savePolicy,
    startAutoRefresh, stopAutoRefresh
  }
})
