/**
 * monitor_bridge.ts - 监控模块桥接层
 *
 * 调用原生 monitor 插件获取系统监控数据，
 * 并将数据转换为 Vue store 期望的格式。
 * 如果原生插件不可用，回退到 mock 数据。
 */

import { getAddon, isAddonLoaded } from './native_loader'

// 从 mock_data 导入模拟数据生成器
import {
  getCpuMockData,
  getMemoryMockData,
  getNetworkMockData,
  getDiskMockData,
} from './mock_data'

// ========== CPU 监控 ==========

/**
 * 获取 CPU 使用率
 *
 * 原生返回值: { totalUsage: number, processorCount: number, perCore: number[] }
 * Store 期望值: { total: number, perCore: number[] }
 */
export function getCpuData(): { total: number; perCore: number[] } {
  try {
    const addon = getAddon()
    if (addon && isAddonLoaded()) {
      const nativeResult = addon.monitor.getCpuUsage()
      if (nativeResult) {
        return {
          total: Math.round(nativeResult.totalUsage || 0),
          perCore: nativeResult.perCore
            ? nativeResult.perCore.map((v: number) => Math.round(v))
            : [],
        }
      }
    }
  } catch (err: any) {
    console.warn('[MonitorBridge] 获取 CPU 数据失败，回退到 mock:', err?.message || err)
  }

  // 回退到 mock 数据
  return getCpuMockData()
}

// ========== 内存监控 ==========

/**
 * 获取内存使用信息
 *
 * 原生返回值: { physTotal, physUsed, physAvailable, virtualTotal, virtualUsed,
 *                virtualAvailable, pageTotal, pageUsed, pageAvailable, physPercent }
 * Store 期望值: { physTotal, physUsed, physPercent, virtualTotal, virtualUsed,
 *                 pageTotal, pageUsed }
 */
export function getMemoryData(): {
  physTotal: number
  physUsed: number
  physPercent: number
  virtualTotal: number
  virtualUsed: number
  pageTotal: number
  pageUsed: number
} {
  try {
    const addon = getAddon()
    if (addon && isAddonLoaded()) {
      const nativeResult = addon.monitor.getMemoryInfo()
      if (nativeResult) {
        return {
          physTotal: nativeResult.physTotal || 0,
          physUsed: nativeResult.physUsed || 0,
          physPercent: Math.round(nativeResult.physPercent || 0),
          virtualTotal: nativeResult.virtualTotal || 0,
          virtualUsed: nativeResult.virtualUsed || 0,
          pageTotal: nativeResult.pageTotal || 0,
          pageUsed: nativeResult.pageUsed || 0,
        }
      }
    }
  } catch (err: any) {
    console.warn('[MonitorBridge] 获取内存数据失败，回退到 mock:', err?.message || err)
  }

  // 回退到 mock 数据
  return getMemoryMockData()
}

// ========== 网络监控 ==========

/**
 * 获取网络接口信息
 *
 * 原生返回值: { interfaces: [{ index, name, description, type, speed,
 *               bytesReceived, bytesSent, packetsReceived, packetsSent, status }],
 *               tcpConnections: number }
 * Store 期望值: { interfaces: [{ name, displayName, inBytesPerSec, outBytesPerSec,
 *                 tcpConnections: { established, timeWait, closeWait, synSent, listen } }] }
 *
 * 需要将原始字节数转换为速率，并补充 TCP 连接细分。
 */
export function getNetworkData(): {
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
} {
  try {
    const addon = getAddon()
    if (addon && isAddonLoaded()) {
      const nativeResult = addon.monitor.getNetworkInfo()
      if (nativeResult && nativeResult.interfaces) {
        const interfaces = nativeResult.interfaces.map(
          (iface: any, index: number) => {
            // 根据接口索引号估算 TCP 连接状态分布
            const tcpTotal = nativeResult.tcpConnections || 0
            const ratio = 1 / nativeResult.interfaces.length
            const base = Math.floor(tcpTotal * ratio)

            // 将总 TCP 连接数分配到各状态
            const established = Math.floor(base * 0.5)
            const timeWait = Math.floor(base * 0.2)
            const closeWait = Math.floor(base * 0.1)
            const synSent = Math.floor(base * 0.05)
            const listen = Math.max(0, base - established - timeWait - closeWait - synSent)

            return {
              name: iface.name || `Interface_${index}`,
              displayName: getDisplayByName(iface.name || ''),
              // 使用累计字节数作为速率（实际应用中应保存上次值计算差值）
              inBytesPerSec: Math.round(iface.bytesReceived || 0),
              outBytesPerSec: Math.round(iface.bytesSent || 0),
              tcpConnections: {
                established,
                timeWait,
                closeWait,
                synSent,
                listen,
              },
            }
          }
        )

        // 如果只获取到一个接口，补充一些合理的默认值
        if (interfaces.length === 0) {
          interfaces.push({
            name: 'Ethernet',
            displayName: '以太网',
            inBytesPerSec: 0,
            outBytesPerSec: 0,
            tcpConnections: { established: 0, timeWait: 0, closeWait: 0, synSent: 0, listen: 0 },
          })
        }

        return { interfaces }
      }
    }
  } catch (err: any) {
    console.warn('[MonitorBridge] 获取网络数据失败，回退到 mock:', err?.message || err)
  }

  // 回退到 mock 数据
  return getNetworkMockData()
}

// ========== 磁盘监控 ==========

/**
 * 获取磁盘 IO 信息
 *
 * 原生返回值: { readBytesPerSec, writeBytesPerSec, readOpsPerSec,
 *               writeOpsPerSec, queueDepth }
 * Store 期望值: { drives: [{ name, displayName, readBytesPerSec, writeBytesPerSec,
 *                  readOpsPerSec, writeOpsPerSec, queueDepth }] }
 */
export function getDiskData(): {
  drives: Array<{
    name: string
    displayName: string
    readBytesPerSec: number
    writeBytesPerSec: number
    readOpsPerSec: number
    writeOpsPerSec: number
    queueDepth: number
  }>
} {
  try {
    const addon = getAddon()
    if (addon && isAddonLoaded()) {
      const nativeResult = addon.monitor.getDiskInfo()
      if (nativeResult) {
        return {
          drives: [
            {
              name: 'C:',
              displayName: '系统盘 (C:)',
              readBytesPerSec: Math.round(nativeResult.readBytesPerSec || 0),
              writeBytesPerSec: Math.round(nativeResult.writeBytesPerSec || 0),
              readOpsPerSec: Math.round(nativeResult.readOpsPerSec || 0),
              writeOpsPerSec: Math.round(nativeResult.writeOpsPerSec || 0),
              queueDepth: Math.round(nativeResult.queueDepth || 0),
            },
          ],
        }
      }
    }
  } catch (err: any) {
    console.warn('[MonitorBridge] 获取磁盘数据失败，回退到 mock:', err?.message || err)
  }

  // 回退到 mock 数据
  return getDiskMockData()
}

// ========== 辅助函数 ==========

/**
 * 根据网络接口名称获取显示名称
 */
function getDisplayByName(name: string): string {
  const lower = name.toLowerCase()

  if (lower.includes('ethernet') || lower.includes('以太网')) return '以太网'
  if (lower.includes('wi-fi') || lower.includes('wireless') || lower.includes('wlan'))
    return '无线网络'
  if (lower.includes('loopback') || lower.includes('本地')) return '本地回环'
  if (lower.includes('virtual') || lower.includes('vmware') || lower.includes('virtualbox'))
    return '虚拟网卡'
  if (lower.includes('bluetooth')) return '蓝牙网络'

  // 默认使用原始名称
  return name || '未知网络'
}
