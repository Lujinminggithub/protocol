// 全局类型声明
declare global {
  interface Window {
    saferAPI: {
      monitor: {
        getCpu: () => Promise<any>
        getMemory: () => Promise<any>
        getNetwork: () => Promise<any>
        getDisk: () => Promise<any>
        getRegistryEvents: (limit?: number) => Promise<{ available: boolean; error: string | null; events: any[] }>
        onStart: (moduleName: string) => void
        onStop: (moduleName: string) => void
        onEvent: (callback: (data: any) => void) => void
      }
      dlp: {
        getStatus: () => Promise<any>
        loadDriver: () => Promise<any>
        unloadDriver: () => Promise<any>
        getPolicy: () => Promise<any>
        setPolicy: (policy: any) => Promise<any>
        getQuarantineConfig: () => Promise<any>
        setQuarantineConfig: (config: any) => Promise<any>
        getQuarantineCatalog: (query?: any) => Promise<any[]>
        getQuarantineCatalogInfo: () => Promise<any>
        restoreQuarantineEntry: (id: number) => Promise<any>
        purgeQuarantineEntry: (id: number) => Promise<any>
        onAlert: (callback: (data: any) => void) => void
      }
      proxy: {
        getStatus: () => Promise<any>
        start: (config?: any) => Promise<any>
        stop: () => Promise<any>
      }
      audit: {
        getLogs: (filters?: any) => Promise<any[]>
        getStats: () => Promise<any>
        clearLogs: () => Promise<any>
        getDeliveryStatus: () => Promise<any>
        getDeliverySinkConfig: () => Promise<any>
        setDeliverySinkConfig: (config: any) => Promise<any>
        startDeliveryWorker: (intervalMs?: number, batchSize?: number) => Promise<any>
        stopDeliveryWorker: () => Promise<any>
        clearDeliveryBackpressure: (sinkId?: string) => Promise<any>
        getDeadLetterQueue: (limit?: number) => Promise<any[]>
        getDeadLetterRecords: (limit?: number) => Promise<any[]>
        getDeliveredQueue: (limit?: number) => Promise<any[]>
        getDeliveredRecords: (limit?: number) => Promise<any[]>
        getDeliveryCheckpoints: (limit?: number) => Promise<any[]>
        getArchivedCheckpoints: (limit?: number) => Promise<any[]>
        getDeliveryPipelineHistory: (limit?: number) => Promise<any[]>
        requeueAbandonedEvents: (limit?: number) => Promise<any>
        clearCompletedCheckpoints: () => Promise<any>
        exportDeliveryReport: (reason?: string) => Promise<any>
        onEvent: (callback: (data: any) => void) => void
      }
      app: {
        getInfo: () => Promise<{ version: string; platform: string; arch: string }>
      }
    }
  }
}

export {}
