const http = require('node:http')
const crypto = require('node:crypto')
const net = require('node:net')
const tls = require('node:tls')
const path = require('node:path')
const { existsSync } = require('node:fs')
const { app } = require('electron')
const os = require('node:os')

const addonPath = path.resolve(__dirname, '..', 'src', 'native', 'build', 'Release', 'personal_safer.node')
const driverPath = path.resolve(__dirname, '..', 'build', 'kernel', 'x64', 'Release', 'PersonalSafer.sys')
const proxyModulePath = path.resolve(__dirname, '..', 'src', 'electron', 'main', 'modules', 'local_proxy.validate.cjs')

const EXPLICIT_PROXY_HOST = '127.0.0.1'
let explicitProxyPort = 8899
const EXPLICIT_WS_ENDPOINT = {
  host: '127.0.0.1',
  port: 18080,
  securePort: 18443,
  path: '/',
}
const TRANSPARENT_WS_ENDPOINT = {
  host: 'echo.websocket.org',
  port: 80,
  securePort: 443,
  path: '/',
}
const MITM_CERT_PASSWORD = 'PersonalSafer-MITM-2026!'

function createDeferred() {
  let resolve
  let reject
  const promise = new Promise((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

function getEphemeralPort(host = '127.0.0.1') {
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

async function withCleanupTimeout(label, fn, timeoutMs = 3000) {
  try {
    await Promise.race([
      Promise.resolve().then(fn),
      new Promise((_, reject) => setTimeout(() => reject(new Error(`${label} timeout`)), timeoutMs)),
    ])
  } catch (error) {
    console.log(`[WebSocketValidate] ${label} skipped: ${error.message || error}`)
  }
}

function onceSocketData(socket, timeoutMs = 15000) {
  const deferred = createDeferred()
  const timer = setTimeout(() => {
    cleanup()
    deferred.reject(new Error('socket read timeout'))
  }, timeoutMs)

  const onData = (chunk) => {
    cleanup()
    deferred.resolve(chunk)
  }
  const onError = (error) => {
    cleanup()
    deferred.reject(error)
  }
  const onClose = () => {
    cleanup()
    deferred.reject(new Error('socket closed before data'))
  }
  const cleanup = () => {
    clearTimeout(timer)
    socket.off('data', onData)
    socket.off('error', onError)
    socket.off('close', onClose)
  }

  socket.on('data', onData)
  socket.on('error', onError)
  socket.on('close', onClose)
  return deferred.promise
}

async function readHttpHeader(socket, timeoutMs = 15000) {
  let buffer = Buffer.alloc(0)

  while (true) {
    const marker = buffer.indexOf('\r\n\r\n')
    if (marker >= 0) {
      return {
        header: buffer.subarray(0, marker + 4).toString('latin1'),
        leftover: buffer.subarray(marker + 4),
      }
    }

    const chunk = await onceSocketData(socket, timeoutMs)
    buffer = Buffer.concat([buffer, chunk])
  }
}

function buildUpgradeRequest(url, hostHeader) {
  const key = crypto.randomBytes(16).toString('base64')
  const request =
    `GET ${url.path} HTTP/1.1\r\n` +
    `Host: ${hostHeader}\r\n` +
    `Upgrade: websocket\r\n` +
    `Connection: Upgrade\r\n` +
    `Sec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n` +
    `User-Agent: PersonalSafer-WebSocket-Validate\r\n\r\n`
  return Buffer.from(request, 'utf8')
}

function buildExplicitProxyUpgradeRequest(url) {
  const key = crypto.randomBytes(16).toString('base64')
  const request =
    `GET ${url.path} HTTP/1.1\r\n` +
    `Host: ${url.host}:${url.port}\r\n` +
    `Upgrade: websocket\r\n` +
    `Connection: Upgrade\r\n` +
    `Sec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n` +
    `User-Agent: PersonalSafer-WebSocket-Validate\r\n\r\n`
  return Buffer.from(request, 'utf8')
}

function createMaskedTextFrame(text) {
  const payload = Buffer.from(text, 'utf8')
  const mask = crypto.randomBytes(4)
  let header

  if (payload.length < 126) {
    header = Buffer.alloc(2)
    header[1] = 0x80 | payload.length
  } else {
    header = Buffer.alloc(4)
    header[1] = 0x80 | 126
    header.writeUInt16BE(payload.length, 2)
  }
  header[0] = 0x81

  const masked = Buffer.alloc(payload.length)
  for (let i = 0; i < payload.length; i++) {
    masked[i] = payload[i] ^ mask[i % 4]
  }

  return Buffer.concat([header, mask, masked])
}

function createUnmaskedTextFrame(text) {
  const payload = Buffer.from(text, 'utf8')
  let header
  if (payload.length < 126) {
    header = Buffer.from([0x81, payload.length])
  } else {
    header = Buffer.alloc(4)
    header[0] = 0x81
    header[1] = 126
    header.writeUInt16BE(payload.length, 2)
  }
  return Buffer.concat([header, payload])
}

function createAcceptValue(key) {
  return crypto
    .createHash('sha1')
    .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`, 'utf8')
    .digest('base64')
}

async function readWebSocketOutcome(socket, initialBuffer = Buffer.alloc(0), timeoutMs = 15000) {
  let pending = initialBuffer

  while (true) {
    if (pending.length < 2) {
      pending = Buffer.concat([pending, await onceSocketData(socket, timeoutMs)])
      continue
    }

    const first = pending[0]
    const second = pending[1]
    const opcode = first & 0x0f
    let payloadLength = second & 0x7f
    let offset = 2

    if (payloadLength === 126) {
      if (pending.length < 4) {
        pending = Buffer.concat([pending, await onceSocketData(socket, timeoutMs)])
        continue
      }
      payloadLength = pending.readUInt16BE(2)
      offset = 4
    } else if (payloadLength === 127) {
      if (pending.length < 10) {
        pending = Buffer.concat([pending, await onceSocketData(socket, timeoutMs)])
        continue
      }
      payloadLength = Number(pending.readBigUInt64BE(2))
      offset = 10
    }

    if (pending.length < offset + payloadLength) {
      pending = Buffer.concat([pending, await onceSocketData(socket, timeoutMs)])
      continue
    }

    const payload = pending.subarray(offset, offset + payloadLength)
    pending = pending.subarray(offset + payloadLength)

    if (opcode === 0x1) {
      return { type: 'text', text: payload.toString('utf8') }
    }
    if (opcode === 0x8) {
      const code = payload.length >= 2 ? payload.readUInt16BE(0) : 0
      const reason = payload.length > 2 ? payload.subarray(2).toString('utf8') : ''
      return { type: 'close', code, reason }
    }
  }
}

function attachEchoBehavior(socket) {
  let pending = Buffer.alloc(0)
  socket.on('data', (chunk) => {
    pending = Buffer.concat([pending, chunk])
    while (pending.length >= 2) {
      const first = pending[0]
      const second = pending[1]
      const opcode = first & 0x0f
      const masked = (second & 0x80) !== 0
      let payloadLength = second & 0x7f
      let offset = 2

      if (payloadLength === 126) {
        if (pending.length < 4) return
        payloadLength = pending.readUInt16BE(2)
        offset = 4
      } else if (payloadLength === 127) {
        return
      }

      const maskLength = masked ? 4 : 0
      if (pending.length < offset + maskLength + payloadLength) return

      let mask = null
      if (masked) {
        mask = pending.subarray(offset, offset + 4)
        offset += 4
      }

      const payload = Buffer.from(pending.subarray(offset, offset + payloadLength))
      pending = pending.subarray(offset + payloadLength)

      if (mask) {
        for (let i = 0; i < payload.length; i++) {
          payload[i] ^= mask[i % 4]
        }
      }

      if (opcode === 0x8) {
        try { socket.end() } catch {}
        return
      }

      if (opcode === 0x1) {
        socket.write(createUnmaskedTextFrame(payload.toString('utf8')))
      }
    }
  })
}

async function startLocalEchoServers() {
  const appData = process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming')
  const pfxPath = path.join(appData, 'Electron', 'mitm', 'localhost.pfx')
  if (!existsSync(pfxPath)) {
    throw new Error(`localhost.pfx not found: ${pfxPath}`)
  }

  const wsServer = http.createServer()
  const wssServer = tls.createServer({
    pfx: require('node:fs').readFileSync(pfxPath),
    passphrase: MITM_CERT_PASSWORD,
    ALPNProtocols: ['http/1.1'],
  })

  const upgradeHandler = (socket, head, key) => {
    console.log('[WebSocketValidate] local echo upgrade accepted')
    const response =
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${createAcceptValue(key)}\r\n\r\n`
    socket.write(response)
    if (head.length > 0) {
      socket.unshift(head)
    }
    attachEchoBehavior(socket)
  }

  wsServer.on('upgrade', (req, socket, head) => {
    console.log(`[WebSocketValidate] local ws upgrade ${req.url || '/'}`)
    const key = String(req.headers['sec-websocket-key'] || '')
    upgradeHandler(socket, head, key)
  })

  wssServer.on('data', () => {})
  const parserServer = http.createServer()
  parserServer.on('upgrade', (req, socket, head) => {
    console.log(`[WebSocketValidate] local wss upgrade ${req.url || '/'}`)
    const key = String(req.headers['sec-websocket-key'] || '')
    upgradeHandler(socket, head, key)
  })
  wssServer.on('secureConnection', (socket) => {
    parserServer.emit('connection', socket)
  })

  await Promise.all([
    new Promise((resolve) => wsServer.listen(EXPLICIT_WS_ENDPOINT.port, EXPLICIT_WS_ENDPOINT.host, resolve)),
    new Promise((resolve) => wssServer.listen(EXPLICIT_WS_ENDPOINT.securePort, EXPLICIT_WS_ENDPOINT.host, resolve)),
  ])

  return async () => {
    const closeServer = (server) => new Promise((resolve) => {
      if (!server.listening) {
        resolve()
        return
      }
      server.close(() => resolve())
    })

    await Promise.all([
      closeServer(wsServer),
      closeServer(parserServer),
      closeServer(wssServer),
    ])
  }
}

async function connectExplicitWs(url) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(explicitProxyPort, EXPLICIT_PROXY_HOST, async () => {
      try {
        socket.write(buildExplicitProxyUpgradeRequest(url))
        const { header, leftover } = await readHttpHeader(socket)
        if (!/^HTTP\/1\.1 101/i.test(header)) {
          reject(new Error(`unexpected ws proxy response: ${header.split('\r\n', 1)[0]}`))
          return
        }
        resolve({ socket, leftover })
      } catch (error) {
        reject(error)
      }
    })
    socket.on('error', reject)
  })
}

async function connectExplicitWss(url) {
  return new Promise((resolve, reject) => {
    const proxySocket = net.connect(explicitProxyPort, EXPLICIT_PROXY_HOST, async () => {
      try {
        proxySocket.write(
          Buffer.from(
            `CONNECT ${url.host}:${url.securePort} HTTP/1.1\r\nHost: ${url.host}:${url.securePort}\r\n\r\n`,
            'utf8'
          )
        )
        const { header } = await readHttpHeader(proxySocket)
        if (!/^HTTP\/1\.1 200/i.test(header)) {
          reject(new Error(`unexpected CONNECT response: ${header.split('\r\n', 1)[0]}`))
          return
        }

        const tlsSocket = tls.connect({
          socket: proxySocket,
          servername: url.host,
          rejectUnauthorized: false,
        }, async () => {
          try {
            tlsSocket.write(buildUpgradeRequest({ ...url, path: url.path }, `${url.host}:${url.securePort}`))
            const { header: wsHeader, leftover } = await readHttpHeader(tlsSocket)
            if (!/^HTTP\/1\.1 101/i.test(wsHeader)) {
              reject(new Error(`unexpected wss response: ${wsHeader.split('\r\n', 1)[0]}`))
              return
            }
            resolve({ socket: tlsSocket, leftover })
          } catch (error) {
            reject(error)
          }
        })
        tlsSocket.on('error', reject)
      } catch (error) {
        reject(error)
      }
    })
    proxySocket.on('error', reject)
  })
}

async function connectTransparentWs(url) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(url.port, url.host, async () => {
      try {
        socket.write(buildUpgradeRequest(url, `${url.host}:${url.port}`))
        const { header, leftover } = await readHttpHeader(socket)
        if (!/^HTTP\/1\.1 101/i.test(header)) {
          reject(new Error(`unexpected transparent ws response: ${header.split('\r\n', 1)[0]}`))
          return
        }
        resolve({ socket, leftover })
      } catch (error) {
        reject(error)
      }
    })
    socket.on('error', reject)
  })
}

async function connectTransparentWss(url) {
  return new Promise((resolve, reject) => {
    const socket = tls.connect({
      host: url.host,
      port: url.securePort,
      servername: url.host,
      rejectUnauthorized: false,
    }, async () => {
      try {
        socket.write(buildUpgradeRequest({ ...url, path: url.path }, `${url.host}:${url.securePort}`))
        const { header, leftover } = await readHttpHeader(socket)
        if (!/^HTTP\/1\.1 101/i.test(header)) {
          reject(new Error(`unexpected transparent wss response: ${header.split('\r\n', 1)[0]}`))
          return
        }
        resolve({ socket, leftover })
      } catch (error) {
        reject(error)
      }
    })
    socket.on('error', reject)
  })
}

async function runScenario(label, connector, endpoint, allowMessage, blockMessage, expectTransparent) {
  console.log(`\n[WebSocketValidate] scenario=${label}`)

  const allowConn = await connector(endpoint)
  allowConn.socket.write(createMaskedTextFrame(allowMessage))
  const allowOutcome = await readWebSocketOutcome(allowConn.socket, allowConn.leftover)
  console.log(`[WebSocketValidate] allow outcome: ${JSON.stringify(allowOutcome)}`)
  try { allowConn.socket.end() } catch {}

  const blockConn = await connector(endpoint)
  blockConn.socket.write(createMaskedTextFrame(blockMessage))
  const blockOutcome = await readWebSocketOutcome(blockConn.socket, blockConn.leftover)
  console.log(`[WebSocketValidate] block outcome: ${JSON.stringify(blockOutcome)}`)
  try { blockConn.socket.end() } catch {}

  if (allowOutcome.type !== 'text' || allowOutcome.text !== allowMessage) {
    throw new Error(`${label}: allow message was not echoed`)
  }
  if (blockOutcome.type !== 'close' || blockOutcome.code !== 1008) {
    throw new Error(`${label}: blocked message did not produce 1008 close`)
  }

  console.log(`[WebSocketValidate] ${label} passed transparent=${expectTransparent ? 'yes' : 'no'}`)
}

async function main() {
  if (!existsSync(addonPath)) throw new Error(`addon not found: ${addonPath}`)
  if (!existsSync(driverPath)) throw new Error(`driver not found: ${driverPath}`)
  if (!existsSync(proxyModulePath)) throw new Error(`proxy module not found: ${proxyModulePath}`)

  const addon = require(addonPath)
  const proxy = require(proxyModulePath)
  const websocketPolicy = {
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

  let transparentReady = false
  let stopEchoServers = async () => {}

  try {
    explicitProxyPort = await getEphemeralPort(EXPLICIT_PROXY_HOST)
    EXPLICIT_WS_ENDPOINT.port = await getEphemeralPort(EXPLICIT_WS_ENDPOINT.host)
    EXPLICIT_WS_ENDPOINT.securePort = await getEphemeralPort(EXPLICIT_WS_ENDPOINT.host)

    try { addon.dlp.driver_loader.unload() } catch {}
    const loadResult = addon.dlp.driver_loader.load(driverPath)
    console.log('[WebSocketValidate] loadResult:', JSON.stringify(loadResult))
    if (loadResult && loadResult.success) {
      addon.dlp.kernel_comm.connect()
      addon.dlp.policy_manager.setPolicy(websocketPolicy)
      transparentReady = true
    } else {
      console.log('[WebSocketValidate] transparent redirect unavailable, falling back to explicit-proxy websocket validation')
    }

    proxy.updateLocalProxyPolicy(websocketPolicy)

    const proxyStatus = await proxy.startLocalProxy({
      enabled: true,
      installSystemProxy: false,
      mitmEnabled: true,
      host: '127.0.0.1',
      port: explicitProxyPort,
    })
    console.log('[WebSocketValidate] proxyStatus:', JSON.stringify(proxyStatus))
    stopEchoServers = await startLocalEchoServers()

    await runScenario(
      'explicit-ws',
      connectExplicitWs,
      EXPLICIT_WS_ENDPOINT,
      'ws-hello-plain',
      JSON.stringify({ secretKey: 'ws-block-marker' }),
      false
    )

    await runScenario(
      'explicit-wss',
      connectExplicitWss,
      EXPLICIT_WS_ENDPOINT,
      'wss-hello-plain',
      JSON.stringify({ secretKey: 'ws-block-marker' }),
      false
    )

    if (transparentReady) {
      await runScenario(
        'transparent-ws',
        connectTransparentWs,
        TRANSPARENT_WS_ENDPOINT,
        'transparent-ws-hello',
        JSON.stringify({ secretKey: 'ws-block-marker' }),
        true
      )

      await runScenario(
        'transparent-wss',
        connectTransparentWss,
        TRANSPARENT_WS_ENDPOINT,
        'transparent-wss-hello',
        JSON.stringify({ secretKey: 'ws-block-marker' }),
        true
      )
    }
  } finally {
    await withCleanupTimeout('stop echo', () => stopEchoServers())
    await withCleanupTimeout('stop proxy', () => proxy.stopLocalProxy())
    try { addon.dlp.kernel_comm.disconnect() } catch {}
    try { addon.dlp.driver_loader.unload() } catch {}
  }
}

app.whenReady().then(async () => {
  try {
    await main()
    app.exit(0)
  } catch (error) {
    console.error('[WebSocketValidate] failed:', error && error.stack ? error.stack : error)
    app.exit(1)
  }
})
