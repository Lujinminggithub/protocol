import * as http from 'node:http'
import * as https from 'node:https'
import { existsSync, readFileSync } from 'node:fs'
import type {
  DeliveryAttemptResult,
  DeliveryBackpressureHint,
  DeliveryDecision,
  DeliveryEventResult,
  EventSinkTlsConfig,
} from './event_sink'
import type { PersistedDlpEvent } from './persistent_event_queue'

type PostJsonOptions = {
  endpointUrl: string
  timeoutMs: number
  headers: Record<string, string>
  body: string
} & EventSinkTlsConfig

type HttpResponseData = {
  statusCode: number
  headers: http.IncomingHttpHeaders
  text: string
  json: any
}

export async function postJson(options: PostJsonOptions): Promise<HttpResponseData> {
  const url = new URL(options.endpointUrl)
  const transport = url.protocol === 'https:' ? https : http
  const tlsOptions = loadTlsOptions(options)

  return new Promise<HttpResponseData>((resolve, reject) => {
    const req = transport.request(
      {
        protocol: url.protocol,
        hostname: url.hostname,
        port: url.port ? Number(url.port) : (url.protocol === 'https:' ? 443 : 80),
        path: `${url.pathname}${url.search}`,
        method: 'POST',
        headers: {
          ...options.headers,
          'content-length': String(Buffer.byteLength(options.body)),
        },
        rejectUnauthorized: options.allowInvalidTls ? false : true,
        ...tlsOptions,
      },
      (res) => {
        const chunks: Buffer[] = []
        res.on('data', (chunk) => chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)))
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8')
          let json: any = null
          try {
            json = text ? JSON.parse(text) : null
          } catch {
            json = null
          }
          resolve({
            statusCode: res.statusCode || 0,
            headers: res.headers,
            text,
            json,
          })
        })
      }
    )

    req.setTimeout(options.timeoutMs, () => {
      req.destroy(new Error(`timeout after ${options.timeoutMs}ms`))
    })
    req.on('error', reject)
    req.write(options.body)
    req.end()
  })
}

function loadTlsOptions(options: EventSinkTlsConfig): Record<string, Buffer | string> {
  const tlsOptions: Record<string, Buffer | string> = {}
  if (options.caCertificatePath && existsSync(options.caCertificatePath)) {
    tlsOptions.ca = readFileSync(options.caCertificatePath)
  }
  if (options.clientCertificatePath && existsSync(options.clientCertificatePath)) {
    tlsOptions.cert = readFileSync(options.clientCertificatePath)
  }
  if (options.clientKeyPath && existsSync(options.clientKeyPath)) {
    tlsOptions.key = readFileSync(options.clientKeyPath)
  }
  if (options.clientKeyPassphrase) {
    tlsOptions.passphrase = options.clientKeyPassphrase
  }
  return tlsOptions
}

export function parseRetryAfterMs(value: string | string[] | undefined): number | undefined {
  if (!value) {
    return undefined
  }

  const raw = Array.isArray(value) ? value[0] : value
  const seconds = Number(raw)
  if (Number.isFinite(seconds) && seconds >= 0) {
    return seconds * 1000
  }

  const dateMs = Date.parse(raw)
  if (!Number.isFinite(dateMs)) {
    return undefined
  }

  return Math.max(0, dateMs - Date.now())
}

function parseNumber(value: unknown): number | undefined {
  const num = Number(value)
  return Number.isFinite(num) ? num : undefined
}

export function parseBackpressureHint(
  headers: http.IncomingHttpHeaders,
  json: any
): DeliveryBackpressureHint | undefined {
  const retryAfterMs = parseRetryAfterMs(headers['retry-after'])
  const pauseMs = parseNumber(headers['x-ps-pause-ms']) ??
    parseNumber(json?.backpressure?.pauseMs) ??
    retryAfterMs
  const maxBatchSize = parseNumber(headers['x-ps-max-batch-size']) ??
    parseNumber(json?.backpressure?.maxBatchSize)
  const reason = typeof json?.backpressure?.reason === 'string'
    ? json.backpressure.reason
    : (typeof headers['x-ps-backpressure-reason'] === 'string' ? headers['x-ps-backpressure-reason'] : undefined)

  if (pauseMs === undefined && maxBatchSize === undefined && !reason) {
    return undefined
  }

  return {
    scope: json?.backpressure?.scope === 'global' ? 'global' : 'sink',
    pauseMs,
    maxBatchSize,
    reason,
  }
}

function normalizeDecision(value: unknown): DeliveryDecision | null {
  return value === 'ack' || value === 'retry' || value === 'drop' ? value : null
}

function parseEventResult(input: any): DeliveryEventResult | null {
  const eventId = Number(input?.eventId)
  const decision = normalizeDecision(input?.decision)
  if (!Number.isFinite(eventId) || !decision) {
    return null
  }

  return {
    eventId,
    decision,
    error: typeof input?.error === 'string' ? input.error : undefined,
    retryAfterMs: typeof input?.retryAfterMs === 'number' ? input.retryAfterMs : undefined,
    statusCode: typeof input?.statusCode === 'number' ? input.statusCode : undefined,
    remoteEventId: typeof input?.remoteEventId === 'string' ? input.remoteEventId : undefined,
  }
}

function parseExplicitEventResults(json: any): DeliveryEventResult[] {
  if (!Array.isArray(json?.eventResults)) {
    return []
  }
  return json.eventResults
    .map((item: any) => parseEventResult(item))
    .filter((item: DeliveryEventResult | null): item is DeliveryEventResult => item !== null)
}

function inferDecisionFromStatus(statusCode: number): DeliveryDecision {
  if (statusCode >= 200 && statusCode < 300) {
    return 'ack'
  }
  if (statusCode === 408 || statusCode === 425 || statusCode === 429 || statusCode >= 500) {
    return 'retry'
  }
  return 'drop'
}

function buildUniformResults(
  events: PersistedDlpEvent[],
  decision: DeliveryDecision,
  statusCode: number,
  error?: string,
  retryAfterMs?: number
): DeliveryEventResult[] {
  return events.map((event) => ({
    eventId: event.id,
    decision,
    error: decision === 'ack' ? undefined : error,
    retryAfterMs,
    statusCode,
  }))
}

export function buildGenericHttpAttemptResult(
  sinkId: string,
  sinkName: string,
  endpointUrl: string,
  batchId: string,
  events: PersistedDlpEvent[],
  response: HttpResponseData
): DeliveryAttemptResult {
  const retryAfterMs = parseRetryAfterMs(response.headers['retry-after'])
  const backpressure = parseBackpressureHint(response.headers, response.json)
  const explicitResults = parseExplicitEventResults(response.json)

  if (explicitResults.length > 0) {
    return {
      statusCode: response.statusCode,
      sinkId,
      sinkName,
      batchId,
      retryAfterMs,
      eventResults: explicitResults,
      backpressure,
      sinkInfo: {
        endpointUrl,
        responseText: response.text.slice(0, 512),
      },
    }
  }

  const decision = inferDecisionFromStatus(response.statusCode)
  return {
    statusCode: response.statusCode,
    sinkId,
    sinkName,
    batchId,
    retryAll: decision === 'retry',
    retryAfterMs,
    backpressure,
    eventResults: buildUniformResults(
      events,
      decision,
      response.statusCode,
      response.text ? response.text.slice(0, 512) : undefined,
      retryAfterMs
    ),
    sinkInfo: {
      endpointUrl,
      responseText: response.text.slice(0, 512),
    },
  }
}
