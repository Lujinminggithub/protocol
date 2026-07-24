"use strict";
var __create = Object.create;
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __getProtoOf = Object.getPrototypeOf;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toESM = (mod, isNodeMode, target) => (target = mod != null ? __create(__getProtoOf(mod)) : {}, __copyProps(
  // If the importer is in node compatibility mode or this is not an ESM
  // file that has been converted to a CommonJS file using a Babel-
  // compatible transform (i.e. "__esModule" has not been set), then set
  // "default" to the CommonJS "module.exports" for node compatibility.
  isNodeMode || !mod || !mod.__esModule ? __defProp(target, "default", { value: mod, enumerable: true }) : target,
  mod
));
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

// main/modules/local_proxy.ts
var local_proxy_exports = {};
__export(local_proxy_exports, {
  getLocalProxyStatus: () => getLocalProxyStatus,
  startLocalProxy: () => startLocalProxy,
  stopLocalProxy: () => stopLocalProxy,
  updateLocalProxyPolicy: () => updateLocalProxyPolicy
});
module.exports = __toCommonJS(local_proxy_exports);
var import_electron4 = require("electron");
var import_node_child_process = require("node:child_process");
var import_node_fs2 = require("node:fs");
var import_node_crypto = require("node:crypto");
var http = __toESM(require("node:http"));
var http2 = __toESM(require("node:http2"));
var https = __toESM(require("node:https"));
var net = __toESM(require("node:net"));
var import_node_path2 = require("node:path");
var tls = __toESM(require("node:tls"));
var import_node_zlib = require("node:zlib");

// main/modules/native_loader.ts
var import_electron = require("electron");
var import_path = require("path");
var import_fs = require("fs");
var cachedAddon = null;
var loadAttempted = false;
function getAddonPath() {
  let relativePath;
  if (import_electron.app.isPackaged) {
    relativePath = (0, import_path.join)(process.resourcesPath, "native", "personal_safer.node");
  } else {
    relativePath = (0, import_path.join)(__dirname, "..", "..", "..", "native", "build", "Release", "personal_safer.node");
  }
  if ((0, import_fs.existsSync)(relativePath)) {
    return relativePath;
  }
  const alternativePaths = [
    (0, import_path.join)(__dirname, "..", "..", "..", "native", "build", "Debug", "personal_safer.node"),
    (0, import_path.join)(__dirname, "personal_safer.node")
  ];
  for (const altPath of alternativePaths) {
    if ((0, import_fs.existsSync)(altPath)) {
      return altPath;
    }
  }
  return null;
}
function loadAddon() {
  if (loadAttempted) {
    return cachedAddon ? { success: true } : { success: false, error: "\u63D2\u4EF6\u52A0\u8F7D\u5931\u8D25\uFF0C\u4E4B\u524D\u5DF2\u5C1D\u8BD5\u8FC7" };
  }
  loadAttempted = true;
  try {
    const addonPath = getAddonPath();
    if (!addonPath) {
      return {
        success: false,
        error: "\u672A\u627E\u5230 personal_safer.node \u6587\u4EF6"
      };
    }
    const addon = require(addonPath);
    cachedAddon = addon;
    return { success: true };
  } catch (err) {
    const errorMessage = err?.message || String(err);
    console.warn("[NativeLoader] \u63D2\u4EF6\u52A0\u8F7D\u5931\u8D25\uFF0C\u5C06\u4F7F\u7528\u6A21\u62DF\u6570\u636E:", errorMessage);
    return {
      success: false,
      error: errorMessage
    };
  }
}
function getAddon() {
  if (!cachedAddon) {
    loadAddon();
  }
  return cachedAddon;
}
function isAddonLoaded() {
  return !!cachedAddon;
}

// main/modules/quarantine_catalog.ts
var import_electron2 = require("electron");

// main/modules/persistent_event_queue.ts
var import_electron3 = require("electron");
var import_node_fs = require("node:fs");
var import_node_path = require("node:path");
var QUEUE_DIR = "dlp-queue";
var SNAPSHOT_FILE = "snapshot.json";
var JOURNAL_FILE = "journal.ndjson";
var COMPACT_EVERY_RECORDS = 128;
var COMPACT_FILE_BYTES = 1024 * 1024;
var initialized = false;
var appendedSinceCompact = 0;
function getQueueDir() {
  return (0, import_node_path.join)(import_electron3.app.getPath("userData"), QUEUE_DIR);
}
function getSnapshotPath() {
  return (0, import_node_path.join)(getQueueDir(), SNAPSHOT_FILE);
}
function getJournalPath() {
  return (0, import_node_path.join)(getQueueDir(), JOURNAL_FILE);
}
function ensureQueueDir() {
  (0, import_node_fs.mkdirSync)(getQueueDir(), { recursive: true });
}
function writeAtomicJson(path, data) {
  const temp = `${path}.tmp`;
  (0, import_node_fs.writeFileSync)(temp, JSON.stringify(data, null, 2), "utf8");
  (0, import_node_fs.renameSync)(temp, path);
}
function shouldCompactJournal() {
  if (appendedSinceCompact >= COMPACT_EVERY_RECORDS) {
    return true;
  }
  if (!(0, import_node_fs.existsSync)(getJournalPath())) {
    return false;
  }
  try {
    return (0, import_node_fs.statSync)(getJournalPath()).size >= COMPACT_FILE_BYTES;
  } catch {
    return false;
  }
}
function appendJournalRecord(record) {
  ensureQueueDir();
  (0, import_node_fs.appendFileSync)(getJournalPath(), `${JSON.stringify(record)}
`, "utf8");
  appendedSinceCompact++;
}
function trimEvents(events2, maxEvents) {
  if (events2.length <= maxEvents) {
    return events2;
  }
  const pending = events2.filter((event) => event.state !== "sent");
  if (pending.length >= maxEvents) {
    return pending.slice(pending.length - maxEvents);
  }
  const sentBudget = maxEvents - pending.length;
  const sent = events2.filter((event) => event.state === "sent");
  return [...sent.slice(Math.max(0, sent.length - sentBudget)), ...pending];
}
function createPersistedEvent(base) {
  return {
    ...base,
    state: "pending",
    retryCount: 0,
    firstAttemptAt: null,
    lastAttemptAt: null,
    ackAt: null,
    nextAttemptAt: null,
    leaseId: null,
    leaseExpireAt: null,
    error: null,
    lastHttpStatus: null,
    lastRemoteId: null,
    sinkStates: {}
  };
}
function appendPersistentEvent(event, currentEvents, nextId2, maxEvents) {
  if (!initialized) {
    return;
  }
  appendJournalRecord({ op: "append", event });
  if (shouldCompactJournal()) {
    compactPersistentEventQueue(currentEvents, nextId2, maxEvents);
  }
}
function compactPersistentEventQueue(currentEvents, nextId2, maxEvents) {
  if (!initialized) {
    return;
  }
  ensureQueueDir();
  const snapshotEvents = trimEvents(currentEvents.slice(), maxEvents);
  writeAtomicJson(getSnapshotPath(), {
    nextId: nextId2,
    events: snapshotEvents,
    updatedAt: (/* @__PURE__ */ new Date()).toISOString()
  });
  (0, import_node_fs.writeFileSync)(getJournalPath(), "", "utf8");
  appendedSinceCompact = 0;
}

// main/modules/dlp_events.ts
var MAX_EVENTS = 5e3;
var events = [];
var nextId = 1;
var persistenceInitialized = false;
function push(e) {
  events.push(e);
  if (events.length > MAX_EVENTS) events.splice(0, events.length - MAX_EVENTS);
  if (persistenceInitialized) {
    appendPersistentEvent(e, events, nextId, MAX_EVENTS);
  }
}
function mirrorNetAudit(addon, ev, processName) {
  const eventType = ev.type === "http_request" ? 7 : ev.type === "http_response" ? 8 : ev.type === "ftp_command" ? 9 : ev.type === "sni_capture" ? 12 : ev.type === "network_disconnect" ? 6 : 6;
  try {
    addon?.audit?.net_audit?.logEvent?.(
      eventType,
      processName,
      ev.url || ev.sniDomain || `${ev.remoteAddress || ""}:${ev.remotePort || 0}`,
      ev.processId || 0,
      ev.remotePort || 0,
      ev.localPort || 0,
      ev.protocol || 0,
      ev.action || "logged",
      ev.timestampMs || 0
    );
  } catch {
  }
}
function recordExternalNetEvent(ev) {
  const addon = getAddon();
  const processName = ev?.processName || "local_proxy";
  mirrorNetAudit(addon, ev, processName);
  push(createPersistedEvent({
    id: nextId++,
    type: ev?.type || "network_connect",
    action: ev?.action || "logged",
    processName,
    pid: ev?.processId || process.pid,
    timestamp: ev?.timestamp || (/* @__PURE__ */ new Date()).toISOString(),
    details: ev?.details || ev?.url || ev?.sniDomain || `${ev?.remoteAddress || ""}:${ev?.remotePort || 0}`
  }));
}

// main/modules/local_proxy.ts
var ROOT_CA_SUBJECT = "CN=PersonalSafer Local Root CA V2";
var LEGACY_ROOT_CA_SUBJECT = "CN=PersonalSafer Local Root CA";
var MAX_CAPTURE_BYTES = 16 * 1024 * 1024;
var certificatePassword = null;
var state = {
  running: false,
  config: {
    enabled: false,
    installSystemProxy: false,
    mitmEnabled: true,
    host: "127.0.0.1",
    port: 8899
  },
  frontServer: null,
  httpServer: null,
  httpsServer: null,
  httpsParserServer: null,
  policy: {},
  certDir: "",
  leafCache: /* @__PURE__ */ new Map()
};
function getCertDir() {
  if (!state.certDir) {
    state.certDir = (0, import_node_path2.join)(import_electron4.app.getPath("userData"), "mitm-v2");
  }
  (0, import_node_fs2.mkdirSync)(state.certDir, { recursive: true });
  return state.certDir;
}
function psQuote(value) {
  return `'${value.replace(/'/g, "''")}'`;
}
function runPowerShell(script) {
  (0, import_node_child_process.execFileSync)(
    "powershell.exe",
    ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
    { stdio: "ignore" }
  );
}
function getRootCerPath() {
  return (0, import_node_path2.join)(getCertDir(), "root.cer");
}
function getCertificatePasswordPath() {
  return (0, import_node_path2.join)(getCertDir(), "pfx-password.dpapi");
}
function getCertificatePassword() {
  if (certificatePassword) return certificatePassword;
  if (!import_electron4.safeStorage.isEncryptionAvailable()) {
    throw new Error("Windows credential protection is unavailable; refusing to create MITM keys");
  }
  const passwordPath = getCertificatePasswordPath();
  if ((0, import_node_fs2.existsSync)(passwordPath)) {
    certificatePassword = import_electron4.safeStorage.decryptString((0, import_node_fs2.readFileSync)(passwordPath));
  } else {
    certificatePassword = (0, import_node_crypto.randomBytes)(32).toString("base64url");
    (0, import_node_fs2.writeFileSync)(passwordPath, import_electron4.safeStorage.encryptString(certificatePassword), { mode: 384 });
  }
  return certificatePassword;
}
function getLeafPfxPath(hostname) {
  return (0, import_node_path2.join)(getCertDir(), `${hostname.replace(/[^a-zA-Z0-9.-]/g, "_")}.pfx`);
}
function ensureRootCertificate() {
  const rootCerPath = getRootCerPath();
  const legacyPfxPath = (0, import_node_path2.join)(import_electron4.app.getPath("userData"), "mitm", "root.pfx");
  if ((0, import_node_fs2.existsSync)(legacyPfxPath)) (0, import_node_fs2.unlinkSync)(legacyPfxPath);
  const script = `
$legacySubject = ${psQuote(LEGACY_ROOT_CA_SUBJECT)}
@('Cert:\\CurrentUser\\Root','Cert:\\CurrentUser\\TrustedPublisher','Cert:\\CurrentUser\\My') | ForEach-Object {
  Get-ChildItem $_ | Where-Object { $_.Subject -eq $legacySubject } | Remove-Item -Force
}
$subject = ${psQuote(ROOT_CA_SUBJECT)}
$rootCer = ${psQuote(rootCerPath)}
$cert = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type Custom -Subject $subject -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -KeyUsage CertSign, CRLSign, DigitalSignature -TextExtension @('2.5.29.19={critical}{text}CA=true') -CertStoreLocation 'Cert:\\CurrentUser\\My' -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath $rootCer -Force | Out-Null
`;
  runPowerShell(script);
  try {
    (0, import_node_child_process.execFileSync)("certutil", ["-user", "-addstore", "-f", "Root", rootCerPath], { stdio: "ignore" });
  } catch {
  }
  try {
    (0, import_node_child_process.execFileSync)("certutil", ["-user", "-addstore", "-f", "TrustedPublisher", rootCerPath], { stdio: "ignore" });
  } catch {
  }
}
function ensureLeafCertificate(hostname) {
  const normalizedHost = hostname.toLowerCase();
  const leafPfxPath = getLeafPfxPath(normalizedHost);
  const password = getCertificatePassword();
  ensureRootCertificate();
  const script = `
$hostName = ${psQuote(normalizedHost)}
$subject = ${psQuote(ROOT_CA_SUBJECT)}
$leafPfx = ${psQuote(leafPfxPath)}
$pwd = ConvertTo-SecureString ${psQuote(password)} -AsPlainText -Force
$root = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $root) { throw 'Root CA missing' }
$leafSubject = 'CN=' + $hostName
$leaf = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $leafSubject -and $_.Issuer -eq $root.Subject } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $leaf) {
  $leaf = New-SelfSignedCertificate -Type Custom -DnsName $hostName -Subject $leafSubject -Signer $root -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy Exportable -TextExtension @('2.5.29.19={critical}{text}CA=false','2.5.29.37={text}1.3.6.1.5.5.7.3.1') -CertStoreLocation 'Cert:\\CurrentUser\\My' -NotAfter (Get-Date).AddYears(2)
}
Export-PfxCertificate -Cert $leaf -FilePath $leafPfx -Password $pwd -Force | Out-Null
`;
  runPowerShell(script);
  return leafPfxPath;
}
function getSecureContext(hostname) {
  const normalizedHost = hostname.toLowerCase();
  const cached = state.leafCache.get(normalizedHost);
  if (cached) {
    return cached;
  }
  const pfxPath = ensureLeafCertificate(normalizedHost);
  const context = tls.createSecureContext({
    pfx: (0, import_node_fs2.readFileSync)(pfxPath),
    passphrase: getCertificatePassword()
  });
  state.leafCache.set(normalizedHost, context);
  return context;
}
function containsNoCase(haystack, needle) {
  return haystack.toLowerCase().includes(needle.toLowerCase());
}
function domainMatches(host, rule) {
  const normalizedHost = host.toLowerCase();
  const normalizedRule = rule.toLowerCase();
  return normalizedHost === normalizedRule || normalizedHost.endsWith(`.${normalizedRule}`);
}
function toHeaderStrings(headers) {
  const values = [];
  for (const [key, value] of Object.entries(headers)) {
    if (Array.isArray(value)) {
      for (const item of value) {
        values.push(`${key}: ${item}`);
      }
    } else if (typeof value === "string") {
      values.push(`${key}: ${value}`);
    }
  }
  return values;
}
function isInspectableContentType(contentType) {
  const lowered = contentType.toLowerCase();
  return lowered.startsWith("text/") || lowered.startsWith("application/json");
}
function decodeBodyPreview(body, contentEncoding) {
  try {
    const encoding = contentEncoding.toLowerCase();
    if (encoding === "gzip") {
      return (0, import_node_zlib.gunzipSync)(body).toString("utf8");
    }
    if (encoding === "deflate") {
      return (0, import_node_zlib.inflateSync)(body).toString("utf8");
    }
  } catch {
    return body.toString("utf8");
  }
  return body.toString("utf8");
}
function collectJsonSemantics(input, basePath, out) {
  if (Array.isArray(input)) {
    input.forEach((item, index) => collectJsonSemantics(item, `${basePath}[${index}]`, out));
    return;
  }
  if (input && typeof input === "object") {
    for (const [key, value] of Object.entries(input)) {
      const nextPath = basePath ? `${basePath}.${key}` : key;
      out.keys.push(key);
      out.paths.push(nextPath);
      collectJsonSemantics(value, nextPath, out);
    }
    return;
  }
  if (input !== void 0 && input !== null) {
    const value = String(input);
    out.values.push(value);
    out.entries.push({
      key: basePath.includes(".") ? basePath.slice(basePath.lastIndexOf(".") + 1) : basePath,
      path: basePath,
      value
    });
  }
}
function normalizeJsonPathRule(rule) {
  return rule.replace(/\[\]/g, "[*]");
}
function globMatchNoCase(text, pattern) {
  let textIndex = 0;
  let patternIndex = 0;
  let starPattern = -1;
  let starText = -1;
  const foldedText = text.toLowerCase();
  const foldedPattern = normalizeJsonPathRule(pattern).toLowerCase();
  while (textIndex < foldedText.length) {
    if (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === "*") {
      starPattern = ++patternIndex;
      starText = textIndex;
      continue;
    }
    if (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === foldedText[textIndex]) {
      patternIndex++;
      textIndex++;
      continue;
    }
    if (starPattern !== -1) {
      patternIndex = starPattern;
      textIndex = ++starText;
      continue;
    }
    return false;
  }
  while (patternIndex < foldedPattern.length && foldedPattern[patternIndex] === "*") {
    patternIndex++;
  }
  return patternIndex === foldedPattern.length;
}
function jsonPathRuleMatches(path, rule) {
  if (!rule) {
    return false;
  }
  return globMatchNoCase(path, rule) || containsNoCase(path, rule);
}
function parseJsonPredicateRule(rule) {
  const leftBrace = rule.indexOf("{");
  const rightBrace = leftBrace >= 0 ? rule.indexOf("}", leftBrace + 1) : -1;
  if (leftBrace < 0 || rightBrace <= leftBrace + 1) {
    return null;
  }
  const predicate = rule.slice(leftBrace + 1, rightBrace);
  const prefixPattern = rule.slice(0, leftBrace);
  const suffix = rule.slice(rightBrace + 1);
  const groupParts = predicate.split("||").map((item) => item.trim()).filter(Boolean);
  const groups = [];
  for (const groupPart of groupParts) {
    const parts = groupPart.split(/&&|,/).map((item) => item.trim()).filter(Boolean);
    const conditions = [];
    for (const part of parts) {
      const operators = [
        { token: "!=", operator: "ne" },
        { token: ">=", operator: "ge" },
        { token: "<=", operator: "le" },
        { token: ">", operator: "gt" },
        { token: "<", operator: "lt" },
        { token: "=", operator: "eq" }
      ];
      let resolvedOperator = null;
      let splitIndex = -1;
      let separatorLength = 0;
      for (const candidate of operators) {
        const index = part.indexOf(candidate.token);
        if (index > 0) {
          resolvedOperator = candidate.operator;
          splitIndex = index;
          separatorLength = candidate.token.length;
          break;
        }
      }
      if (!resolvedOperator) {
        return null;
      }
      if (splitIndex <= 0 || splitIndex >= part.length - separatorLength) {
        return null;
      }
      const key = part.slice(0, splitIndex).trim();
      const valuePattern = part.slice(splitIndex + separatorLength).trim();
      if (!key || !valuePattern) {
        return null;
      }
      conditions.push({ key, valuePattern, operator: resolvedOperator });
    }
    if (conditions.length === 0) {
      return null;
    }
    groups.push(conditions);
  }
  if (groups.length === 0 || !suffix) {
    return null;
  }
  return {
    prefixPattern,
    groups,
    suffix
  };
}
function endsWithNoCase(text, suffix) {
  return text.toLowerCase().endsWith(suffix.toLowerCase());
}
function equalsNoCase(left, right) {
  return left.toLowerCase() === right.toLowerCase();
}
function valuePatternMatches(value, pattern) {
  return globMatchNoCase(value, pattern) || containsNoCase(value, pattern);
}
function parseNumericValue(value) {
  const trimmed = value.trim();
  if (!trimmed) {
    return null;
  }
  if (!/^-?\d+(\.\d+)?$/.test(trimmed)) {
    return null;
  }
  const parsed = Number(trimmed);
  return Number.isFinite(parsed) ? parsed : null;
}
function predicateConditionMatches(candidateValue, condition) {
  if (condition.operator === "eq") {
    return valuePatternMatches(candidateValue, condition.valuePattern);
  }
  if (condition.operator === "ne") {
    return !valuePatternMatches(candidateValue, condition.valuePattern);
  }
  const left = parseNumericValue(candidateValue);
  const right = parseNumericValue(condition.valuePattern);
  if (left === null || right === null) {
    return false;
  }
  if (condition.operator === "gt") return left > right;
  if (condition.operator === "ge") return left >= right;
  if (condition.operator === "lt") return left < right;
  return left <= right;
}
function jsonPredicateRuleMatches(semantic, rule) {
  const parsed = parseJsonPredicateRule(rule);
  if (!parsed) {
    return false;
  }
  for (const entry of semantic.entries) {
    if (!endsWithNoCase(entry.path, parsed.suffix)) {
      continue;
    }
    const objectPath = entry.path.slice(0, entry.path.length - parsed.suffix.length);
    if (!jsonPathRuleMatches(objectPath, parsed.prefixPattern)) {
      continue;
    }
    if (parsed.groups.some((conditions) => conditions.every((condition) => {
      const siblingPath = objectPath ? `${objectPath}.${condition.key}` : condition.key;
      const candidateMatched = semantic.entries.some(
        (candidate) => equalsNoCase(candidate.path, siblingPath) && predicateConditionMatches(candidate.value, condition)
      );
      return candidateMatched;
    }))) {
      return true;
    }
  }
  return false;
}
function inspectTextualBody(bodyText, contentType) {
  const semantic = { keys: [], paths: [], values: [], entries: [] };
  if (contentType.toLowerCase().startsWith("application/json")) {
    try {
      collectJsonSemantics(JSON.parse(bodyText), "", semantic);
    } catch {
    }
  }
  return semantic;
}
function evaluateProxyPolicy(targetUrl, headers, bodyText, contentType, headerRules, bodyRules, jsonKeyRules, jsonPathRules, jsonValueRules) {
  const policy = state.policy;
  const headerLines = toHeaderStrings(headers);
  for (const domainRule of policy.blockedDomains || []) {
    if (domainMatches(targetUrl.hostname, domainRule)) {
      return { blocked: true, reason: `domain:${domainRule}` };
    }
  }
  for (const urlRule of policy.blockedUrls || []) {
    if (containsNoCase(targetUrl.toString(), urlRule)) {
      return { blocked: true, reason: `url:${urlRule}` };
    }
  }
  for (const headerRule of headerRules) {
    if (headerLines.some((line) => containsNoCase(line, headerRule))) {
      return { blocked: true, reason: `header:${headerRule}` };
    }
  }
  if (bodyText) {
    for (const bodyRule of bodyRules) {
      if (containsNoCase(bodyText, bodyRule)) {
        return { blocked: true, reason: `body:${bodyRule}` };
      }
    }
    const semantic = inspectTextualBody(bodyText, contentType);
    for (const keyRule of jsonKeyRules) {
      if (semantic.keys.some((value) => containsNoCase(value, keyRule))) {
        return { blocked: true, reason: `json-key:${keyRule}` };
      }
    }
    for (const pathRule of jsonPathRules) {
      if (semantic.paths.some((value) => jsonPathRuleMatches(value, pathRule)) || jsonPredicateRuleMatches(semantic, pathRule)) {
        return { blocked: true, reason: `json-path:${pathRule}` };
      }
    }
    for (const valueRule of jsonValueRules) {
      if (semantic.values.some((value) => containsNoCase(value, valueRule))) {
        return { blocked: true, reason: `json-value:${valueRule}` };
      }
    }
  }
  return { blocked: false };
}
async function readBody(stream, maxBytes) {
  const chunks = [];
  let total = 0;
  for await (const chunk of stream) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    total += buffer.length;
    if (total > maxBytes) {
      throw new Error(`payload too large: ${total}`);
    }
    chunks.push(buffer);
  }
  return Buffer.concat(chunks);
}
function normalizeAddress(address) {
  if (!address) return "";
  if (address.startsWith("::ffff:")) {
    return address.slice(7);
  }
  return address;
}
function getAddressFamily(address) {
  return address.includes(":") ? 23 : 2;
}
function lookupSocketProcess(socket) {
  const cached = socket.__psProcessInfo;
  if (cached) {
    return cached;
  }
  const fallback = { pid: process.pid, processName: "local_proxy" };
  const addon = getAddon();
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.lookupConnectionProcess) {
    ;
    socket.__psProcessInfo = fallback;
    return fallback;
  }
  const remoteAddress = normalizeAddress(socket.remoteAddress);
  const localAddress = normalizeAddress(socket.localAddress);
  const remotePort = socket.remotePort || 0;
  const localPort = socket.localPort || 0;
  if (!remoteAddress || !localAddress || !remotePort || !localPort) {
    ;
    socket.__psProcessInfo = fallback;
    return fallback;
  }
  const mapped = addon.dlp.kernel_comm.lookupConnectionProcess({
    localAddress: remoteAddress,
    localPort: remotePort,
    remoteAddress: localAddress,
    remotePort: localPort
  });
  const result = {
    pid: Number(mapped?.pid || process.pid),
    processName: String(mapped?.processName || "local_proxy")
  };
  socket.__psProcessInfo = result;
  return result;
}
function queryTransparentOriginalDestination(socket, processInfo) {
  const cached = socket.__psOriginalDestination;
  if (cached) {
    return cached;
  }
  const addon = getAddon();
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.queryRedirectDestination) {
    return null;
  }
  const localAddress = normalizeAddress(socket.remoteAddress || "127.0.0.1");
  const localPort = socket.remotePort || 0;
  if (!localAddress || !localPort || !processInfo.pid) {
    return null;
  }
  const query = addon.dlp.kernel_comm.queryRedirectDestination({
    processId: processInfo.pid,
    protocol: 6,
    addressFamily: getAddressFamily(localAddress),
    localPort,
    localAddress
  });
  if (!query?.found) {
    console.log(`[TransparentProxy] no redirect mapping pid=${processInfo.pid} local=${localAddress}:${localPort}`);
    return null;
  }
  const destination = {
    address: String(query.originalRemoteAddress || ""),
    port: Number(query.originalRemotePort || 0)
  };
  console.log(`[TransparentProxy] redirect mapping pid=${processInfo.pid} local=${localAddress}:${localPort} -> ${destination.address}:${destination.port}`);
  socket.__psOriginalDestination = destination;
  return destination;
}
function buildTargetUrl(req, forcedScheme) {
  const authority = String(
    req.headers.host || req.headers[":authority"] || req.socket.__psTargetHost || ""
  );
  const originalDestination = req.socket.__psOriginalDestination;
  if (forcedScheme) {
    const host = authority || (originalDestination ? `${originalDestination.address}:${originalDestination.port}` : "");
    return new URL(`${forcedScheme}://${host}${req.url || "/"}`);
  }
  if (req.url && /^https?:\/\//i.test(req.url)) {
    return new URL(req.url);
  }
  return new URL(`http://${authority || (originalDestination ? `${originalDestination.address}:${originalDestination.port}` : "")}${req.url || "/"}`);
}
function filterUpstreamHeaders(headers, protocol) {
  const output = {};
  for (const [key, value] of Object.entries(headers)) {
    const lowered = key.toLowerCase();
    if (lowered === "proxy-connection" || lowered === "connection" || lowered === "keep-alive" || lowered === "transfer-encoding" || lowered === "upgrade") {
      continue;
    }
    if (protocol === "http2" && lowered.startsWith(":")) {
      continue;
    }
    if (value !== void 0) {
      output[key] = value;
    }
  }
  return output;
}
function filterDownstreamHeaders(headers) {
  const output = {};
  for (const [key, value] of Object.entries(headers)) {
    const lowered = key.toLowerCase();
    if (lowered.startsWith(":") || lowered === "transfer-encoding" || lowered === "connection") {
      continue;
    }
    output[key] = value;
  }
  return output;
}
function respondBlocked(res, reason) {
  res.writeHead(403, { "content-type": "text/plain; charset=utf-8" });
  res.end(`Blocked by PersonalSafer local proxy: ${reason}`);
}
function recordProxyAudit(type, action, url, processInfo, remotePort, details) {
  recordExternalNetEvent({
    type,
    action,
    processName: processInfo.processName || "local_proxy",
    processId: processInfo.pid || process.pid,
    url,
    details: details || url,
    timestamp: (/* @__PURE__ */ new Date()).toISOString(),
    timestampMs: Date.now(),
    remotePort,
    localPort: state.config.port,
    protocol: 6
  });
}
function recordWebSocketAudit(action, url, processInfo, remotePort, details) {
  recordExternalNetEvent({
    type: "websocket_message",
    action,
    processName: processInfo.processName || "local_proxy",
    processId: processInfo.pid || process.pid,
    url,
    details: details || url,
    timestamp: (/* @__PURE__ */ new Date()).toISOString(),
    timestampMs: Date.now(),
    remotePort,
    localPort: state.config.port,
    protocol: 6
  });
}
async function proxyViaHttp1(targetUrl, req, requestBody) {
  const transport = targetUrl.protocol === "https:" ? https : http;
  return new Promise((resolve, reject) => {
    const upstreamReq = transport.request(
      {
        protocol: targetUrl.protocol,
        hostname: targetUrl.hostname,
        port: targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "https:" ? 443 : 80,
        method: req.method,
        path: `${targetUrl.pathname}${targetUrl.search}`,
        headers: {
          ...filterUpstreamHeaders(req.headers, "http1"),
          host: targetUrl.host,
          "content-length": String(requestBody.length)
        },
        rejectUnauthorized: true,
        servername: targetUrl.hostname
      },
      async (upstreamRes) => {
        try {
          const body = await readBody(upstreamRes, MAX_CAPTURE_BYTES);
          resolve({
            statusCode: upstreamRes.statusCode || 200,
            headers: filterDownstreamHeaders(upstreamRes.headers),
            trailers: filterDownstreamHeaders(upstreamRes.trailers),
            body
          });
        } catch (error) {
          reject(error);
        }
      }
    );
    upstreamReq.on("error", reject);
    if (requestBody.length > 0) {
      upstreamReq.write(requestBody);
    }
    upstreamReq.end();
  });
}
async function proxyViaHttp2(targetUrl, req, requestBody) {
  const origin = `${targetUrl.protocol}//${targetUrl.host}`;
  return new Promise((resolve, reject) => {
    const session = http2.connect(origin, {
      rejectUnauthorized: true,
      servername: targetUrl.hostname
    });
    const headers = {
      ":method": req.method || "GET",
      ":path": `${targetUrl.pathname}${targetUrl.search}`,
      ":scheme": targetUrl.protocol.replace(":", ""),
      ":authority": targetUrl.host,
      ...filterUpstreamHeaders(req.headers, "http2")
    };
    const bodyChunks = [];
    const trailers = {};
    let responseHeaders = {};
    let statusCode = 200;
    let finished = false;
    const finish = (error, response) => {
      if (finished) return;
      finished = true;
      try {
        session.close();
      } catch {
      }
      if (error) reject(error);
      else if (response) resolve(response);
    };
    session.on("error", finish);
    const stream = session.request(headers);
    stream.on("response", (headersMap) => {
      const normalized = {};
      for (const [key, value] of Object.entries(headersMap)) {
        if (key === ":status") {
          statusCode = Number(value || 200);
        } else if (typeof value === "string" || Array.isArray(value)) {
          normalized[key] = value;
        }
      }
      responseHeaders = filterDownstreamHeaders(normalized);
    });
    stream.on("trailers", (headersMap) => {
      for (const [key, value] of Object.entries(headersMap)) {
        if (typeof value === "string" || Array.isArray(value)) {
          trailers[key] = value;
        }
      }
    });
    stream.on("data", (chunk) => {
      bodyChunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
    });
    stream.on("end", () => {
      finish(void 0, {
        statusCode,
        headers: responseHeaders,
        trailers: filterDownstreamHeaders(trailers),
        body: Buffer.concat(bodyChunks)
      });
    });
    stream.on("error", finish);
    if (requestBody.length > 0) {
      stream.write(requestBody);
    }
    stream.end();
  });
}
async function proxyUpstream(targetUrl, req, requestBody) {
  if (targetUrl.protocol === "https:") {
    try {
      return await proxyViaHttp2(targetUrl, req, requestBody);
    } catch {
      return proxyViaHttp1(targetUrl, req, requestBody);
    }
  }
  return proxyViaHttp1(targetUrl, req, requestBody);
}
async function handleProxyRequest(req, res, forcedScheme) {
  let targetUrl;
  let requestBody = Buffer.alloc(0);
  try {
    targetUrl = buildTargetUrl(req, forcedScheme);
    requestBody = await readBody(req, MAX_CAPTURE_BYTES);
    console.log(`[TransparentProxy] request ${req.method || "GET"} ${targetUrl.toString()} transparent=${req.socket.__psTransparent ? "yes" : "no"}`);
  } catch (error) {
    res.writeHead(400, { "content-type": "text/plain; charset=utf-8" });
    res.end(error?.message || "invalid proxy request");
    return;
  }
  const processInfo = lookupSocketProcess(req.socket);
  const requestContentType = String(req.headers["content-type"] || "");
  const requestText = requestBody.length > 0 && isInspectableContentType(requestContentType) ? requestBody.toString("utf8") : "";
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
  );
  recordProxyAudit(
    "http_request",
    requestDecision.blocked ? "blocked" : "logged",
    targetUrl.toString(),
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "https:" ? 443 : 80,
    requestDecision.reason
  );
  if (requestDecision.blocked) {
    respondBlocked(res, requestDecision.reason || "request-policy");
    return;
  }
  let upstream;
  try {
    upstream = await proxyUpstream(targetUrl, req, requestBody);
  } catch (error) {
    res.writeHead(502, { "content-type": "text/plain; charset=utf-8" });
    res.end(error?.message || "upstream request failed");
    return;
  }
  const responseContentType = String(upstream.headers["content-type"] || "");
  const responseEncoding = String(upstream.headers["content-encoding"] || "");
  const responseText = upstream.body.length > 0 && isInspectableContentType(responseContentType) ? decodeBodyPreview(upstream.body, responseEncoding) : "";
  const responseDecision = evaluateProxyPolicy(
    targetUrl,
    { ...upstream.headers, ...upstream.trailers },
    responseText,
    responseContentType,
    [
      ...state.policy.blockedHttpHeaders || [],
      ...state.policy.blockedHttpTrailers || []
    ],
    state.policy.blockedHttpBodyPatterns || [],
    state.policy.blockedJsonKeys || [],
    state.policy.blockedJsonPaths || [],
    state.policy.blockedJsonValues || []
  );
  recordProxyAudit(
    "http_response",
    responseDecision.blocked ? "blocked" : "logged",
    targetUrl.toString(),
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "https:" ? 443 : 80,
    responseDecision.reason
  );
  if (responseDecision.blocked) {
    respondBlocked(res, responseDecision.reason || "response-policy");
    return;
  }
  const downstreamHeaders = {
    ...upstream.headers,
    "content-length": String(upstream.body.length)
  };
  res.writeHead(upstream.statusCode, downstreamHeaders);
  res.end(upstream.body);
}
function buildUpgradeRequest(targetUrl, req) {
  const path = `${targetUrl.pathname}${targetUrl.search}`;
  const lines = [`${req.method || "GET"} ${path} HTTP/${req.httpVersion || "1.1"}`];
  let hasHost = false;
  const rawHeaders = req.rawHeaders || [];
  for (let i = 0; i < rawHeaders.length; i += 2) {
    const name = rawHeaders[i];
    const value = rawHeaders[i + 1];
    const lowered = name.toLowerCase();
    if (lowered === "proxy-connection") continue;
    if (lowered === "host") hasHost = true;
    lines.push(`${name}: ${value}`);
  }
  if (!hasHost) {
    lines.push(`Host: ${targetUrl.host}`);
  }
  lines.push("", "");
  return Buffer.from(lines.join("\r\n"), "utf8");
}
function createCloseFrame(code, reason = "") {
  const reasonBuffer = Buffer.from(reason, "utf8");
  const payload = Buffer.alloc(2 + reasonBuffer.length);
  payload.writeUInt16BE(code, 0);
  reasonBuffer.copy(payload, 2);
  const frame = Buffer.alloc(2 + payload.length);
  frame[0] = 136;
  frame[1] = payload.length;
  payload.copy(frame, 2);
  return frame;
}
function inspectWebSocketTextMessage(text, targetUrl, processInfo, remotePort, direction) {
  const contentType = text.trim().startsWith("{") || text.trim().startsWith("[") ? "application/json" : "text/plain";
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
  );
  recordWebSocketAudit(
    result.blocked ? "blocked" : "logged",
    `WS ${targetUrl.toString()}`,
    processInfo,
    remotePort,
    `${direction} ${text.slice(0, 256)}`
  );
  return result;
}
function parseWebSocketCompressionOptions(headerText) {
  const match = /sec-websocket-extensions:\s*([^\r\n]+)/i.exec(headerText);
  if (!match) {
    return {
      perMessageDeflate: false,
      clientNoContextTakeover: false,
      serverNoContextTakeover: false
    };
  }
  const value = match[1].toLowerCase();
  return {
    perMessageDeflate: value.includes("permessage-deflate"),
    clientNoContextTakeover: value.includes("client_no_context_takeover"),
    serverNoContextTakeover: value.includes("server_no_context_takeover")
  };
}
function inflatePerMessageDeflateMessage(payload) {
  return (0, import_node_zlib.inflateSync)(Buffer.concat([payload, Buffer.from([0, 0, 255, 255])]));
}
function attachWebSocketInspector(source, destination, targetUrl, processInfo, direction, remotePort, compression, closeBoth) {
  let pending = Buffer.alloc(0);
  let messageState = null;
  source.on("data", (chunk) => {
    pending = Buffer.concat([pending, chunk]);
    while (pending.length >= 2) {
      const first = pending[0];
      const second = pending[1];
      const fin = (first & 128) !== 0;
      const rsv1 = (first & 64) !== 0;
      const opcode = first & 15;
      const masked = (second & 128) !== 0;
      let payloadLength = second & 127;
      let offset = 2;
      if (payloadLength === 126) {
        if (pending.length < offset + 2) return;
        payloadLength = pending.readUInt16BE(offset);
        offset += 2;
      } else if (payloadLength === 127) {
        if (pending.length < offset + 8) return;
        const bigLength = Number(pending.readBigUInt64BE(offset));
        if (!Number.isFinite(bigLength) || bigLength > MAX_CAPTURE_BYTES) {
          closeBoth("websocket-frame-too-large");
          return;
        }
        payloadLength = bigLength;
        offset += 8;
      }
      const maskBytes = masked ? 4 : 0;
      if (pending.length < offset + maskBytes + payloadLength) return;
      const rawFrame = pending.subarray(0, offset + maskBytes + payloadLength);
      pending = pending.subarray(offset + maskBytes + payloadLength);
      let mask = null;
      if (masked) {
        mask = rawFrame.subarray(offset, offset + 4);
        offset += 4;
      }
      const payload = Buffer.from(rawFrame.subarray(offset, offset + payloadLength));
      if (mask) {
        for (let i = 0; i < payload.length; i++) {
          payload[i] ^= mask[i % 4];
        }
      }
      if (opcode === 8) {
        destination.write(rawFrame);
        try {
          destination.end();
        } catch {
        }
        try {
          source.end();
        } catch {
        }
        return;
      }
      if (opcode === 9 || opcode === 10) {
        destination.write(rawFrame);
        continue;
      }
      if (opcode === 1 || opcode === 0 && messageState?.opcode === 1) {
        if (opcode === 1) {
          messageState = { opcode: 1, compressed: rsv1, chunks: [payload], rawFrames: [Buffer.from(rawFrame)] };
        } else if (messageState) {
          messageState.chunks.push(payload);
          messageState.rawFrames.push(Buffer.from(rawFrame));
        }
        if (fin && messageState) {
          let content = Buffer.concat(messageState.chunks);
          let inspectable = true;
          if (messageState.compressed && compression.perMessageDeflate) {
            const noContextTakeover = direction === "c2s" ? compression.clientNoContextTakeover : compression.serverNoContextTakeover;
            if (noContextTakeover) {
              try {
                content = inflatePerMessageDeflateMessage(content);
              } catch {
                closeBoth("websocket-permessage-deflate");
                return;
              }
            } else {
              inspectable = false;
            }
          }
          const rawFrames = messageState.rawFrames;
          messageState = null;
          if (inspectable) {
            const text = content.toString("utf8");
            const result = inspectWebSocketTextMessage(text, targetUrl, processInfo, remotePort, direction);
            if (result.blocked) {
              closeBoth(result.reason || "websocket-policy");
              return;
            }
          }
          for (const frame of rawFrames) {
            destination.write(frame);
          }
        }
      } else if (opcode === 2 || opcode === 0 && messageState?.opcode === 2) {
        if (opcode === 2) {
          messageState = { opcode: 2, compressed: rsv1, chunks: [payload], rawFrames: [Buffer.from(rawFrame)] };
        } else if (messageState) {
          messageState.chunks.push(payload);
          messageState.rawFrames.push(Buffer.from(rawFrame));
        }
        if (fin) {
          if (messageState) {
            for (const frame of messageState.rawFrames) {
              destination.write(frame);
            }
          }
          messageState = null;
        }
      } else {
        destination.write(rawFrame);
      }
    }
  });
}
function parseHttpResponseHeader(buffer) {
  const marker = buffer.indexOf("\r\n\r\n");
  if (marker < 0) return null;
  const headerText = buffer.subarray(0, marker + 4).toString("latin1");
  const firstLine = headerText.split("\r\n", 1)[0] || "";
  const match = /^HTTP\/\d+\.\d+\s+(\d+)/i.exec(firstLine);
  return {
    headerEnd: marker + 4,
    statusCode: match ? Number(match[1]) : 0
  };
}
function handleUpgrade(req, clientSocket, head, forcedScheme) {
  const targetUrl = buildTargetUrl(req, forcedScheme);
  const processInfo = lookupSocketProcess(clientSocket);
  console.log(`[TransparentProxy] upgrade request ${req.method || "GET"} ${targetUrl.toString()} scheme=${forcedScheme || "http"} transparent=${clientSocket.__psTransparent ? "yes" : "no"}`);
  const decision = evaluateProxyPolicy(
    targetUrl,
    req.headers,
    "",
    "",
    state.policy.blockedHttpHeaders || [],
    [],
    [],
    [],
    []
  );
  recordProxyAudit(
    "http_request",
    decision.blocked ? "blocked" : "logged",
    `WS ${targetUrl.toString()}`,
    processInfo,
    targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "wss:" || targetUrl.protocol === "https:" ? 443 : 80,
    decision.reason
  );
  if (decision.blocked) {
    clientSocket.write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    clientSocket.destroy();
    return;
  }
  const closeBoth = (reason) => {
    const closeFrame = createCloseFrame(1008, reason.slice(0, 64));
    try {
      clientSocket.write(closeFrame);
    } catch {
    }
    try {
      upstream.write(closeFrame);
    } catch {
    }
    setTimeout(() => {
      try {
        clientSocket.destroy();
      } catch {
      }
      try {
        upstream.destroy();
      } catch {
      }
    }, 50);
  };
  const upstream = targetUrl.protocol === "wss:" || forcedScheme === "https" ? tls.connect(
    targetUrl.port ? Number(targetUrl.port) : 443,
    targetUrl.hostname,
    { rejectUnauthorized: true, servername: targetUrl.hostname },
    () => {
      console.log(`[TransparentProxy] upstream websocket connect secure ${targetUrl.host}`);
      upstream.write(buildUpgradeRequest(targetUrl, req));
      if (head.length > 0) upstream.write(head);
    }
  ) : net.connect(
    targetUrl.port ? Number(targetUrl.port) : 80,
    targetUrl.hostname,
    () => {
      console.log(`[TransparentProxy] upstream websocket connect plain ${targetUrl.host}`);
      upstream.write(buildUpgradeRequest(targetUrl, req));
      if (head.length > 0) upstream.write(head);
    }
  );
  let responseBuffer = Buffer.alloc(0);
  const onHandshakeData = (chunk) => {
    responseBuffer = Buffer.concat([responseBuffer, chunk]);
    const parsed = parseHttpResponseHeader(responseBuffer);
    if (!parsed) {
      return;
    }
    upstream.off("data", onHandshakeData);
    console.log(`[TransparentProxy] websocket handshake response ${parsed.statusCode} ${targetUrl.toString()}`);
    const headerChunk = responseBuffer.subarray(0, parsed.headerEnd);
    const leftover = responseBuffer.subarray(parsed.headerEnd);
    const compression = parseWebSocketCompressionOptions(headerChunk.toString("latin1"));
    clientSocket.write(headerChunk);
    if (parsed.statusCode !== 101) {
      if (leftover.length > 0) {
        clientSocket.write(leftover);
      }
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
      return;
    }
    attachWebSocketInspector(
      clientSocket,
      upstream,
      targetUrl,
      processInfo,
      "c2s",
      targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "wss:" || targetUrl.protocol === "https:" ? 443 : 80,
      compression,
      closeBoth
    );
    attachWebSocketInspector(
      upstream,
      clientSocket,
      targetUrl,
      processInfo,
      "s2c",
      targetUrl.port ? Number(targetUrl.port) : targetUrl.protocol === "wss:" || targetUrl.protocol === "https:" ? 443 : 80,
      compression,
      closeBoth
    );
    if (leftover.length > 0) {
      upstream.emit("data", leftover);
    }
  };
  upstream.on("data", onHandshakeData);
  upstream.on("error", () => clientSocket.destroy());
  clientSocket.on("error", () => upstream.destroy());
}
function enableSystemProxy() {
  const proxy = `${state.config.host}:${state.config.port}`;
  const refreshScript = `
Add-Type -Namespace WinInet -Name NativeMethods -MemberDefinition @"
[DllImport("wininet.dll", SetLastError=true)]
public static extern bool InternetSetOption(IntPtr hInternet, int dwOption, IntPtr lpBuffer, int dwBufferLength);
"@
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 39, [IntPtr]::Zero, 0) | Out-Null
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 37, [IntPtr]::Zero, 0) | Out-Null
`;
  (0, import_node_child_process.execFileSync)("reg", [
    "add",
    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
    "/v",
    "ProxyEnable",
    "/t",
    "REG_DWORD",
    "/d",
    "1",
    "/f"
  ], { stdio: "ignore" });
  (0, import_node_child_process.execFileSync)("reg", [
    "add",
    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
    "/v",
    "ProxyServer",
    "/t",
    "REG_SZ",
    "/d",
    proxy,
    "/f"
  ], { stdio: "ignore" });
  try {
    (0, import_node_child_process.execFileSync)("netsh", ["winhttp", "set", "proxy", proxy], { stdio: "ignore" });
  } catch {
  }
  runPowerShell(refreshScript);
}
function disableSystemProxy() {
  const refreshScript = `
Add-Type -Namespace WinInet -Name NativeMethods -MemberDefinition @"
[DllImport("wininet.dll", SetLastError=true)]
public static extern bool InternetSetOption(IntPtr hInternet, int dwOption, IntPtr lpBuffer, int dwBufferLength);
"@
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 39, [IntPtr]::Zero, 0) | Out-Null
[WinInet.NativeMethods]::InternetSetOption([IntPtr]::Zero, 37, [IntPtr]::Zero, 0) | Out-Null
`;
  (0, import_node_child_process.execFileSync)("reg", [
    "add",
    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
    "/v",
    "ProxyEnable",
    "/t",
    "REG_DWORD",
    "/d",
    "0",
    "/f"
  ], { stdio: "ignore" });
  try {
    (0, import_node_child_process.execFileSync)("netsh", ["winhttp", "reset", "proxy"], { stdio: "ignore" });
  } catch {
  }
  runPowerShell(refreshScript);
}
function createHttpsMitmServer() {
  ensureRootCertificate();
  return tls.createServer(
    {
      SNICallback: (servername, callback) => {
        try {
          callback(null, getSecureContext(servername || "localhost"));
        } catch (error) {
          callback(error);
        }
      },
      pfx: (0, import_node_fs2.readFileSync)(getLeafPfxPath("localhost")),
      passphrase: getCertificatePassword(),
      ALPNProtocols: ["http/1.1"]
    },
    (secureSocket) => {
      console.log(
        `[TransparentProxy] tls secure connection servername=${String(secureSocket.servername || "")} remote=${normalizeAddress(secureSocket.remoteAddress)}:${secureSocket.remotePort}`
      );
      state.httpsParserServer?.emit("connection", secureSocket);
    }
  );
}
function syncKernelRedirectConfig(enabled) {
  const addon = getAddon();
  if (!addon || !isAddonLoaded() || !addon.dlp?.kernel_comm?.setRedirectConfig) {
    return;
  }
  addon.dlp.kernel_comm.setRedirectConfig({
    enabled,
    proxyProcessId: process.pid,
    proxyPort: state.config.port,
    addressFamily: getAddressFamily(state.config.host),
    proxyAddress: state.config.host
  });
  console.log(`[TransparentProxy] kernel redirect ${enabled ? "enabled" : "disabled"} endpoint=${state.config.host}:${state.config.port} pid=${process.pid}`);
}
function dispatchIncomingSocket(socket) {
  socket.once("data", (firstChunk) => {
    const firstByte = firstChunk[0];
    const isTls = firstByte === 22 || firstByte === 128;
    socket.unshift(firstChunk);
    console.log(`[TransparentProxy] ingress ${isTls ? "tls" : "http"} remote=${normalizeAddress(socket.remoteAddress)}:${socket.remotePort} local=${normalizeAddress(socket.localAddress)}:${socket.localPort} transparent=${socket.__psTransparent ? "yes" : "no"}`);
    if (isTls) {
      ;
      state.httpsServer?.emit("connection", socket);
    } else {
      state.httpServer?.emit("connection", socket);
    }
  });
}
function attachProxyServers() {
  state.httpServer = http.createServer((req, res) => {
    void handleProxyRequest(req, res);
  });
  state.httpsParserServer = http.createServer((req, res) => {
    console.log(`[TransparentProxy] https parser request ${req.method || "GET"} ${String(req.url || "")}`);
    void handleProxyRequest(req, res, "https");
  });
  state.httpsServer = createHttpsMitmServer();
  state.frontServer = net.createServer((socket) => {
    const processInfo = lookupSocketProcess(socket);
    const originalDestination = queryTransparentOriginalDestination(socket, processInfo);
    socket.__psProcessInfo = processInfo;
    if (originalDestination) {
      ;
      socket.__psTransparent = true;
      socket.__psOriginalDestination = originalDestination;
      socket.__psTargetHost = `${originalDestination.address}:${originalDestination.port}`;
    }
    dispatchIncomingSocket(socket);
  });
  state.httpServer.on("upgrade", (req, socket, head) => {
    handleUpgrade(req, socket, head, "http");
  });
  state.httpsParserServer.on("upgrade", (req, socket, head) => {
    handleUpgrade(req, socket, head, "https");
  });
  state.httpServer.on("connect", (req, clientSocket, head) => {
    const authority = String(req.url || "");
    const [host, portText] = authority.split(":");
    const port = Number(portText || 443);
    const processInfo = lookupSocketProcess(clientSocket);
    const shouldMitmPort = port === 443 || port === 18443 || host === "127.0.0.1" && port >= 1024;
    if (!state.config.mitmEnabled || !shouldMitmPort) {
      const upstream = net.connect(port, host, () => {
        recordProxyAudit("network_connect", "logged", `CONNECT ${authority}`, processInfo, port);
        clientSocket.write("HTTP/1.1 200 Connection Established\r\n\r\n");
        if (head.length > 0) upstream.write(head);
        upstream.pipe(clientSocket);
        clientSocket.pipe(upstream);
      });
      upstream.on("error", () => clientSocket.destroy());
      return;
    }
    try {
      const secureContext = getSecureContext(host || "localhost");
      clientSocket.__psTargetHost = authority;
      clientSocket.__psProcessInfo = processInfo;
      clientSocket.write("HTTP/1.1 200 Connection Established\r\n\r\n");
      const secureSocket = new tls.TLSSocket(clientSocket, {
        isServer: true,
        secureContext
      });
      secureSocket.__psTargetHost = authority;
      secureSocket.__psProcessInfo = processInfo;
      secureSocket.on("error", () => secureSocket.destroy());
      if (head.length > 0) {
        secureSocket.unshift(head);
      }
      console.log(`[TransparentProxy] explicit CONNECT MITM host=${authority}`);
      recordProxyAudit("network_connect", "logged", `MITM ${authority}`, processInfo, port);
      state.httpsParserServer?.emit("connection", secureSocket);
    } catch {
      clientSocket.destroy();
    }
  });
}
function updateLocalProxyPolicy(policy) {
  state.policy = { ...policy };
}
function getLocalProxyStatus() {
  return {
    running: state.running,
    config: { ...state.config },
    rootCertificatePath: getRootCerPath()
  };
}
async function startLocalProxy(config) {
  if (state.running) {
    return getLocalProxyStatus();
  }
  state.config = {
    ...state.config,
    ...config,
    host: config?.host || state.config.host,
    port: config?.port || state.config.port,
    mitmEnabled: config?.mitmEnabled ?? state.config.mitmEnabled,
    installSystemProxy: config?.installSystemProxy ?? state.config.installSystemProxy,
    enabled: true
  };
  ensureRootCertificate();
  ensureLeafCertificate("localhost");
  attachProxyServers();
  await new Promise((resolve, reject) => {
    state.frontServer?.once("error", reject);
    state.frontServer?.listen(state.config.port, state.config.host, () => resolve());
  });
  state.running = true;
  syncKernelRedirectConfig(true);
  if (state.config.installSystemProxy) {
    enableSystemProxy();
  }
  recordExternalNetEvent({
    type: "network_connect",
    action: "logged",
    processName: "local_proxy",
    processId: process.pid,
    url: `proxy://${state.config.host}:${state.config.port}`,
    timestamp: (/* @__PURE__ */ new Date()).toISOString(),
    timestampMs: Date.now(),
    remotePort: state.config.port,
    localPort: state.config.port,
    protocol: 6
  });
  return getLocalProxyStatus();
}
async function stopLocalProxy() {
  if (!state.running) {
    return getLocalProxyStatus();
  }
  if (state.config.installSystemProxy) {
    disableSystemProxy();
  }
  syncKernelRedirectConfig(false);
  await Promise.all([
    new Promise((resolve) => state.frontServer?.close(() => resolve()) ?? resolve()),
    new Promise((resolve) => state.httpServer?.close(() => resolve()) ?? resolve()),
    new Promise((resolve) => state.httpsParserServer?.close(() => resolve()) ?? resolve()),
    new Promise((resolve) => state.httpsServer?.close(() => resolve()) ?? resolve())
  ]);
  state.frontServer = null;
  state.httpServer = null;
  state.httpsParserServer = null;
  state.httpsServer = null;
  state.running = false;
  state.leafCache.clear();
  recordExternalNetEvent({
    type: "network_disconnect",
    action: "logged",
    processName: "local_proxy",
    processId: process.pid,
    url: `proxy://${state.config.host}:${state.config.port}`,
    timestamp: (/* @__PURE__ */ new Date()).toISOString(),
    timestampMs: Date.now(),
    remotePort: state.config.port,
    localPort: state.config.port,
    protocol: 6
  });
  return getLocalProxyStatus();
}
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  getLocalProxyStatus,
  startLocalProxy,
  stopLocalProxy,
  updateLocalProxyPolicy
});
