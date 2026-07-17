import { defineStore } from 'pinia'
import { ref } from 'vue'

export interface CpuData {
  total: number
  perCore: number[]
}

export interface MemoryData {
  physTotal: number
  physUsed: number
  physPercent: number
  virtualTotal: number
  virtualUsed: number
  pageTotal: number
  pageUsed: number
}

export interface NetworkData {
  interfaces: Array<{
    name: string
    displayName: string
    inBytesPerSec: number
    outBytesPerSec: number
    tcpConnections: {
      established: number
      timeWait: number
      closeWait: number
      synSent: number
      listen: number
    }
  }>
}

export interface DiskData {
  drives: Array<{
    name: string
    displayName: string
    readBytesPerSec: number
    writeBytesPerSec: number
    readOpsPerSec: number
    writeOpsPerSec: number
    queueDepth: number
  }>
}

// 历史数据存储
interface HistoryPoint {
  time: number
  value: number
}

export const useMonitorStore = defineStore('monitor', () => {
  const cpuData = ref<CpuData>({ total: 0, perCore: [] })
  const memoryData = ref<MemoryData>({
    physTotal: 0, physUsed: 0, physPercent: 0,
    virtualTotal: 0, virtualUsed: 0,
    pageTotal: 0, pageUsed: 0
  })
  const networkData = ref<NetworkData>({ interfaces: [] })
  const diskData = ref<DiskData>({ drives: [] })

  // 历史数据（用于图表）
  const cpuHistory = ref<HistoryPoint[]>([])
  const memoryHistory = ref<HistoryPoint[]>([])
  const networkRxHistory = ref<HistoryPoint[]>([])
  const networkTxHistory = ref<HistoryPoint[]>([])
  const diskReadHistory = ref<HistoryPoint[]>([])
  const diskWriteHistory = ref<HistoryPoint[]>([])

  const loading = ref(false)
  const lastUpdate = ref<Date | null>(null)

  async function refreshAll() {
    loading.value = true
    try {
      const [cpu, memory, network, disk] = await Promise.all([
        window.saferAPI.monitor.getCpu(),
        window.saferAPI.monitor.getMemory(),
        window.saferAPI.monitor.getNetwork(),
        window.saferAPI.monitor.getDisk()
      ])
      cpuData.value = cpu
      memoryData.value = memory
      networkData.value = network
      diskData.value = disk
      lastUpdate.value = new Date()

      // 更新历史数据
      const now = Date.now()
      cpuHistory.value.push({ time: now, value: cpu.total })
      memoryHistory.value.push({ time: now, value: memory.physPercent })

      // 保留最近60个点
      if (cpuHistory.value.length > 60) cpuHistory.value.shift()
      if (memoryHistory.value.length > 60) memoryHistory.value.shift()
      if (networkRxHistory.value.length > 60) networkRxHistory.value.shift()
      if (networkTxHistory.value.length > 60) networkTxHistory.value.shift()
      if (diskReadHistory.value.length > 60) diskReadHistory.value.shift()
      if (diskWriteHistory.value.length > 60) diskWriteHistory.value.shift()
    } finally {
      loading.value = false
    }
  }

  function formatBytes(bytes: number): string {
    if (bytes === 0) return '0 B'
    const k = 1024
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
    const i = Math.floor(Math.log(bytes) / Math.log(k))
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i]
  }

  function formatSpeed(bytesPerSec: number): string {
    return formatBytes(bytesPerSec) + '/s'
  }

  return {
    cpuData, memoryData, networkData, diskData,
    cpuHistory, memoryHistory, networkRxHistory, networkTxHistory,
    diskReadHistory, diskWriteHistory,
    loading, lastUpdate,
    refreshAll, formatBytes, formatSpeed
  }
})
