import type { PersistedDlpEvent } from './persistent_event_queue'

export type EventSinkKind = 'http' | 'central-http' | 'local-test'

export type EventSinkTlsConfig = {
  allowInvalidTls?: boolean
  caCertificatePath?: string
  clientCertificatePath?: string
  clientKeyPath?: string
  clientKeyPassphrase?: string
}

export type HttpEventSinkConfig = {
  id: string
  kind: 'http'
  enabled: boolean
  endpointUrl: string
  timeoutMs: number
  headers: Record<string, string>
} & EventSinkTlsConfig

export type CentralHttpEventSinkConfig = {
  id: string
  kind: 'central-http'
  enabled: boolean
  endpointUrl: string
  timeoutMs: number
  headers: Record<string, string>
  nodeId: string
  tenantId?: string
  apiKey?: string
  requestSigningKey?: string
  receiptSigningKey?: string
  receiptSignatureHeader?: string
  requireSignedReceipt?: boolean
} & EventSinkTlsConfig

export type LocalTestEventSinkConfig = {
  id: string
  kind: 'local-test'
  enabled: boolean
  label: string
}

export type DeliverySinkConfig = HttpEventSinkConfig | CentralHttpEventSinkConfig | LocalTestEventSinkConfig
export type DeliveryPipelineStrategy = 'all' | 'any'

export type DeliveryPipelineConfig = {
  enabled: boolean
  strategy: DeliveryPipelineStrategy
  sinks: DeliverySinkConfig[]
}

export type DeliveryDecision = 'ack' | 'retry' | 'drop'

export type DeliveryEventResult = {
  eventId: number
  decision: DeliveryDecision
  error?: string
  retryAfterMs?: number
  statusCode?: number
  remoteEventId?: string
}

export type DeliveryBackpressureHint = {
  scope?: 'global' | 'sink'
  pauseMs?: number
  maxBatchSize?: number
  reason?: string
}

export type DeliveryReceipt = {
  receiptId?: string
  signature?: string
  algorithm?: string
  verified?: boolean
}

export type DeliveryAttemptResult = {
  statusCode: number
  sinkId?: string
  sinkName: string
  batchId?: string
  error?: string
  retryAll?: boolean
  retryAfterMs?: number
  eventResults?: DeliveryEventResult[]
  backpressure?: DeliveryBackpressureHint
  receipt?: DeliveryReceipt
  sinkInfo?: Record<string, unknown>
}

export interface EventSink {
  readonly id: string
  readonly kind: EventSinkKind
  readonly name: string
  deliver(events: PersistedDlpEvent[], batchId: string): Promise<DeliveryAttemptResult>
}

export function createDefaultDeliverySinkConfig(): DeliverySinkConfig {
  return {
    id: 'default-http',
    kind: 'http',
    enabled: false,
    endpointUrl: 'http://127.0.0.1:7777/api/dlp/events',
    timeoutMs: 10000,
    headers: {
      'content-type': 'application/json',
    },
    allowInvalidTls: false,
  }
}

export function createDefaultDeliveryPipelineConfig(): DeliveryPipelineConfig {
  return {
    enabled: false,
    strategy: 'all',
    sinks: [createDefaultDeliverySinkConfig()],
  }
}
