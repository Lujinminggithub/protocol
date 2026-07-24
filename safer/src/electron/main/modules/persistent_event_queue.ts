import { app } from 'electron'
import { appendFileSync, existsSync, mkdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

export type PersistentEventState = 'pending' | 'sending' | 'sent' | 'failed' | 'abandoned'
export type PersistedSinkDeliveryState = 'pending' | 'acked' | 'retry' | 'dropped'

export interface PersistedSinkDeliveryStatus {
  state: PersistedSinkDeliveryState
  retryCount: number
  ackAt?: string | null
  lastAttemptAt?: string | null
  nextAttemptAt?: string | null
  error?: string | null
  lastHttpStatus?: number | null
  remoteEventId?: string | null
  receiptVerified?: boolean | null
}

export interface PersistedDlpEvent {
  id: number
  type: string
  action: string
  processName: string
  pid: number
  timestamp: string
  details: string
  state: PersistentEventState
  retryCount: number
  firstAttemptAt?: string | null
  lastAttemptAt?: string | null
  ackAt?: string | null
  nextAttemptAt?: string | null
  leaseId?: string | null
  leaseExpireAt?: string | null
  error?: string | null
  lastHttpStatus?: number | null
  lastRemoteId?: string | null
  sinkStates?: Record<string, PersistedSinkDeliveryStatus>
}

type QueueJournalRecord =
  | { op: 'append'; event: PersistedDlpEvent }
  | {
    op: 'state'
    id: number
    state: PersistentEventState
    retryCount?: number
    firstAttemptAt?: string | null
    lastAttemptAt?: string | null
    ackAt?: string | null
    nextAttemptAt?: string | null
    leaseId?: string | null
    leaseExpireAt?: string | null
    error?: string | null
    lastHttpStatus?: number | null
    lastRemoteId?: string | null
    sinkStates?: Record<string, PersistedSinkDeliveryStatus>
  }

const QUEUE_DIR = 'dlp-queue'
const SNAPSHOT_FILE = 'snapshot.json'
const JOURNAL_FILE = 'journal.ndjson'
const COMPACT_EVERY_RECORDS = 128
const COMPACT_FILE_BYTES = 1024 * 1024

let initialized = false
let appendedSinceCompact = 0

function hasOwn<T extends object>(value: T, key: keyof any): boolean {
  return Object.prototype.hasOwnProperty.call(value, key)
}

function getQueueDir(): string {
  return join(app.getPath('userData'), QUEUE_DIR)
}

function getSnapshotPath(): string {
  return join(getQueueDir(), SNAPSHOT_FILE)
}

function getJournalPath(): string {
  return join(getQueueDir(), JOURNAL_FILE)
}

function ensureQueueDir(): void {
  mkdirSync(getQueueDir(), { recursive: true })
}

function writeAtomicJson(path: string, data: unknown): void {
  const temp = `${path}.tmp`
  writeFileSync(temp, JSON.stringify(data, null, 2), 'utf8')
  renameSync(temp, path)
}

function shouldCompactJournal(): boolean {
  if (appendedSinceCompact >= COMPACT_EVERY_RECORDS) {
    return true
  }
  if (!existsSync(getJournalPath())) {
    return false
  }
  try {
    return statSync(getJournalPath()).size >= COMPACT_FILE_BYTES
  } catch {
    return false
  }
}

function appendJournalRecord(record: QueueJournalRecord): void {
  ensureQueueDir()
  appendFileSync(getJournalPath(), `${JSON.stringify(record)}\n`, 'utf8')
  appendedSinceCompact++
}

function applyStateRecord(events: PersistedDlpEvent[], record: Extract<QueueJournalRecord, { op: 'state' }>): void {
  const event = events.find((item) => item.id === record.id)
  if (!event) {
    return
  }

  event.state = record.state
  if (typeof record.retryCount === 'number') event.retryCount = record.retryCount
  if (hasOwn(record, 'firstAttemptAt')) event.firstAttemptAt = record.firstAttemptAt ?? null
  if (hasOwn(record, 'lastAttemptAt')) event.lastAttemptAt = record.lastAttemptAt ?? null
  if (hasOwn(record, 'ackAt')) event.ackAt = record.ackAt ?? null
  if (hasOwn(record, 'nextAttemptAt')) event.nextAttemptAt = record.nextAttemptAt ?? null
  if (hasOwn(record, 'leaseId')) event.leaseId = record.leaseId ?? null
  if (hasOwn(record, 'leaseExpireAt')) event.leaseExpireAt = record.leaseExpireAt ?? null
  if (hasOwn(record, 'error')) event.error = record.error ?? null
  if (hasOwn(record, 'lastHttpStatus')) event.lastHttpStatus = record.lastHttpStatus ?? null
  if (hasOwn(record, 'lastRemoteId')) event.lastRemoteId = record.lastRemoteId ?? null
  if (hasOwn(record, 'sinkStates')) event.sinkStates = record.sinkStates ? { ...record.sinkStates } : {}
}

function trimEvents(events: PersistedDlpEvent[], maxEvents: number): PersistedDlpEvent[] {
  if (events.length <= maxEvents) {
    return events
  }

  const pending = events.filter((event) => event.state !== 'sent')
  if (pending.length >= maxEvents) {
    return pending.slice(pending.length - maxEvents)
  }

  const sentBudget = maxEvents - pending.length
  const sent = events.filter((event) => event.state === 'sent')
  return [...sent.slice(Math.max(0, sent.length - sentBudget)), ...pending]
}

function recoverInflightEvents(events: PersistedDlpEvent[]): void {
  for (const event of events) {
    if (event.state !== 'sending') {
      continue
    }

    event.state = 'failed'
    event.leaseId = null
    event.leaseExpireAt = null
    event.nextAttemptAt = null
    event.error = event.error || 'recovered after restart while inflight'
    event.lastAttemptAt = event.lastAttemptAt || new Date().toISOString()
  }
}

export function initializePersistentEventQueue(maxEvents: number): { events: PersistedDlpEvent[]; nextId: number } {
  ensureQueueDir()
  initialized = true
  appendedSinceCompact = 0

  let events: PersistedDlpEvent[] = []
  let nextId = 1

  if (existsSync(getSnapshotPath())) {
    try {
      const snapshot = JSON.parse(readFileSync(getSnapshotPath(), 'utf8'))
      if (Array.isArray(snapshot?.events)) {
        events = snapshot.events
      }
      if (typeof snapshot?.nextId === 'number' && snapshot.nextId > 0) {
        nextId = snapshot.nextId
      }
    } catch {
      events = []
      nextId = 1
    }
  }

  if (existsSync(getJournalPath())) {
    const rawJournal = readFileSync(getJournalPath(), 'utf8')
    const lines = rawJournal.split(/\r?\n/).map((line) => line.trim()).filter(Boolean)
    const validLines: string[] = []
    for (let index = 0; index < lines.length; index++) {
      const line = lines[index]
      try {
        const record = JSON.parse(line) as QueueJournalRecord
        if (record.op === 'append') {
          events.push(record.event)
          if (record.event.id >= nextId) {
            nextId = record.event.id + 1
          }
        } else if (record.op === 'state') {
          applyStateRecord(events, record)
        }
        validLines.push(line)
      } catch {
        const corruptPath = `${getJournalPath()}.corrupt-${Date.now()}`
        writeFileSync(corruptPath, `${lines.slice(index).join('\n')}\n`, 'utf8')
        writeFileSync(getJournalPath(), validLines.length > 0 ? `${validLines.join('\n')}\n` : '', 'utf8')
        break
      }
    }
    appendedSinceCompact = validLines.length
  }

  events = trimEvents(events, maxEvents)
  recoverInflightEvents(events)
  return { events, nextId }
}

export function createPersistedEvent(base: Omit<PersistedDlpEvent, 'state' | 'retryCount'>): PersistedDlpEvent {
  return {
    ...base,
    state: 'pending',
    retryCount: 0,
    firstAttemptAt: null,
    lastAttemptAt: null,
    ackAt: null,
    nextAttemptAt: null,
    leaseId: null,
    leaseExpireAt: null,
    error: null,
    lastHttpStatus: null,
    lastRemoteId: null,
    sinkStates: {},
  }
}

export function appendPersistentEvent(
  event: PersistedDlpEvent,
  currentEvents: PersistedDlpEvent[],
  nextId: number,
  maxEvents: number
): void {
  if (!initialized) {
    return
  }

  appendJournalRecord({ op: 'append', event })
  if (shouldCompactJournal()) {
    compactPersistentEventQueue(currentEvents, nextId, maxEvents)
  }
}

export function updatePersistentEventState(
  currentEvents: PersistedDlpEvent[],
  eventId: number,
  patch: Partial<
    Pick<
      PersistedDlpEvent,
      | 'state'
      | 'retryCount'
      | 'firstAttemptAt'
      | 'lastAttemptAt'
      | 'ackAt'
      | 'nextAttemptAt'
      | 'leaseId'
      | 'leaseExpireAt'
      | 'error'
      | 'lastHttpStatus'
      | 'lastRemoteId'
      | 'sinkStates'
    >
  >,
  nextId: number,
  maxEvents: number
): void {
  if (!initialized) {
    return
  }

  const event = currentEvents.find((item) => item.id === eventId)
  if (!event) {
    return
  }

  if (patch.state) event.state = patch.state
  if (typeof patch.retryCount === 'number') event.retryCount = patch.retryCount
  if (hasOwn(patch, 'firstAttemptAt')) event.firstAttemptAt = patch.firstAttemptAt ?? null
  if (hasOwn(patch, 'lastAttemptAt')) event.lastAttemptAt = patch.lastAttemptAt ?? null
  if (hasOwn(patch, 'ackAt')) event.ackAt = patch.ackAt ?? null
  if (hasOwn(patch, 'nextAttemptAt')) event.nextAttemptAt = patch.nextAttemptAt ?? null
  if (hasOwn(patch, 'leaseId')) event.leaseId = patch.leaseId ?? null
  if (hasOwn(patch, 'leaseExpireAt')) event.leaseExpireAt = patch.leaseExpireAt ?? null
  if (hasOwn(patch, 'error')) event.error = patch.error ?? null
  if (hasOwn(patch, 'lastHttpStatus')) event.lastHttpStatus = patch.lastHttpStatus ?? null
  if (hasOwn(patch, 'lastRemoteId')) event.lastRemoteId = patch.lastRemoteId ?? null
  if (hasOwn(patch, 'sinkStates')) event.sinkStates = patch.sinkStates ? { ...patch.sinkStates } : {}

  appendJournalRecord({
    op: 'state',
    id: eventId,
    state: event.state,
    retryCount: event.retryCount,
    firstAttemptAt: event.firstAttemptAt ?? null,
    lastAttemptAt: event.lastAttemptAt,
    ackAt: event.ackAt,
    nextAttemptAt: event.nextAttemptAt,
    leaseId: event.leaseId,
    leaseExpireAt: event.leaseExpireAt,
    error: event.error,
    lastHttpStatus: event.lastHttpStatus,
    lastRemoteId: event.lastRemoteId,
    sinkStates: event.sinkStates ? { ...event.sinkStates } : {},
  })

  if (shouldCompactJournal()) {
    compactPersistentEventQueue(currentEvents, nextId, maxEvents)
  }
}

export function leasePendingEvents(currentEvents: PersistedDlpEvent[], limit: number): PersistedDlpEvent[] {
  const now = Date.now()
  return currentEvents
    .filter((event) => {
      if (event.state === 'sent' || event.state === 'abandoned') return false
      if (event.state === 'sending') {
        if (!event.leaseExpireAt) return true
        const leaseExpireAt = Date.parse(event.leaseExpireAt)
        return !Number.isFinite(leaseExpireAt) || leaseExpireAt <= now
      }
      if (!event.nextAttemptAt) return true
      const nextAttempt = Date.parse(event.nextAttemptAt)
      return !Number.isFinite(nextAttempt) || nextAttempt <= now
    })
    .slice(0, limit)
}

export function getPersistentEventQueueStats(currentEvents: PersistedDlpEvent[]) {
  let pending = 0
  let sending = 0
  let sent = 0
  let failed = 0
  let abandoned = 0
  for (const event of currentEvents) {
    if (event.state === 'pending') pending++
    else if (event.state === 'sending') sending++
    else if (event.state === 'sent') sent++
    else if (event.state === 'failed') failed++
    else if (event.state === 'abandoned') abandoned++
  }
  return { pending, sending, sent, failed, abandoned, total: currentEvents.length }
}

export function compactPersistentEventQueue(
  currentEvents: PersistedDlpEvent[],
  nextId: number,
  maxEvents: number
): void {
  if (!initialized) {
    return
  }

  ensureQueueDir()
  const snapshotEvents = trimEvents(currentEvents.slice(), maxEvents)
  writeAtomicJson(getSnapshotPath(), {
    nextId,
    events: snapshotEvents,
    updatedAt: new Date().toISOString(),
  })
  writeFileSync(getJournalPath(), '', 'utf8')
  appendedSinceCompact = 0
}

export function clearPersistentEventQueue(): void {
  if (!initialized) {
    return
  }

  try {
    if (existsSync(getSnapshotPath())) unlinkSync(getSnapshotPath())
  } catch {}
  try {
    if (existsSync(getJournalPath())) unlinkSync(getJournalPath())
  } catch {}
  appendedSinceCompact = 0
}

export function getPersistentEventQueueInfo(): { directory: string; snapshotPath: string; journalPath: string } {
  return {
    directory: getQueueDir(),
    snapshotPath: getSnapshotPath(),
    journalPath: getJournalPath(),
  }
}
