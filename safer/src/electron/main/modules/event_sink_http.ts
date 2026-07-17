import type { EventSink, DeliveryAttemptResult, HttpEventSinkConfig } from './event_sink'
import type { PersistedDlpEvent } from './persistent_event_queue'
import { buildGenericHttpAttemptResult, postJson } from './event_sink_http_common'

export class HttpEventSink implements EventSink {
  readonly id: string
  readonly kind = 'http' as const
  readonly name: string
  private readonly config: HttpEventSinkConfig

  constructor(config: HttpEventSinkConfig) {
    this.config = config
    this.id = config.id
    this.name = `http:${config.endpointUrl}`
  }

  async deliver(events: PersistedDlpEvent[], batchId: string): Promise<DeliveryAttemptResult> {
    const body = JSON.stringify({
      source: 'PersonalSafer',
      batchId,
      sentAt: new Date().toISOString(),
      events,
    })
    const response = await postJson({
      endpointUrl: this.config.endpointUrl,
      timeoutMs: this.config.timeoutMs,
      headers: this.config.headers,
      allowInvalidTls: this.config.allowInvalidTls,
      body,
    })

    return buildGenericHttpAttemptResult(this.id, this.name, this.config.endpointUrl, batchId, events, response)
  }
}
