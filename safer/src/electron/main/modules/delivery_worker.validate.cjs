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

// main/modules/delivery_worker.ts
var delivery_worker_exports = {};
__export(delivery_worker_exports, {
  getDeliveredQueueInfo: () => getDeliveredQueueInfo,
  getDeliverySinkConfig: () => getDeliverySinkConfig,
  getDeliveryWorkerStatus: () => getDeliveryWorkerStatus,
  setDeliverySinkConfig: () => setDeliverySinkConfig,
  startDeliveryWorker: () => startDeliveryWorker,
  stopDeliveryWorker: () => stopDeliveryWorker
});
module.exports = __toCommonJS(delivery_worker_exports);
var import_electron3 = require("electron");
var import_node_fs2 = require("node:fs");
var http = __toESM(require("node:http"));
var https = __toESM(require("node:https"));
var import_node_path2 = require("node:path");

// main/modules/native_loader.ts
var import_electron = require("electron");

// main/modules/persistent_event_queue.ts
var import_electron2 = require("electron");
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
  return (0, import_node_path.join)(import_electron2.app.getPath("userData"), QUEUE_DIR);
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
  (0, import_node_fs.writeFileSync)(getJournalPath(), "", "utf8");
  appendedSinceCompact = 0;
}

// main/modules/dlp_events.ts
var MAX_EVENTS = 5e3;
var events = [];
var nextId = 1;
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
var defaultSinkConfig = {
  enabled: false,
  endpointUrl: "http://127.0.0.1:7777/api/dlp/events",
  timeoutMs: 1e4,
  headers: {
    "content-type": "application/json"
  }
};
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
  sink: { ...defaultSinkConfig }
};
function getOutboxDir() {
  return (0, import_node_path2.join)(import_electron3.app.getPath("userData"), DELIVERY_DIR);
}
function getDeliveredPath() {
  return (0, import_node_path2.join)(getOutboxDir(), DELIVERED_FILE);
}
function getSinkConfigPath() {
  return (0, import_node_path2.join)(getOutboxDir(), SINK_CONFIG_FILE);
}
function ensureOutboxDir() {
  (0, import_node_fs2.mkdirSync)(getOutboxDir(), { recursive: true });
}
function writeAtomicJson2(path, data) {
  const temp = `${path}.tmp`;
  (0, import_node_fs2.writeFileSync)(temp, JSON.stringify(data, null, 2), "utf8");
  (0, import_node_fs2.renameSync)(temp, path);
}
function loadSinkConfig() {
  ensureOutboxDir();
  if ((0, import_node_fs2.existsSync)(getSinkConfigPath())) {
    try {
      const parsed = JSON.parse((0, import_node_fs2.readFileSync)(getSinkConfigPath(), "utf8"));
      return {
        enabled: parsed?.enabled === true,
        endpointUrl: typeof parsed?.endpointUrl === "string" ? parsed.endpointUrl : defaultSinkConfig.endpointUrl,
        timeoutMs: typeof parsed?.timeoutMs === "number" ? parsed.timeoutMs : defaultSinkConfig.timeoutMs,
        headers: typeof parsed?.headers === "object" && parsed.headers ? parsed.headers : { ...defaultSinkConfig.headers }
      };
    } catch {
      return { ...defaultSinkConfig, headers: { ...defaultSinkConfig.headers } };
    }
  }
  return { ...defaultSinkConfig, headers: { ...defaultSinkConfig.headers } };
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
function appendDeliveredRecord(event, responseCode) {
  ensureOutboxDir();
  (0, import_node_fs2.appendFileSync)(
    getDeliveredPath(),
    `${JSON.stringify({ deliveredAt: (/* @__PURE__ */ new Date()).toISOString(), responseCode, event })}
`,
    "utf8"
  );
}
async function postBatchToSink(events2) {
  ensureSinkConfigLoaded();
  const sink = workerStatus.sink;
  const url = new URL(sink.endpointUrl);
  const body = JSON.stringify({
    source: "PersonalSafer",
    sentAt: (/* @__PURE__ */ new Date()).toISOString(),
    events: events2
  });
  const transport = url.protocol === "https:" ? https : http;
  return new Promise((resolve, reject) => {
    const req = transport.request(
      {
        protocol: url.protocol,
        hostname: url.hostname,
        port: url.port ? Number(url.port) : url.protocol === "https:" ? 443 : 80,
        path: `${url.pathname}${url.search}`,
        method: "POST",
        headers: {
          ...sink.headers,
          "content-length": String(Buffer.byteLength(body))
        }
      },
      (res) => {
        const statusCode = res.statusCode || 0;
        res.resume();
        res.on("end", () => {
          if (statusCode >= 200 && statusCode < 300) {
            resolve(statusCode);
          } else {
            reject(new Error(`HTTP ${statusCode}`));
          }
        });
      }
    );
    req.setTimeout(sink.timeoutMs, () => {
      req.destroy(new Error(`timeout after ${sink.timeoutMs}ms`));
    });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}
async function deliverBatch(events2) {
  if (events2.length === 0) {
    return;
  }
  const statusCode = await postBatchToSink(events2);
  for (const event of events2) {
    appendDeliveredRecord(event, statusCode);
    markQueuedEventSent(event.id);
    workerStatus.deliveredCount++;
    workerStatus.lastSuccessAt = (/* @__PURE__ */ new Date()).toISOString();
  }
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
  workerStatus.sink = {
    ...workerStatus.sink,
    ...config,
    headers: config.headers ? { ...config.headers } : { ...workerStatus.sink.headers }
  };
  persistSinkConfig();
  return { ...workerStatus.sink, headers: { ...workerStatus.sink.headers } };
}
function getDeliverySinkConfig() {
  ensureSinkConfigLoaded();
  return { ...workerStatus.sink, headers: { ...workerStatus.sink.headers } };
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
        const sink = getDeliverySinkConfig();
        if (sink.enabled) {
          const leased = leaseQueuedEvents(batchSize);
          if (leased.length > 0) {
            try {
              await deliverBatch(leased);
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
function getDeliveredQueueInfo() {
  return {
    directory: getOutboxDir(),
    deliveredPath: getDeliveredPath(),
    sinkConfigPath: getSinkConfigPath(),
    exists: (0, import_node_fs2.existsSync)(getDeliveredPath())
  };
}
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  getDeliveredQueueInfo,
  getDeliverySinkConfig,
  getDeliveryWorkerStatus,
  setDeliverySinkConfig,
  startDeliveryWorker,
  stopDeliveryWorker
});
