const { app } = require('electron')
const { spawn, spawnSync } = require('node:child_process')
const { existsSync } = require('node:fs')
const path = require('node:path')

const addonPath = path.resolve(__dirname, '..', 'src', 'native', 'build', 'Release', 'personal_safer.node')
const driverPath = path.resolve(__dirname, '..', 'build', 'kernel', 'x64', 'Release', 'PersonalSafer.sys')
const proxyModulePath = path.resolve(__dirname, '..', 'src', 'electron', 'main', 'modules', 'local_proxy.validate.cjs')

function runCommand(label, file, args) {
  console.log(`\n[Validate] ${label}: ${file} ${args.join(' ')}`)
  return new Promise((resolve) => {
    const child = spawn(file, args, { stdio: ['ignore', 'pipe', 'pipe'] })
    let stdout = ''
    let stderr = ''
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
    }, 45000)

    child.stdout.on('data', (chunk) => { stdout += chunk.toString() })
    child.stderr.on('data', (chunk) => { stderr += chunk.toString() })
    child.on('close', (code) => {
      clearTimeout(timer)
      console.log(`[Validate] exit=${code} stdout:\n${stdout || ''}`)
      if (stderr) {
        console.log(`[Validate] stderr:\n${stderr}`)
      }
      resolve({ status: code, stdout, stderr })
    })
  })
}

async function main() {
  if (!existsSync(addonPath)) {
    throw new Error(`addon not found: ${addonPath}`)
  }
  if (!existsSync(driverPath)) {
    throw new Error(`driver not found: ${driverPath}`)
  }
  if (!existsSync(proxyModulePath)) {
    throw new Error(`proxy module not found: ${proxyModulePath}`)
  }

  const addon = require(addonPath)
  const proxy = require(proxyModulePath)
  const emptyPolicy = {
    fileFilterEnabled: true,
    networkFilterEnabled: true,
    auditEnabled: true,
    blockedExtensions: [],
    processBlacklist: [],
    blockedPorts: [],
    blockedDomains: [],
    blockedUrls: [],
    blockedFtpCommands: [],
    blockedHttpHeaders: [],
    blockedHttpTrailers: [],
    blockedHttpBodyPatterns: [],
    blockedJsonKeys: [],
    blockedJsonPaths: [],
    blockedJsonValues: [],
  }

  let transparentReady = false

  try {
    try { addon.dlp.driver_loader.unload() } catch {}
    const loadResult = addon.dlp.driver_loader.load(driverPath)
    console.log('[Validate] loadResult:', JSON.stringify(loadResult))
    if (loadResult && loadResult.success) {
      addon.dlp.kernel_comm.connect()
      addon.dlp.policy_manager.setPolicy(emptyPolicy)
      transparentReady = true
    } else {
      console.log('[Validate] transparent redirect unavailable, falling back to explicit-proxy validation')
    }

    const proxyStatus = await proxy.startLocalProxy({
      enabled: true,
      installSystemProxy: false,
      mitmEnabled: true,
      host: '127.0.0.1',
      port: 8899,
    })
    console.log('[Validate] proxyStatus:', JSON.stringify(proxyStatus))

    if (transparentReady) {
      await runCommand('curl-http-transparent', 'curl.exe', ['-I', '--max-time', '20', 'http://example.com/'])
      await runCommand('curl-https-transparent', 'curl.exe', ['-I', '--max-time', '20', 'https://example.com/'])
    }

    const httpResult = await runCommand('curl-http-explicit', 'curl.exe', ['-I', '--proxy', 'http://127.0.0.1:8899', '--max-time', '20', 'http://example.com/'])
    if (httpResult.status !== 0) throw new Error('explicit HTTP proxy validation failed')
    const httpsResult = await runCommand('curl-https-explicit', 'curl.exe', ['--ssl-no-revoke', '-I', '--proxy', 'http://127.0.0.1:8899', '--max-time', '20', 'https://example.com/'])
    if (httpsResult.status !== 0) throw new Error('trusted explicit HTTPS proxy validation failed')

    const edgePath = spawnSync('where.exe', ['msedge.exe'], { encoding: 'utf8' })
    if (edgePath.status === 0) {
      const edgeExe = edgePath.stdout.split(/\r?\n/).find(Boolean)
      if (edgeExe) {
        if (transparentReady) {
          await runCommand('edge-http-transparent', edgeExe, ['--headless=new', '--disable-gpu', '--dump-dom', 'http://example.com/'])
          await runCommand('edge-https-transparent', edgeExe, ['--headless=new', '--disable-gpu', '--dump-dom', 'https://example.com/'])
        }
        await runCommand('edge-http-explicit', edgeExe, ['--proxy-server=http://127.0.0.1:8899', '--headless=new', '--disable-gpu', '--dump-dom', 'http://example.com/'])
        await runCommand('edge-https-explicit', edgeExe, ['--proxy-server=http://127.0.0.1:8899', '--headless=new', '--disable-gpu', '--ignore-certificate-errors', '--dump-dom', 'https://example.com/'])
      }
    } else {
      console.log('[Validate] msedge.exe not found, skipped browser-style validation')
    }

    if (transparentReady) {
      const status = addon.dlp.kernel_comm.getDriverStatus()
      console.log('[Validate] driverStatus:', JSON.stringify(status))
    }
  } finally {
    try { await proxy.stopLocalProxy() } catch (error) { console.log('[Validate] stop proxy failed', error) }
    try { addon.dlp.kernel_comm.disconnect() } catch {}
    try { addon.dlp.driver_loader.unload() } catch {}
  }
}

app.whenReady().then(async () => {
  try {
    await main()
    app.exit(0)
  } catch (error) {
    console.error('[Validate] failed:', error && error.stack ? error.stack : error)
    app.exit(1)
  }
})
