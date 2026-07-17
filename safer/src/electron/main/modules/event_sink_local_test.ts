import { recordDeliveryTestBatch } from './delivery_test_sink'
import type { DeliveryAttemptResult, EventSink, LocalTestEventSinkConfig } from './event_sink'
import type { PersistedDlpEvent } from './persistent_event_queue'

export class LocalTestEventSink implements EventSink {
  readonly id: string
  readonly kind = 'local-test' as const
  readonly name: string
  private readonly config: LocalTestEventSinkConfig

  constructor(config: LocalTestEventSinkConfig) {
    this.config = config
    this.id = config.id
    this.name = `local-test:${config.label}`
  }

  async deliver(events: PersistedDlpEvent[], batchId: string): Promise<DeliveryAttemptResult> {
    const info = recordDeliveryTestBatch(events, { label: this.config.label })
    return {
      statusCode: 200,
      sinkId: this.id,
      sinkName: this.name,
      batchId,
      eventResults: events.map((event) => ({
        eventId: event.id,
        decision: 'ack' as const,
        statusCode: 200,
      })),
      sinkInfo: {
        ...info,
        batchId,
      },
    }
  }
}
