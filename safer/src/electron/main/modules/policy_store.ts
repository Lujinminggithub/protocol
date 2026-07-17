import { app } from 'electron'
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { getDefaultPolicy } from './mock_data'

type RuleLimit = { maxItems: number; maxChars: number; label: string }

const STRING_RULE_LIMITS: Record<string, RuleLimit> = {
  fileExtensions: { maxItems: 16, maxChars: 15, label: '文件扩展名' },
  processBlacklist: { maxItems: 16, maxChars: 63, label: '进程黑名单' },
  blockedDomains: { maxItems: 8, maxChars: 63, label: '域名规则' },
  blockedUrls: { maxItems: 4, maxChars: 47, label: 'URL 规则' },
  blockedFtpCommands: { maxItems: 8, maxChars: 15, label: 'FTP 命令规则' },
  blockedFtpPaths: { maxItems: 8, maxChars: 63, label: 'FTP 路径规则' },
  blockedFtpContentPatterns: { maxItems: 8, maxChars: 63, label: 'FTP 内容规则' },
  blockedHttpHeaders: { maxItems: 8, maxChars: 47, label: 'HTTP Header 规则' },
  blockedHttpTrailers: { maxItems: 8, maxChars: 47, label: 'HTTP Trailer 规则' },
  blockedHttpBodyPatterns: { maxItems: 8, maxChars: 63, label: 'HTTP Body 规则' },
  blockedJsonKeys: { maxItems: 8, maxChars: 31, label: 'JSON Key 规则' },
  blockedJsonPaths: { maxItems: 8, maxChars: 63, label: 'JSON Path 规则' },
  blockedJsonValues: { maxItems: 8, maxChars: 47, label: 'JSON Value 规则' },
}

let activePolicy: any = null

function getPolicyDirectory(): string {
  return join(app.getPath('userData'), 'config')
}

function getPolicyPath(): string {
  return join(getPolicyDirectory(), 'policy.json')
}

function stringArray(value: unknown): string[] {
  return Array.isArray(value)
    ? value.map((item) => String(item).trim()).filter(Boolean)
    : []
}

export function normalizePolicy(policy: any): any {
  const defaults = getDefaultPolicy()
  const source = policy && typeof policy === 'object' ? policy : {}
  const normalized: any = {
    fileFilterEnabled: source.fileFilterEnabled !== false,
    networkFilterEnabled: source.networkFilterEnabled !== false,
    auditEnabled: source.auditEnabled !== false,
    fileActions: { ...defaults.fileActions, ...(source.fileActions || {}) },
    networkActions: { ...defaults.networkActions, ...(source.networkActions || {}) },
    processWhitelist: stringArray(source.processWhitelist),
    blockedPorts: Array.isArray(source.blockedPorts)
      ? source.blockedPorts.map((value: unknown) => Number(value)).filter(Number.isFinite)
      : [],
  }

  for (const field of Object.keys(STRING_RULE_LIMITS)) {
    const sourceValue = field === 'fileExtensions'
      ? (source.fileExtensions || source.blockedExtensions)
      : source[field]
    normalized[field] = stringArray(sourceValue)
  }
  return normalized
}

export function validatePolicy(policy: any): string[] {
  const normalized = normalizePolicy(policy)
  const errors: string[] = []

  for (const [field, limit] of Object.entries(STRING_RULE_LIMITS)) {
    const values = normalized[field] as string[]
    if (values.length > limit.maxItems) {
      errors.push(`${limit.label}最多允许 ${limit.maxItems} 条`)
    }
    values.forEach((value, index) => {
      if (value.includes('\0')) errors.push(`${limit.label}第 ${index + 1} 条包含 NUL 字符`)
      if (value.length > limit.maxChars) {
        errors.push(`${limit.label}第 ${index + 1} 条超过 ${limit.maxChars} 个字符`)
      }
    })
    if (new Set(values.map((value) => value.toLowerCase())).size !== values.length) {
      errors.push(`${limit.label}包含重复项`)
    }
  }

  const rawPorts = Array.isArray(policy?.blockedPorts) ? policy.blockedPorts : []
  if (rawPorts.length > 32) errors.push('阻断端口最多允许 32 条')
  rawPorts.forEach((value: unknown, index: number) => {
    const port = Number(value)
    if (!Number.isInteger(port) || port < 1 || port > 65535) {
      errors.push(`阻断端口第 ${index + 1} 条必须是 1-65535 的整数`)
    }
  })
  if (new Set(normalized.blockedPorts).size !== normalized.blockedPorts.length) {
    errors.push('阻断端口包含重复项')
  }
  return errors
}

function writePolicy(policy: any): void {
  mkdirSync(getPolicyDirectory(), { recursive: true })
  const path = getPolicyPath()
  const temporaryPath = `${path}.tmp`
  writeFileSync(temporaryPath, JSON.stringify({ version: 1, policy, updatedAt: new Date().toISOString() }, null, 2), 'utf8')
  renameSync(temporaryPath, path)
}

export function initializePolicyStore(): any {
  if (activePolicy) return activePolicy
  let loaded: any = null
  if (existsSync(getPolicyPath())) {
    try {
      const document = JSON.parse(readFileSync(getPolicyPath(), 'utf8'))
      if (document?.version === 1 && document.policy) loaded = document.policy
    } catch {
      try {
        renameSync(getPolicyPath(), `${getPolicyPath()}.corrupt-${Date.now()}`)
      } catch {}
      loaded = null
    }
  }
  activePolicy = normalizePolicy(loaded || getDefaultPolicy())
  writePolicy(activePolicy)
  return activePolicy
}

export function getPersistedPolicy(): any {
  return normalizePolicy(activePolicy || initializePolicyStore())
}

export function persistPolicy(policy: any): any {
  const errors = validatePolicy(policy)
  if (errors.length > 0) throw new Error(errors.join('；'))
  activePolicy = normalizePolicy(policy)
  writePolicy(activePolicy)
  return getPersistedPolicy()
}

export function toKernelPolicy(policy: any): any {
  const normalized = normalizePolicy(policy)
  return {
    fileFilterEnabled: normalized.fileFilterEnabled,
    networkFilterEnabled: normalized.networkFilterEnabled,
    auditEnabled: normalized.auditEnabled,
    blockedExtensions: normalized.fileExtensions,
    blockedProcesses: normalized.processBlacklist,
    processBlacklist: normalized.processBlacklist,
    blockedPorts: normalized.blockedPorts,
    blockedDomains: normalized.blockedDomains,
    blockedUrls: normalized.blockedUrls,
    blockedFtpCommands: normalized.blockedFtpCommands,
    blockedFtpPaths: normalized.blockedFtpPaths,
    blockedFtpContentPatterns: normalized.blockedFtpContentPatterns,
    blockedHttpHeaders: normalized.blockedHttpHeaders,
    blockedHttpTrailers: normalized.blockedHttpTrailers,
    blockedHttpBodyPatterns: normalized.blockedHttpBodyPatterns,
    blockedJsonKeys: normalized.blockedJsonKeys,
    blockedJsonPaths: normalized.blockedJsonPaths,
    blockedJsonValues: normalized.blockedJsonValues,
  }
}

export function transformNativePolicy(nativePolicy: any): any {
  return normalizePolicy({
    ...nativePolicy,
    fileExtensions: nativePolicy?.fileExtensions || nativePolicy?.blockedExtensions || [],
  })
}

export function getPolicyReadbackDifferences(requested: any, applied: any): string[] {
  const requestedPolicy = normalizePolicy(requested)
  const appliedPolicy = normalizePolicy(applied)
  const differences: string[] = []
  for (const field of Object.keys(STRING_RULE_LIMITS)) {
    const left = (requestedPolicy[field] || []).map((value: unknown) => String(value).toLowerCase())
    const right = (appliedPolicy[field] || []).map((value: unknown) => String(value).toLowerCase())
    if (JSON.stringify(left) !== JSON.stringify(right)) differences.push(field)
  }
  if (JSON.stringify(requestedPolicy.blockedPorts) !== JSON.stringify(appliedPolicy.blockedPorts)) {
    differences.push('blockedPorts')
  }
  for (const field of ['fileFilterEnabled', 'networkFilterEnabled', 'auditEnabled']) {
    if (requestedPolicy[field] !== appliedPolicy[field]) differences.push(field)
  }
  return differences
}

export function applyPolicyToKernel(addon: any, policy: any) {
  const errors = validatePolicy(policy)
  if (errors.length > 0) return { success: false, error: errors.join('；') }
  const kernelPolicy = toKernelPolicy(policy)
  if (!addon?.dlp?.policy_manager?.setPolicy?.(kernelPolicy)) {
    return { success: false, error: 'kernel rejected policy update' }
  }
  const nativePolicy = addon.dlp.policy_manager.getPolicy?.()
  if (!nativePolicy) {
    return { success: false, error: 'policy write succeeded but kernel readback failed' }
  }
  const appliedPolicy = transformNativePolicy(nativePolicy)
  return {
    success: true,
    policy: appliedPolicy,
    differences: getPolicyReadbackDifferences(policy, appliedPolicy),
  }
}

export function getPolicyStoreInfo() {
  return { path: getPolicyPath(), initialized: !!activePolicy }
}
