import { app } from 'electron'
import {
  appendFileSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  statSync,
  unlinkSync,
  writeFileSync,
} from 'node:fs'
import { dirname, join } from 'node:path'
import { getAddon, isAddonLoaded } from './native_loader'

export interface QuarantineCatalogEntry {
  id: number
  originalPath: string
  quarantinePath: string
  processName: string
  pid: number
  eventType: string
  action: string
  firstSeenAt: string
  lastSeenAt: string
  occurrenceCount: number
  status: 'present' | 'missing'
  size: number | null
}

type QuarantineCatalogQuery = {
  search?: string
  limit?: number
}

const entries: QuarantineCatalogEntry[] = []
let nextId = 1
let initialized = false
let journalRecords = 0

function getCatalogDirectory(): string {
  return join(app.getPath('userData'), 'quarantine-catalog')
}

function getSnapshotPath(): string {
  return join(getCatalogDirectory(), 'snapshot.json')
}

function getJournalPath(): string {
  return join(getCatalogDirectory(), 'journal.ndjson')
}

function ensureDirectory(): void {
  mkdirSync(getCatalogDirectory(), { recursive: true })
}

function writeSnapshot(): void {
  ensureDirectory()
  const path = getSnapshotPath()
  const temporaryPath = `${path}.tmp`
  writeFileSync(temporaryPath, JSON.stringify({ nextId, entries, updatedAt: new Date().toISOString() }, null, 2), 'utf8')
  renameSync(temporaryPath, path)
  writeFileSync(getJournalPath(), '', 'utf8')
  journalRecords = 0
}

function applyEntry(entry: QuarantineCatalogEntry): void {
  const index = entries.findIndex((item) => item.id === entry.id)
  if (index >= 0) entries[index] = entry
  else entries.push(entry)
  if (entry.id >= nextId) nextId = entry.id + 1
}

function persistEntry(entry: QuarantineCatalogEntry): void {
  applyEntry(entry)
  ensureDirectory()
  appendFileSync(getJournalPath(), `${JSON.stringify({ op: 'upsert', entry })}\n`, 'utf8')
  journalRecords++
  if (journalRecords >= 64) writeSnapshot()
}

export function initializeQuarantineCatalog(): void {
  if (initialized) return
  ensureDirectory()
  entries.length = 0
  nextId = 1

  if (existsSync(getSnapshotPath())) {
    try {
      const snapshot = JSON.parse(readFileSync(getSnapshotPath(), 'utf8'))
      if (Array.isArray(snapshot?.entries)) {
        for (const entry of snapshot.entries) applyEntry(entry)
      }
      if (typeof snapshot?.nextId === 'number' && snapshot.nextId > nextId) nextId = snapshot.nextId
    } catch {
      entries.length = 0
      nextId = 1
    }
  }

  if (existsSync(getJournalPath())) {
    try {
      const lines = readFileSync(getJournalPath(), 'utf8').split(/\r?\n/).filter(Boolean)
      for (const line of lines) {
        const record = JSON.parse(line)
        if (record?.op === 'upsert' && record.entry) applyEntry(record.entry)
      }
      journalRecords = lines.length
    } catch {
      journalRecords = 0
    }
  }

  initialized = true
}

export function recordQuarantineCatalogEntry(input: {
  originalPath: string
  quarantinePath: string
  processName: string
  pid: number
  eventType: string
  action: string
  timestamp: string
}): void {
  if (!input.originalPath || !input.quarantinePath) return
  if (!initialized) initializeQuarantineCatalog()

  const key = input.quarantinePath.toLowerCase()
  const existing = entries.find((entry) => entry.quarantinePath.toLowerCase() === key)
  const entry: QuarantineCatalogEntry = existing
    ? {
        ...existing,
        originalPath: input.originalPath,
        processName: input.processName,
        pid: input.pid,
        eventType: input.eventType,
        action: input.action,
        lastSeenAt: input.timestamp,
        occurrenceCount: existing.occurrenceCount + 1,
      }
    : {
        id: nextId++,
        originalPath: input.originalPath,
        quarantinePath: input.quarantinePath,
        processName: input.processName,
        pid: input.pid,
        eventType: input.eventType,
        action: input.action,
        firstSeenAt: input.timestamp,
        lastSeenAt: input.timestamp,
        occurrenceCount: 1,
        status: 'missing',
        size: null,
      }

  persistEntry(entry)
}

function getPresentEntry(id: number): QuarantineCatalogEntry {
  if (!initialized) initializeQuarantineCatalog()
  const entry = entries.find((item) => item.id === id)
  if (!entry) throw new Error('quarantine record not found')
  const stat = statSync(entry.quarantinePath)
  if (!stat.isFile()) throw new Error('quarantine target is not a regular file')
  return entry
}

function setMaintenanceMode(enabled: boolean): void {
  const addon = getAddon()
  if (!addon || !isAddonLoaded() || !addon.dlp.kernel_comm?.setProtectionMaintenanceMode?.(enabled)) {
    throw new Error(enabled ? 'cannot enter protected maintenance mode' : 'cannot relock protection')
  }
}

export function restoreQuarantineEntry(id: number): QuarantineCatalogEntry {
  const entry = getPresentEntry(id)
  setMaintenanceMode(true)
  try {
    mkdirSync(dirname(entry.originalPath), { recursive: true })
    copyFileSync(entry.quarantinePath, entry.originalPath)
    unlinkSync(entry.quarantinePath)
    const updated: QuarantineCatalogEntry = {
      ...entry,
      action: 'restored',
      lastSeenAt: new Date().toISOString(),
      status: 'missing',
      size: null,
    }
    persistEntry(updated)
    return updated
  } finally {
    setMaintenanceMode(false)
  }
}

export function purgeQuarantineEntry(id: number): QuarantineCatalogEntry {
  const entry = getPresentEntry(id)
  setMaintenanceMode(true)
  try {
    unlinkSync(entry.quarantinePath)
    const updated: QuarantineCatalogEntry = {
      ...entry,
      action: 'purged',
      lastSeenAt: new Date().toISOString(),
      status: 'missing',
      size: null,
    }
    persistEntry(updated)
    return updated
  } finally {
    setMaintenanceMode(false)
  }
}

function refreshFileState(entry: QuarantineCatalogEntry): QuarantineCatalogEntry {
  try {
    const stat = statSync(entry.quarantinePath)
    return { ...entry, status: 'present', size: stat.size }
  } catch {
    return { ...entry, status: 'missing', size: null }
  }
}

export function listQuarantineCatalog(query: QuarantineCatalogQuery = {}): QuarantineCatalogEntry[] {
  if (!initialized) initializeQuarantineCatalog()
  const search = String(query.search || '').trim().toLowerCase()
  const limit = Math.min(Math.max(Number(query.limit) || 200, 1), 1000)
  return entries
    .slice()
    .sort((a, b) => Date.parse(b.lastSeenAt) - Date.parse(a.lastSeenAt))
    .filter((entry) => !search ||
      entry.originalPath.toLowerCase().includes(search) ||
      entry.quarantinePath.toLowerCase().includes(search) ||
      entry.processName.toLowerCase().includes(search))
    .slice(0, limit)
    .map(refreshFileState)
}

export function flushQuarantineCatalog(): void {
  if (initialized) writeSnapshot()
}

export function getQuarantineCatalogInfo() {
  return {
    directory: getCatalogDirectory(),
    snapshotPath: getSnapshotPath(),
    journalPath: getJournalPath(),
    count: entries.length,
  }
}
