import * as http from 'node:http'
import { createHmac } from 'node:crypto'
import { appendFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { app } from 'electron'
import type { PersistedDlpEvent } from './persistent_event_queue'

type ReceivedSinkRecord = {
  receivedAt: string
  body: unknown
}

type DeliveryTestSinkState = {
  running: boolean
  port: number
  server: http.Server | null
  outDir: string
  recordsPath: string
  receivedCount: number
  eventAttempts: Record<number, number>
}

const SINK_DIR = 'delivery-test-sink'
const RECORDS_FILE = 'received.ndjson'
const TEST_RECEIPT_SECRET = 'delivery-test-receipt-secret'

const state: DeliveryTestSinkState = {
  running: false,
  port: 0,
  server: null,
  outDir: '',
  recordsPath: '',
  receivedCount: 0,
  eventAttempts: {},
}

function getSinkDir(): string {
  return join(app.getPath('userData'), SINK_DIR)
}

function getRecordsPath(): string {
  return join(getSinkDir(), RECORDS_FILE)
}

function ensureSinkDir(): void {
  mkdirSync(getSinkDir(), { recursive: true })
}

function appendRecord(record: ReceivedSinkRecord): void {
  ensureSinkDir()
  appendFileSync(getRecordsPath(), `${JSON.stringify(record)}\n`, 'utf8')
}

function buildCenterResponse(body: any) {
  const events = Array.isArray(body?.events) ? body.events : []
  const accepted: Array<Record<string, unknown>> = []
  const retry: Array<Record<string, unknown>> = []
  const rejected: Array<Record<string, unknown>> = []
  let backpressure: Record<string, unknown> | undefined

  for (const event of events) {
    const eventId = Number(event?.id)
    if (!Number.isFinite(eventId)) {
      continue
    }

    state.eventAttempts[eventId] = (state.eventAttempts[eventId] || 0) + 1
    const details = String(event?.details || '')
    if (details.includes('center-drop')) {
      rejected.push({
        eventId,
        error: 'rejected by test center',
      })
      continue
    }

    if (details.includes('center-retry-once') && state.eventAttempts[eventId] === 1) {
      backpressure = {
        scope: 'sink',
        pauseMs: 750,
        maxBatchSize: 1,
        reason: 'transient test backpressure',
      }
      retry.push({
        eventId,
        error: 'transient retry requested by test center',
        retryAfterMs: 750,
      })
      continue
    }

    accepted.push({
      eventId,
      remoteEventId: `center-${eventId}-${state.eventAttempts[eventId]}`,
    })
  }

  return {
    ok: true,
    batchId: typeof body?.batchId === 'string' ? body.batchId : '',
    accepted,
    retry,
    rejected,
    backpressure,
  }
}

function signResponse(body: string): string {
  return createHmac('sha256', TEST_RECEIPT_SECRET).update(body, 'utf8').digest('hex')
}

function verifyRequestSignature(req: http.IncomingMessage, rawBody: string): boolean {
  const signatureHeader = req.headers['x-ps-request-signature']
  const signature = Array.isArray(signatureHeader) ? signatureHeader[0] : signatureHeader
  if (!signature) {
    return true
  }
  return signature === signResponse(rawBody)
}

export function recordDeliveryTestBatch(events: PersistedDlpEvent[], meta?: Record<string, unknown>) {
  appendRecord({
    receivedAt: new Date().toISOString(),
    body: {
      source: 'local-test-sink',
      meta: meta || {},
      events,
    },
  })
  state.receivedCount++
  return {
    recordsPath: getRecordsPath(),
    receivedCount: state.receivedCount,
  }
}

export async function startDeliveryTestSink(port: number = 7777): Promise<{ port: number; recordsPath: string }> {
  if (state.running && state.server) {
    return { port: state.port, recordsPath: state.recordsPath }
  }

  ensureSinkDir()
  state.recordsPath = getRecordsPath()
  state.outDir = getSinkDir()

  state.server = http.createServer((req, res) => {
    if (req.method !== 'POST') {
      res.writeHead(405, { 'content-type': 'application/json' })
      res.end(JSON.stringify({ ok: false, error: 'method not allowed' }))
      return
    }

    const chunks: Buffer[] = []
    req.on('data', (chunk) => chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)))
    req.on('end', () => {
      const raw = Buffer.concat(chunks).toString('utf8')
      let body: unknown = raw
      try {
        body = JSON.parse(raw)
      } catch {
        // Keep raw body if JSON parsing fails.
      }
      appendRecord({
        receivedAt: new Date().toISOString(),
        body: {
          path: req.url || '',
          payload: body,
        },
      })
      state.receivedCount++
      if ((req.url || '').startsWith('/api/dlp/center/events')) {
        if (!verifyRequestSignature(req, raw)) {
          res.writeHead(401, { 'content-type': 'application/json' })
          res.end(JSON.stringify({ ok: false, error: 'invalid request signature' }))
          return
        }
        const responseText = JSON.stringify(buildCenterResponse(body))
        res.setHeader('x-ps-receipt-signature', signResponse(responseText))
        res.writeHead(200, { 'content-type': 'application/json' })
        res.end(responseText)
        return
      }
      res.writeHead(200, { 'content-type': 'application/json' })
      res.end(JSON.stringify({ ok: true, receivedCount: state.receivedCount }))
    })
  })

  await new Promise<void>((resolve, reject) => {
    state.server?.once('error', reject)
    state.server?.listen(port, '127.0.0.1', () => resolve())
  })

  state.running = true
  state.port = port
  return { port: state.port, recordsPath: state.recordsPath }
}

export async function stopDeliveryTestSink(): Promise<void> {
  if (!state.server) {
    state.running = false
    return
  }

  await new Promise<void>((resolve) => state.server?.close(() => resolve()) ?? resolve())
  state.server = null
  state.running = false
}

export function clearDeliveryTestSinkRecords(): void {
  ensureSinkDir()
  writeFileSync(getRecordsPath(), '', 'utf8')
  state.receivedCount = 0
  state.eventAttempts = {}
}

export function getDeliveryTestSinkStatus() {
  return {
    running: state.running,
    port: state.port,
    outDir: state.outDir || getSinkDir(),
    recordsPath: state.recordsPath || getRecordsPath(),
    exists: existsSync(getRecordsPath()),
    receivedCount: state.receivedCount,
    eventAttempts: { ...state.eventAttempts },
  }
}
