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

// main/modules/delivery_self_check.ts
var delivery_self_check_exports = {};
__export(delivery_self_check_exports, {
  parseDeliverySelfCheckArgs: () => parseDeliverySelfCheckArgs,
  runInstalledDeliverySelfCheck: () => runInstalledDeliverySelfCheck
});
module.exports = __toCommonJS(delivery_self_check_exports);
var import_electron5 = require("electron");
var import_node_fs4 = require("node:fs");
var import_node_path4 = require("node:path");

// main/modules/delivery_test_sink.ts
var http = __toESM(require("node:http"));
var import_node_fs = require("node:fs");
var import_node_path = require("node:path");
var import_electron = require("electron");
var SINK_DIR = "delivery-test-sink";
var RECORDS_FILE = "received.ndjson";
var state = {
  running: false,
  port: 0,
  server: null,
  outDir: "",
  recordsPath: "",
  receivedCount: 0
};
function getSinkDir() {
  return (0, import_node_path.join)(import_electron.app.getPath("userData"), SINK_DIR);
}
function getRecordsPath() {
  return (0, import_node_path.join)(getSinkDir(), RECORDS_FILE);
}
function ensureSinkDir() {
  (0, import_node_fs.mkdirSync)(getSinkDir(), { recursive: true });
}
function appendRecord(record) {
  ensureSinkDir();
  (0, import_node_fs.appendFileSync)(getRecordsPath(), `${JSON.stringify(record)}
`, "utf8");
}
function recordDeliveryTestBatch(events2, meta) {
  appendRecord({
    receivedAt: (/* @__PURE__ */ new Date()).toISOString(),
    body: {
      source: "local-test-sink",
      meta: meta || {},
      events: events2
    }
  });
  state.receivedCount++;
  return {
    recordsPath: getRecordsPath(),
    receivedCount: state.receivedCount
  };
}
async function startDeliveryTestSink(port = 7777) {
  if (state.running && state.server) {
    return { port: state.port, recordsPath: state.recordsPath };
  }
  ensureSinkDir();
  state.recordsPath = getRecordsPath();
  state.outDir = getSinkDir();
  state.server = http.createServer((req, res) => {
    if (req.method !== "POST") {
      res.writeHead(405, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: false, error: "method not allowed" }));
      return;
    }
    const chunks = [];
    req.on("data", (chunk) => chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)));
    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf8");
      let body = raw;
      try {
        body = JSON.parse(raw);
      } catch {
      }
      appendRecord({
        receivedAt: (/* @__PURE__ */ new Date()).toISOString(),
        body
      });
      state.receivedCount++;
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, receivedCount: state.receivedCount }));
    });
  });
  await new Promise((resolve, reject) => {
    state.server?.once("error", reject);
    state.server?.listen(port, "127.0.0.1", () => resolve());
  });
  state.running = true;
  state.port = port;
  return { port: state.port, recordsPath: state.recordsPath };
}
async function stopDeliveryTestSink() {
  if (!state.server) {
    state.running = false;
    return;
  }
  await new Promise((resolve) => state.server?.close(() => resolve()) ?? resolve());
  state.server = null;
  state.running = false;
}
function clearDeliveryTestSinkRecords() {
  ensureSinkDir();
  (0, import_node_fs.writeFileSync)(getRecordsPath(), "", "utf8");
  state.receivedCount = 0;
}
function getDeliveryTestSinkStatus() {
  return {
    running: state.running,
    port: state.port,
    outDir: state.outDir || getSinkDir(),
    recordsPath: state.recordsPath || getRecordsPath(),
    exists: (0, import_node_fs.existsSync)(getRecordsPath()),
    receivedCount: state.receivedCount
  };
}

// main/modules/delivery_worker.ts
var import_electron4 = require("electron");
var import_node_fs3 = require("node:fs");
var import_node_path3 = require("node:path");

// main/modules/event_sink_http.ts
var http2 = __toESM(require("node:http"));
var https = __toESM(require("node:https"));
var HttpEventSink = class {
  kind = "http";
  name;
  config;
  constructor(config) {
    this.config = config;
    this.name = `http:${config.endpointUrl}`;
  }
  async deliver(events2) {
    const url = new URL(this.config.endpointUrl);
    const body = JSON.stringify({
      source: "PersonalSafer",
      sentAt: (/* @__PURE__ */ new Date()).toISOString(),
      events: events2
    });
    const transport = url.protocol === "https:" ? https : http2;
    const statusCode = await new Promise((resolve, reject) => {
      const req = transport.request(
        {
          protocol: url.protocol,
          hostname: url.hostname,
          port: url.port ? Number(url.port) : url.protocol === "https:" ? 443 : 80,
          path: `${url.pathname}${url.search}`,
          method: "POST",
          headers: {
            ...this.config.headers,
            "content-length": String(Buffer.byteLength(body))
          }
        },
        (res) => {
          const code = res.statusCode || 0;
          res.resume();
          res.on("end", () => {
            if (code >= 200 && code < 300) resolve(code);
            else reject(new Error(`HTTP ${code}`));
          });
        }
      );
      req.setTimeout(this.config.timeoutMs, () => {
        req.destroy(new Error(`timeout after ${this.config.timeoutMs}ms`));
      });
      req.on("error", reject);
      req.write(body);
      req.end();
    });
    return {
      statusCode,
      sinkName: this.name,
      sinkInfo: { endpointUrl: this.config.endpointUrl }
    };
  }
};

// main/modules/event_sink_local_test.ts
var LocalTestEventSink = class {
  kind = "local-test";
  name;
  config;
  constructor(config) {
    this.config = config;
    this.name = `local-test:${config.label}`;
  }
  async deliver(events2) {
    const info = recordDeliveryTestBatch(events2, { label: this.config.label });
    return {
      statusCode: 200,
      sinkName: this.name,
      sinkInfo: info
    };
  }
};

// main/modules/event_sink.ts
function createDefaultDeliverySinkConfig() {
  return {
    kind: "http",
    enabled: false,
    endpointUrl: "http://127.0.0.1:7777/api/dlp/events",
    timeoutMs: 1e4,
    headers: {
      "content-type": "application/json"
    }
  };
}

// main/modules/native_loader.ts
var import_electron2 = require("electron");
var import_path = require("path");
var import_fs = require("fs");
var cachedAddon = null;
var loadAttempted = false;
function getAddonPath() {
  let relativePath;
  if (import_electron2.app.isPackaged) {
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

// main/modules/persistent_event_queue.ts
var import_electron3 = require("electron");
var import_node_fs2 = require("node:fs");
var import_node_path2 = require("node:path");
var QUEUE_DIR = "dlp-queue";
var SNAPSHOT_FILE = "snapshot.json";
var JOURNAL_FILE = "journal.ndjson";
var COMPACT_EVERY_RECORDS = 128;
var COMPACT_FILE_BYTES = 1024 * 1024;
var initialized = false;
var appendedSinceCompact = 0;
function getQueueDir() {
  return (0, import_node_path2.join)(import_electron3.app.getPath("userData"), QUEUE_DIR);
}
function getSnapshotPath() {
  return (0, import_node_path2.join)(getQueueDir(), SNAPSHOT_FILE);
}
function getJournalPath() {
  return (0, import_node_path2.join)(getQueueDir(), JOURNAL_FILE);
}
function ensureQueueDir() {
  (0, import_node_fs2.mkdirSync)(getQueueDir(), { recursive: true });
}
function writeAtomicJson(path, data) {
  const temp = `${path}.tmp`;
  (0, import_node_fs2.writeFileSync)(temp, JSON.stringify(data, null, 2), "utf8");
  (0, import_node_fs2.renameSync)(temp, path);
}
function shouldCompactJournal() {
  if (appendedSinceCompact >= COMPACT_EVERY_RECORDS) {
    return true;
  }
  if (!(0, import_node_fs2.existsSync)(getJournalPath())) {
    return false;
  }
  try {
    return (0, import_node_fs2.statSync)(getJournalPath()).size >= COMPACT_FILE_BYTES;
  } catch {
    return false;
  }
}
function appendJournalRecord(record) {
  ensureQueueDir();
  (0, import_node_fs2.appendFileSync)(getJournalPath(), `${JSON.stringify(record)}
`, "utf8");
  appendedSinceCompact++;
}
function applyStateRecord(events2, record) {
  const event = events2.find((item) => item.id === record.id);
  if (!event) {
    return;
  }
  event.state = record.state;
  if (typeof record.retryCount === "number") event.retryCount = record.retryCount;
  if (record.lastAttemptAt) event.lastAttemptAt = record.lastAttemptAt;
  if (record.ackAt) event.ackAt = record.ackAt;
  if (record.nextAttemptAt !== void 0) event.nextAttemptAt = record.nextAttemptAt;
  if (record.error !== void 0) event.error = record.error;
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
function initializePersistentEventQueue(maxEvents) {
  ensureQueueDir();
  initialized = true;
  appendedSinceCompact = 0;
  let events2 = [];
  let nextId2 = 1;
  if ((0, import_node_fs2.existsSync)(getSnapshotPath())) {
    try {
      const snapshot = JSON.parse((0, import_node_fs2.readFileSync)(getSnapshotPath(), "utf8"));
      if (Array.isArray(snapshot?.events)) {
        events2 = snapshot.events;
      }
      if (typeof snapshot?.nextId === "number" && snapshot.nextId > 0) {
        nextId2 = snapshot.nextId;
      }
    } catch {
      events2 = [];
      nextId2 = 1;
    }
  }
  if ((0, import_node_fs2.existsSync)(getJournalPath())) {
    try {
      const lines = (0, import_node_fs2.readFileSync)(getJournalPath(), "utf8").split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
      for (const line of lines) {
        const record = JSON.parse(line);
        if (record.op === "append") {
          events2.push(record.event);
          if (record.event.id >= nextId2) {
            nextId2 = record.event.id + 1;
          }
        } else if (record.op === "state") {
          applyStateRecord(events2, record);
        }
      }
      appendedSinceCompact = lines.length;
    } catch {
      appendedSinceCompact = 0;
    }
  }
  events2 = trimEvents(events2, maxEvents);
  return { events: events2, nextId: nextId2 };
}
function createPersistedEvent(base) {
  return {
    ...base,
    state: "pending",
    retryCount: 0,
    nextAttemptAt: void 0
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
function updatePersistentEventState(currentEvents, eventId, patch, nextId2, maxEvents) {
  if (!initialized) {
    return;
  }
  const event = currentEvents.find((item) => item.id === eventId);
  if (!event) {
    return;
  }
  if (patch.state) event.state = patch.state;
  if (typeof patch.retryCount === "number") event.retryCount = patch.retryCount;
  if (patch.lastAttemptAt !== void 0) event.lastAttemptAt = patch.lastAttemptAt;
  if (patch.ackAt !== void 0) event.ackAt = patch.ackAt;
  if (patch.nextAttemptAt !== void 0) event.nextAttemptAt = patch.nextAttemptAt;
  if (patch.error !== void 0) event.error = patch.error;
  appendJournalRecord({
    op: "state",
    id: eventId,
    state: event.state,
    retryCount: event.retryCount,
    lastAttemptAt: event.lastAttemptAt,
    ackAt: event.ackAt,
    nextAttemptAt: event.nextAttemptAt,
    error: event.error
  });
  if (shouldCompactJournal()) {
    compactPersistentEventQueue(currentEvents, nextId2, maxEvents);
  }
}
function leasePendingEvents(currentEvents, limit) {
  const now = Date.now();
  return currentEvents.filter((event) => {
    if (!(event.state === "pending" || event.state === "failed")) return false;
    if (!event.nextAttemptAt) return true;
    const nextAttempt = Date.parse(event.nextAttemptAt);
    return !Number.isFinite(nextAttempt) || nextAttempt <= now;
  }).slice(0, limit);
}
function getPersistentEventQueueStats(currentEvents) {
  let pending = 0;
  let sent = 0;
  let failed = 0;
  for (const event of currentEvents) {
    if (event.state === "pending") pending++;
    else if (event.state === "sent") sent++;
    else if (event.state === "failed") failed++;
  }
  return { pending, sent, failed, total: currentEvents.length };
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
  (0, import_node_fs2.writeFileSync)(getJournalPath(), "", "utf8");
  appendedSinceCompact = 0;
}
function clearPersistentEventQueue() {
  if (!initialized) {
    return;
  }
  try {
    if ((0, import_node_fs2.existsSync)(getSnapshotPath())) (0, import_node_fs2.unlinkSync)(getSnapshotPath());
  } catch {
  }
  try {
    if ((0, import_node_fs2.existsSync)(getJournalPath())) (0, import_node_fs2.unlinkSync)(getJournalPath());
  } catch {
  }
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
function initializeEventPersistence() {
  if (persistenceInitialized) {
    return;
  }
  const loaded = initializePersistentEventQueue(MAX_EVENTS);
  events.length = 0;
  events.push(...loaded.events);
  nextId = loaded.nextId;
  persistenceInitialized = true;
}
function flushEventPersistence() {
  if (!persistenceInitialized) {
    return;
  }
  compactPersistentEventQueue(events, nextId, MAX_EVENTS);
}
function clearEvents() {
  events.length = 0;
  nextId = 1;
  clearPersistentEventQueue();
}
function leaseQueuedEvents(limit = 100) {
  return leasePendingEvents(events, limit);
}
function markQueuedEventSent(eventId) {
  updatePersistentEventState(events, eventId, {
    state: "sent",
    ackAt: (/* @__PURE__ */ new Date()).toISOString(),
    nextAttemptAt: void 0,
    error: ""
  }, nextId, MAX_EVENTS);
}
function markQueuedEventFailed(eventId, error, retryCount, nextAttemptAt) {
  const event = events.find((item) => item.id === eventId);
  const resolvedRetryCount = retryCount ?? (event?.retryCount || 0) + 1;
  updatePersistentEventState(events, eventId, {
    state: "failed",
    retryCount: resolvedRetryCount,
    lastAttemptAt: (/* @__PURE__ */ new Date()).toISOString(),
    nextAttemptAt,
    error
  }, nextId, MAX_EVENTS);
}
function getPersistentQueueStats() {
  return getPersistentEventQueueStats(events);
}

// main/modules/delivery_worker.ts
var DELIVERY_DIR = "dlp-delivery";
var DELIVERED_FILE = "delivered.ndjson";
var SINK_CONFIG_FILE = "sink.json";
var workerRunning = false;
var workerPromise = null;
var sinkConfigLoaded = false;
var workerStatus = {
  running: false,
  intervalMs: 5e3,
  batchSize: 100,
  deliveredCount: 0,
  failedCount: 0,
  pendingCount: 0,
  sentCount: 0,
  queueDepth: 0,
  outboxDir: "",
  deliveredPath: "",
  sink: createDefaultDeliverySinkConfig()
};
function getOutboxDir() {
  return (0, import_node_path3.join)(import_electron4.app.getPath("userData"), DELIVERY_DIR);
}
function getDeliveredPath() {
  return (0, import_node_path3.join)(getOutboxDir(), DELIVERED_FILE);
}
function getSinkConfigPath() {
  return (0, import_node_path3.join)(getOutboxDir(), SINK_CONFIG_FILE);
}
function ensureOutboxDir() {
  (0, import_node_fs3.mkdirSync)(getOutboxDir(), { recursive: true });
}
function writeAtomicJson2(path, data) {
  const temp = `${path}.tmp`;
  (0, import_node_fs3.writeFileSync)(temp, JSON.stringify(data, null, 2), "utf8");
  (0, import_node_fs3.renameSync)(temp, path);
}
function normalizeSinkConfig(input) {
  const base = createDefaultDeliverySinkConfig();
  const kind = input.kind || base.kind;
  if (kind === "local-test") {
    return {
      kind: "local-test",
      enabled: input.enabled === true,
      label: typeof input.label === "string" ? input.label : "default"
    };
  }
  return {
    kind: "http",
    enabled: input.enabled === true,
    endpointUrl: typeof input.endpointUrl === "string" ? input.endpointUrl : base.endpointUrl,
    timeoutMs: typeof input.timeoutMs === "number" ? input.timeoutMs : base.timeoutMs,
    headers: typeof input.headers === "object" && input.headers ? { ...input.headers } : { ...base.headers }
  };
}
function resolveEventSink(config) {
  if (config.kind === "local-test") {
    return new LocalTestEventSink(config);
  }
  return new HttpEventSink(config);
}
function loadSinkConfig() {
  ensureOutboxDir();
  if (!(0, import_node_fs3.existsSync)(getSinkConfigPath())) {
    return createDefaultDeliverySinkConfig();
  }
  try {
    return normalizeSinkConfig(JSON.parse((0, import_node_fs3.readFileSync)(getSinkConfigPath(), "utf8")));
  } catch {
    return createDefaultDeliverySinkConfig();
  }
}
function ensureSinkConfigLoaded() {
  if (!sinkConfigLoaded) {
    workerStatus.sink = loadSinkConfig();
    sinkConfigLoaded = true;
  }
}
function persistSinkConfig() {
  ensureOutboxDir();
  writeAtomicJson2(getSinkConfigPath(), workerStatus.sink);
}
function refreshQueueStats() {
  const stats = getPersistentQueueStats();
  workerStatus.pendingCount = stats.pending;
  workerStatus.sentCount = stats.sent;
  workerStatus.failedCount = stats.failed;
  workerStatus.queueDepth = stats.total;
  workerStatus.outboxDir = getOutboxDir();
  workerStatus.deliveredPath = getDeliveredPath();
}
function deliveryBackoffMs(retryCount) {
  const base = 5e3;
  const max = 5 * 60 * 1e3;
  return Math.min(max, base * Math.max(1, retryCount));
}
function appendDeliveredRecord(event, result) {
  ensureOutboxDir();
  (0, import_node_fs3.appendFileSync)(
    getDeliveredPath(),
    `${JSON.stringify({ deliveredAt: (/* @__PURE__ */ new Date()).toISOString(), result, event })}
`,
    "utf8"
  );
}
async function deliverBatch(events2) {
  if (events2.length === 0) {
    return { statusCode: 204, sinkName: "noop" };
  }
  ensureSinkConfigLoaded();
  const sink = resolveEventSink(workerStatus.sink);
  return sink.deliver(events2);
}
function failBatch(events2, error) {
  const message = error instanceof Error ? error.message : String(error);
  for (const event of events2) {
    const retryCount = (event.retryCount || 0) + 1;
    const nextAttemptAt = new Date(Date.now() + deliveryBackoffMs(retryCount)).toISOString();
    markQueuedEventFailed(event.id, message, retryCount, nextAttemptAt);
  }
  workerStatus.lastError = message;
}
function setDeliverySinkConfig(config) {
  ensureSinkConfigLoaded();
  workerStatus.sink = normalizeSinkConfig({ ...workerStatus.sink, ...config });
  persistSinkConfig();
  return getDeliverySinkConfig();
}
function getDeliverySinkConfig() {
  ensureSinkConfigLoaded();
  return normalizeSinkConfig(workerStatus.sink);
}
function startDeliveryWorker(intervalMs = 5e3, batchSize = 100) {
  if (workerRunning) {
    return;
  }
  ensureSinkConfigLoaded();
  workerRunning = true;
  workerStatus.running = true;
  workerStatus.intervalMs = intervalMs;
  workerStatus.batchSize = batchSize;
  refreshQueueStats();
  workerPromise = (async () => {
    while (workerRunning) {
      workerStatus.lastRunAt = (/* @__PURE__ */ new Date()).toISOString();
      try {
        const sinkConfig = getDeliverySinkConfig();
        if (sinkConfig.enabled) {
          const leased = leaseQueuedEvents(batchSize);
          if (leased.length > 0) {
            try {
              const result = await deliverBatch(leased);
              workerStatus.lastResult = result;
              for (const event of leased) {
                appendDeliveredRecord(event, result);
                markQueuedEventSent(event.id);
                workerStatus.deliveredCount++;
              }
              workerStatus.lastSuccessAt = (/* @__PURE__ */ new Date()).toISOString();
            } catch (error) {
              failBatch(leased, error);
            }
          }
        }
        refreshQueueStats();
      } catch (error) {
        workerStatus.lastError = error?.message || String(error);
      }
      if (!workerRunning) {
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, intervalMs));
    }
  })();
}
async function stopDeliveryWorker() {
  workerRunning = false;
  workerStatus.running = false;
  if (workerPromise) {
    try {
      await workerPromise;
    } catch {
    }
  }
  workerPromise = null;
  refreshQueueStats();
}
function getDeliveryWorkerStatus() {
  ensureSinkConfigLoaded();
  refreshQueueStats();
  return {
    ...workerStatus,
    sink: getDeliverySinkConfig()
  };
}

// main/modules/delivery_self_check.ts
function writeText(outDir, name, content) {
  (0, import_node_fs4.mkdirSync)(outDir, { recursive: true });
  (0, import_node_fs4.writeFileSync)((0, import_node_path4.join)(outDir, name), content, "utf8");
}
async function runInstalledDeliverySelfCheck(outDir) {
  (0, import_node_fs4.mkdirSync)(outDir, { recursive: true });
  let exitCode = 0;
  try {
    initializeEventPersistence();
    clearEvents();
    const sink = await startDeliveryTestSink(7777);
    clearDeliveryTestSinkRecords();
    setDeliverySinkConfig({
      enabled: true,
      endpointUrl: `http://127.0.0.1:${sink.port}/api/dlp/events`,
      timeoutMs: 5e3,
      headers: { "content-type": "application/json" }
    });
    recordExternalNetEvent({
      type: "network_connect",
      action: "logged",
      processName: "selfcheck.exe",
      processId: 1234,
      timestamp: (/* @__PURE__ */ new Date()).toISOString(),
      details: "delivery-test-1",
      url: "http://delivery-test.local/1"
    });
    recordExternalNetEvent({
      type: "network_connect",
      action: "logged",
      processName: "selfcheck.exe",
      processId: 1234,
      timestamp: (/* @__PURE__ */ new Date()).toISOString(),
      details: "delivery-test-2",
      url: "http://delivery-test.local/2"
    });
    startDeliveryWorker(1e3, 10);
    await new Promise((resolve) => setTimeout(resolve, 2500));
    const result = {
      deliveryStatus: getDeliveryWorkerStatus(),
      sinkStatus: getDeliveryTestSinkStatus(),
      queueStats: getPersistentQueueStats()
    };
    writeText(outDir, "delivery-self-check-summary.json", JSON.stringify(result, null, 2));
    if (!(result.deliveryStatus.deliveredCount >= 2 && result.sinkStatus.receivedCount >= 1 && result.queueStats.sent >= 2)) {
      exitCode = 1;
    }
  } catch (error) {
    writeText(outDir, "delivery-self-check-error.txt", error?.stack || error?.message || String(error));
    exitCode = 1;
  } finally {
    try {
      await stopDeliveryWorker();
    } catch {
    }
    try {
      await stopDeliveryTestSink();
    } catch {
    }
    try {
      flushEventPersistence();
    } catch {
    }
  }
  return exitCode;
}
function parseDeliverySelfCheckArgs() {
  const enabled = process.argv.includes("--ps-self-check-delivery");
  const arg = process.argv.find((item) => item.startsWith("--ps-self-check-out="));
  const outDir = arg ? arg.slice("--ps-self-check-out=".length) : (0, import_node_path4.join)(import_electron5.app.getPath("temp"), `PersonalSafer-delivery-self-check-${Date.now()}`);
  return { enabled, outDir };
}
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  parseDeliverySelfCheckArgs,
  runInstalledDeliverySelfCheck
});
