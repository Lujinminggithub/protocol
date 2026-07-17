/**
 * dlp_events.ts - polling and in-memory buffering of DLP events
 */
import { getAddon, isAddonLoaded } from './native_loader'
import { gunzipSync, inflateSync } from 'node:zlib'
import { dirname, join } from 'node:path'
import { recordQuarantineCatalogEntry } from './quarantine_catalog'
import {
  appendPersistentEvent,
  clearPersistentEventQueue,
  compactPersistentEventQueue,
  createPersistedEvent,
  getPersistentEventQueueInfo,
  getPersistentEventQueueStats,
  initializePersistentEventQueue,
  leasePendingEvents,
  PersistedDlpEvent,
  PersistedSinkDeliveryStatus,
  updatePersistentEventState,
} from './persistent_event_queue'

export interface DlpEvent {
  id: number
  type: string
  action: string
  processName: string
  pid: number
  timestamp: string
  details: string
}

const MAX_EVENTS = 5000
const events: PersistedDlpEvent[] = []
let nextId = 1
let pollingActive = false
let pollLoopPromise: Promise<void> | null = null
let persistenceInitialized = false

function push(e: PersistedDlpEvent): void {
  events.push(e)
  if (events.length > MAX_EVENTS) events.splice(0, events.length - MAX_EVENTS)
  if (persistenceInitialized) {
    appendPersistentEvent(e, events, nextId, MAX_EVENTS)
  }
}

function normalizeProcessName(ev: any): string {
  if (ev?.processName) return ev.processName
  if (ev?.processId === 0 || ev?.processId === 4) return 'System'
  return 'unknown'
}

function toIsoTimestamp(ev: any): string {
  if (typeof ev?.timestampMs === 'number' && Number.isFinite(ev.timestampMs) && ev.timestampMs > 0) {
    return new Date(ev.timestampMs).toISOString()
  }
  return new Date().toISOString()
}

function normalizeFilePath(addon: any, value: unknown): string {
  const path = typeof value === 'string' ? value : ''
  if (!path) return ''
  try {
    const normalized = addon?.dlp?.kernel_comm?.normalizeFilePath?.(path)
    return typeof normalized === 'string' && normalized ? normalized : path
  } catch {
    return path
  }
}

function normalizeFileDetails(addon: any, details: string): string {
  const marker = ' | QUARANTINE '
  const markerIndex = details.indexOf(marker)
  if (markerIndex < 0) return normalizeFilePath(addon, details)
  return `${normalizeFilePath(addon, details.slice(0, markerIndex))}${marker}${normalizeFilePath(addon, details.slice(markerIndex + marker.length))}`
}

function isInternalAuditPath(path: string): boolean {
  if (!path) return false
  const normalized = path.replace(/\//g, '\\').toLowerCase()
  const queueDirectory = getPersistentEventQueueInfo().directory
    .replace(/\//g, '\\')
    .replace(/\\+$/, '')
    .toLowerCase()
  const userDataDirectory = dirname(queueDirectory).replace(/\\+$/, '')
  const programDataDirectory = join(
    process.env.ProgramData || 'C:\\ProgramData',
    'PersonalSafer'
  ).replace(/\//g, '\\').replace(/\\+$/, '').toLowerCase()
  return normalized === userDataDirectory ||
    normalized.startsWith(`${userDataDirectory}\\`) ||
    normalized === programDataDirectory ||
    normalized.startsWith(`${programDataDirectory}\\`) ||
    normalized.includes('\\personalsafer\\dlp-queue\\')
}

function isInternalFileDetails(details: string): boolean {
  const marker = ' | QUARANTINE '
  const originalPath = details.split(marker, 1)[0]
  return isInternalAuditPath(originalPath)
}

function mirrorFileAudit(addon: any, ev: any, processName: string, fileName: string): void {
  try {
    addon?.audit?.file_audit?.logEvent?.(
      ev.type === 'file_write' ? 1 :
      ev.type === 'file_delete' ? 2 :
      ev.type === 'file_rename' ? 3 : 0,
      processName,
      fileName,
      ev.processId || 0,
      ev.action || 'logged',
      ev.timestampMs || 0
    )
  } catch {}
}

function mirrorNetAudit(addon: any, ev: any, processName: string): void {
  const eventType =
    ev.type === 'http_request' ? 7 :
    ev.type === 'http_response' ? 8 :
    ev.type === 'ftp_command' ? 9 :
    ev.type === 'sni_capture' ? 12 :
    ev.type === 'network_disconnect' ? 6 : 6
  try {
    addon?.audit?.net_audit?.logEvent?.(
      eventType,
      processName,
      ev.url || ev.sniDomain || `${ev.remoteAddress || ''}:${ev.remotePort || 0}`,
      ev.processId || 0,
      ev.remotePort || 0,
      ev.localPort || 0,
      ev.protocol || 0,
      ev.action || 'logged',
      ev.timestampMs || 0
    )
  } catch {}
}

function decodeCompressedHttpBody(ev: any): string | null {
  if (!ev?.bodyPreview || !ev?.contentEncoding || !ev?.url?.startsWith?.('BODY ')) {
    return null
  }

  try {
    const body = Buffer.isBuffer(ev.bodyPreview) ? ev.bodyPreview : Buffer.from(ev.bodyPreview)
    const encoding = String(ev.contentEncoding).toLowerCase()
    let decoded: Buffer
    if (encoding === 'gzip') decoded = gunzipSync(body)
    else if (encoding === 'deflate') decoded = inflateSync(body)
    else return null
    const text = decoded.toString('utf8').replace(/\s+/g, ' ').trim()
    return text ? `${ev.url} | DECODED ${text.slice(0, 512)}` : null
  } catch {
    return null
  }
}

function recordFileEvent(addon: any, ev: any): void {
  const processName = normalizeProcessName(ev)
  const originalPath = normalizeFilePath(addon, ev.originalFileName || ev.fileName || '')
  const quarantinePath = normalizeFilePath(addon, ev.quarantineFileName || '')
  const details = quarantinePath
    ? `${originalPath} | QUARANTINE ${quarantinePath}`
    : originalPath
  if (isInternalFileDetails(details)) return

  mirrorFileAudit(addon, ev, processName, originalPath)
  const timestamp = toIsoTimestamp(ev)
  if (quarantinePath) {
    recordQuarantineCatalogEntry({
      originalPath,
      quarantinePath,
      processName,
      pid: ev.processId || 0,
      eventType: ev.type || 'file_write',
      action: ev.action || 'quarantined',
      timestamp,
    })
  }
  push(createPersistedEvent({
    id: nextId++,
    type: ev.type || 'file_create',
    action: ev.action || 'logged',
    processName,
    pid: ev.processId || 0,
    timestamp,
    details,
  }))
}

function recordNetEvent(addon: any, ev: any): void {
  const addr = `${ev.remoteAddress || ''}:${ev.remotePort || 0}`
  const decodedBody = decodeCompressedHttpBody(ev)
  if (decodedBody) {
    ev.url = decodedBody
  }
  const processName = normalizeProcessName(ev)
  mirrorNetAudit(addon, ev, processName)
  push(createPersistedEvent({
    id: nextId++,
    type: ev.type || 'network_connect',
    action: ev.action || 'logged',
    processName,
    pid: ev.processId || 0,
    timestamp: toIsoTimestamp(ev),
    details: ev.url || ev.sniDomain || addr,
  }))
}

function drainFileBatch(addon: any, kc: any): boolean {
  if (typeof kc.readFileEventsBatch !== 'function') {
    return false
  }

  let any = false
  for (let i = 0; i < 8; i++) {
    let batch: any
    try { batch = kc.readFileEventsBatch() } catch { break }
    if (!batch || !batch.hasEvent || !Array.isArray(batch.events) || batch.events.length === 0) break
    any = true
    for (const ev of batch.events) {
      recordFileEvent(addon, ev)
    }
  }
  return any
}

function drainNetBatch(addon: any, kc: any): boolean {
  if (typeof kc.readNetEventsBatch !== 'function') {
    return false
  }

  let any = false
  for (let i = 0; i < 8; i++) {
    let batch: any
    try { batch = kc.readNetEventsBatch() } catch { break }
    if (!batch || !batch.hasEvent || !Array.isArray(batch.events) || batch.events.length === 0) break
    any = true
    for (const ev of batch.events) {
      recordNetEvent(addon, ev)
    }
  }
  return any
}

function drainOnce(): boolean {
  const addon = getAddon()
  if (!addon || !isAddonLoaded()) return false
  const kc = addon.dlp.kernel_comm
  if (!kc) return false

  let drained = false

  if (!drainFileBatch(addon, kc)) {
    for (let i = 0; i < 64; i++) {
      let ev: any
      try { ev = kc.readFileEvent() } catch { break }
      if (!ev || !ev.hasEvent) break
      drained = true
      recordFileEvent(addon, ev)
    }
  } else drained = true

  if (!drainNetBatch(addon, kc)) {
    for (let i = 0; i < 32; i++) {
      let ev: any
      try { ev = kc.readNetEvent() } catch { break }
      if (!ev || !ev.hasEvent) break
      drained = true
      recordNetEvent(addon, ev)
    }
  } else drained = true
  return drained
}

export function recordExternalNetEvent(ev: any): void {
  const addon = getAddon()
  const processName = ev?.processName || 'local_proxy'
  mirrorNetAudit(addon, ev, processName)
  push(createPersistedEvent({
    id: nextId++,
    type: ev?.type || 'network_connect',
    action: ev?.action || 'logged',
    processName,
    pid: ev?.processId || process.pid,
    timestamp: ev?.timestamp || new Date().toISOString(),
    details: ev?.details || ev?.url || ev?.sniDomain || `${ev?.remoteAddress || ''}:${ev?.remotePort || 0}`,
  }))
}

export function initializeEventPersistence(): void {
  if (persistenceInitialized) {
    return
  }

  const loaded = initializePersistentEventQueue(MAX_EVENTS)
  const addon = getAddon()
  const migratedEvents = loaded.events.flatMap((event) => {
    if (!event.type.startsWith('file')) return [event]
    const details = normalizeFileDetails(addon, event.details || '')
    return isInternalFileDetails(details) ? [] : [{ ...event, details }]
  })
  events.length = 0
  events.push(...migratedEvents)
  nextId = loaded.nextId
  persistenceInitialized = true
  if (migratedEvents.length !== loaded.events.length ||
      migratedEvents.some((event, index) => event.details !== loaded.events[index]?.details)) {
    compactPersistentEventQueue(events, nextId, MAX_EVENTS)
  }
}

export function flushEventPersistence(): void {
  if (!persistenceInitialized) {
    return
  }
  compactPersistentEventQueue(events, nextId, MAX_EVENTS)
}

export function getEventPersistenceInfo() {
  return getPersistentEventQueueInfo()
}

export function startEventPolling(intervalMs: number = 1000): void {
  if (pollingActive) return
  pollingActive = true
  pollLoopPromise = (async () => {
    while (pollingActive) {
      try {
        const addon = getAddon()
        const kc = addon?.dlp?.kernel_comm
        if (addon && isAddonLoaded() && kc && typeof kc.waitForEvents === 'function') {
          await kc.waitForEvents(intervalMs)
        } else {
          await new Promise((resolve) => setTimeout(resolve, intervalMs))
        }
      } catch {
        await new Promise((resolve) => setTimeout(resolve, intervalMs))
      }

      if (!pollingActive) break
      let drained = false
      try { drained = drainOnce() } catch {}
      if (drained) {
        await new Promise((resolve) => setTimeout(resolve, 10))
      }
    }
  })()
  console.log('[DlpEvents] polling started')
}

export function stopEventPolling(): void {
  pollingActive = false
  pollLoopPromise = null
}

export function isPolling(): boolean {
  return pollingActive
}

export function getEvents(limit?: number): DlpEvent[] {
  const arr = events.slice().reverse()
  return typeof limit === 'number' ? arr.slice(0, limit) : arr
}

export function clearEvents(): void {
  events.length = 0
  nextId = 1
  clearPersistentEventQueue()
}

export function getStats() {
  let fileEvents = 0
  let networkEvents = 0
  let blockedEvents = 0
  for (const e of events) {
    if (e.type.startsWith('file')) fileEvents++
    else if (e.type.startsWith('network') || e.type.startsWith('http') || e.type.startsWith('ftp') || e.type.startsWith('sni') || e.type.startsWith('websocket')) networkEvents++
    if (e.action === 'blocked') blockedEvents++
  }
  return {
    totalEvents: events.length,
    fileEvents,
    networkEvents,
    registryEvents: 0,
    blockedEvents,
  }
}

export function leaseQueuedEvents(limit: number = 100): PersistedDlpEvent[] {
  const leased = leasePendingEvents(events, limit)
  if (leased.length === 0) {
    return leased
  }

  const now = new Date().toISOString()
  const leaseId = `lease-${Date.now()}-${Math.random().toString(16).slice(2, 10)}`
  const leaseExpireAt = new Date(Date.now() + 60_000).toISOString()

  for (const event of leased) {
    updatePersistentEventState(events, event.id, {
      state: 'sending',
      firstAttemptAt: event.firstAttemptAt || now,
      lastAttemptAt: now,
      leaseId,
      leaseExpireAt,
      error: null,
    }, nextId, MAX_EVENTS)
  }

  return leased
}

export function markQueuedEventSent(
  eventId: number,
  statusCode?: number,
  remoteEventId?: string,
  ackAt?: string
): void {
  updatePersistentEventState(events, eventId, {
    state: 'sent',
    ackAt: ackAt || new Date().toISOString(),
    nextAttemptAt: null,
    leaseId: null,
    leaseExpireAt: null,
    error: null,
    lastHttpStatus: typeof statusCode === 'number' ? statusCode : null,
    lastRemoteId: remoteEventId || null,
  }, nextId, MAX_EVENTS)
}

export function markQueuedEventFailed(
  eventId: number,
  error: string,
  retryCount?: number,
  nextAttemptAt?: string,
  statusCode?: number
): void {
  const event = events.find((item) => item.id === eventId)
  const resolvedRetryCount = retryCount ?? ((event?.retryCount || 0) + 1)
  updatePersistentEventState(events, eventId, {
    state: 'failed',
    retryCount: resolvedRetryCount,
    lastAttemptAt: new Date().toISOString(),
    nextAttemptAt: nextAttemptAt || null,
    leaseId: null,
    leaseExpireAt: null,
    error,
    lastHttpStatus: typeof statusCode === 'number' ? statusCode : (event?.lastHttpStatus ?? null),
  }, nextId, MAX_EVENTS)
}

export function markQueuedEventAbandoned(eventId: number, error: string, statusCode?: number): void {
  updatePersistentEventState(events, eventId, {
    state: 'abandoned',
    lastAttemptAt: new Date().toISOString(),
    nextAttemptAt: null,
    leaseId: null,
    leaseExpireAt: null,
    error,
    lastHttpStatus: typeof statusCode === 'number' ? statusCode : null,
  }, nextId, MAX_EVENTS)
}

export function getQueuedEventById(eventId: number): PersistedDlpEvent | undefined {
  return events.find((item) => item.id === eventId)
}

export function patchQueuedEvent(
  eventId: number,
  patch: Partial<Pick<PersistedDlpEvent, 'state' | 'retryCount' | 'firstAttemptAt' | 'lastAttemptAt' | 'ackAt' | 'nextAttemptAt' | 'leaseId' | 'leaseExpireAt' | 'error' | 'lastHttpStatus' | 'lastRemoteId' | 'sinkStates'>>
): void {
  updatePersistentEventState(events, eventId, patch, nextId, MAX_EVENTS)
}

export function listQueuedEventsByState(state: 'abandoned' | 'sent' | 'failed' | 'pending' | 'sending', limit: number = 100): PersistedDlpEvent[] {
  return events
    .filter((item) => item.state === state)
    .slice()
    .reverse()
    .slice(0, Math.max(0, limit))
}

export type { PersistedSinkDeliveryStatus }

export function requeueAbandonedEvents(limit: number = 100): number {
  const targets = events.filter((item) => item.state === 'abandoned').slice(0, Math.max(0, limit))
  const now = new Date().toISOString()
  for (const event of targets) {
    updatePersistentEventState(events, event.id, {
      state: 'pending',
      lastAttemptAt: now,
      nextAttemptAt: null,
      leaseId: null,
      leaseExpireAt: null,
      error: null,
    }, nextId, MAX_EVENTS)
  }
  return targets.length
}

export function getPersistentQueueStats() {
  return getPersistentEventQueueStats(events)
}
