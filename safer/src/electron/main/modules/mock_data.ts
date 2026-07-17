/**
 * mock_data.ts - Mock 数据生成器
 *
 * 在原生插件不可用时提供模拟数据。
 * 单独提取此模块以避免与 handler_registry 的循环依赖。
 */

export function getCpuMockData() {
  return {
    total: Math.floor(Math.random() * 30) + 5,
    perCore: Array.from({ length: 8 }, () => Math.floor(Math.random() * 50) + 5),
  }
}

export function getMemoryMockData() {
  const total = 32 * 1024 * 1024 * 1024 // 32GB
  const used = Math.floor(total * (0.4 + Math.random() * 0.3))
  return {
    physTotal: total,
    physUsed: used,
    physPercent: Math.round((used / total) * 100),
    virtualTotal: 137_438_953_472,
    virtualUsed: Math.floor(Math.random() * 10_000_000_000),
    pageTotal: 64 * 1024 * 1024 * 1024,
    pageUsed: Math.floor(Math.random() * 16 * 1024 * 1024 * 1024),
  }
}

export function getNetworkMockData() {
  return {
    interfaces: [
      {
        name: 'Ethernet',
        displayName: '以太网',
        inBytesPerSec: Math.floor(Math.random() * 10_000_000),
        outBytesPerSec: Math.floor(Math.random() * 2_000_000),
        tcpConnections: {
          established: Math.floor(Math.random() * 50),
          timeWait: Math.floor(Math.random() * 30),
          closeWait: Math.floor(Math.random() * 10),
          synSent: Math.floor(Math.random() * 5),
          listen: Math.floor(Math.random() * 5),
        },
      },
    ],
  }
}

export function getDiskMockData() {
  return {
    drives: [
      {
        name: 'C:',
        displayName: '系统盘 (C:)',
        readBytesPerSec: Math.floor(Math.random() * 5_000_000),
        writeBytesPerSec: Math.floor(Math.random() * 2_000_000),
        readOpsPerSec: Math.floor(Math.random() * 100),
        writeOpsPerSec: Math.floor(Math.random() * 50),
        queueDepth: Math.floor(Math.random() * 5),
      },
      {
        name: 'D:',
        displayName: '数据盘 (D:)',
        readBytesPerSec: Math.floor(Math.random() * 10_000_000),
        writeBytesPerSec: Math.floor(Math.random() * 5_000_000),
        readOpsPerSec: Math.floor(Math.random() * 200),
        writeOpsPerSec: Math.floor(Math.random() * 100),
        queueDepth: Math.floor(Math.random() * 10),
      },
    ],
  }
}

export function getDefaultPolicy() {
  return {
    fileFilterEnabled: true,
    networkFilterEnabled: true,
    auditEnabled: true,
    fileActions: {
      create: 'log',
      write: 'log',
      read: 'none',
      delete: 'log',
      rename: 'log',
    },
    networkActions: {
      http: 'log',
      https: 'log',
      ftp: 'log',
    },
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
    fileExtensions: ['.exe', '.dll', '.docx', '.xlsx', '.pdf', '.zip'],
  }
}

export function getAuditLogsMock(filters?: any) {
  const types = ['file_create', 'file_write', 'network_connect', 'registry_change']
  const actions = ['allowed', 'blocked', 'logged']
  const results: any[] = []
  const count = filters?.limit || 50
  for (let i = 0; i < count; i++) {
    results.push({
      id: i + 1,
      type: types[Math.floor(Math.random() * types.length)],
      action: actions[Math.floor(Math.random() * actions.length)],
      processName: [
        'chrome.exe',
        'notepad.exe',
        'code.exe',
        'explorer.exe',
      ][Math.floor(Math.random() * 4)],
      pid: Math.floor(Math.random() * 10000) + 1000,
      timestamp: new Date(Date.now() - Math.random() * 86400000).toISOString(),
      details: `模拟审计日志 #${i + 1}`,
    })
  }
  return results.sort(
    (a, b) => new Date(b.timestamp).getTime() - new Date(a.timestamp).getTime()
  )
}

export function getAuditStatsMock() {
  return {
    totalEvents: Math.floor(Math.random() * 10000),
    fileEvents: Math.floor(Math.random() * 5000),
    networkEvents: Math.floor(Math.random() * 3000),
    registryEvents: Math.floor(Math.random() * 2000),
    blockedEvents: Math.floor(Math.random() * 500),
  }
}
