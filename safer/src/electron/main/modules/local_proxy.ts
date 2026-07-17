import { app } from 'electron'
import { execFileSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync } from 'node:fs'
import * as http from 'node:http'
import * as http2 from 'node:http2'
import * as https from 'node:https'
import * as net from 'node:net'
import { join } from 'node:path'
import * as tls from 'node:tls'
import { gunzipSync, inflateSync } from 'node:zlib'
import { recordExternalNetEvent } from './dlp_events'
import { getAddon, isAddonLoaded } from './native_loader'

type DlpPolicyLike = {
  blockedDomains?: string[]
  blockedUrls?: string[]
  blockedHttpHeaders?: string[]
  blockedHttpTrailers?: string[]
  blockedHttpBodyPatterns?: string[]
  blockedJsonKeys?: string[]
  blockedJsonPaths?: string[]
  blockedJsonValues?: string[]
}

export type LocalProxyConfig = {
  enabled?: boolean
  installSystemProxy?: boolean
  mitmEnabled?: boolean
  host?: string
  port?: number
}

type ProxyState = {
  running: boolean
  config: Required<LocalProxyConfig>
  frontServer: net.Server | null
  httpServer: http.Server | null
  httpsServer: tls.Server | null
  httpsParserServer: http.Server | null
  policy: DlpPolicyLike
  certDir: string
  leafCache: Map<string, tls.SecureContext>
}

type InspectionResult = {
  blocked: boolean
  reason?: string
}

type JsonSemantic = {
  keys: string[]
  paths: string[]
  values: string[]
  entries: Array<{ key: string; path: string; value: string }>
}

type ProcessInfo = {
  pid: number
  processName: string
}

type UpstreamResponse = {
  statusCode: number
  headers: Record<string, string | string[]>
  trailers: Record<string, string | string[]>
  body: Buffer
}

type GenericRequest = {
  method?: string
  url?: string
  headers: http.IncomingHttpHeaders
  rawHeaders?: string[]
  httpVersion?: string
  httpVersionMajor?: number
  socket: net.Socket
}

type GenericResponse = {
  writeHead: (statusCode: number, headers?: Record<string, any>) => GenericResponse
  end: (chunk?: any) => void
}

type OriginalDestination = {
  address: string
  port: number
}

type WebSocketMessageState = {
  opcode: number
  compressed: boolean
  chunks: Buffer[]
  rawFrames: Buffer[]
}

type WebSocketCompressionOptions = {
  perMessageDeflate: boolean
  clientNoContextTakeover: boolean
  serverNoContextTakeover: boolean
}

const ROOT_CA_SUBJECT = 'CN=PersonalSafer Local Root CA'
const CERT_PASSWORD = 'PersonalSafer-MITM-2026!'
const MAX_CAPTURE_BYTES = 16 * 1024 * 1024

const state: ProxyState = {
  running: false,
  config: {
    enabled: false,
    installSystemProxy: false,
    mitmEnabled: true,
    host: '127.0.0.1',
    port: 8899,
  },
  frontServer: null,
  httpServer: null,
  httpsServer: null,
  httpsParserServer: null,
  policy: {},
  certDir: '',
  leafCache: new Map(),
}

function getCertDir(): string {
  if (!state.certDir) {
    state.certDir = join(app.getPath('userData'), 'mitm')
  }
  mkdirSync(state.certDir, { recursive: true })
  return state.certDir
}

function psQuote(value: string): string {
  return `'${value.replace(/'/g, "''")}'`
}

function runPowerShell(script: string): void {
  execFileSync(
    'powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script],
    { stdio: 'ignore' }
  )
}

function getRootCerPath(): string {
  return join(getCertDir(), 'root.cer')
}

function getRootPfxPath(): string {
  return join(getCertDir(), 'root.pfx')
}

function getLeafPfxPath(hostname: string): string {
  return join(getCertDir(), `${hostname.replace(/[^a-zA-Z0-9.-]/g, '_')}.pfx`)
}

function ensureRootCertificate(): void {
  const rootCerPath = getRootCerPath()
  const rootPfxPath = getRootPfxPath()

  if (existsSync(rootCerPath) && existsSync(rootPfxPath)) {
    return
  }

  const script = `
$subject = ${psQuote(ROOT_CA_SUBJECT)}
$rootCer = ${psQuote(rootCerPath)}
$rootPfx = ${psQuote(rootPfxPath)}
$pwd = ConvertTo-SecureString ${psQuote(CERT_PASSWORD)} -AsPlainText -Force
$cert = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type Custom -Subject $subject -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy Exportable -KeyUsage CertSign, CRLSign, DigitalSignature -TextExtension @('2.5.29.19={critical}{text}CA=true') -CertStoreLocation 'Cert:\\CurrentUser\\My' -NotAfter (Get-Date).AddYears(10)
}
Export-Certificate -Cert $cert -FilePath $rootCer -Force | Out-Null
Export-PfxCertificate -Cert $cert -FilePath $rootPfx -Password $pwd -Force | Out-Null
`

  runPowerShell(script)
  try {
    execFileSync('certutil', ['-user', '-addstore', '-f', 'Root', rootCerPath], { stdio: 'ignore' })
  } catch {
    // Ignore duplicate import failures.
  }
  try {
    execFileSync('certutil', ['-user', '-addstore', '-f', 'TrustedPublisher', rootCerPath], { stdio: 'ignore' })
  } catch {
    // Ignore duplicate import failures.
  }
}

function ensureLeafCertificate(hostname: string): string {
  const normalizedHost = hostname.toLowerCase()
  const leafPfxPath = getLeafPfxPath(normalizedHost)

  if (existsSync(leafPfxPath)) {
    return leafPfxPath
  }

  ensureRootCertificate()

  const script = `
$hostName = ${psQuote(normalizedHost)}
$subject = ${psQuote(ROOT_CA_SUBJECT)}
$leafPfx = ${psQuote(leafPfxPath)}
$pwd = ConvertTo-SecureString ${psQuote(CERT_PASSWORD)} -AsPlainText -Force
$root = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $root) { throw 'Root CA missing' }
$leafSubject = 'CN=' + $hostName
$leaf = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $leafSubject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $leaf) {
  $leaf = New-SelfSignedCertificate -Type Custom -DnsName $hostName -Subject $leafSubject -Signer $root -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy Exportable -TextExtension @('2.5.29.19={critical}{text}CA=false','2.5.29.37={text}1.3.6.1.5.5.7.3.1') -CertStoreLocation 'Cert:\\CurrentUser\\My' -NotAfter (Get-Date).AddYears(2)
}
Export-PfxCertificate -Cert $leaf -FilePath $leafPfx -Password $pwd -Force | Out-Null
`

  runPowerShell(script)
  return leafPfxPath
}

function getSecureContext(hostname: string): tls.SecureContext {
  const normalizedHost = hostname.toLowerCase()
  const cached = state.leafCache.get(normalizedHost)
  if (cached) {
    return cached
  }

  const pfxPath = ensureLeafCertificate(normalizedHost)
  const context = tls.createSecureContext({
    pfx: readFileSync(pfxPath),
    passphrase: CERT_PASSWORD,
  })
  state.leafCache.set(normalizedHost, context)
  return context
}

function containsNoCase(haystack: string, needle: string): boolean {
  return haystack.toLowerCase().includes(needle.toLowerCase())
}

function domainMatches(host: string, rule: string): boolean {
  const normalizedHost = host.toLowerCase()
  const normalizedRule = rule.toLowerCase()
  return normalizedHost === normalizedRule || normalizedHost.endsWith(`.${normalizedRule}`)
}

function toHeaderStrings(headers: http.IncomingHttpHeaders | Record<string, string | string[]>): string[] {
  const values: string[] = []

  for (const [key, value] of Object.entries(headers)) {
    if (Array.isArray(value)) {
      for (const item of value) {
        values.push(`${key}: ${item}`)
      }
    } else if (typeof value === 'string') {
      values.push(`${key}: ${value}`)
    }
  }

  return values
}

function isInspectableContentType(contentType: string): boolean {
  const lowered = contentType.toLowerCase()
  return lowered.startsWith('text/') || lowered.startsWith('application/json')
}

function decodeBodyPreview(body: Buffer, contentEncoding: string): string {
  try {
    const encoding = contentEncoding.toLowerCase()
    if (encoding === 'gzip') {
      return gunzipSync(body).toString('utf8')
    }
    if (encoding === 'deflate') {
      return inflateSync(body).toString('utf8')
    }
  } catch {
    return body.toString('utf8')
  }

  return body.toString('utf8')
}

function collectJsonSemantics(input: unknown, basePath: string, out: JsonSemantic): void {
  if (Array.isArray(input)) {
    input.forEach((item, index) => collectJsonSemantics(item, `${basePath}[${index}]`, out))
    return
  }

  if (input && typeof input === 'object') {
    for (const [key, value] of Object.entries(input as Record<string, unknown>)) {
      const nextPath = basePath ? `${basePath}.${key}` : key
      out.keys.push(key)
      out.paths.push(nextPath)
      collectJsonSemantics(value, nextPath, out)
    }
    return
  }

  if (input !== undefined && input !== null) {
    const value = String(input)
    out.values.push(value)
    out.entries.push({
      key: basePath.includes('.') ? basePath.slice(basePath.lastIndexOf('.') + 1) : basePath,
      path: basePath,
      value,
    })
  }
}

function normalizeJsonPathRule(rule: string): string {
  return rule.replace(/\[\]/g, '[*]')
}

function globMatchNoCase(text: string, pattern: string): boolean {
  let textIndex = 0
  let patternIndex = 0
  let starPattern = -1
  let starText = -1

  const foldedText = text.toLowerCase()
  const foldedPattern = normalizeJsonPathRule(pattern).toLowerCase()

  while (textIndex < foldedText.length) {
    if (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === '*') {
      starPattern = ++patternIndex
      starText = textIndex
      continue
    }

    if (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === foldedText[textIndex]) {
      patternIndex++
      textIndex++
      continue
    }

    if (starPattern !== -1) {
      patternIndex = starPattern
      textIndex = ++starText
      continue
    }

    return false
  }

  while (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === '*') {
    patternIndex++
  }

  return patternIndex === foldedPattern.length
}

function jsonPathRuleMatches(path: string, rule: string): boolean {
  if (!rule) {
    return false
  }
  return globMatchNoCase(path, rule) || containsNoCase(path, rule)
}

function parseJsonPredicateRule(rule: string): null | {
  prefixPattern: string
  groups: Array<Array<{ key: string; valuePattern: string; operator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' }>>
  suffix: string
} {
  const leftBrace = rule.indexOf('{')
  const rightBrace = leftBrace >= 0 ? rule.indexOf('}', leftBrace + 1) : -1
  if (leftBrace < 0 || rightBrace <= leftBrace + 1) {
    return null
  }

  const predicate = rule.slice(leftBrace + 1, rightBrace)
  const prefixPattern = rule.slice(0, leftBrace)
  const suffix = rule.slice(rightBrace + 1)
  const groupParts = predicate
    .split('||')
    .map((item) => item.trim())
    .filter(Boolean)
  const groups: Array<Array<{ key: string; valuePattern: string; operator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' }>> = []

  for (const groupPart of groupParts) {
    const parts = groupPart
      .split(/&&|,/)
      .map((item) => item.trim())
      .filter(Boolean)
    const conditions: Array<{ key: string; valuePattern: string; operator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' }> = []

    for (const part of parts) {
      const operators: Array<{ token: string; operator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' }> = [
        { token: '!=', operator: 'ne' },
        { token: '>=', operator: 'ge' },
        { token: '<=', operator: 'le' },
        { token: '>', operator: 'gt' },
        { token: '<', operator: 'lt' },
        { token: '=', operator: 'eq' },
      ]
      let resolvedOperator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' | null = null
      let splitIndex = -1
      let separatorLength = 0

      for (const candidate of operators) {
        const index = part.indexOf(candidate.token)
        if (index > 0) {
          resolvedOperator = candidate.operator
          splitIndex = index
          separatorLength = candidate.token.length
          break
        }
      }

      if (!resolvedOperator) {
        return null
      }

      if (splitIndex <= 0 || splitIndex >= part.length - separatorLength) {
        return null
      }
      const key = part.slice(0, splitIndex).trim()
      const valuePattern = part.slice(splitIndex + separatorLength).trim()
      if (!key || !valuePattern) {
        return null
      }
      conditions.push({ key, valuePattern, operator: resolvedOperator })
    }

    if (conditions.length === 0) {
      return null
    }
    groups.push(conditions)
  }

  if (groups.length === 0 || !suffix) {
    return null
  }

  return {
    prefixPattern,
    groups,
    suffix,
  }
}

function endsWithNoCase(text: string, suffix: string): boolean {
  return text.toLowerCase().endsWith(suffix.toLowerCase())
}

function equalsNoCase(left: string, right: string): boolean {
  return left.toLowerCase() === right.toLowerCase()
}

function valuePatternMatches(value: string, pattern: string): boolean {
  return globMatchNoCase(value, pattern) || containsNoCase(value, pattern)
}

function parseNumericValue(value: string): number | null {
  const trimmed = value.trim()
  if (!trimmed) {
    return null
  }
  if (!/^-?\d+(\.\d+)?$/.test(trimmed)) {
    return null
  }
  const parsed = Number(trimmed)
  return Number.isFinite(parsed) ? parsed : null
}

function predicateConditionMatches(
  candidateValue: string,
  condition: { valuePattern: string; operator: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le' }
): boolean {
  if (condition.operator === 'eq') {
    return valuePatternMatches(candidateValue, condition.valuePattern)
  }
  if (condition.operator === 'ne') {
    return !valuePatternMatches(candidateValue, condition.valuePattern)
  }

  const left = parseNumericValue(candidateValue)
  const right = parseNumericValue(condition.valuePattern)
  if (left === null || right === null) {
    return false
  }

  if (condition.operator === 'gt') return left > right
  if (condition.operator === 'ge') return left >= right
  if (condition.operator === 'lt') return left < right
  return left <= right
}

function jsonPredicateRuleMatches(
  semantic: JsonSemantic,
  rule: string
): boolean {
  const parsed = parseJsonPredicateRule(rule)
  if (!parsed) {
    return false
  }

  for (const entry of semantic.entries) {
    if (!endsWithNoCase(entry.path, parsed.suffix)) {
      continue
    }

    const objectPath = entry.path.slice(0, entry.path.length - parsed.suffix.length)
    if (!jsonPathRuleMatches(objectPath, parsed.prefixPattern)) {
      continue
    }

    if (parsed.groups.some((conditions) => conditions.every((condition) => {
      const siblingPath = objectPath
        ? `${objectPath}.${condition.key}`
        : condition.key
      const candidateMatched = semantic.entries.some((candidate) =>
        equalsNoCase(candidate.path, siblingPath) &&
        predicateConditionMatches(candidate.value, condition)
      )
      return candidateMatched
    }))) {
      return true
    }
  }

  return false
}

function inspectTextualBody(bodyText: string, contentType: string): JsonSemantic {
  const semantic: JsonSemantic = { keys: [], paths: [], values: [], entries: [] }

  if (contentType.toLowerCase().startsWith('application/json')) {
    try {
      collectJsonSemantics(JSON.parse(bodyText), '', semantic)
    } catch {
      // Ignore malformed JSON here.
    }
  }

  return semantic
}

function evaluateProxyPolicy(
  targetUrl: URL,
  headers: http.IncomingHttpHeaders | Record<string, string | string[]>,
  bodyText: string,
  contentType: string,
  headerRules: string[],
  bodyRules: string[],
  jsonKeyRules: string[],
  jsonPathRules: string[],
  jsonValueRules: string[]
): InspectionResult {
  const policy = state.policy
  const headerLines = toHeaderStrings(headers)

  for (const domainRule of policy.blockedDomains || []) {
    if (domainMatches(targetUrl.hostname, domainRule)) {
      return { blocked: true, reason: `domain:${domainRule}` }
    }
  }

  for (const urlRule of policy.blockedUrls || []) {
    if (containsNoCase(targetUrl.toString(), urlRule)) {
      return { blocked: true, reason: `url:${urlRule}` }
    }
  }

  for (const headerRule of headerRules) {
    if (headerLines.some((line) => containsNoCase(line, headerRule))) {
      return { blocked: true, reason: `header:${headerRule}` }
    }
  }

  if (bodyText) {
    for (const bodyRule of bodyRules) {
      if (containsNoCase(bodyText, bodyRule)) {
        return { blocked: true, reason: `body:${bodyRule}` }
      }
    }

    const semantic = inspectTextualBody(bodyText, contentType)
    for (const keyRule of jsonKeyRules) {
      if (semantic.keys.some((value) => containsNoCase(value, keyRule))) {
        return { blocked: true, reason: `json-key:${keyRule}` }
      }
    }
    for (const pathRule of jsonPathRules) {
      if (
        semantic.paths.some((value) => jsonPathRuleMatches(value, pathRule)) ||
        jsonPredicateRuleMatches(semantic, pathRule)
      ) {
        return { blocked: true, reason: `json-path:${pathRule}` }
      }
    }
    for (const valueRule of jsonValueRules) {
      if (semantic.values.some((value) => containsNoCase(value, valueRule))) {
        return { blocked: true, reason: `json-value:${valueRule}` }
      }
    }
  }

  return { blocked: false }
}

async function readBody(stream: AsyncIterable<any>, maxBytes: number): Promise<Buffer> {
  const chunks: Buffer[] = []
  let total = 0

  for await (const chunk of stream) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)
    total += buffer.length
    if (total > maxBytes) {
      throw new Error(`payload too large: ${total}`)
    }
    chunks.push(buffer)
  }

  return Buffer.concat(chunks)
}

function normalizeAddress(address: string | undefined): string {
  if (!address) return ''
  if (address.startsWith('::ffff:')) {
    return address.slice(7)
  }
  return address
}

function getAddressFamily(address: string): number {
  return address.includes(':') ? 23 : 2
}

function lookupSocketProcess(socket: net.Socket): ProcessInfo {
  const cached = (socket as any).__psProcessInfo as ProcessInfo | undefined
  if (cached) {
    return cached
  }

  const fallback: ProcessInfo = { pid: process.pid, processName: 'local_proxy' }
  const addon = getAddon()
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.lookupConnectionProcess) {
    ;(socket as any).__psProcessInfo = fallback
    return fallback
  }

  const remoteAddress = normalizeAddress(socket.remoteAddress)
  const localAddress = normalizeAddress(socket.localAddress)
  const remotePort = socket.remotePort || 0
  const localPort = socket.localPort || 0

  if (!remoteAddress || !localAddress || !remotePort || !localPort) {
    ;(socket as any).__psProcessInfo = fallback
    return fallback
  }

  const mapped = addon.dlp.kernel_comm.lookupConnectionProcess({
    localAddress: remoteAddress,
    localPort: remotePort,
    remoteAddress: localAddress,
    remotePort: localPort,
  })

  const result: ProcessInfo = {
    pid: Number(mapped?.pid || process.pid),
    processName: String(mapped?.processName || 'local_proxy'),
  }
  ;(socket as any).__psProcessInfo = result
  return result
}

function queryTransparentOriginalDestination(socket: net.Socket, processInfo: ProcessInfo): OriginalDestination | null {
  const cached = (socket as any).__psOriginalDestination as OriginalDestination | undefined
  if (cached) {
    return cached
  }

  const addon = getAddon()
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.queryRedirectDestination) {
    return null
  }

  const localAddress = normalizeAddress(socket.remoteAddress || '127.0.0.1')
  const localPort = socket.remotePort || 0
  if (!localAddress || !localPort || !processInfo.pid) {
    return null
  }

  const query = addon.dlp.kernel_comm.queryRedirectDestination({
    processId: processInfo.pid,
    protocol: 6,
    addressFamily: getAddressFamily(localAddress),
    localPort,
    localAddress,
  })
  if (!query?.found) {
    console.log(`[TransparentProxy] no redirect mapping pid=${processInfo.pid} local=${localAddress}:${localPort}`)
    return null
  }

  const destination = {
    address: String(query.originalRemoteAddress || ''),
    port: Number(query.originalRemotePort || 0),
  }
  console.log(`[TransparentProxy] redirect mapping pid=${processInfo.pid} local=${localAddress}:${localPort} -> ${destination.address}:${destination.port}`)
  ;(socket as any).__psOriginalDestination = destination
  return destination
}

function buildTargetUrl(req: GenericRequest, forcedScheme?: 'http' | 'https'): URL {
  const authority = String(
    req.headers.host ||
    (req.headers as any)[':authority'] ||
    (req.socket as any).__psTargetHost ||
    ''
  )
  const originalDestination = (req.socket as any).__psOriginalDestination as OriginalDestination | undefined

  if (forcedScheme) {
    const host = authority || (originalDestination ? `${originalDestination.address}:${originalDestination.port}` : '')
    return new URL(`${forcedScheme}://${host}${req.url || '/'}`)
  }

  if (req.url && /^https?:\/\//i.test(req.url)) {
    return new URL(req.url)
  }

  return new URL(`http://${authority || (originalDestination ? `${originalDestination.address}:${originalDestination.port}` : '')}${req.url || '/'}`)
}

function filterUpstreamHeaders(
  headers: http.IncomingHttpHeaders,
  protocol: 'http1' | 'http2'
): Record<string, string | string[]> {
  const output: Record<string, string | string[]> = {}

  for (const [key, value] of Object.entries(headers)) {
    const lowered = key.toLowerCase()
    if (
      lowered === 'proxy-connection' ||
      lowered === 'connection' ||
      lowered === 'keep-alive' ||
      lowered === 'transfer-encoding' ||
      lowered === 'upgrade'
    ) {
      continue
    }
    if (protocol === 'http2' && lowered.startsWith(':')) {
      continue
    }
    if (value !== undefined) {
      output[key] = value
    }
  }

  return output
}

function filterDownstreamHeaders(headers: Record<string, string | string[]>): Record<string, string | string[]> {
  const output: Record<string, string | string[]> = {}

  for (const [key, value] of Object.entries(headers)) {
    const lowered = key.toLowerCase()
    if (lowered.startsWith(':') || lowered === 'transfer-encoding' || lowered === 'connection') {
      continue
    }
    output[key] = value
  }

  return output
}

function respondBlocked(res: GenericResponse, reason: string): void {
  res.writeHead(403, { 'content-type': 'text/plain; charset=utf-8' })
  res.end(`Blocked by PersonalSafer local proxy: ${reason}`)
}

function recordProxyAudit(
  type: string,
  action: string,
  url: string,
  processInfo: ProcessInfo,
  remotePort: number,
  details?: string
): void {
  recordExternalNetEvent({
    type,
    action,
    processName: processInfo.processName || 'local_proxy',
    processId: processInfo.pid || process.pid,
    url,
    details: details || url,
    timestamp: new Date().toISOString(),
    timestampMs: Date.now(),
    remotePort,
    localPort: state.config.port,
    protocol: 6,
  })
}

function recordWebSocketAudit(
  action: string,
  url: string,
  processInfo: ProcessInfo,
  remotePort: number,
  details?: string
): void {
  recordExternalNetEvent({
    type: 'websocket_message',
    action,
    processName: processInfo.processName || 'local_proxy',
    processId: processInfo.pid || process.pid,
    url,
    details: details || url,
    timestamp: new Date().toISOString(),
    timestampMs: Date.now(),
    remotePort,
    localPort: state.config.port,
    protocol: 6,
  })
}

async function proxyViaHttp1(
  targetUrl: URL,
  req: GenericRequest,
  requestBody: Buffer
): Promise<UpstreamResponse> {
  const transport = targetUrl.protocol === 'https:' ? https : http

  return new Promise<UpstreamResponse>((resolve, reject) => {
    const upstreamReq = transport.request(
      {
        protocol: targetUrl.protocol,
        hostname: targetUrl.hostname,
        port: targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'https:' ? 443 : 80),
        method: req.method,
        path: `${targetUrl.pathname}${targetUrl.search}`,
        headers: {
          ...filterUpstreamHeaders(req.headers, 'http1'),
          host: targetUrl.host,
          'content-length': String(requestBody.length),
        },
        rejectUnauthorized: false,
      },
      async (upstreamRes) => {
        try {
          const body = await readBody(upstreamRes, MAX_CAPTURE_BYTES)
          resolve({
            statusCode: upstreamRes.statusCode || 200,
            headers: filterDownstreamHeaders(upstreamRes.headers as Record<string, string | string[]>),
            trailers: filterDownstreamHeaders(upstreamRes.trailers as Record<string, string | string[]>),
            body,
          })
        } catch (error) {
          reject(error)
        }
      }
    )

    upstreamReq.on('error', reject)
    if (requestBody.length > 0) {
      upstreamReq.write(requestBody)
    }
    upstreamReq.end()
  })
}

async function proxyViaHttp2(
  targetUrl: URL,
  req: GenericRequest,
  requestBody: Buffer
): Promise<UpstreamResponse> {
  const origin = `${targetUrl.protocol}//${targetUrl.host}`

  return new Promise<UpstreamResponse>((resolve, reject) => {
    const session = http2.connect(origin, { rejectUnauthorized: false })
    const headers: http2.OutgoingHttpHeaders = {
      ':method': req.method || 'GET',
      ':path': `${targetUrl.pathname}${targetUrl.search}`,
      ':scheme': targetUrl.protocol.replace(':', ''),
      ':authority': targetUrl.host,
      ...filterUpstreamHeaders(req.headers, 'http2'),
    }
    const bodyChunks: Buffer[] = []
    const trailers: Record<string, string | string[]> = {}
    let responseHeaders: Record<string, string | string[]> = {}
    let statusCode = 200
    let finished = false

    const finish = (error?: any, response?: UpstreamResponse) => {
      if (finished) return
      finished = true
      try { session.close() } catch { /* ignore */ }
      if (error) reject(error)
      else if (response) resolve(response)
    }

    session.on('error', finish)

    const stream = session.request(headers)
    stream.on('response', (headersMap) => {
      const normalized: Record<string, string | string[]> = {}
      for (const [key, value] of Object.entries(headersMap)) {
        if (key === ':status') {
          statusCode = Number(value || 200)
        } else if (typeof value === 'string' || Array.isArray(value)) {
          normalized[key] = value
        }
      }
      responseHeaders = filterDownstreamHeaders(normalized)
    })
    stream.on('trailers', (headersMap) => {
      for (const [key, value] of Object.entries(headersMap)) {
        if (typeof value === 'string' || Array.isArray(value)) {
          trailers[key] = value
        }
      }
    })
    stream.on('data', (chunk) => {
      bodyChunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk))
    })
    stream.on('end', () => {
      finish(undefined, {
        statusCode,
        headers: responseHeaders,
        trailers: filterDownstreamHeaders(trailers),
        body: Buffer.concat(bodyChunks),
      })
    })
    stream.on('error', finish)
    if (requestBody.length > 0) {
      stream.write(requestBody)
    }
    stream.end()
  })
}

async function proxyUpstream(
  targetUrl: URL,
  req: GenericRequest,
  requestBody: Buffer
): Promise<UpstreamResponse> {
  if (targetUrl.protocol === 'https:') {
    try {
      return await proxyViaHttp2(targetUrl, req, requestBody)
    } catch {
      return proxyViaHttp1(targetUrl, req, requestBody)
    }
  }

  return proxyViaHttp1(targetUrl, req, requestBody)
}

async function handleProxyRequest(
  req: GenericRequest,
  res: GenericResponse,
  forcedScheme?: 'http' | 'https'
): Promise<void> {
  let targetUrl: URL
  let requestBody = Buffer.alloc(0)

  try {
    targetUrl = buildTargetUrl(req, forcedScheme)
    requestBody = await readBody(req as any, MAX_CAPTURE_BYTES)
    console.log(`[TransparentProxy] request ${req.method || 'GET'} ${targetUrl.toString()} transparent=${(req.socket as any).__psTransparent ? 'yes' : 'no'}`)
  } catch (error: any) {
    res.writeHead(400, { 'content-type': 'text/plain; charset=utf-8' })
    res.end(error?.message || 'invalid proxy request')
    return
  }

  const processInfo = lookupSocketProcess(req.socket)
  const requestContentType = String(req.headers['content-type'] || '')
  const requestText =
    requestBody.length > 0 && isInspectableContentType(requestContentType)
      ? requestBody.toString('utf8')
      : ''
  const requestDecision = evaluateProxyPolicy(
    targetUrl,
    req.headers,
    requestText,
    requestContentType,
    state.policy.blockedHttpHeaders || [],
    state.policy.blockedHttpBodyPatterns || [],
    state.policy.blockedJsonKeys || [],
    state.policy.blockedJsonPaths || [],
    state.policy.blockedJsonValues || []
  )

  recordProxyAudit(
    'http_request',
    requestDecision.blocked ? 'blocked' : 'logged',
    targetUrl.toString(),
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'https:' ? 443 : 80),
    requestDecision.reason
  )

  if (requestDecision.blocked) {
    respondBlocked(res, requestDecision.reason || 'request-policy')
    return
  }

  let upstream: UpstreamResponse
  try {
    upstream = await proxyUpstream(targetUrl, req, requestBody)
  } catch (error: any) {
    res.writeHead(502, { 'content-type': 'text/plain; charset=utf-8' })
    res.end(error?.message || 'upstream request failed')
    return
  }

  const responseContentType = String(upstream.headers['content-type'] || '')
  const responseEncoding = String(upstream.headers['content-encoding'] || '')
  const responseText =
    upstream.body.length > 0 && isInspectableContentType(responseContentType)
      ? decodeBodyPreview(upstream.body, responseEncoding)
      : ''
  const responseDecision = evaluateProxyPolicy(
    targetUrl,
    { ...upstream.headers, ...upstream.trailers },
    responseText,
    responseContentType,
    [
      ...(state.policy.blockedHttpHeaders || []),
      ...(state.policy.blockedHttpTrailers || []),
    ],
    state.policy.blockedHttpBodyPatterns || [],
    state.policy.blockedJsonKeys || [],
    state.policy.blockedJsonPaths || [],
    state.policy.blockedJsonValues || []
  )

  recordProxyAudit(
    'http_response',
    responseDecision.blocked ? 'blocked' : 'logged',
    targetUrl.toString(),
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'https:' ? 443 : 80),
    responseDecision.reason
  )

  if (responseDecision.blocked) {
    respondBlocked(res, responseDecision.reason || 'response-policy')
    return
  }

  const downstreamHeaders = {
    ...upstream.headers,
    'content-length': String(upstream.body.length),
  }
  res.writeHead(upstream.statusCode, downstreamHeaders)
  res.end(upstream.body)
}

function buildUpgradeRequest(targetUrl: URL, req: http.IncomingMessage): Buffer {
  const path = `${targetUrl.pathname}${targetUrl.search}`
  const lines = [`${req.method || 'GET'} ${path} HTTP/${req.httpVersion || '1.1'}`]
  let hasHost = false

  const rawHeaders = req.rawHeaders || []
  for (let i = 0; i < rawHeaders.length; i += 2) {
    const name = rawHeaders[i]
    const value = rawHeaders[i + 1]
    const lowered = name.toLowerCase()
    if (lowered === 'proxy-connection') continue
    if (lowered === 'host') hasHost = true
    lines.push(`${name}: ${value}`)
  }

  if (!hasHost) {
    lines.push(`Host: ${targetUrl.host}`)
  }

  lines.push('', '')
  return Buffer.from(lines.join('\r\n'), 'utf8')
}

function createCloseFrame(code: number, reason = ''): Buffer {
  const reasonBuffer = Buffer.from(reason, 'utf8')
  const payload = Buffer.alloc(2 + reasonBuffer.length)
  payload.writeUInt16BE(code, 0)
  reasonBuffer.copy(payload, 2)
  const frame = Buffer.alloc(2 + payload.length)
  frame[0] = 0x88
  frame[1] = payload.length
  payload.copy(frame, 2)
  return frame
}

function inspectWebSocketTextMessage(
  text: string,
  targetUrl: URL,
  processInfo: ProcessInfo,
  remotePort: number,
  direction: 'c2s' | 's2c'
): InspectionResult {
  const contentType = text.trim().startsWith('{') || text.trim().startsWith('[')
    ? 'application/json'
    : 'text/plain'
  const result = evaluateProxyPolicy(
    targetUrl,
    {},
    text,
    contentType,
    [],
    state.policy.blockedHttpBodyPatterns || [],
    state.policy.blockedJsonKeys || [],
    state.policy.blockedJsonPaths || [],
    state.policy.blockedJsonValues || []
  )

  recordWebSocketAudit(
    result.blocked ? 'blocked' : 'logged',
    `WS ${targetUrl.toString()}`,
    processInfo,
    remotePort,
    `${direction} ${text.slice(0, 256)}`
  )
  return result
}

function parseWebSocketCompressionOptions(headerText: string): WebSocketCompressionOptions {
  const match = /sec-websocket-extensions:\s*([^\r\n]+)/i.exec(headerText)
  if (!match) {
    return {
      perMessageDeflate: false,
      clientNoContextTakeover: false,
      serverNoContextTakeover: false,
    }
  }

  const value = match[1].toLowerCase()
  return {
    perMessageDeflate: value.includes('permessage-deflate'),
    clientNoContextTakeover: value.includes('client_no_context_takeover'),
    serverNoContextTakeover: value.includes('server_no_context_takeover'),
  }
}

function inflatePerMessageDeflateMessage(payload: Buffer): Buffer {
  return inflateSync(Buffer.concat([payload, Buffer.from([0x00, 0x00, 0xff, 0xff])]))
}

function attachWebSocketInspector(
  source: net.Socket,
  destination: net.Socket,
  targetUrl: URL,
  processInfo: ProcessInfo,
  direction: 'c2s' | 's2c',
  remotePort: number,
  compression: WebSocketCompressionOptions,
  closeBoth: (reason: string) => void
): void {
  let pending = Buffer.alloc(0)
  let messageState: WebSocketMessageState | null = null

  source.on('data', (chunk: Buffer) => {
    pending = Buffer.concat([pending, chunk])

    while (pending.length >= 2) {
      const first = pending[0]
      const second = pending[1]
      const fin = (first & 0x80) !== 0
      const rsv1 = (first & 0x40) !== 0
      const opcode = first & 0x0f
      const masked = (second & 0x80) !== 0
      let payloadLength = second & 0x7f
      let offset = 2

      if (payloadLength === 126) {
        if (pending.length < offset + 2) return
        payloadLength = pending.readUInt16BE(offset)
        offset += 2
      } else if (payloadLength === 127) {
        if (pending.length < offset + 8) return
        const bigLength = Number(pending.readBigUInt64BE(offset))
        if (!Number.isFinite(bigLength) || bigLength > MAX_CAPTURE_BYTES) {
          closeBoth('websocket-frame-too-large')
          return
        }
        payloadLength = bigLength
        offset += 8
      }

      const maskBytes = masked ? 4 : 0
      if (pending.length < offset + maskBytes + payloadLength) return

      const rawFrame = pending.subarray(0, offset + maskBytes + payloadLength)
      pending = pending.subarray(offset + maskBytes + payloadLength)

      let mask: Buffer | null = null
      if (masked) {
        mask = rawFrame.subarray(offset, offset + 4)
        offset += 4
      }

      const payload = Buffer.from(rawFrame.subarray(offset, offset + payloadLength))
      if (mask) {
        for (let i = 0; i < payload.length; i++) {
          payload[i] ^= mask[i % 4]
        }
      }

      if (opcode === 0x8) {
        destination.write(rawFrame)
        try { destination.end() } catch {}
        try { source.end() } catch {}
        return
      }

      if (opcode === 0x9 || opcode === 0xa) {
        destination.write(rawFrame)
        continue
      }

      if (opcode === 0x1 || (opcode === 0x0 && messageState?.opcode === 0x1)) {
        if (opcode === 0x1) {
          messageState = { opcode: 0x1, compressed: rsv1, chunks: [payload], rawFrames: [Buffer.from(rawFrame)] }
        } else if (messageState) {
          messageState.chunks.push(payload)
          messageState.rawFrames.push(Buffer.from(rawFrame))
        }

        if (fin && messageState) {
          let content = Buffer.concat(messageState.chunks)
          let inspectable = true
          if (messageState.compressed && compression.perMessageDeflate) {
            const noContextTakeover = direction === 'c2s'
              ? compression.clientNoContextTakeover
              : compression.serverNoContextTakeover
            if (noContextTakeover) {
              try {
                content = inflatePerMessageDeflateMessage(content)
              } catch {
                closeBoth('websocket-permessage-deflate')
                return
              }
            } else {
              inspectable = false
            }
          }

          const rawFrames = messageState.rawFrames
          messageState = null
          if (inspectable) {
            const text = content.toString('utf8')
            const result = inspectWebSocketTextMessage(text, targetUrl, processInfo, remotePort, direction)
            if (result.blocked) {
              closeBoth(result.reason || 'websocket-policy')
              return
            }
          }
          for (const frame of rawFrames) {
            destination.write(frame)
          }
        }
      } else if (opcode === 0x2 || (opcode === 0x0 && messageState?.opcode === 0x2)) {
        if (opcode === 0x2) {
          messageState = { opcode: 0x2, compressed: rsv1, chunks: [payload], rawFrames: [Buffer.from(rawFrame)] }
        } else if (messageState) {
          messageState.chunks.push(payload)
          messageState.rawFrames.push(Buffer.from(rawFrame))
        }
        if (fin) {
          if (messageState) {
            for (const frame of messageState.rawFrames) {
              destination.write(frame)
            }
          }
          messageState = null
        }
      } else {
        destination.write(rawFrame)
      }
    }
  })
}

function parseHttpResponseHeader(buffer: Buffer): { headerEnd: number; statusCode: number } | null {
  const marker = buffer.indexOf('\r\n\r\n')
  if (marker < 0) return null
  const headerText = buffer.subarray(0, marker + 4).toString('latin1')
  const firstLine = headerText.split('\r\n', 1)[0] || ''
  const match = /^HTTP\/\d+\.\d+\s+(\d+)/i.exec(firstLine)
  return {
    headerEnd: marker + 4,
    statusCode: match ? Number(match[1]) : 0,
  }
}

function handleUpgrade(
  req: http.IncomingMessage,
  clientSocket: net.Socket,
  head: Buffer,
  forcedScheme?: 'http' | 'https'
): void {
  const targetUrl = buildTargetUrl(req as GenericRequest, forcedScheme)
  const processInfo = lookupSocketProcess(clientSocket)
  console.log(`[TransparentProxy] upgrade request ${req.method || 'GET'} ${targetUrl.toString()} scheme=${forcedScheme || 'http'} transparent=${(clientSocket as any).__psTransparent ? 'yes' : 'no'}`)
  const decision = evaluateProxyPolicy(
    targetUrl,
    req.headers,
    '',
    '',
    state.policy.blockedHttpHeaders || [],
    [],
    [],
    [],
    []
  )

  recordProxyAudit(
    'http_request',
    decision.blocked ? 'blocked' : 'logged',
    `WS ${targetUrl.toString()}`,
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'wss:' || targetUrl.protocol === 'https:' ? 443 : 80),
    decision.reason
  )

  if (decision.blocked) {
    clientSocket.write('HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n')
    clientSocket.destroy()
    return
  }

  const closeBoth = (reason: string) => {
    const closeFrame = createCloseFrame(1008, reason.slice(0, 64))
    try { clientSocket.write(closeFrame) } catch {}
    try { upstream.write(closeFrame) } catch {}
    setTimeout(() => {
      try { clientSocket.destroy() } catch {}
      try { upstream.destroy() } catch {}
    }, 50)
  }

  const upstream =
    targetUrl.protocol === 'wss:' || forcedScheme === 'https'
      ? tls.connect(
          targetUrl.port ? Number(targetUrl.port) : 443,
          targetUrl.hostname,
          { rejectUnauthorized: false },
          () => {
            console.log(`[TransparentProxy] upstream websocket connect secure ${targetUrl.host}`)
            upstream.write(buildUpgradeRequest(targetUrl, req))
            if (head.length > 0) upstream.write(head)
          }
        )
      : net.connect(
          targetUrl.port ? Number(targetUrl.port) : 80,
          targetUrl.hostname,
          () => {
            console.log(`[TransparentProxy] upstream websocket connect plain ${targetUrl.host}`)
            upstream.write(buildUpgradeRequest(targetUrl, req))
            if (head.length > 0) upstream.write(head)
          }
        )

  let responseBuffer = Buffer.alloc(0)
  const onHandshakeData = (chunk: Buffer) => {
    responseBuffer = Buffer.concat([responseBuffer, chunk])
    const parsed = parseHttpResponseHeader(responseBuffer)
    if (!parsed) {
      return
    }

    upstream.off('data', onHandshakeData)
    console.log(`[TransparentProxy] websocket handshake response ${parsed.statusCode} ${targetUrl.toString()}`)
    const headerChunk = responseBuffer.subarray(0, parsed.headerEnd)
    const leftover = responseBuffer.subarray(parsed.headerEnd)
    const compression = parseWebSocketCompressionOptions(headerChunk.toString('latin1'))
    clientSocket.write(headerChunk)

    if (parsed.statusCode !== 101) {
      if (leftover.length > 0) {
        clientSocket.write(leftover)
      }
      upstream.pipe(clientSocket)
      clientSocket.pipe(upstream)
      return
    }

    attachWebSocketInspector(
      clientSocket,
      upstream,
      targetUrl,
      processInfo,
      'c2s',
      targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'wss:' || targetUrl.protocol === 'https:' ? 443 : 80),
      compression,
      closeBoth
    )
    attachWebSocketInspector(
      upstream,
      clientSocket,
      targetUrl,
      processInfo,
      's2c',
      targetUrl.port ? Number(targetUrl.port) : (targetUrl.protocol === 'wss:' || targetUrl.protocol === 'https:' ? 443 : 80),
      compression,
      closeBoth
    )
    if (leftover.length > 0) {
      upstream.emit('data', leftover)
    }
  }

  upstream.on('data', onHandshakeData)
  upstream.on('error', () => clientSocket.destroy())
  clientSocket.on('error', () => upstream.destroy())
}

function enableSystemProxy(): void {
  const proxy = `${state.config.host}:${state.config.port}`
  const refreshScript = `
Add-Type -Namespace WinInet -Name NativeMethods -MemberDefinition @"
[DllImport("wininet.dll", SetLastError=true)]
public static extern bool InternetSetOption(IntPtr hInternet, int dwOption, IntPtr lpBuffer, int dwBufferLength);
"@
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 39, [IntPtr]::Zero, 0) | Out-Null
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 37, [IntPtr]::Zero, 0) | Out-Null
`

  execFileSync('reg', [
    'add',
    'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings',
    '/v',
    'ProxyEnable',
    '/t',
    'REG_DWORD',
    '/d',
    '1',
    '/f',
  ], { stdio: 'ignore' })
  execFileSync('reg', [
    'add',
    'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings',
    '/v',
    'ProxyServer',
    '/t',
    'REG_SZ',
    '/d',
    proxy,
    '/f',
  ], { stdio: 'ignore' })
  try {
    execFileSync('netsh', ['winhttp', 'set', 'proxy', proxy], { stdio: 'ignore' })
  } catch {
    // Ignore WinHTTP proxy failures without elevation.
  }
  runPowerShell(refreshScript)
}

function disableSystemProxy(): void {
  const refreshScript = `
Add-Type -Namespace WinInet -Name NativeMethods -MemberDefinition @"
[DllImport("wininet.dll", SetLastError=true)]
public static extern bool InternetSetOption(IntPtr hInternet, int dwOption, IntPtr lpBuffer, int dwBufferLength);
"@
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 39, [IntPtr]::Zero, 0) | Out-Null
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 37, [IntPtr]::Zero, 0) | Out-Null
`

  execFileSync('reg', [
    'add',
    'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings',
    '/v',
    'ProxyEnable',
    '/t',
    'REG_DWORD',
    '/d',
    '0',
    '/f',
  ], { stdio: 'ignore' })
  try {
    execFileSync('netsh', ['winhttp', 'reset', 'proxy'], { stdio: 'ignore' })
  } catch {
    // Ignore WinHTTP proxy reset failures without elevation.
  }
  runPowerShell(refreshScript)
}

function createHttpsMitmServer(): tls.Server {
  ensureRootCertificate()
  return tls.createServer(
    {
      SNICallback: (servername, callback) => {
        try {
          callback(null, getSecureContext(servername || 'localhost'))
        } catch (error) {
          callback(error as Error)
        }
      },
      pfx: readFileSync(getLeafPfxPath('localhost')),
      passphrase: CERT_PASSWORD,
      ALPNProtocols: ['http/1.1'],
    },
    (secureSocket) => {
      console.log(
        `[TransparentProxy] tls secure connection servername=${String((secureSocket as any).servername || '')} remote=${normalizeAddress(secureSocket.remoteAddress)}:${secureSocket.remotePort}`
      )
      ;(state.httpsParserServer as any)?.emit('connection', secureSocket)
    }
  )
}

function syncKernelRedirectConfig(enabled: boolean): void {
  const addon = getAddon()
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.setRedirectConfig) {
    return
  }

  addon.dlp.kernel_comm.setRedirectConfig({
    enabled,
    proxyProcessId: process.pid,
    proxyPort: state.config.port,
    addressFamily: getAddressFamily(state.config.host),
    proxyAddress: state.config.host,
  })
  console.log(`[TransparentProxy] kernel redirect ${enabled ? 'enabled' : 'disabled'} endpoint=${state.config.host}:${state.config.port} pid=${process.pid}`)
}

function dispatchIncomingSocket(socket: net.Socket): void {
  socket.once('data', (firstChunk: Buffer) => {
    const firstByte = firstChunk[0]
    const isTls = firstByte === 0x16 || firstByte === 0x80
    socket.unshift(firstChunk)
    console.log(`[TransparentProxy] ingress ${isTls ? 'tls' : 'http'} remote=${normalizeAddress(socket.remoteAddress)}:${socket.remotePort} local=${normalizeAddress(socket.localAddress)}:${socket.localPort} transparent=${(socket as any).__psTransparent ? 'yes' : 'no'}`)

    if (isTls) {
      ;(state.httpsServer as any)?.emit('connection', socket)
    } else {
      state.httpServer?.emit('connection', socket)
    }
  })
}

function attachProxyServers(): void {
  state.httpServer = http.createServer((req, res) => {
    void handleProxyRequest(req as unknown as GenericRequest, res as unknown as GenericResponse)
  })
  state.httpsParserServer = http.createServer((req, res) => {
    console.log(`[TransparentProxy] https parser request ${req.method || 'GET'} ${String(req.url || '')}`)
    void handleProxyRequest(req as unknown as GenericRequest, res as unknown as GenericResponse, 'https')
  })
  state.httpsServer = createHttpsMitmServer()
  state.frontServer = net.createServer((socket) => {
    const processInfo = lookupSocketProcess(socket)
    const originalDestination = queryTransparentOriginalDestination(socket, processInfo)
    ;(socket as any).__psProcessInfo = processInfo
    if (originalDestination) {
      ;(socket as any).__psTransparent = true
      ;(socket as any).__psOriginalDestination = originalDestination
      ;(socket as any).__psTargetHost = `${originalDestination.address}:${originalDestination.port}`
    }
    dispatchIncomingSocket(socket)
  })

  state.httpServer.on('upgrade', (req, socket, head) => {
    handleUpgrade(req, socket, head, 'http')
  })

  state.httpsParserServer.on('upgrade', (req, socket, head) => {
    handleUpgrade(req, socket, head, 'https')
  })

  state.httpServer.on('connect', (req, clientSocket, head) => {
    const authority = String(req.url || '')
    const [host, portText] = authority.split(':')
    const port = Number(portText || 443)
    const processInfo = lookupSocketProcess(clientSocket)

    const shouldMitmPort =
      port === 443 ||
      port === 18443 ||
      (host === '127.0.0.1' && port >= 1024)

    if (!state.config.mitmEnabled || !shouldMitmPort) {
      const upstream = net.connect(port, host, () => {
        recordProxyAudit('network_connect', 'logged', `CONNECT ${authority}`, processInfo, port)
        clientSocket.write('HTTP/1.1 200 Connection Established\r\n\r\n')
        if (head.length > 0) upstream.write(head)
        upstream.pipe(clientSocket)
        clientSocket.pipe(upstream)
      })
      upstream.on('error', () => clientSocket.destroy())
      return
    }

    try {
      const secureContext = getSecureContext(host || 'localhost')
      ;(clientSocket as any).__psTargetHost = authority
      ;(clientSocket as any).__psProcessInfo = processInfo
      clientSocket.write('HTTP/1.1 200 Connection Established\r\n\r\n')

      const secureSocket = new tls.TLSSocket(clientSocket, {
        isServer: true,
        secureContext,
      })
      ;(secureSocket as any).__psTargetHost = authority
      ;(secureSocket as any).__psProcessInfo = processInfo
      secureSocket.on('error', () => secureSocket.destroy())
      if (head.length > 0) {
        secureSocket.unshift(head)
      }

      console.log(`[TransparentProxy] explicit CONNECT MITM host=${authority}`)
      recordProxyAudit('network_connect', 'logged', `MITM ${authority}`, processInfo, port)
      ;(state.httpsParserServer as any)?.emit('connection', secureSocket)
    } catch {
      clientSocket.destroy()
    }
  })
}

export function updateLocalProxyPolicy(policy: DlpPolicyLike): void {
  state.policy = { ...policy }
}

export function getLocalProxyStatus(): Record<string, unknown> {
  return {
    running: state.running,
    config: { ...state.config },
    rootCertificatePath: getRootCerPath(),
  }
}

export async function startLocalProxy(config?: LocalProxyConfig): Promise<Record<string, unknown>> {
  if (state.running) {
    return getLocalProxyStatus()
  }

  state.config = {
    ...state.config,
    ...config,
    host: config?.host || state.config.host,
    port: config?.port || state.config.port,
    mitmEnabled: config?.mitmEnabled ?? state.config.mitmEnabled,
    installSystemProxy: config?.installSystemProxy ?? state.config.installSystemProxy,
    enabled: true,
  }

  ensureRootCertificate()
  ensureLeafCertificate('localhost')
  attachProxyServers()

  await new Promise<void>((resolve, reject) => {
    state.frontServer?.once('error', reject)
    state.frontServer?.listen(state.config.port, state.config.host, () => resolve())
  })

  state.running = true
  syncKernelRedirectConfig(true)
  if (state.config.installSystemProxy) {
    enableSystemProxy()
  }

  recordExternalNetEvent({
    type: 'network_connect',
    action: 'logged',
    processName: 'local_proxy',
    processId: process.pid,
    url: `proxy://${state.config.host}:${state.config.port}`,
    timestamp: new Date().toISOString(),
    timestampMs: Date.now(),
    remotePort: state.config.port,
    localPort: state.config.port,
    protocol: 6,
  })

  return getLocalProxyStatus()
}

export async function stopLocalProxy(): Promise<Record<string, unknown>> {
  if (!state.running) {
    return getLocalProxyStatus()
  }

  if (state.config.installSystemProxy) {
    disableSystemProxy()
  }
  syncKernelRedirectConfig(false)

  await Promise.all([
    new Promise<void>((resolve) => state.frontServer?.close(() => resolve()) ?? resolve()),
    new Promise<void>((resolve) => state.httpServer?.close(() => resolve()) ?? resolve()),
    new Promise<void>((resolve) => state.httpsParserServer?.close(() => resolve()) ?? resolve()),
    new Promise<void>((resolve) => state.httpsServer?.close(() => resolve()) ?? resolve()),
  ])

  state.frontServer = null
  state.httpServer = null
  state.httpsParserServer = null
  state.httpsServer = null
  state.running = false
  state.leafCache.clear()

  recordExternalNetEvent({
    type: 'network_disconnect',
    action: 'logged',
    processName: 'local_proxy',
    processId: process.pid,
    url: `proxy://${state.config.host}:${state.config.port}`,
    timestamp: new Date().toISOString(),
    timestampMs: Date.now(),
    remotePort: state.config.port,
    localPort: state.config.port,
    protocol: 6,
  })

  return getLocalProxyStatus()
}
