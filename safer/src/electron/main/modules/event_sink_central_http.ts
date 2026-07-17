import { createHmac, timingSafeEqual } from 'node:crypto'
import type {
  CentralHttpEventSinkConfig,
  DeliveryAttemptResult,
  DeliveryEventResult,
  EventSink,
} from './event_sink'
import { parseBackpressureHint, parseRetryAfterMs, postJson } from './event_sink_http_common'
import type { PersistedDlpEvent } from './persistent_event_queue'

function parseCentralResults(json: any, statusCode: number): DeliveryEventResult[] {
  const accepted = Array.isArray(json?.accepted) ? json.accepted : []
  const retry = Array.isArray(json?.retry) ? json.retry : []
  const rejected = Array.isArray(json?.rejected) ? json.rejected : []
  const results: DeliveryEventResult[] = []

  for (const item of accepted) {
    const eventId = Number(item?.eventId)
    if (!Number.isFinite(eventId)) {
      continue
    }
    results.push({
      eventId,
      decision: 'ack',
      statusCode,
      remoteEventId: typeof item?.remoteEventId === 'string' ? item.remoteEventId : undefined,
    })
  }

  for (const item of retry) {
    const eventId = Number(item?.eventId)
    if (!Number.isFinite(eventId)) {
      continue
    }
    results.push({
      eventId,
      decision: 'retry',
      statusCode,
      error: typeof item?.error === 'string' ? item.error : 'retry requested by center',
      retryAfterMs: typeof item?.retryAfterMs === 'number' ? item.retryAfterMs : undefined,
    })
  }

  for (const item of rejected) {
    const eventId = Number(item?.eventId)
    if (!Number.isFinite(eventId)) {
      continue
    }
    results.push({
      eventId,
      decision: 'drop',
      statusCode,
      error: typeof item?.error === 'string' ? item.error : 'rejected by center',
    })
  }

  return results
}

export class CentralHttpEventSink implements EventSink {
  readonly id: string
  readonly kind = 'central-http' as const
  readonly name: string
  private readonly config: CentralHttpEventSinkConfig

  constructor(config: CentralHttpEventSinkConfig) {
    this.config = config
    this.id = config.id
    this.name = `central-http:${config.endpointUrl}`
  }

  async deliver(events: PersistedDlpEvent[], batchId: string): Promise<DeliveryAttemptResult> {
    const body = JSON.stringify({
      source: 'PersonalSafer',
      batchId,
      sentAt: new Date().toISOString(),
      nodeId: this.config.nodeId,
      tenantId: this.config.tenantId,
      agentVersion: process.versions.electron || process.version,
      events,
    })

    const requestSignature = this.config.requestSigningKey
      ? hmacSha256(body, this.config.requestSigningKey)
      : ''

    const headers = {
      ...this.config.headers,
      ...(this.config.apiKey ? { authorization: `Bearer ${this.config.apiKey}` } : {}),
      'x-ps-node-id': this.config.nodeId,
      ...(this.config.tenantId ? { 'x-ps-tenant-id': this.config.tenantId } : {}),
      ...(requestSignature ? { 'x-ps-request-signature': requestSignature } : {}),
    }

    const response = await postJson({
      endpointUrl: this.config.endpointUrl,
      timeoutMs: this.config.timeoutMs,
      headers,
      allowInvalidTls: this.config.allowInvalidTls,
      caCertificatePath: this.config.caCertificatePath,
      clientCertificatePath: this.config.clientCertificatePath,
      clientKeyPath: this.config.clientKeyPath,
      clientKeyPassphrase: this.config.clientKeyPassphrase,
      body,
    })

    const retryAfterMs = parseRetryAfterMs(response.headers['retry-after'])
    const backpressure = parseBackpressureHint(response.headers, response.json)
    const receipt = this.verifyReceipt(response.text, response.headers)
    if (this.config.requireSignedReceipt && !receipt.verified) {
      return {
        statusCode: response.statusCode || 502,
        sinkId: this.id,
        sinkName: this.name,
        batchId,
        retryAll: true,
        retryAfterMs: retryAfterMs ?? 5000,
        error: 'signed receipt verification failed',
        backpressure,
        receipt,
        eventResults: events.map((event) => ({
          eventId: event.id,
          decision: 'retry',
          statusCode: response.statusCode,
          retryAfterMs: retryAfterMs ?? 5000,
          error: 'signed receipt verification failed',
        })),
      }
    }
    const eventResults = parseCentralResults(response.json, response.statusCode)
    if (eventResults.length > 0) {
      return {
        statusCode: response.statusCode,
        sinkId: this.id,
        sinkName: this.name,
        batchId,
        retryAfterMs,
        eventResults,
        backpressure,
        receipt,
        sinkInfo: {
          endpointUrl: this.config.endpointUrl,
          nodeId: this.config.nodeId,
          tenantId: this.config.tenantId || '',
          responseText: response.text.slice(0, 512),
        },
      }
    }

    if (response.statusCode >= 200 && response.statusCode < 300) {
      return {
        statusCode: response.statusCode,
        sinkId: this.id,
        sinkName: this.name,
        batchId,
        eventResults: events.map((event) => ({
          eventId: event.id,
          decision: 'ack',
          statusCode: response.statusCode,
        })),
        backpressure,
        receipt,
        sinkInfo: {
          endpointUrl: this.config.endpointUrl,
          nodeId: this.config.nodeId,
          tenantId: this.config.tenantId || '',
          responseText: response.text.slice(0, 512),
        },
      }
    }

    const retryAll = response.statusCode === 408 ||
      response.statusCode === 425 ||
      response.statusCode === 429 ||
      response.statusCode >= 500

    return {
      statusCode: response.statusCode,
      sinkId: this.id,
      sinkName: this.name,
      batchId,
      retryAll,
      retryAfterMs,
      backpressure,
      receipt,
      eventResults: events.map((event) => ({
        eventId: event.id,
        decision: retryAll ? 'retry' : 'drop',
        statusCode: response.statusCode,
        retryAfterMs,
        error: response.text ? response.text.slice(0, 512) : `HTTP ${response.statusCode}`,
      })),
      sinkInfo: {
        endpointUrl: this.config.endpointUrl,
        nodeId: this.config.nodeId,
        tenantId: this.config.tenantId || '',
        responseText: response.text.slice(0, 512),
      },
    }
  }

  private verifyReceipt(responseText: string, headers: Record<string, string | string[] | undefined>) {
    const headerName = (this.config.receiptSignatureHeader || 'x-ps-receipt-signature').toLowerCase()
    const signatureHeader = headers[headerName]
    const signature = Array.isArray(signatureHeader) ? signatureHeader[0] : signatureHeader
    const verified = !!(signature && this.config.receiptSigningKey &&
      secureEqual(signature, hmacSha256(responseText, this.config.receiptSigningKey)))

    return {
      signature: signature || '',
      algorithm: signature ? 'hmac-sha256' : '',
      verified,
    }
  }
}

function hmacSha256(content: string, key: string): string {
  return createHmac('sha256', key).update(content, 'utf8').digest('hex')
}

function secureEqual(left: string, right: string): boolean {
  const leftBuffer = Buffer.from(left, 'utf8')
  const rightBuffer = Buffer.from(right, 'utf8')
  if (leftBuffer.length !== rightBuffer.length) {
    return false
  }
  return timingSafeEqual(leftBuffer, rightBuffer)
}
