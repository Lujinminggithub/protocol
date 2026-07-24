/**
 * native_loader.ts - 原生 N-API 插件加载器
 *
 * 负责动态加载 personal_safer.node 插件，
 * 当插件不可用时回退到 mock 数据。
 */

import { app } from 'electron'
import { join } from 'path'
import { existsSync } from 'fs'

// 原生插件的导出接口
export interface NativeAddon {
  monitor: {
    getCpuUsage: () => any
    getMemoryInfo: () => any
    getNetworkInfo: () => any
    getDiskInfo: () => any
    watchKey: (key: string) => any
    unwatchKey: () => any
    readEvents: (limit?: number) => any[]
  }
  dlp: {
    kernel_comm: {
      connect: () => any
      disconnect: () => any
      getConnectionState: () => {
        connected: boolean
        canControl: boolean
        lastErrorCode: number
        lastErrorStage: string
        lastError: string
      }
      sendIoctl: (code: number, data: string) => any
      getDriverStatus: () => any
      getProtectionChallenge: (ttlSeconds?: number) => any
      unlockProtection: (ticket: Buffer, signature: Buffer) => boolean
      relockProtection: () => boolean
      getProtectionState: () => any
      setProtectionList: (config: { enabled?: boolean; paths?: string[]; processIds?: number[]; processImages?: string[] }) => boolean
      setProtectionMaintenanceMode: (enabled: boolean) => boolean
      waitForEvents: (timeoutMs?: number) => Promise<any>
      setQuarantineConfig: (config: any) => any
      getQuarantineConfig: () => any
      setRedirectConfig: (config: any) => any
      queryRedirectDestination: (query: any) => any
      lookupConnectionProcess: (connection: any) => any
      readFileEvent: () => any
      readFileEventsBatch: () => any
      readNetEvent: () => any
      readNetEventsBatch: () => any
    }
    driver_loader: {
      load: (sysPath?: string) => any
      unload: () => any
      isLoaded: () => any
      getLoadState: () => {
        loaded: boolean
        filterManagerLoaded: boolean
        serviceRunning: boolean
        deviceReachable: boolean
      }
    }
    file_filter_handler: {
      handleEvent: (...args: any[]) => any
      setPolicy: (policy: any) => any
    }
    net_filter_handler: {
      handleEvent: (...args: any[]) => any
      setPolicy: (policy: any) => any
    }
    policy_manager: {
      getPolicy: () => any
      setPolicy: (policy: any) => any
      reloadPolicy: () => any
    }
  }
  audit: {
    file_audit: {
      logEvent: (...args: any[]) => any
      queryLogs: (filters?: any) => any
      clearLogs: () => any
    }
    net_audit: {
      logEvent: (...args: any[]) => any
      queryLogs: (filters?: any) => any
      clearLogs: () => any
    }
  }
}

// 插件加载状态
export interface AddonLoadResult {
  success: boolean
  error?: string
}

// 缓存已加载的插件
let cachedAddon: NativeAddon | null = null
let loadAttempted = false

/**
 * 获取原生插件的 .node 文件路径
 */
function getAddonPath(): string | null {
  let relativePath: string

  if (app.isPackaged) {
    // 打包后：addon 位于 app.asar 解压目录的 native/ 下
    relativePath = join(process.resourcesPath, 'native', 'personal_safer.node')
  } else {
    // 开发环境：addon 编译产物位于 src/native/build/Release/
    // __dirname 运行时为 src/electron/dist-electron/main
    relativePath = join(__dirname, '..', '..', '..', 'native', 'build', 'Release', 'personal_safer.node')
  }

  if (existsSync(relativePath)) {
    return relativePath
  }

  // 尝试其他可能的构建输出路径
  const alternativePaths = [
    join(__dirname, '..', '..', '..', 'native', 'build', 'Debug', 'personal_safer.node'),
    join(__dirname, 'personal_safer.node'),
  ]

  for (const altPath of alternativePaths) {
    if (existsSync(altPath)) {
      return altPath
    }
  }

  return null
}

/**
 * 加载原生插件
 * 使用 require() 动态加载 .node 文件
 */
function loadAddon(): AddonLoadResult {
  if (loadAttempted) {
    // 已经尝试过加载且失败了，直接返回缓存结果
    return cachedAddon
      ? { success: true }
      : { success: false, error: '插件加载失败，之前已尝试过' }
  }

  loadAttempted = true

  try {
    const addonPath = getAddonPath()
    if (!addonPath) {
      return {
        success: false,
        error: '未找到 personal_safer.node 文件',
      }
    }

    // 动态加载 .node 原生插件
    const addon = require(addonPath) as NativeAddon
    cachedAddon = addon

    return { success: true }
  } catch (err: any) {
    const errorMessage = err?.message || String(err)
    console.warn('[NativeLoader] 插件加载失败，将使用模拟数据:', errorMessage)

    return {
      success: false,
      error: errorMessage,
    }
  }
}

/**
 * 获取已加载的插件实例
 * 如果尚未加载则先尝试加载
 */
export function getAddon(): NativeAddon | null {
  if (!cachedAddon) {
    loadAddon()
  }
  return cachedAddon
}

/**
 * 检查插件是否已成功加载
 */
export function isAddonLoaded(): boolean {
  return !!cachedAddon
}

/**
 * 重新加载插件（用于热重载或错误恢复）
 */
export function reloadAddon(): AddonLoadResult {
  cachedAddon = null
  loadAttempted = false
  return loadAddon()
}
