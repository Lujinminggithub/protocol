import { app } from 'electron'
import { appendFileSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { CentralHttpEventSink } from './event_sink_central_http'
import { HttpEventSink } from './event_sink_http'
import { LocalTestEventSink } from './event_sink_local_test'
import {
  createDefaultDeliveryPipelineConfig,
  createDefaultDeliverySinkConfig,
  type DeliveryAttemptResult,
  type DeliveryDecision,
  type DeliveryEventResult,
  type DeliveryPipelineConfig,
  type DeliveryPipelineStrategy,
  type DeliverySinkConfig,
  type EventSink,
} from './event_sink'
import {
  getQueuedEventById,
  getPersistentQueueStats,
  leaseQueuedEvents,
  listQueuedEventsByState,
  markQueuedEventAbandoned,
  markQueuedEventFailed,
  markQueuedEventSent,
  patchQueuedEvent,
  type PersistedDlpEvent,
  type PersistedSinkDeliveryStatus,
} from './dlp_events'

type SinkBackpressureState = {
  sinkId: string
  pausedUntil?: string
  maxBatchSize?: number
  reason?: string
  lastUpdatedAt?: string
}

type DeliveryStatus = {
  running: boolean
  intervalMs: number
  batchSize: number
  maxRetryCount: number
  deliveredCount: number
  failedCount: number
  pendingCount: number
  sendingCount: number
  sentCount: number
  abandonedCount: number
  queueDepth: number
  lastRunAt?: string
  lastSuccessAt?: string
  lastError?: string
  lastResults?: DeliveryAttemptResult[]
  outboxDir: string
  deliveredPath: string
  deadLetterPath: string
  pipeline: DeliveryPipelineConfig
  sinkBackpressure: Record<string, SinkBackpressureState>
}

type DeadLetterRecord = {
  abandonedAt: string
  decision: DeliveryDecision
  error: string
  statusCode: number
  results: DeliveryAttemptResult[]
  event: PersistedDlpEvent
}

type SinkCheckpointState = 'pending' | 'sending' | 'completed' | 'failed' | 'dropped'

type SinkCheckpointStatus = {
  state: SinkCheckpointState
  updatedAt: string
  eventIds: number[]
  deliveredEventIds: number[]
  retryEventIds: number[]
  droppedEventIds: number[]
  statusCode?: number
  error?: string
  receiptVerified?: boolean
}

type DeliveryBatchCheckpoint = {
  batchId: string
  createdAt: string
  updatedAt: string
  strategy: DeliveryPipelineStrategy
  eventIds: number[]
  sinkIds: string[]
  state: 'created' | 'inflight' | 'partial' | 'completed' | 'recovered'
  sinkStatuses: Record<string, SinkCheckpointStatus>
}

type DeliveryCheckpointStore = {
  version: 1
  batches: Record<string, DeliveryBatchCheckpoint>
}

type PipelineHistoryEntry = {
  changedAt: string
  reason: string
  pipeline: DeliveryPipelineConfig
}

type DeliveryExportReport = {
  generatedAt: string
  reason: string
  status: DeliveryStatus
  queueInfo: ReturnType<typeof getDeliveredQueueInfo>
  checkpoints: DeliveryBatchCheckpoint[]
  archivedCheckpoints: DeliveryBatchCheckpoint[]
  deadLetterQueue: PersistedDlpEvent[]
  deadLetterRecords: DeadLetterRecord[]
  deliveredRecords: Array<{ deliveredAt: string; results: DeliveryAttemptResult[]; event: PersistedDlpEvent }>
}

const DELIVERY_DIR = 'dlp-delivery'
const DELIVERED_FILE = 'delivered.ndjson'
const DEAD_LETTER_FILE = 'dead-letter.ndjson'
const SINK_CONFIG_FILE = 'sink.json'
const BACKPRESSURE_FILE = 'backpressure.json'
const CHECKPOINT_FILE = 'checkpoints.json'
const CHECKPOINT_ARCHIVE_FILE = 'checkpoint-archive.ndjson'
const PIPELINE_HISTORY_FILE = 'pipeline-history.ndjson'
const REPORTS_DIR = 'reports'
const DEFAULT_MAX_RETRY_COUNT = 8
const MAX_COMPLETED_CHECKPOINTS = 64

let workerRunning = false
let workerPromise: Promise<void> | null = null
let pipelineConfigLoaded = false
let checkpointsLoaded = false
let checkpointStore: DeliveryCheckpointStore = {
  version: 1,
  batches: {},
}

const workerStatus: DeliveryStatus = {
  running: false,
  intervalMs: 5000,
  batchSize: 100,
  maxRetryCount: DEFAULT_MAX_RETRY_COUNT,
  deliveredCount: 0,
  failedCount: 0,
  pendingCount: 0,
  sendingCount: 0,
  sentCount: 0,
  abandonedCount: 0,
  queueDepth: 0,
  outboxDir: '',
  deliveredPath: '',
  deadLetterPath: '',
  pipeline: createDefaultDeliveryPipelineConfig(),
  sinkBackpressure: {},
}

function getOutboxDir(): string {
  return join(app.getPath('userData'), DELIVERY_DIR)
}

function getDeliveredPath(): string {
  return join(getOutboxDir(), DELIVERED_FILE)
}

function getDeadLetterPath(): string {
  return join(getOutboxDir(), DEAD_LETTER_FILE)
}

function getSinkConfigPath(): string {
  return join(getOutboxDir(), SINK_CONFIG_FILE)
}

function getBackpressurePath(): string {
  return join(getOutboxDir(), BACKPRESSURE_FILE)
}

function getCheckpointPath(): string {
  return join(getOutboxDir(), CHECKPOINT_FILE)
}

function getCheckpointArchivePath(): string {
  return join(getOutboxDir(), CHECKPOINT_ARCHIVE_FILE)
}

function getPipelineHistoryPath(): string {
  return join(getOutboxDir(), PIPELINE_HISTORY_FILE)
}

function getReportsDir(): string {
  return join(getOutboxDir(), REPORTS_DIR)
}

function ensureOutboxDir(): void {
  mkdirSync(getOutboxDir(), { recursive: true })
}

function writeAtomicJson(path: string, data: unknown): void {
  const temp = `${path}.tmp`
  writeFileSync(temp, JSON.stringify(data, null, 2), 'utf8')
  renameSync(temp, path)
}

function sanitizeSinkId(candidate: string, fallback: string): string {
  const value = candidate.trim().replace(/[^a-zA-Z0-9._-]/g, '-')
  return value || fallback
}

function normalizeSinkConfig(input: Partial<DeliverySinkConfig> | DeliverySinkConfig, index: number = 0): DeliverySinkConfig {
  const base = createDefaultDeliverySinkConfig()
  const kind = input.kind || base.kind
  const sinkId = sanitizeSinkId(
    typeof (input as any).id === 'string' ? (input as any).id : '',
    `${kind}-${index + 1}`
  )

  if (kind === 'local-test') {
    return {
      id: sinkId,
      kind: 'local-test',
      enabled: input.enabled === true,
      label: typeof (input as any).label === 'string' ? (input as any).label : sinkId,
    }
  }

  if (kind === 'central-http') {
    return {
      id: sinkId,
      kind: 'central-http',
      enabled: input.enabled === true,
      endpointUrl: typeof (input as any).endpointUrl === 'string' ? (input as any).endpointUrl : (base as any).endpointUrl,
      timeoutMs: typeof (input as any).timeoutMs === 'number' ? (input as any).timeoutMs : (base as any).timeoutMs,
      headers: typeof (input as any).headers === 'object' && (input as any).headers
        ? { ...(input as any).headers }
        : { ...(base as any).headers },
      nodeId: typeof (input as any).nodeId === 'string' && (input as any).nodeId.trim()
        ? String((input as any).nodeId).trim()
        : `${process.env.COMPUTERNAME || 'node'}-${process.pid}`,
      tenantId: typeof (input as any).tenantId === 'string' ? (input as any).tenantId : '',
      apiKey: typeof (input as any).apiKey === 'string' ? (input as any).apiKey : '',
      requestSigningKey: typeof (input as any).requestSigningKey === 'string' ? (input as any).requestSigningKey : '',
      receiptSigningKey: typeof (input as any).receiptSigningKey === 'string' ? (input as any).receiptSigningKey : '',
      receiptSignatureHeader: typeof (input as any).receiptSignatureHeader === 'string'
        ? (input as any).receiptSignatureHeader
        : 'x-ps-receipt-signature',
      requireSignedReceipt: (input as any).requireSignedReceipt === true,
      allowInvalidTls: (input as any).allowInvalidTls === true,
      caCertificatePath: typeof (input as any).caCertificatePath === 'string' ? (input as any).caCertificatePath : '',
      clientCertificatePath: typeof (input as any).clientCertificatePath === 'string' ? (input as any).clientCertificatePath : '',
      clientKeyPath: typeof (input as any).clientKeyPath === 'string' ? (input as any).clientKeyPath : '',
      clientKeyPassphrase: typeof (input as any).clientKeyPassphrase === 'string' ? (input as any).clientKeyPassphrase : '',
    }
  }

  return {
    id: sinkId,
    kind: 'http',
    enabled: input.enabled === true,
    endpointUrl: typeof (input as any).endpointUrl === 'string' ? (input as any).endpointUrl : (base as any).endpointUrl,
    timeoutMs: typeof (input as any).timeoutMs === 'number' ? (input as any).timeoutMs : (base as any).timeoutMs,
    headers: typeof (input as any).headers === 'object' && (input as any).headers
      ? { ...(input as any).headers }
      : { ...(base as any).headers },
    allowInvalidTls: (input as any).allowInvalidTls === true,
    caCertificatePath: typeof (input as any).caCertificatePath === 'string' ? (input as any).caCertificatePath : '',
    clientCertificatePath: typeof (input as any).clientCertificatePath === 'string' ? (input as any).clientCertificatePath : '',
    clientKeyPath: typeof (input as any).clientKeyPath === 'string' ? (input as any).clientKeyPath : '',
    clientKeyPassphrase: typeof (input as any).clientKeyPassphrase === 'string' ? (input as any).clientKeyPassphrase : '',
  }
}

function normalizePipelineConfig(input: any): DeliveryPipelineConfig {
  const base = createDefaultDeliveryPipelineConfig()
  const sinksInput = Array.isArray(input?.sinks)
    ? input.sinks
    : [input && typeof input === 'object' ? input : createDefaultDeliverySinkConfig()]
  const strategy: DeliveryPipelineStrategy = input?.strategy === 'any' ? 'any' : base.strategy
  const normalizedSinks = sinksInput.map((sink: any, index: number) => normalizeSinkConfig(sink, index))
  const enabled = input?.enabled === true || normalizedSinks.some((sink) => sink.enabled)

  return {
    enabled,
    strategy,
    sinks: normalizedSinks,
  }
}

function resolveEventSink(config: DeliverySinkConfig): EventSink {
  if (config.kind === 'local-test') {
    return new LocalTestEventSink(config)
  }
  if (config.kind === 'central-http') {
    return new CentralHttpEventSink(config)
  }
  return new HttpEventSink(config)
}

function loadPipelineConfig(): DeliveryPipelineConfig {
  ensureOutboxDir()
  if (!existsSync(getSinkConfigPath())) {
    return createDefaultDeliveryPipelineConfig()
  }
  try {
    return normalizePipelineConfig(JSON.parse(readFileSync(getSinkConfigPath(), 'utf8')))
  } catch {
    return createDefaultDeliveryPipelineConfig()
  }
}

function loadBackpressureState(): Record<string, SinkBackpressureState> {
  ensureOutboxDir()
  if (!existsSync(getBackpressurePath())) {
    return {}
  }
  try {
    const loaded = JSON.parse(readFileSync(getBackpressurePath(), 'utf8'))
    const result: Record<string, SinkBackpressureState> = {}
    for (const [sinkId, value] of Object.entries(loaded || {})) {
      if (!value || typeof value !== 'object') {
        continue
      }
      result[sinkId] = {
        sinkId,
        pausedUntil: typeof (value as any).pausedUntil === 'string' ? (value as any).pausedUntil : undefined,
        maxBatchSize: typeof (value as any).maxBatchSize === 'number' ? (value as any).maxBatchSize : undefined,
        reason: typeof (value as any).reason === 'string' ? (value as any).reason : undefined,
        lastUpdatedAt: typeof (value as any).lastUpdatedAt === 'string' ? (value as any).lastUpdatedAt : undefined,
      }
    }
    return result
  } catch {
    return {}
  }
}

function persistBackpressureState(): void {
  ensureOutboxDir()
  writeAtomicJson(getBackpressurePath(), workerStatus.sinkBackpressure)
}

function loadCheckpointStore(): DeliveryCheckpointStore {
  ensureOutboxDir()
  if (!existsSync(getCheckpointPath())) {
    return { version: 1, batches: {} }
  }
  try {
    const loaded = JSON.parse(readFileSync(getCheckpointPath(), 'utf8'))
    if (loaded?.version !== 1 || typeof loaded?.batches !== 'object' || !loaded.batches) {
      return { version: 1, batches: {} }
    }
    return {
      version: 1,
      batches: loaded.batches,
    }
  } catch {
    return { version: 1, batches: {} }
  }
}

function compactCheckpointStore(): void {
  const completed = Object.values(checkpointStore.batches)
    .filter((item) => item.state === 'completed')
    .sort((left, right) => Date.parse(right.updatedAt) - Date.parse(left.updatedAt))

  for (const stale of completed.slice(MAX_COMPLETED_CHECKPOINTS)) {
    archiveCheckpoint(stale, 'compaction')
    delete checkpointStore.batches[stale.batchId]
  }
}

function persistCheckpointStore(): void {
  ensureOutboxDir()
  compactCheckpointStore()
  writeAtomicJson(getCheckpointPath(), checkpointStore)
}

function archiveCheckpoint(checkpoint: DeliveryBatchCheckpoint, reason: string): void {
  ensureOutboxDir()
  appendFileSync(
    getCheckpointArchivePath(),
    `${JSON.stringify({ archivedAt: new Date().toISOString(), reason, checkpoint })}\n`,
    'utf8'
  )
}

function appendPipelineHistoryEntry(entry: PipelineHistoryEntry): void {
  ensureOutboxDir()
  appendFileSync(
    getPipelineHistoryPath(),
    `${JSON.stringify(entry)}\n`,
    'utf8'
  )
}

function ensurePipelineConfigLoaded(): void {
  if (!pipelineConfigLoaded) {
    workerStatus.pipeline = loadPipelineConfig()
    workerStatus.sinkBackpressure = loadBackpressureState()
    pipelineConfigLoaded = true
  }
  if (!checkpointsLoaded) {
    checkpointStore = loadCheckpointStore()
    recoverPendingCheckpoints()
    checkpointsLoaded = true
  }
}

function persistPipelineConfig(reason: string = 'manual-update'): void {
  ensureOutboxDir()
  writeAtomicJson(getSinkConfigPath(), workerStatus.pipeline)
  appendPipelineHistoryEntry({
    changedAt: new Date().toISOString(),
    reason,
    pipeline: getDeliverySinkConfig(),
  })
}

function refreshQueueStats(): void {
  const stats = getPersistentQueueStats()
  workerStatus.pendingCount = stats.pending
  workerStatus.sendingCount = stats.sending
  workerStatus.sentCount = stats.sent
  workerStatus.failedCount = stats.failed
  workerStatus.abandonedCount = stats.abandoned
  workerStatus.queueDepth = stats.total
  workerStatus.outboxDir = getOutboxDir()
  workerStatus.deliveredPath = getDeliveredPath()
  workerStatus.deadLetterPath = getDeadLetterPath()
}

function pruneExpiredBackpressure(): void {
  let changed = false
  for (const [sinkId, state] of Object.entries(workerStatus.sinkBackpressure)) {
    if (!state.pausedUntil) {
      continue
    }
    const pausedUntil = Date.parse(state.pausedUntil)
    if (Number.isFinite(pausedUntil) && pausedUntil <= Date.now()) {
      delete workerStatus.sinkBackpressure[sinkId]
      changed = true
    }
  }
  if (changed) {
    persistBackpressureState()
  }
}

function ensureCheckpoint(batchId: string, events: PersistedDlpEvent[], sinkIds: string[]): DeliveryBatchCheckpoint {
  const existing = checkpointStore.batches[batchId]
  if (existing) {
    return existing
  }
  const now = new Date().toISOString()
  const created: DeliveryBatchCheckpoint = {
    batchId,
    createdAt: now,
    updatedAt: now,
    strategy: workerStatus.pipeline.strategy,
    eventIds: events.map((event) => event.id),
    sinkIds: sinkIds.slice(),
    state: 'created',
    sinkStatuses: {},
  }
  checkpointStore.batches[batchId] = created
  persistCheckpointStore()
  return created
}

function updateCheckpointSinkState(
  batchId: string,
  sinkId: string,
  patch: Partial<SinkCheckpointStatus> & { state: SinkCheckpointState }
): void {
  const checkpoint = checkpointStore.batches[batchId]
  if (!checkpoint) {
    return
  }
  const current = checkpoint.sinkStatuses[sinkId]
  checkpoint.sinkStatuses[sinkId] = {
    state: patch.state,
    updatedAt: new Date().toISOString(),
    eventIds: patch.eventIds ? patch.eventIds.slice() : current?.eventIds || [],
    deliveredEventIds: patch.deliveredEventIds ? patch.deliveredEventIds.slice() : current?.deliveredEventIds || [],
    retryEventIds: patch.retryEventIds ? patch.retryEventIds.slice() : current?.retryEventIds || [],
    droppedEventIds: patch.droppedEventIds ? patch.droppedEventIds.slice() : current?.droppedEventIds || [],
    statusCode: patch.statusCode ?? current?.statusCode,
    error: patch.error ?? current?.error,
    receiptVerified: patch.receiptVerified ?? current?.receiptVerified,
  }
  checkpoint.updatedAt = new Date().toISOString()
  const sinkStates = Object.values(checkpoint.sinkStatuses)
  if (sinkStates.some((item) => item.state === 'sending')) {
    checkpoint.state = 'inflight'
  } else if (
    checkpoint.sinkIds.length > 0 &&
    checkpoint.sinkIds.every((id) => {
      const item = checkpoint.sinkStatuses[id]
      return !!item && (item.state === 'completed' || item.state === 'dropped')
    })
  ) {
    checkpoint.state = 'completed'
  } else if (sinkStates.length > 0) {
    checkpoint.state = 'partial'
  } else {
    checkpoint.state = 'created'
  }
  persistCheckpointStore()
}

function finalizeCheckpoint(batchId: string, events: PersistedDlpEvent[]): void {
  const checkpoint = checkpointStore.batches[batchId]
  if (!checkpoint) {
    return
  }

  const latestEvents = events.map((event) => getQueuedEventById(event.id) || event)
  const finalStates = latestEvents.map((event) => event.state)
  checkpoint.updatedAt = new Date().toISOString()
  checkpoint.state = finalStates.every((state) => state === 'sent' || state === 'abandoned')
    ? 'completed'
    : 'partial'
  persistCheckpointStore()
}

function recoverPendingCheckpoints(): void {
  let changed = false
  for (const checkpoint of Object.values(checkpointStore.batches)) {
    if (checkpoint.state === 'completed' || checkpoint.state === 'recovered') {
      continue
    }

    for (const eventId of checkpoint.eventIds) {
      const event = getQueuedEventById(eventId)
      if (!event) {
        continue
      }

      const sinkStates = cloneSinkStates(event.sinkStates)
      let eventChanged = false
      for (const sinkId of checkpoint.sinkIds) {
        const sinkCheckpoint = checkpoint.sinkStatuses[sinkId]
        if (!sinkCheckpoint) {
          continue
        }
        const current = sinkStates[sinkId] || getEventSinkState(event, sinkId)
        if (current.state === 'acked' || current.state === 'dropped') {
          continue
        }

        if (sinkCheckpoint.deliveredEventIds.includes(eventId)) {
          sinkStates[sinkId] = {
            ...current,
            state: 'acked',
            ackAt: sinkCheckpoint.updatedAt,
            lastAttemptAt: sinkCheckpoint.updatedAt,
            nextAttemptAt: null,
            error: null,
            lastHttpStatus: sinkCheckpoint.statusCode ?? current.lastHttpStatus ?? null,
            receiptVerified: sinkCheckpoint.receiptVerified ?? current.receiptVerified ?? null,
          }
          eventChanged = true
          continue
        }

        if (sinkCheckpoint.droppedEventIds.includes(eventId)) {
          sinkStates[sinkId] = {
            ...current,
            state: 'dropped',
            lastAttemptAt: sinkCheckpoint.updatedAt,
            nextAttemptAt: null,
            error: sinkCheckpoint.error || current.error || 'recovered from durable checkpoint as dropped',
            lastHttpStatus: sinkCheckpoint.statusCode ?? current.lastHttpStatus ?? null,
            receiptVerified: sinkCheckpoint.receiptVerified ?? current.receiptVerified ?? null,
          }
          eventChanged = true
          continue
        }

        sinkStates[sinkId] = {
          ...current,
          state: 'retry',
          retryCount: current.retryCount,
          lastAttemptAt: sinkCheckpoint.updatedAt,
          nextAttemptAt: null,
          error: sinkCheckpoint.error || 'recovered from durable checkpoint',
          lastHttpStatus: sinkCheckpoint.statusCode ?? current.lastHttpStatus ?? null,
          receiptVerified: sinkCheckpoint.receiptVerified ?? current.receiptVerified ?? null,
        }
        eventChanged = true
      }

      if (eventChanged) {
        const enabledSinkIds = checkpoint.sinkIds
        const sinkStateValues = enabledSinkIds.map((sinkId) => sinkStates[sinkId] || getEventSinkState(event, sinkId))
        const allAcked = sinkStateValues.length > 0 && sinkStateValues.every((item) => item.state === 'acked')
        const hasDropped = sinkStateValues.some((item) => item.state === 'dropped')
        patchQueuedEvent(eventId, {
          state: allAcked ? 'sent' : (hasDropped && checkpoint.strategy === 'all' ? 'abandoned' : 'failed'),
          ackAt: allAcked ? new Date().toISOString() : event.ackAt ?? null,
          nextAttemptAt: allAcked ? null : event.nextAttemptAt ?? null,
          leaseId: null,
          leaseExpireAt: null,
          error: allAcked ? null : (hasDropped && checkpoint.strategy === 'all' ? 'recovered from durable checkpoint as dropped' : 'recovered from durable checkpoint'),
          sinkStates,
        })
      }
    }

    checkpoint.state = 'recovered'
    checkpoint.updatedAt = new Date().toISOString()
    changed = true
  }

  if (changed) {
    persistCheckpointStore()
  }
}

function cloneSinkStates(source?: Record<string, PersistedSinkDeliveryStatus>): Record<string, PersistedSinkDeliveryStatus> {
  const target: Record<string, PersistedSinkDeliveryStatus> = {}
  for (const [sinkId, state] of Object.entries(source || {})) {
    target[sinkId] = { ...state }
  }
  return target
}

function getEventSinkState(event: PersistedDlpEvent, sinkId: string): PersistedSinkDeliveryStatus {
  return event.sinkStates?.[sinkId] || {
    state: 'pending',
    retryCount: 0,
    ackAt: null,
    lastAttemptAt: null,
    nextAttemptAt: null,
    error: null,
    lastHttpStatus: null,
    remoteEventId: null,
    receiptVerified: null,
  }
}

function isSinkStateReadyForDelivery(event: PersistedDlpEvent, sinkId: string): boolean {
  const state = getEventSinkState(event, sinkId)
  if (state.state === 'acked' || state.state === 'dropped') {
    return false
  }
  if (!state.nextAttemptAt) {
    return true
  }
  const nextAttemptAt = Date.parse(state.nextAttemptAt)
  return !Number.isFinite(nextAttemptAt) || nextAttemptAt <= Date.now()
}

function deliveryBackoffMs(retryCount: number, retryAfterMs?: number): number {
  if (typeof retryAfterMs === 'number' && retryAfterMs >= 0) {
    return retryAfterMs
  }
  const base = 5000
  const max = 5 * 60 * 1000
  const jitter = Math.floor(Math.random() * 1000)
  return Math.min(max, base * Math.max(1, 2 ** Math.max(0, retryCount - 1))) + jitter
}

function appendDeliveredRecord(event: PersistedDlpEvent, results: DeliveryAttemptResult[]): void {
  ensureOutboxDir()
  appendFileSync(
    getDeliveredPath(),
    `${JSON.stringify({ deliveredAt: new Date().toISOString(), results, event })}\n`,
    'utf8'
  )
}

function appendDeadLetterRecord(
  event: PersistedDlpEvent,
  results: DeliveryAttemptResult[],
  decision: DeliveryDecision,
  error: string,
  statusCode?: number
): void {
  ensureOutboxDir()
  appendFileSync(
    getDeadLetterPath(),
    `${JSON.stringify({
      abandonedAt: new Date().toISOString(),
      decision,
      error,
      statusCode: statusCode || 0,
      results,
      event,
    })}\n`,
    'utf8'
  )
}

function createBatchId(): string {
  return `batch-${Date.now()}-${Math.random().toString(16).slice(2, 10)}`
}

function updateSinkBackpressure(result: DeliveryAttemptResult): void {
  if (!result.sinkId || !result.backpressure) {
    return
  }

  const pauseMs = result.backpressure.pauseMs
  workerStatus.sinkBackpressure[result.sinkId] = {
    sinkId: result.sinkId,
    pausedUntil: typeof pauseMs === 'number' ? new Date(Date.now() + pauseMs).toISOString() : undefined,
    maxBatchSize: typeof result.backpressure.maxBatchSize === 'number' ? result.backpressure.maxBatchSize : undefined,
    reason: result.backpressure.reason,
    lastUpdatedAt: new Date().toISOString(),
  }
  persistBackpressureState()
}

function getEnabledSinks(): DeliverySinkConfig[] {
  ensurePipelineConfigLoaded()
  pruneExpiredBackpressure()
  return workerStatus.pipeline.sinks.filter((sink) => sink.enabled)
}

function isSinkPaused(sinkId: string): boolean {
  const state = workerStatus.sinkBackpressure[sinkId]
  if (!state?.pausedUntil) {
    return false
  }
  const pausedUntil = Date.parse(state.pausedUntil)
  return Number.isFinite(pausedUntil) && pausedUntil > Date.now()
}

function getPausedSinkWaitMs(sinkId: string): number {
  const state = workerStatus.sinkBackpressure[sinkId]
  if (!state?.pausedUntil) {
    return 0
  }
  const pausedUntil = Date.parse(state.pausedUntil)
  if (!Number.isFinite(pausedUntil)) {
    return 0
  }
  return Math.max(0, pausedUntil - Date.now())
}

function getReadySinksForCurrentStrategy(): DeliverySinkConfig[] {
  const enabledSinks = getEnabledSinks()
  if (enabledSinks.length === 0) {
    return []
  }

  if (workerStatus.pipeline.strategy === 'any') {
    return enabledSinks.filter((sink) => !isSinkPaused(sink.id))
  }

  return enabledSinks.every((sink) => !isSinkPaused(sink.id)) ? enabledSinks : []
}

function getPipelinePauseMs(): number {
  const enabledSinks = getEnabledSinks()
  if (enabledSinks.length === 0) {
    return 0
  }

  const waits = enabledSinks
    .map((sink) => getPausedSinkWaitMs(sink.id))
    .filter((value) => value > 0)

  if (waits.length === 0) {
    return 0
  }

  if (workerStatus.pipeline.strategy === 'any') {
    const available = enabledSinks.some((sink) => getPausedSinkWaitMs(sink.id) === 0)
    return available ? 0 : Math.min(...waits)
  }

  return Math.min(...waits)
}

function getEffectiveBatchSize(defaultBatchSize: number, sinks: DeliverySinkConfig[]): number {
  let limit = defaultBatchSize
  for (const sink of sinks) {
    const backpressure = workerStatus.sinkBackpressure[sink.id]
    if (typeof backpressure?.maxBatchSize === 'number' && backpressure.maxBatchSize > 0) {
      limit = Math.min(limit, backpressure.maxBatchSize)
    }
  }
  return Math.max(1, limit)
}

async function deliverBatchToSinks(events: PersistedDlpEvent[]): Promise<DeliveryAttemptResult[]> {
  if (events.length === 0) {
    return []
  }

  const sinks = getReadySinksForCurrentStrategy()
  if (sinks.length === 0) {
    return []
  }

  const batchId = createBatchId()
  ensureCheckpoint(batchId, events, sinks.map((sink) => sink.id))
  const results: DeliveryAttemptResult[] = []
  const anySatisfied = new Set<number>()
  for (const sinkConfig of sinks) {
    const subset = events.filter((event) => {
      if (!isSinkStateReadyForDelivery(event, sinkConfig.id)) {
        return false
      }
      if (workerStatus.pipeline.strategy === 'any' && anySatisfied.has(event.id)) {
        return false
      }
      return true
    })
    if (subset.length === 0) {
      continue
    }

    updateCheckpointSinkState(batchId, sinkConfig.id, {
      state: 'sending',
      eventIds: subset.map((event) => event.id),
      deliveredEventIds: [],
      retryEventIds: [],
      droppedEventIds: [],
    })

    const sink = resolveEventSink(sinkConfig)
    let result: DeliveryAttemptResult
    try {
      result = await sink.deliver(subset, batchId)
    } catch (error: any) {
      updateCheckpointSinkState(batchId, sinkConfig.id, {
        state: 'failed',
        eventIds: subset.map((event) => event.id),
        deliveredEventIds: [],
        retryEventIds: subset.map((event) => event.id),
        droppedEventIds: [],
        error: error?.message || String(error),
      })
      throw error
    }
    results.push(result)
    updateSinkBackpressure(result)
    const deliveredEventIds: number[] = []
    const retryEventIds: number[] = []
    const droppedEventIds: number[] = []
    for (const event of subset) {
      const eventResult = getEventResultFromAttempt(event, result)
      if (eventResult.decision === 'ack') deliveredEventIds.push(event.id)
      else if (eventResult.decision === 'retry') retryEventIds.push(event.id)
      else droppedEventIds.push(event.id)
    }
    updateCheckpointSinkState(batchId, sinkConfig.id, {
      state: droppedEventIds.length === subset.length
        ? 'dropped'
        : (retryEventIds.length > 0 ? 'failed' : 'completed'),
      eventIds: subset.map((event) => event.id),
      deliveredEventIds,
      retryEventIds,
      droppedEventIds,
      statusCode: result.statusCode,
      error: result.error,
      receiptVerified: result.receipt?.verified,
    })
    if (workerStatus.pipeline.strategy === 'any') {
      for (const event of subset) {
        const eventResult = getEventResultFromAttempt(event, result)
        if (eventResult.decision === 'ack') {
          anySatisfied.add(event.id)
        }
      }
    }
  }
  return results
}

function scheduleRetry(event: PersistedDlpEvent, message: string, results: DeliveryAttemptResult[], retryAfterMs?: number, statusCode?: number): void {
  const retryCount = (event.retryCount || 0) + 1
  if (retryCount > workerStatus.maxRetryCount) {
    appendDeadLetterRecord(event, results, 'drop', `retry exhausted: ${message}`, statusCode)
    markQueuedEventAbandoned(event.id, `retry exhausted: ${message}`, statusCode)
    return
  }
  const nextAttemptAt = new Date(Date.now() + deliveryBackoffMs(retryCount, retryAfterMs)).toISOString()
  markQueuedEventFailed(event.id, message, retryCount, nextAttemptAt, statusCode)
}

function failBatch(events: PersistedDlpEvent[], error: unknown): void {
  const message = error instanceof Error ? error.message : String(error)
  for (const event of events) {
    scheduleRetry(event, message, [])
  }
  workerStatus.lastError = message
}

function getDefaultDecision(statusCode: number): DeliveryDecision {
  if (statusCode >= 200 && statusCode < 300) {
    return 'ack'
  }
  if (statusCode === 408 || statusCode === 425 || statusCode === 429 || statusCode >= 500) {
    return 'retry'
  }
  return 'drop'
}

function getEventResultFromAttempt(event: PersistedDlpEvent, result: DeliveryAttemptResult): DeliveryEventResult {
  const explicit = result.eventResults?.find((item) => item.eventId === event.id)
  if (explicit) {
    return explicit
  }
  return {
    eventId: event.id,
    decision: result.retryAll ? 'retry' : getDefaultDecision(result.statusCode),
    error: result.error,
    retryAfterMs: result.retryAfterMs,
    statusCode: result.statusCode,
  }
}

function updatePerSinkStates(event: PersistedDlpEvent, results: DeliveryAttemptResult[]): Record<string, PersistedSinkDeliveryStatus> {
  const sinkStates = cloneSinkStates(event.sinkStates)
  const now = new Date().toISOString()

  for (const result of results) {
    if (!result.sinkId) {
      continue
    }
    const eventResult = getEventResultFromAttempt(event, result)
    const current = getEventSinkState({ ...event, sinkStates }, result.sinkId)
    if (current.state === 'acked' || current.state === 'dropped') {
      continue
    }

    if (eventResult.decision === 'ack') {
      sinkStates[result.sinkId] = {
        state: 'acked',
        retryCount: current.retryCount,
        ackAt: now,
        lastAttemptAt: now,
        nextAttemptAt: null,
        error: null,
        lastHttpStatus: eventResult.statusCode ?? result.statusCode,
        remoteEventId: eventResult.remoteEventId || current.remoteEventId || null,
        receiptVerified: result.receipt?.verified ?? current.receiptVerified ?? null,
      }
      continue
    }

    if (eventResult.decision === 'retry') {
      sinkStates[result.sinkId] = {
        state: 'retry',
        retryCount: current.retryCount + 1,
        ackAt: current.ackAt ?? null,
        lastAttemptAt: now,
        nextAttemptAt: new Date(Date.now() + deliveryBackoffMs(current.retryCount + 1, eventResult.retryAfterMs ?? result.retryAfterMs)).toISOString(),
        error: eventResult.error || result.error || null,
        lastHttpStatus: eventResult.statusCode ?? result.statusCode,
        remoteEventId: current.remoteEventId || null,
        receiptVerified: result.receipt?.verified ?? current.receiptVerified ?? null,
      }
      continue
    }

    sinkStates[result.sinkId] = {
      state: 'dropped',
      retryCount: current.retryCount,
      ackAt: current.ackAt ?? null,
      lastAttemptAt: now,
      nextAttemptAt: null,
      error: eventResult.error || result.error || null,
      lastHttpStatus: eventResult.statusCode ?? result.statusCode,
      remoteEventId: current.remoteEventId || null,
      receiptVerified: result.receipt?.verified ?? current.receiptVerified ?? null,
    }
  }

  return sinkStates
}

function computeOverallNextAttemptAt(sinkStates: Record<string, PersistedSinkDeliveryStatus>, enabledSinkIds: string[]): string | null {
  const candidates: number[] = []

  for (const sinkId of enabledSinkIds) {
    const sinkState = sinkStates[sinkId]
    if (!sinkState || sinkState.state === 'pending') {
      const pausedUntil = workerStatus.sinkBackpressure[sinkId]?.pausedUntil
      if (!pausedUntil) {
        return null
      }
      const pausedAt = Date.parse(pausedUntil)
      if (!Number.isFinite(pausedAt) || pausedAt <= Date.now()) {
        return null
      }
      candidates.push(pausedAt)
      continue
    }

    if (sinkState.state === 'retry') {
      const retryAt = sinkState.nextAttemptAt ? Date.parse(sinkState.nextAttemptAt) : Date.now()
      const pausedAt = workerStatus.sinkBackpressure[sinkId]?.pausedUntil
      const pausedMs = pausedAt ? Date.parse(pausedAt) : NaN
      if ((!Number.isFinite(retryAt) || retryAt <= Date.now()) && (!Number.isFinite(pausedMs) || pausedMs <= Date.now())) {
        return null
      }
      candidates.push(Math.max(
        Number.isFinite(retryAt) ? retryAt : Date.now(),
        Number.isFinite(pausedMs) ? pausedMs : 0
      ))
    }
  }

  if (candidates.length === 0) {
    return null
  }
  return new Date(Math.min(...candidates)).toISOString()
}

function summarizeEventDecision(
  event: PersistedDlpEvent,
  results: DeliveryAttemptResult[],
  sinkStates: Record<string, PersistedSinkDeliveryStatus>
): {
  decision: DeliveryDecision
  retryAfterMs?: number
  nextAttemptAt?: string | null
  statusCode?: number
  error?: string
  remoteEventId?: string
  sinkStates: Record<string, PersistedSinkDeliveryStatus>
} {
  const enabledSinkIds = getEnabledSinks().map((sink) => sink.id)
  const finalStates = enabledSinkIds.map((sinkId) => sinkStates[sinkId] || getEventSinkState(event, sinkId))
  const acked = finalStates.filter((item) => item.state === 'acked')
  const retry = finalStates.filter((item) => item.state === 'retry' || item.state === 'pending')
  const dropped = finalStates.filter((item) => item.state === 'dropped')
  const lastResults = results
    .map((result) => getEventResultFromAttempt(event, result))
    .filter(Boolean)

  if (workerStatus.pipeline.strategy === 'any') {
    if (acked.length > 0) {
      return {
        decision: 'ack',
        statusCode: acked[0].lastHttpStatus ?? lastResults.find((item) => item.decision === 'ack')?.statusCode,
        remoteEventId: acked.find((item) => item.remoteEventId)?.remoteEventId || undefined,
        sinkStates,
      }
    }
    if (dropped.length === enabledSinkIds.length && enabledSinkIds.length > 0) {
      return {
        decision: 'drop',
        statusCode: dropped[0]?.lastHttpStatus ?? lastResults.find((item) => item.decision === 'drop')?.statusCode,
        error: dropped[0]?.error || 'dropped by all sinks',
        sinkStates,
      }
    }
    const nextAttemptAt = computeOverallNextAttemptAt(sinkStates, enabledSinkIds)
    return {
      decision: 'retry',
      nextAttemptAt,
      retryAfterMs: nextAttemptAt ? Math.max(0, Date.parse(nextAttemptAt) - Date.now()) : 0,
      statusCode: lastResults.find((item) => item.decision === 'retry')?.statusCode ?? lastResults[0]?.statusCode,
      error: lastResults.find((item) => item.decision === 'retry')?.error || 'at least one sink still pending',
      sinkStates,
    }
  }

  if (dropped.length > 0) {
    return {
      decision: 'drop',
      statusCode: dropped[0].lastHttpStatus ?? lastResults.find((item) => item.decision === 'drop')?.statusCode,
      error: dropped[0].error || 'dropped by sink',
      sinkStates,
    }
  }

  if (acked.length === enabledSinkIds.length && enabledSinkIds.length > 0) {
    return {
      decision: 'ack',
      statusCode: acked[0].lastHttpStatus ?? lastResults.find((item) => item.decision === 'ack')?.statusCode,
      remoteEventId: acked.find((item) => item.remoteEventId)?.remoteEventId || undefined,
      sinkStates,
    }
  }

  const nextAttemptAt = computeOverallNextAttemptAt(sinkStates, enabledSinkIds)
  return {
    decision: 'retry',
    nextAttemptAt,
    retryAfterMs: nextAttemptAt ? Math.max(0, Date.parse(nextAttemptAt) - Date.now()) : 0,
    statusCode: retry[0]?.lastHttpStatus ?? lastResults.find((item) => item.decision === 'retry')?.statusCode,
    error: retry[0]?.error || 'not all sinks acknowledged',
    sinkStates,
  }
}

function applyBatchResults(events: PersistedDlpEvent[], results: DeliveryAttemptResult[]): void {
  let anyAcked = false
  const batchId = results.find((item) => item.batchId)?.batchId

  for (const event of events) {
    const currentEvent = getQueuedEventById(event.id) || event
    const sinkStates = updatePerSinkStates(currentEvent, results)
    const summary = summarizeEventDecision(currentEvent, results, sinkStates)
    if (summary.decision === 'ack') {
      appendDeliveredRecord(event, results)
      patchQueuedEvent(event.id, {
        state: 'sent',
        ackAt: new Date().toISOString(),
        nextAttemptAt: null,
        leaseId: null,
        leaseExpireAt: null,
        error: null,
        lastHttpStatus: summary.statusCode ?? null,
        lastRemoteId: summary.remoteEventId || null,
        sinkStates: summary.sinkStates,
      })
      workerStatus.deliveredCount++
      anyAcked = true
      continue
    }

    if (summary.decision === 'retry') {
      const current = getQueuedEventById(event.id) || event
      const nextRetryCount = (current.retryCount || 0) + 1
      if (nextRetryCount > workerStatus.maxRetryCount) {
        appendDeadLetterRecord(event, results, 'drop', `retry exhausted: ${summary.error || 'delivery retry requested'}`, summary.statusCode)
        patchQueuedEvent(event.id, {
          state: 'abandoned',
          lastAttemptAt: new Date().toISOString(),
          nextAttemptAt: null,
          leaseId: null,
          leaseExpireAt: null,
          error: `retry exhausted: ${summary.error || 'delivery retry requested'}`,
          lastHttpStatus: summary.statusCode ?? current.lastHttpStatus ?? null,
          sinkStates: summary.sinkStates,
        })
        continue
      }
      patchQueuedEvent(event.id, {
        state: 'failed',
        retryCount: nextRetryCount,
        lastAttemptAt: new Date().toISOString(),
        nextAttemptAt: summary.nextAttemptAt || new Date(Date.now() + deliveryBackoffMs(nextRetryCount, summary.retryAfterMs)).toISOString(),
        leaseId: null,
        leaseExpireAt: null,
        error: summary.error || 'delivery retry requested',
        lastHttpStatus: summary.statusCode ?? current.lastHttpStatus ?? null,
        sinkStates: summary.sinkStates,
      })
      continue
    }

    appendDeadLetterRecord(event, results, 'drop', summary.error || 'delivery dropped', summary.statusCode)
    patchQueuedEvent(event.id, {
      state: 'abandoned',
      lastAttemptAt: new Date().toISOString(),
      nextAttemptAt: null,
      leaseId: null,
      leaseExpireAt: null,
      error: summary.error || 'delivery dropped',
      lastHttpStatus: summary.statusCode ?? null,
      sinkStates: summary.sinkStates,
    })
  }

  if (anyAcked) {
    workerStatus.lastSuccessAt = new Date().toISOString()
  }
  if (batchId) {
    finalizeCheckpoint(batchId, events)
  }
}

export function setDeliverySinkConfig(config: any): DeliveryPipelineConfig {
  ensurePipelineConfigLoaded()

  if (Array.isArray(config?.sinks) || typeof config?.strategy === 'string') {
    workerStatus.pipeline = normalizePipelineConfig({
      ...workerStatus.pipeline,
      ...config,
    })
  } else {
    const currentFirstSink = workerStatus.pipeline.sinks[0] || createDefaultDeliverySinkConfig()
    const mergedFirstSink = normalizeSinkConfig({
      ...currentFirstSink,
      ...config,
    }, 0)
    const restSinks = workerStatus.pipeline.sinks.slice(1).map((sink, index) => normalizeSinkConfig(sink, index + 1))
    workerStatus.pipeline = normalizePipelineConfig({
      enabled: config?.enabled === true || workerStatus.pipeline.enabled,
      strategy: workerStatus.pipeline.strategy,
      sinks: [mergedFirstSink, ...restSinks],
    })
  }

  persistPipelineConfig(typeof config?.historyReason === 'string' ? config.historyReason : 'ui-update')
  return getDeliverySinkConfig()
}

export function getDeliverySinkConfig(): DeliveryPipelineConfig {
  ensurePipelineConfigLoaded()
  return normalizePipelineConfig(workerStatus.pipeline)
}

export function startDeliveryWorker(intervalMs: number = 5000, batchSize: number = 100): void {
  if (workerRunning) {
    return
  }

  ensurePipelineConfigLoaded()
  workerRunning = true
  workerStatus.running = true
  workerStatus.intervalMs = intervalMs
  workerStatus.batchSize = batchSize
  refreshQueueStats()

  workerPromise = (async () => {
    while (workerRunning) {
      workerStatus.lastRunAt = new Date().toISOString()
      try {
        const pipeline = getDeliverySinkConfig()
        const pauseMs = getPipelinePauseMs()
        if (pipeline.enabled && pauseMs === 0) {
          const readySinks = getReadySinksForCurrentStrategy()
          if (readySinks.length > 0) {
            const leased = leaseQueuedEvents(getEffectiveBatchSize(batchSize, readySinks))
            if (leased.length > 0) {
              try {
                const results = await deliverBatchToSinks(leased)
                workerStatus.lastResults = results
                applyBatchResults(leased, results)
              } catch (error) {
                failBatch(leased, error)
              }
            }
          }
        }
        refreshQueueStats()
      } catch (error: any) {
        workerStatus.lastError = error?.message || String(error)
      }

      if (!workerRunning) {
        break
      }
      await new Promise((resolve) => setTimeout(resolve, intervalMs))
    }
  })()
}

export async function stopDeliveryWorker(): Promise<void> {
  workerRunning = false
  workerStatus.running = false
  if (workerPromise) {
    try { await workerPromise } catch {}
  }
  workerPromise = null
  refreshQueueStats()
}

export function getDeliveryWorkerStatus(): DeliveryStatus {
  ensurePipelineConfigLoaded()
  refreshQueueStats()
  return {
    ...workerStatus,
    pipeline: getDeliverySinkConfig(),
  }
}

export function clearDeliveryBackpressure(sinkId?: string): { cleared: string[] } {
  const cleared: string[] = []
  if (typeof sinkId === 'string' && sinkId) {
    if (workerStatus.sinkBackpressure[sinkId]) {
      delete workerStatus.sinkBackpressure[sinkId]
      cleared.push(sinkId)
    }
    persistBackpressureState()
    return { cleared }
  }

  for (const key of Object.keys(workerStatus.sinkBackpressure)) {
    delete workerStatus.sinkBackpressure[key]
    cleared.push(key)
  }
  persistBackpressureState()
  return { cleared }
}

function readRecentNdjson<T>(path: string, limit: number): T[] {
  if (!existsSync(path)) {
    return []
  }
  try {
    return readFileSync(path, 'utf8')
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean)
      .slice(-Math.max(0, limit))
      .map((line) => JSON.parse(line) as T)
      .reverse()
  } catch {
    return []
  }
}

export function listDeadLetterRecords(limit: number = 100): DeadLetterRecord[] {
  return readRecentNdjson<DeadLetterRecord>(getDeadLetterPath(), limit)
}

export function listDeliveredRecords(limit: number = 100): Array<{ deliveredAt: string; results: DeliveryAttemptResult[]; event: PersistedDlpEvent }> {
  return readRecentNdjson<{ deliveredAt: string; results: DeliveryAttemptResult[]; event: PersistedDlpEvent }>(getDeliveredPath(), limit)
}

export function listDeadLetterQueue(limit: number = 100): PersistedDlpEvent[] {
  return listQueuedEventsByState('abandoned', limit)
}

export function listDeliveredQueue(limit: number = 100): PersistedDlpEvent[] {
  return listQueuedEventsByState('sent', limit)
}

export function listDeliveryCheckpoints(limit: number = 100): DeliveryBatchCheckpoint[] {
  ensurePipelineConfigLoaded()
  return Object.values(checkpointStore.batches)
    .sort((left, right) => Date.parse(right.updatedAt) - Date.parse(left.updatedAt))
    .slice(0, Math.max(0, limit))
}

export function listArchivedCheckpoints(limit: number = 100): Array<{ archivedAt: string; reason: string; checkpoint: DeliveryBatchCheckpoint }> {
  return readRecentNdjson<{ archivedAt: string; reason: string; checkpoint: DeliveryBatchCheckpoint }>(
    getCheckpointArchivePath(),
    limit
  )
}

export function listPipelineHistory(limit: number = 100): PipelineHistoryEntry[] {
  return readRecentNdjson<PipelineHistoryEntry>(getPipelineHistoryPath(), limit)
}

export function clearCompletedCheckpoints(): { removed: number } {
  ensurePipelineConfigLoaded()
  let removed = 0
  for (const [batchId, checkpoint] of Object.entries(checkpointStore.batches)) {
    if (checkpoint.state === 'completed' || checkpoint.state === 'recovered') {
      archiveCheckpoint(checkpoint, 'manual-clear')
      delete checkpointStore.batches[batchId]
      removed++
    }
  }
  if (removed > 0) {
    persistCheckpointStore()
  }
  return { removed }
}

export function exportDeliveryReport(reason: string = 'manual-export'): { path: string; report: DeliveryExportReport } {
  ensurePipelineConfigLoaded()
  mkdirSync(getReportsDir(), { recursive: true })
  const report: DeliveryExportReport = {
    generatedAt: new Date().toISOString(),
    reason,
    status: getDeliveryWorkerStatus(),
    queueInfo: getDeliveredQueueInfo(),
    checkpoints: listDeliveryCheckpoints(100),
    archivedCheckpoints: listArchivedCheckpoints(100),
    deadLetterQueue: listDeadLetterQueue(100),
    deadLetterRecords: listDeadLetterRecords(100),
    deliveredRecords: listDeliveredRecords(100),
  }
  const path = join(getReportsDir(), `delivery-report-${Date.now()}.json`)
  writeFileSync(path, JSON.stringify(report, null, 2), 'utf8')
  return { path, report }
}

export function getDeliveredQueueInfo() {
  return {
    directory: getOutboxDir(),
    deliveredPath: getDeliveredPath(),
    deadLetterPath: getDeadLetterPath(),
    sinkConfigPath: getSinkConfigPath(),
    backpressurePath: getBackpressurePath(),
    checkpointPath: getCheckpointPath(),
    checkpointArchivePath: getCheckpointArchivePath(),
    pipelineHistoryPath: getPipelineHistoryPath(),
    reportsDir: getReportsDir(),
    exists: existsSync(getDeliveredPath()),
  }
}
