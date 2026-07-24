import { app } from 'electron'
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import * as http from 'node:http'
import * as net from 'node:net'
import { join } from 'node:path'
import * as tls from 'node:tls'
import { getAddon, isAddonLoaded } from './native_loader'
import { startLocalProxy, stopLocalProxy, updateLocalProxyPolicy } from './local_proxy'

type SelfCheckArgs = {
  enabled: boolean
  outDir: string
}

type WsEndpoint = {
  host: string
  port: number
  securePort: number
  path: string
}

const MITM_CERT_PASSWORD = 'PersonalSafer-MITM-2026!'
const EXPLICIT_PROXY_HOST = '127.0.0.1'

function parseArgValue(prefix: string): string | null {
  const item = process.argv.find((arg) => arg.startsWith(prefix))
  return item ? item.slice(prefix.length) : null
}

export function parseSelfCheckArgs(): SelfCheckArgs {
  const enabled = process.argv.includes('--ps-self-check-proxy')
  const outDirArg = parseArgValue('--ps-self-check-out=')
  const outDir = outDirArg && outDirArg.trim()
    ? outDirArg.trim()
    : join(app.getPath('temp'), `PersonalSafer-self-check-${Date.now()}`)

  return { enabled, outDir }
}

function getDriverSysPath(): string {
  if (app.isPackaged) {
    return join(process.resourcesPath, 'driver', 'PersonalSafer.sys')
  }
  return join(__dirname, '..', '..', '..', 'kernel', 'x64', 'Release', 'PersonalSafer.sys')
}

function writeText(outDir: string, name: string, content: string): void {
  writeFileSync(join(outDir, name), content, 'utf8')
}

function runCommand(file: string, args: string[], timeoutMs = 45000): Promise<{ code: number | null; stdout: string; stderr: string }> {
  return new Promise((resolve) => {
    const child = spawn(file, args, { stdio: ['ignore', 'pipe', 'pipe'] })
    let stdout = ''
    let stderr = ''
    const timer = setTimeout(() => {
      try { child.kill() } catch {}
    }, timeoutMs)

    child.stdout.on('data', (chunk) => { stdout += chunk.toString() })
    child.stderr.on('data', (chunk) => { stderr += chunk.toString() })
    child.on('close', (code) => {
      clearTimeout(timer)
      resolve({ code, stdout, stderr })
    })
  })
}

function getEphemeralPort(host = '127.0.0.1'): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = net.createServer()
    server.on('error', reject)
    server.listen(0, host, () => {
      const address = server.address()
      const port = typeof address === 'object' && address ? address.port : 0
      server.close(() => resolve(port))
    })
  })
}

function buildUpgradeRequest(hostHeader: string, pathValue: string): Buffer {
  const key = Buffer.from(`${Date.now()}-${Math.random()}`).toString('base64')
  const request =
    `GET ${pathValue} HTTP/1.1\r\n` +
    `Host: ${hostHeader}\r\n` +
    `Upgrade: websocket\r\n` +
    `Connection: Upgrade\r\n` +
    `Sec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n\r\n`
  return Buffer.from(request, 'utf8')
}

function createMaskedTextFrame(text: string): Buffer {
  const payload = Buffer.from(text, 'utf8')
  const mask = Buffer.from([1, 2, 3, 4])
  const header = payload.length < 126
    ? Buffer.from([0x81, 0x80 | payload.length])
    : (() => {
        const h = Buffer.alloc(4)
        h[0] = 0x81
        h[1] = 0x80 | 126
        h.writeUInt16BE(payload.length, 2)
        return h
      })()

  const masked = Buffer.alloc(payload.length)
  for (let i = 0; i < payload.length; i++) {
    masked[i] = payload[i] ^ mask[i % 4]
  }
  return Buffer.concat([header, mask, masked])
}

async function readHttpHeader(socket: net.Socket): Promise<{ header: string; leftover: Buffer }> {
  let buffer = Buffer.alloc(0)

  return new Promise((resolve, reject) => {
    const onData = (chunk: Buffer) => {
      buffer = Buffer.concat([buffer, chunk])
      const marker = buffer.indexOf('\r\n\r\n')
      if (marker >= 0) {
        cleanup()
        resolve({
          header: buffer.subarray(0, marker + 4).toString('latin1'),
          leftover: buffer.subarray(marker + 4),
        })
      }
    }
    const onError = (error: Error) => {
      cleanup()
      reject(error)
    }
    const cleanup = () => {
      socket.off('data', onData)
      socket.off('error', onError)
    }

    socket.on('data', onData)
    socket.on('error', onError)
  })
}

async function readWebSocketOutcome(
  socket: net.Socket,
  initial: Buffer<ArrayBufferLike> = Buffer.alloc(0)
): Promise<{ type: string; text?: string; code?: number; reason?: string }> {
  let pending = initial

  while (true) {
    if (pending.length < 2) {
      const chunk = await new Promise<Buffer>((resolve, reject) => {
        socket.once('data', resolve)
        socket.once('error', reject)
      })
      pending = Buffer.concat([pending, chunk])
      continue
    }

    const first = pending[0]
    const second = pending[1]
    const opcode = first & 0x0f
    let payloadLength = second & 0x7f
    let offset = 2
    if (payloadLength === 126) {
      if (pending.length < 4) continue
      payloadLength = pending.readUInt16BE(2)
      offset = 4
    }
    if (pending.length < offset + payloadLength) {
      const chunk = await new Promise<Buffer>((resolve, reject) => {
        socket.once('data', resolve)
        socket.once('error', reject)
      })
      pending = Buffer.concat([pending, chunk])
      continue
    }

    const payload = pending.subarray(offset, offset + payloadLength)
    pending = pending.subarray(offset + payloadLength)

    if (opcode === 0x1) {
      return { type: 'text', text: payload.toString('utf8') }
    }
    if (opcode === 0x8) {
      return {
        type: 'close',
        code: payload.length >= 2 ? payload.readUInt16BE(0) : 0,
        reason: payload.length > 2 ? payload.subarray(2).toString('utf8') : '',
      }
    }
  }
}

function createAcceptValue(key: string): string {
  const crypto = require('node:crypto')
  return crypto.createHash('sha1').update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`, 'utf8').digest('base64')
}

function attachEcho(socket: net.Socket): void {
  let pending = Buffer.alloc(0)
  socket.on('data', (chunk) => {
    pending = Buffer.concat([pending, chunk])
    while (pending.length >= 2) {
      const second = pending[1]
      const opcode = pending[0] & 0x0f
      const masked = (second & 0x80) !== 0
      let payloadLength = second & 0x7f
      let offset = 2
      if (payloadLength === 126) {
        if (pending.length < 4) return
        payloadLength = pending.readUInt16BE(2)
        offset = 4
      }
      const maskBytes = masked ? 4 : 0
      if (pending.length < offset + maskBytes + payloadLength) return

      let mask = null as Buffer | null
      if (masked) {
        mask = pending.subarray(offset, offset + 4)
        offset += 4
      }
      const payload = Buffer.from(pending.subarray(offset, offset + payloadLength))
      pending = pending.subarray(offset + payloadLength)
      if (mask) {
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4]
      }
      if (opcode === 0x1) {
        const reply = Buffer.concat([Buffer.from([0x81, payload.length]), payload])
        socket.write(reply)
      } else if (opcode === 0x8) {
        socket.end()
        return
      }
    }
  })
}

async function startLocalEcho(wsPort: number, wssPort: number): Promise<() => Promise<void>> {
  const wsServer = http.createServer()
  const parserServer = http.createServer()
  const pfxPath = join(app.getPath('userData'), 'mitm', 'localhost.pfx')
  if (!existsSync(pfxPath)) {
    throw new Error(`localhost.pfx missing: ${pfxPath}`)
  }

  const wssServer = tls.createServer({
    pfx: readFileSync(pfxPath),
    passphrase: MITM_CERT_PASSWORD,
    ALPNProtocols: ['http/1.1'],
  })

  const upgradeHandler = (socket: net.Socket, head: Buffer, key: string) => {
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${createAcceptValue(key)}\r\n\r\n`
    )
    if (head.length > 0) socket.unshift(head)
    attachEcho(socket)
  }

  wsServer.on('upgrade', (req, socket, head) => upgradeHandler(socket as net.Socket, head, String(req.headers['sec-websocket-key'] || '')))
  parserServer.on('upgrade', (req, socket, head) => upgradeHandler(socket as net.Socket, head, String(req.headers['sec-websocket-key'] || '')))
  wssServer.on('secureConnection', (socket) => parserServer.emit('connection', socket))

  await Promise.all([
    new Promise<void>((resolve) => wsServer.listen(wsPort, '127.0.0.1', () => resolve())),
    new Promise<void>((resolve) => wssServer.listen(wssPort, '127.0.0.1', () => resolve())),
  ])

  return async () => {
    await Promise.all([
      new Promise<void>((resolve) => wsServer.close(() => resolve())),
      new Promise<void>((resolve) => parserServer.close(() => resolve())),
      new Promise<void>((resolve) => wssServer.close(() => resolve())),
    ])
  }
}

async function runWsScenario(proxyPort: number, endpoint: WsEndpoint, secure: boolean, allowText: string, blockText: string) {
  const connect = async () => {
    if (!secure) {
      const socket = net.connect(proxyPort, EXPLICIT_PROXY_HOST)
      await new Promise<void>((resolve, reject) => {
        socket.once('connect', resolve)
        socket.once('error', reject)
      })
      socket.write(
        Buffer.from(
          `GET ${endpoint.path} HTTP/1.1\r\nHost: ${endpoint.host}:${endpoint.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: test-key-plain==\r\nSec-WebSocket-Version: 13\r\n\r\n`,
          'utf8'
        )
      )
      return { socket, ...(await readHttpHeader(socket)) }
    }

    const proxySocket = net.connect(proxyPort, EXPLICIT_PROXY_HOST)
    await new Promise<void>((resolve, reject) => {
      proxySocket.once('connect', resolve)
      proxySocket.once('error', reject)
    })
    proxySocket.write(
      Buffer.from(`CONNECT ${endpoint.host}:${endpoint.securePort} HTTP/1.1\r\nHost: ${endpoint.host}:${endpoint.securePort}\r\n\r\n`, 'utf8')
    )
    await readHttpHeader(proxySocket)

    const secureSocket = tls.connect({
      socket: proxySocket,
      servername: endpoint.host,
      rejectUnauthorized: false,
    })
    await new Promise<void>((resolve, reject) => {
      secureSocket.once('secureConnect', resolve)
      secureSocket.once('error', reject)
    })
    secureSocket.write(buildUpgradeRequest(`${endpoint.host}:${endpoint.securePort}`, endpoint.path))
    return { socket: secureSocket, ...(await readHttpHeader(secureSocket)) }
  }

  const allowConn = await connect()
  allowConn.socket.write(createMaskedTextFrame(allowText))
  const allowOutcome = await readWebSocketOutcome(allowConn.socket, allowConn.leftover)
  try { allowConn.socket.end() } catch {}

  const blockConn = await connect()
  blockConn.socket.write(createMaskedTextFrame(blockText))
  const blockOutcome = await readWebSocketOutcome(blockConn.socket, blockConn.leftover)
  try { blockConn.socket.end() } catch {}

  return { allowOutcome, blockOutcome }
}

export async function runInstalledProxySelfCheck(outDir: string): Promise<number> {
  mkdirSync(outDir, { recursive: true })
  const addon = getAddon()
  if (!addon || !isAddonLoaded()) {
    writeText(outDir, 'self-check-error.txt', 'native addon not loaded')
    return 1
  }

  const summary: Record<string, any> = {}
  const proxyPolicy = {
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
    blockedHttpBodyPatterns: ['ws-block-marker'],
    blockedJsonKeys: ['secretKey'],
    blockedJsonPaths: [],
    blockedJsonValues: [],
  }

  const proxyPort = await getEphemeralPort('127.0.0.1')
  const wsPort = await getEphemeralPort('127.0.0.1')
  const wssPort = await getEphemeralPort('127.0.0.1')
  let stopEcho = async () => {}

  try {
    const loadResult = addon.dlp.driver_loader.load(getDriverSysPath())
    summary.loadResult = loadResult
    if (loadResult?.success) {
      addon.dlp.kernel_comm.connect()
      addon.dlp.policy_manager.setPolicy(proxyPolicy)
    }

    updateLocalProxyPolicy(proxyPolicy)
    summary.proxyStatus = await startLocalProxy({
      enabled: true,
      installSystemProxy: false,
      mitmEnabled: true,
      host: '127.0.0.1',
      port: proxyPort,
    })

    stopEcho = await startLocalEcho(wsPort, wssPort)
    const endpoint = { host: '127.0.0.1', port: wsPort, securePort: wssPort, path: '/' }
    summary.explicitWs = await runWsScenario(proxyPort, endpoint, false, 'ws-hello-plain', JSON.stringify({ secretKey: 'ws-block-marker' }))
    summary.explicitWss = await runWsScenario(proxyPort, endpoint, true, 'wss-hello-plain', JSON.stringify({ secretKey: 'ws-block-marker' }))

    const httpExplicit = await runCommand('C:\\WINDOWS\\system32\\curl.exe', ['-I', '--proxy', `http://127.0.0.1:${proxyPort}`, '--max-time', '20', 'http://example.com/'])
    const httpsExplicitInsecure = await runCommand('C:\\WINDOWS\\system32\\curl.exe', ['-k', '-I', '--proxy', `http://127.0.0.1:${proxyPort}`, '--max-time', '20', 'https://example.com/'])
    summary.httpExplicit = httpExplicit
    summary.httpsExplicitInsecure = httpsExplicitInsecure

    if (loadResult?.success) {
      try {
        summary.driverStatus = addon.dlp.kernel_comm.getDriverStatus()
      } catch (error: any) {
        summary.driverStatusError = error?.message || String(error)
      }
    }

    writeText(outDir, 'self-check-summary.json', JSON.stringify(summary, null, 2))
    return 0
  } catch (error: any) {
    summary.error = error?.stack || error?.message || String(error)
    writeText(outDir, 'self-check-summary.json', JSON.stringify(summary, null, 2))
    return 1
  } finally {
    try { await stopEcho() } catch {}
    try { await stopLocalProxy() } catch {}
    try { addon.dlp.kernel_comm.disconnect() } catch {}
    try { addon.dlp.driver_loader.unload() } catch {}
  }
}
