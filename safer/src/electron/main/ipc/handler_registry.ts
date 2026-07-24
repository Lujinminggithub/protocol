import { app, ipcMain } from 'electron'
import { mkdirSync } from 'node:fs'
import { join } from 'node:path'
import {
  clearDeliveryBackpressure,
  clearCompletedCheckpoints,
  exportDeliveryReport,
  getDeliveredQueueInfo,
  getDeliverySinkConfig,
  getDeliveryWorkerStatus,
  listArchivedCheckpoints,
  listDeliveryCheckpoints,
  listDeadLetterQueue,
  listDeadLetterRecords,
  listDeliveredQueue,
  listDeliveredRecords,
  listPipelineHistory,
  setDeliverySinkConfig,
  startDeliveryWorker,
  stopDeliveryWorker,
} from '../modules/delivery_worker'
import {
  clearDeliveryTestSinkRecords,
  getDeliveryTestSinkStatus,
  startDeliveryTestSink,
  stopDeliveryTestSink,
} from '../modules/delivery_test_sink'
import {
  getCpuData,
  getDiskData,
  getMemoryData,
  getNetworkData,
} from '../modules/monitor_bridge'
import {
  clearEvents,
  getEventPersistenceInfo,
  getEvents,
  getStats,
  isPolling,
  leaseQueuedEvents,
  markQueuedEventFailed,
  markQueuedEventSent,
  requeueAbandonedEvents,
  startEventPolling,
  stopEventPolling,
} from '../modules/dlp_events'
import { getAddon, isAddonLoaded } from '../modules/native_loader'
import {
  getLocalProxyStatus,
  startLocalProxy,
  stopLocalProxy,
  updateLocalProxyPolicy,
} from '../modules/local_proxy'
import { getQuarantineCatalogInfo, listQuarantineCatalog, purgeQuarantineEntry, restoreQuarantineEntry } from '../modules/quarantine_catalog'
import {
  applyPolicyToKernel,
  getPersistedPolicy,
  getPolicyStoreInfo,
  persistPolicy,
  transformNativePolicy,
} from '../modules/policy_store'
import {
  getAuditLogsMock,
  getAuditStatsMock,
  getCpuMockData,
  getDefaultPolicy,
  getDiskMockData,
  getMemoryMockData,
  getNetworkMockData,
} from '../modules/mock_data'

declare global {
  interface BrowserWindow {
    _isQuitting?: boolean
  }
}

function getDriverSysPath(): string {
  if (app.isPackaged) {
    return join(process.resourcesPath, 'driver', 'PersonalSafer.sys')
  }
  return join(__dirname, '..', '..', '..', 'kernel', 'x64', 'Release', 'PersonalSafer.sys')
}

function getDefaultQuarantineDirectory(): string {
  return join(process.env.ProgramData || 'C:\\ProgramData', 'PersonalSafer', 'Quarantine')
}

function toKernelNtPath(win32Path: string): string {
  if (win32Path.startsWith('\\??\\') || win32Path.startsWith('\\Device\\')) {
    return win32Path
  }
  return `\\??\\${win32Path}`
}

function applyDefaultQuarantine(addon: any): void {
  const rootDirectory = getDefaultQuarantineDirectory()
  mkdirSync(rootDirectory, { recursive: true })
  addon?.dlp?.kernel_comm?.setQuarantineConfig?.({
    enabled: true,
    rootDirectory: toKernelNtPath(rootDirectory),
  })
}

let registryWatcherStarted = false

export function registerHandlers(): void {
  ipcMain.handle('monitor:get-cpu', () => getCpuData())
  ipcMain.handle('monitor:get-memory', () => getMemoryData())
  ipcMain.handle('monitor:get-network', () => getNetworkData())
  ipcMain.handle('monitor:get-disk', () => getDiskData())
  ipcMain.handle('monitor:get-registry-events', (_event, limit?: number) => {
    try {
      const addon = getAddon()
      if (!addon || !isAddonLoaded() || typeof addon.monitor?.readEvents !== 'function') {
        return { available: false, error: '注册表数据源不可用', events: [] }
      }
      if (!registryWatcherStarted) {
        addon.monitor.watchKey('HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer')
        registryWatcherStarted = true
      }
      return {
        available: true,
        error: null,
        events: addon.monitor.readEvents(Math.min(Math.max(Number(limit) || 200, 1), 1000)),
      }
    } catch (error: any) {
      registryWatcherStarted = false
      return { available: false, error: error?.message || '注册表数据源不可用', events: [] }
    }
  })

  ipcMain.on('monitor:start', (_event, moduleName: string) => {
    console.log(`[Monitor] start: ${moduleName}`)
  })

  ipcMain.on('monitor:stop', (_event, moduleName: string) => {
    console.log(`[Monitor] stop: ${moduleName}`)
  })

  ipcMain.handle('dlp:get-status', () => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        const kernelComm = addon.dlp.kernel_comm
        const driverLoader = addon.dlp.driver_loader

        let loadState: any = null
        try {
          loadState = driverLoader?.getLoadState?.() || null
        } catch {
          loadState = null
        }

        let driverLoaded = false
        try {
          driverLoaded = loadState?.loaded ?? !!driverLoader?.isLoaded?.()
        } catch {
          driverLoaded = false
        }

        let connected = false
        try {
          connected = !!kernelComm?.connect?.()
        } catch {
          connected = false
        }
        driverLoaded = driverLoaded || connected

        if (connected && !isPolling()) {
          startEventPolling()
        }

        let status: any = null
        if (connected && kernelComm?.getDriverStatus) {
          try {
            status = kernelComm.getDriverStatus()
          } catch {
            status = null
          }
        }
        driverLoaded = driverLoaded || status?.driverLoaded === true
        const connectionState = kernelComm?.getConnectionState?.()
        const connectionError = connectionState?.lastError
          || (connectionState?.lastErrorCode
            ? `${connectionState.lastErrorStage || 'device'}: Win32 ${connectionState.lastErrorCode}`
            : null)

        return {
          driverLoaded,
          fileFilterActive:
            status?.fileFilterActive ?? loadState?.filterManagerLoaded ?? driverLoaded,
          networkFilterActive: status?.networkFilterActive ?? false,
          lastError: status?.lastErrorCode
            ? `0x${Number(status.lastErrorCode).toString(16).padStart(8, '0')}`
            : driverLoaded && !connected
              ? connectionError || 'DEVICE_CONNECTION_UNAVAILABLE'
              : null,
          deviceConnected: connected,
          deviceControlAvailable: connectionState?.canControl === true,
          localProxyRunning: !!getLocalProxyStatus().running,
        }
      }
    } catch (error: any) {
      console.warn('[DLP] get status failed:', error?.message || error)
    }

    return {
      driverLoaded: false,
      fileFilterActive: false,
      networkFilterActive: false,
      lastError: null,
      localProxyRunning: !!getLocalProxyStatus().running,
    }
  })

  ipcMain.handle('dlp:load-driver', async () => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        const result = addon.dlp.driver_loader.load(getDriverSysPath())
        if (result?.success) {
          const connected = !!addon.dlp.kernel_comm?.connect?.()
          if (!connected) {
            const connectionState = addon.dlp.kernel_comm?.getConnectionState?.()
            if (!result.alreadyRunning) {
              addon.dlp.driver_loader?.unload?.()
            }
            return {
              success: false,
              alreadyRunning: !!result.alreadyRunning,
              error: connectionState?.lastError || 'driver is loaded but the device connection failed',
            }
          }
          applyDefaultQuarantine(addon)
          const policyResult = applyPolicyToKernel(addon, getPersistedPolicy())
          if (!policyResult.success) {
            addon.dlp.kernel_comm?.disconnect?.()
            if (!result.alreadyRunning) {
              addon.dlp.driver_loader?.unload?.()
            }
            return { success: false, error: `driver loaded but policy restore failed: ${policyResult.error}` }
          }
          updateLocalProxyPolicy(policyResult.policy)
          startEventPolling()
          let proxyWarning: string | null = null
          try {
            await startLocalProxy({ enabled: true, installSystemProxy: false, mitmEnabled: true })
          } catch (error: any) {
            proxyWarning = error?.message || 'local proxy start failed'
          }
          return { success: true, alreadyRunning: !!result.alreadyRunning, warning: proxyWarning }
        }
        return { success: false, error: result?.error || 'driver load failed' }
      }
    } catch (error: any) {
      console.warn('[DLP] load driver failed:', error?.message || error)
      return { success: false, error: error?.message || 'unknown error' }
    }

    return { success: false, error: 'native addon not loaded' }
  })

  ipcMain.handle('dlp:unload-driver', async () => {
    try {
      const addon = getAddon()
      await stopLocalProxy()
      if (addon && isAddonLoaded()) {
        await stopEventPolling()
        addon.dlp.kernel_comm?.disconnect?.()
        const result = addon.dlp.driver_loader.unload()
        return result?.success
          ? { success: true }
          : { success: false, error: result?.error || 'driver unload failed' }
      }
    } catch (error: any) {
      console.warn('[DLP] unload driver failed:', error?.message || error)
    }

    return { success: false, error: 'native addon not loaded' }
  })

  ipcMain.handle('dlp:get-policy', () => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded() && addon.dlp.policy_manager) {
        const nativePolicy = addon.dlp.policy_manager.getPolicy()
        if (nativePolicy) {
          const persisted = getPersistedPolicy()
          const transformed = {
            ...persisted,
            ...transformNativePolicy(nativePolicy),
            fileActions: persisted.fileActions,
            networkActions: persisted.networkActions,
            processWhitelist: persisted.processWhitelist,
          }
          updateLocalProxyPolicy(transformed)
          return transformed
        }
      }
    } catch (error: any) {
      console.warn('[DLP] get policy failed:', error?.message || error)
    }

    return getPersistedPolicy()
  })

  ipcMain.handle('dlp:set-policy', (_event, policy: any) => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded() && addon.dlp.policy_manager) {
        let driverLoaded = false
        try {
          driverLoaded = !!addon.dlp.driver_loader?.isLoaded?.()
          if (!driverLoaded) driverLoaded = !!addon.dlp.kernel_comm?.connect?.()
        } catch {
          driverLoaded = false
        }

        if (!driverLoaded) {
          const pendingPolicy = persistPolicy(policy)
          updateLocalProxyPolicy(pendingPolicy)
          return {
            success: true,
            pending: true,
            policy: pendingPolicy,
            warning: '策略已保存到本地，将在驱动下次加载时自动应用',
          }
        }

        const policyResult = applyPolicyToKernel(addon, policy)
        if (!policyResult.success) return policyResult
        const appliedPolicy = persistPolicy({
          ...policy,
          ...policyResult.policy,
          fileActions: policy?.fileActions,
          networkActions: policy?.networkActions,
          processWhitelist: policy?.processWhitelist,
        })
        updateLocalProxyPolicy(appliedPolicy)
        return {
          success: true,
          policy: appliedPolicy,
          warning: (policyResult.differences || []).length > 0
            ? `内核已应用策略，但以下字段被规范化：${(policyResult.differences || []).join('、')}`
            : null,
        }
      }
      const pendingPolicy = persistPolicy(policy)
      updateLocalProxyPolicy(pendingPolicy)
      return {
        success: true,
        pending: true,
        policy: pendingPolicy,
        warning: '策略已保存到本地，将在驱动可用时自动应用',
      }
    } catch (error: any) {
      console.warn('[DLP] set policy failed:', error?.message || error)
      return { success: false, error: error?.message || 'policy save failed' }
    }
  })

  ipcMain.handle('dlp:get-quarantine-config', () => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        return addon.dlp.kernel_comm?.getQuarantineConfig?.() || null
      }
    } catch (error: any) {
      console.warn('[DLP] get quarantine config failed:', error?.message || error)
    }
    return null
  })

  ipcMain.handle('dlp:set-quarantine-config', (_event, config: any) => {
    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        const rootDirectory = String(config?.rootDirectory || getDefaultQuarantineDirectory())
        mkdirSync(rootDirectory, { recursive: true })
        const ok = addon.dlp.kernel_comm?.setQuarantineConfig?.({
          enabled: config?.enabled !== false,
          rootDirectory: toKernelNtPath(rootDirectory),
        })
        return { success: !!ok }
      }
    } catch (error: any) {
      console.warn('[DLP] set quarantine config failed:', error?.message || error)
      return { success: false, error: error?.message || 'set quarantine config failed' }
    }
    return { success: false, error: 'native addon not loaded' }
  })

  ipcMain.handle('dlp:get-quarantine-catalog', (_event, query?: any) => {
    return listQuarantineCatalog({
      search: typeof query?.search === 'string' ? query.search : '',
      limit: typeof query?.limit === 'number' ? query.limit : 200,
    })
  })

  ipcMain.handle('dlp:get-quarantine-catalog-info', () => getQuarantineCatalogInfo())
  ipcMain.handle('dlp:restore-quarantine-entry', (_event, id: number) => {
    try { return { success: true, entry: restoreQuarantineEntry(Number(id)) } }
    catch (error: any) { return { success: false, error: error?.message || 'restore failed' } }
  })
  ipcMain.handle('dlp:purge-quarantine-entry', (_event, id: number) => {
    try { return { success: true, entry: purgeQuarantineEntry(Number(id)) } }
    catch (error: any) { return { success: false, error: error?.message || 'purge failed' } }
  })

  ipcMain.handle('proxy:get-status', () => getLocalProxyStatus())
  ipcMain.handle('proxy:start', (_event, config: any) => startLocalProxy(config || {}))
  ipcMain.handle('proxy:stop', () => stopLocalProxy())

  ipcMain.handle('audit:get-logs', (_event, filters?: any) => {
    if (isPolling() || getEvents(1).length > 0) {
      return getEvents(filters?.limit || 200)
    }

    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        const limit = filters?.limit || 50
        const result: any[] = []
        if (addon.audit.file_audit) {
          const fileLogs = addon.audit.file_audit.queryLogs({ limit })
          if (fileLogs) {
            for (const log of fileLogs) {
              result.push(convertFileAuditLog(log))
            }
          }
        }
        if (addon.audit.net_audit) {
          const netLogs = addon.audit.net_audit.queryLogs({ limit })
          if (netLogs) {
            for (const log of netLogs) {
              result.push(convertNetAuditLog(log))
            }
          }
        }
        if (result.length > 0) {
          return result.sort((a, b) => new Date(b.timestamp).getTime() - new Date(a.timestamp).getTime())
        }
      }
    } catch (error: any) {
      console.warn('[Audit] get logs failed:', error?.message || error)
    }

    return getAuditLogsMock(filters)
  })

  ipcMain.handle('audit:get-stats', () => {
    if (isPolling() || getEvents(1).length > 0) {
      return getStats()
    }

    try {
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        let fileCount = 0
        let netCount = 0
        const fileLogs = addon.audit.file_audit?.queryLogs?.({ limit: 10000 })
        const netLogs = addon.audit.net_audit?.queryLogs?.({ limit: 10000 })
        if (fileLogs) fileCount = fileLogs.length
        if (netLogs) netCount = netLogs.length
        return {
          totalEvents: fileCount + netCount,
          fileEvents: fileCount,
          networkEvents: netCount,
          registryEvents: 0,
          blockedEvents: 0,
        }
      }
    } catch (error: any) {
      console.warn('[Audit] get stats failed:', error?.message || error)
    }

    return getAuditStatsMock()
  })

  ipcMain.handle('audit:clear-logs', () => {
    try {
      clearEvents()
      const addon = getAddon()
      if (addon && isAddonLoaded()) {
        addon.audit.file_audit?.clearLogs?.()
        addon.audit.net_audit?.clearLogs?.()
        return { success: true }
      }
    } catch (error: any) {
      console.warn('[Audit] clear logs failed:', error?.message || error)
      return { success: false, error: error?.message || 'clear logs failed' }
    }

    return { success: true }
  })

  ipcMain.handle('audit:lease-persisted-events', (_event, limit?: number) => {
    return leaseQueuedEvents(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:ack-persisted-event', (_event, eventId: number) => {
    markQueuedEventSent(eventId)
    return { success: true }
  })

  ipcMain.handle('audit:fail-persisted-event', (_event, eventId: number, error?: string) => {
    markQueuedEventFailed(eventId, String(error || 'delivery failed'))
    return { success: true }
  })

  ipcMain.handle('audit:requeue-abandoned-events', (_event, limit?: number) => {
    const count = requeueAbandonedEvents(typeof limit === 'number' ? limit : 100)
    return { success: true, count }
  })

  ipcMain.handle('audit:get-delivery-status', () => {
    return {
      ...getDeliveryWorkerStatus(),
      deliveredQueue: getDeliveredQueueInfo(),
    }
  })

  ipcMain.handle('audit:clear-delivery-backpressure', (_event, sinkId?: string) => {
    return { success: true, ...clearDeliveryBackpressure(typeof sinkId === 'string' ? sinkId : undefined) }
  })

  ipcMain.handle('audit:get-dead-letter-queue', (_event, limit?: number) => {
    return listDeadLetterQueue(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-dead-letter-records', (_event, limit?: number) => {
    return listDeadLetterRecords(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-delivered-queue', (_event, limit?: number) => {
    return listDeliveredQueue(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-delivered-records', (_event, limit?: number) => {
    return listDeliveredRecords(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-delivery-checkpoints', (_event, limit?: number) => {
    return listDeliveryCheckpoints(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-archived-checkpoints', (_event, limit?: number) => {
    return listArchivedCheckpoints(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:get-delivery-pipeline-history', (_event, limit?: number) => {
    return listPipelineHistory(typeof limit === 'number' ? limit : 100)
  })

  ipcMain.handle('audit:clear-completed-checkpoints', () => {
    return { success: true, ...clearCompletedCheckpoints() }
  })

  ipcMain.handle('audit:export-delivery-report', (_event, reason?: string) => {
    return exportDeliveryReport(typeof reason === 'string' && reason.trim() ? reason.trim() : 'manual-export')
  })

  ipcMain.handle('audit:get-delivery-sink-config', () => {
    return getDeliverySinkConfig()
  })

  ipcMain.handle('audit:set-delivery-sink-config', (_event, config: any) => {
    const sink = setDeliverySinkConfig(config || {})
    return { success: true, sink }
  })

  ipcMain.handle('audit:start-delivery-worker', (_event, intervalMs?: number, batchSize?: number) => {
    startDeliveryWorker(
      typeof intervalMs === 'number' ? intervalMs : 5000,
      typeof batchSize === 'number' ? batchSize : 100
    )
    return { success: true, status: getDeliveryWorkerStatus() }
  })

  ipcMain.handle('audit:stop-delivery-worker', async () => {
    await stopDeliveryWorker()
    return { success: true, status: getDeliveryWorkerStatus() }
  })

  ipcMain.handle('audit:start-delivery-test-sink', async (_event, port?: number) => {
    const sink = await startDeliveryTestSink(typeof port === 'number' ? port : 7777)
    return { success: true, sink }
  })

  ipcMain.handle('audit:stop-delivery-test-sink', async () => {
    await stopDeliveryTestSink()
    return { success: true, sink: getDeliveryTestSinkStatus() }
  })

  ipcMain.handle('audit:get-delivery-test-sink-status', () => {
    return getDeliveryTestSinkStatus()
  })

  ipcMain.handle('audit:clear-delivery-test-sink-records', () => {
    clearDeliveryTestSinkRecords()
    return { success: true }
  })

  ipcMain.handle('app:get-info', () => ({
    version: app.getVersion(),
    platform: process.platform,
    arch: process.arch,
    isPackaged: app.isPackaged,
    addonLoaded: isAddonLoaded(),
    policyStore: getPolicyStoreInfo(),
    eventPersistence: getEventPersistenceInfo(),
    deliveryWorker: getDeliveryWorkerStatus(),
  }))
}

function convertFileAuditLog(log: any) {
  const typeMap: Record<number, string> = {
    0: 'file_create',
    1: 'file_write',
    2: 'file_delete',
    3: 'file_rename',
  }

  return {
    id: log.id || 0,
    type: typeMap[log.eventType] || 'file_create',
    action: log.action || 'logged',
    processName: log.processName || 'unknown.exe',
    pid: log.processId || 0,
    timestamp: new Date(Number(log.timestamp) || Date.now()).toISOString(),
    details: log.fileName || '',
  }
}

function convertNetAuditLog(log: any) {
  const typeMap: Record<number, string> = {
    6: 'network_connect',
    7: 'http_request',
    8: 'http_response',
    9: 'ftp_command',
    12: 'sni_capture',
  }

  return {
    id: log.id || 0,
    type: typeMap[log.eventType] || 'network_connect',
    action: log.action || 'logged',
    processName: log.processName || 'unknown.exe',
    pid: log.processId || 0,
    timestamp: new Date(Number(log.timestamp) || Date.now()).toISOString(),
    details: log.url || '',
  }
}

export {
  getCpuMockData,
  getMemoryMockData,
  getNetworkMockData,
  getDiskMockData,
  getDefaultPolicy,
  getAuditLogsMock,
  getAuditStatsMock,
} from '../modules/mock_data'
