import { app } from 'electron'
import { mkdirSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import {
  clearDeliveryTestSinkRecords,
  getDeliveryTestSinkStatus,
  startDeliveryTestSink,
  stopDeliveryTestSink,
} from './delivery_test_sink'
import {
  getDeliveryWorkerStatus,
  setDeliverySinkConfig,
  startDeliveryWorker,
  stopDeliveryWorker,
} from './delivery_worker'
import {
  clearEvents,
  flushEventPersistence,
  getStats,
  getPersistentQueueStats,
  initializeEventPersistence,
  recordExternalNetEvent,
} from './dlp_events'

function writeText(outDir: string, name: string, content: string): void {
  mkdirSync(outDir, { recursive: true })
  writeFileSync(join(outDir, name), content, 'utf8')
}

export async function runInstalledDeliverySelfCheck(outDir: string): Promise<number> {
  mkdirSync(outDir, { recursive: true })
  let exitCode = 0

  try {
    initializeEventPersistence()
    clearEvents()
    await stopDeliveryWorker()

    const sink = await startDeliveryTestSink(7777)
    clearDeliveryTestSinkRecords()
    setDeliverySinkConfig({
      enabled: true,
      strategy: 'all',
      sinks: [
        {
          id: 'loopback-http',
          kind: 'http',
          enabled: true,
          endpointUrl: `http://127.0.0.1:${sink.port}/api/dlp/events`,
          timeoutMs: 5000,
          headers: { 'content-type': 'application/json' },
        },
        {
          id: 'loopback-center',
          kind: 'central-http',
          enabled: true,
          endpointUrl: `http://127.0.0.1:${sink.port}/api/dlp/center/events`,
          timeoutMs: 5000,
          headers: { 'content-type': 'application/json' },
          nodeId: 'delivery-self-check-node',
          tenantId: 'local-test',
          requestSigningKey: 'delivery-test-receipt-secret',
          receiptSigningKey: 'delivery-test-receipt-secret',
          requireSignedReceipt: true,
        },
      ],
    })

    recordExternalNetEvent({
      type: 'network_connect',
      action: 'logged',
      processName: 'selfcheck.exe',
      processId: 1234,
      timestamp: new Date().toISOString(),
      details: 'delivery-center-allow',
      url: 'http://delivery-test.local/1',
    })
    recordExternalNetEvent({
      type: 'network_connect',
      action: 'logged',
      processName: 'selfcheck.exe',
      processId: 1234,
      timestamp: new Date().toISOString(),
      details: 'delivery-center-retry-once',
      url: 'http://delivery-test.local/2',
    })
    recordExternalNetEvent({
      type: 'network_connect',
      action: 'logged',
      processName: 'selfcheck.exe',
      processId: 1234,
      timestamp: new Date().toISOString(),
      details: 'delivery-center-drop',
      url: 'http://delivery-test.local/3',
    })

    startDeliveryWorker(500, 10)
    await new Promise((resolve) => setTimeout(resolve, 3500))

    const result = {
      deliveryStatus: getDeliveryWorkerStatus(),
      sinkStatus: getDeliveryTestSinkStatus(),
      queueStats: getPersistentQueueStats(),
      auditStats: getStats(),
    }
    writeText(outDir, 'delivery-self-check-summary.json', JSON.stringify(result, null, 2))

    if (!(result.deliveryStatus.deliveredCount >= 2 &&
        result.sinkStatus.receivedCount >= 3 &&
        result.queueStats.sent >= 2 &&
        result.queueStats.abandoned >= 1 &&
        result.auditStats.networkEvents === 3 &&
        result.auditStats.fileEvents === 0 &&
        result.queueStats.pending === 0 &&
        result.queueStats.sending === 0 &&
        result.sinkStatus.eventAttempts?.['2'] === 2 &&
        Array.isArray(result.deliveryStatus.lastResults) &&
        result.deliveryStatus.lastResults.some((item: any) => item?.receipt?.verified === true))) {
      exitCode = 1
    }
  } catch (error: any) {
    writeText(outDir, 'delivery-self-check-error.txt', error?.stack || error?.message || String(error))
    exitCode = 1
  } finally {
    try { await stopDeliveryWorker() } catch {}
    try { await stopDeliveryTestSink() } catch {}
    try { flushEventPersistence() } catch {}
  }

  return exitCode
}

export function parseDeliverySelfCheckArgs(): { enabled: boolean; outDir: string } {
  const enabled = process.argv.includes('--ps-self-check-delivery')
  const arg = process.argv.find((item) => item.startsWith('--ps-self-check-out='))
  const outDir = arg
    ? arg.slice('--ps-self-check-out='.length)
    : join(app.getPath('temp'), `PersonalSafer-delivery-self-check-${Date.now()}`)
  return { enabled, outDir }
}
