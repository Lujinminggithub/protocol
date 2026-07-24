import { contextBridge, ipcRenderer } from 'electron'

function toIpcDto<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T
}

const api = {
  monitor: {
    getCpu: () => ipcRenderer.invoke('monitor:get-cpu'),
    getMemory: () => ipcRenderer.invoke('monitor:get-memory'),
    getNetwork: () => ipcRenderer.invoke('monitor:get-network'),
    getDisk: () => ipcRenderer.invoke('monitor:get-disk'),
    getRegistryEvents: (limit?: number) => ipcRenderer.invoke('monitor:get-registry-events', limit),
    onStart: (moduleName: string) => ipcRenderer.send('monitor:start', moduleName),
    onStop: (moduleName: string) => ipcRenderer.send('monitor:stop', moduleName),
    onEvent: (callback: (data: any) => void) => {
      ipcRenderer.on('monitor:event', (_event, data) => callback(data))
    },
  },

  dlp: {
    getStatus: () => ipcRenderer.invoke('dlp:get-status'),
    loadDriver: () => ipcRenderer.invoke('dlp:load-driver'),
    unloadDriver: () => ipcRenderer.invoke('dlp:unload-driver'),
    getPolicy: () => ipcRenderer.invoke('dlp:get-policy'),
    setPolicy: (policy: any) => ipcRenderer.invoke('dlp:set-policy', toIpcDto(policy)),
    getQuarantineConfig: () => ipcRenderer.invoke('dlp:get-quarantine-config'),
    setQuarantineConfig: (config: any) => ipcRenderer.invoke('dlp:set-quarantine-config', config),
    getQuarantineCatalog: (query?: any) => ipcRenderer.invoke('dlp:get-quarantine-catalog', query),
    getQuarantineCatalogInfo: () => ipcRenderer.invoke('dlp:get-quarantine-catalog-info'),
    restoreQuarantineEntry: (id: number) => ipcRenderer.invoke('dlp:restore-quarantine-entry', id),
    purgeQuarantineEntry: (id: number) => ipcRenderer.invoke('dlp:purge-quarantine-entry', id),
    onAlert: (callback: (alert: any) => void) => {
      ipcRenderer.on('dlp:alert', (_event, data) => callback(data))
    },
  },

  proxy: {
    getStatus: () => ipcRenderer.invoke('proxy:get-status'),
    start: (config?: any) => ipcRenderer.invoke('proxy:start', config),
    stop: () => ipcRenderer.invoke('proxy:stop'),
  },

  audit: {
    getLogs: (filters?: any) => ipcRenderer.invoke('audit:get-logs', filters),
    getStats: () => ipcRenderer.invoke('audit:get-stats'),
    clearLogs: () => ipcRenderer.invoke('audit:clear-logs'),
    getDeliveryStatus: () => ipcRenderer.invoke('audit:get-delivery-status'),
    getDeliverySinkConfig: () => ipcRenderer.invoke('audit:get-delivery-sink-config'),
    setDeliverySinkConfig: (config: any) => ipcRenderer.invoke('audit:set-delivery-sink-config', config),
    startDeliveryWorker: (intervalMs?: number, batchSize?: number) => ipcRenderer.invoke('audit:start-delivery-worker', intervalMs, batchSize),
    stopDeliveryWorker: () => ipcRenderer.invoke('audit:stop-delivery-worker'),
    clearDeliveryBackpressure: (sinkId?: string) => ipcRenderer.invoke('audit:clear-delivery-backpressure', sinkId),
    getDeadLetterQueue: (limit?: number) => ipcRenderer.invoke('audit:get-dead-letter-queue', limit),
    getDeadLetterRecords: (limit?: number) => ipcRenderer.invoke('audit:get-dead-letter-records', limit),
    getDeliveredQueue: (limit?: number) => ipcRenderer.invoke('audit:get-delivered-queue', limit),
    getDeliveredRecords: (limit?: number) => ipcRenderer.invoke('audit:get-delivered-records', limit),
    getDeliveryCheckpoints: (limit?: number) => ipcRenderer.invoke('audit:get-delivery-checkpoints', limit),
    getArchivedCheckpoints: (limit?: number) => ipcRenderer.invoke('audit:get-archived-checkpoints', limit),
    getDeliveryPipelineHistory: (limit?: number) => ipcRenderer.invoke('audit:get-delivery-pipeline-history', limit),
    requeueAbandonedEvents: (limit?: number) => ipcRenderer.invoke('audit:requeue-abandoned-events', limit),
    clearCompletedCheckpoints: () => ipcRenderer.invoke('audit:clear-completed-checkpoints'),
    exportDeliveryReport: (reason?: string) => ipcRenderer.invoke('audit:export-delivery-report', reason),
    onEvent: (callback: (event: any) => void) => {
      ipcRenderer.on('audit:event', (_event, data) => callback(data))
    },
  },

  app: {
    getInfo: () => ipcRenderer.invoke('app:get-info'),
  },
}

contextBridge.exposeInMainWorld('saferAPI', api)
