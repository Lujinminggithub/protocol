const fs = require('node:fs')
const path = require('node:path')
const { app } = require('electron')

function fail(message) {
  console.error(`[quarantine-catalog] ${message}`)
  app.exit(1)
}

app.whenReady().then(() => {
  const modulePath = process.env.PS_QUARANTINE_CATALOG_MODULE
  const phase = process.env.PS_QUARANTINE_CATALOG_PHASE || 'write'
  const shadowPath = process.env.PS_QUARANTINE_SHADOW_PATH
  if (!modulePath || !shadowPath) return fail('validation environment is incomplete')

  try {
    const catalog = require(modulePath)
    catalog.initializeQuarantineCatalog()

    if (phase === 'write') {
      fs.mkdirSync(path.dirname(shadowPath), { recursive: true })
      fs.writeFileSync(shadowPath, 'catalog-validation', 'utf8')
      catalog.recordQuarantineCatalogEntry({
        originalPath: 'C:\\Sensitive\\quarterly-report.txt',
        quarantinePath: shadowPath,
        processName: 'catalog-test.exe',
        pid: 4242,
        eventType: 'file_write',
        action: 'quarantined',
        timestamp: new Date().toISOString(),
      })
      catalog.flushQuarantineCatalog()
    }

    const records = catalog.listQuarantineCatalog({ search: 'quarterly-report', limit: 10 })
    const record = records.find((item) => item.processName === 'catalog-test.exe')
    if (!record) return fail(`record not found during ${phase}`)
    if (record.status !== 'present' || record.size !== 18) {
      return fail(`unexpected file state during ${phase}: ${record.status}/${record.size}`)
    }

    console.log(JSON.stringify({ phase, count: records.length, record }))
    app.exit(0)
  } catch (error) {
    fail(error?.stack || error?.message || String(error))
  }
})
