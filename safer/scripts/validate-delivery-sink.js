const { app } = require('electron')
const os = require('node:os')
const path = require('node:path')

const deliverySelfCheckPath = path.resolve(__dirname, '..', 'src', 'electron', 'main', 'modules', 'delivery_self_check.validate.cjs')

async function main() {
  const selfCheck = require(deliverySelfCheckPath)
  const outDir = path.join(os.tmpdir(), `PersonalSafer-delivery-self-check-${Date.now()}`)
  const exitCode = await selfCheck.runInstalledDeliverySelfCheck(outDir)
  console.log(`[ValidateDelivery] output directory: ${outDir}`)
  process.exitCode = exitCode
}

app.whenReady().then(async () => {
  try {
    await main()
    app.exit(process.exitCode || 0)
  } catch (error) {
    console.error(error && error.stack ? error.stack : error)
    app.exit(1)
  }
})
