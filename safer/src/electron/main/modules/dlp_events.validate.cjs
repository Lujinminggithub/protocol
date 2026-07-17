"use strict";
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
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
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

// main/modules/dlp_events.ts
var dlp_events_exports = {};
__export(dlp_events_exports, {
  clearEvents: () => clearEvents,
  flushEventPersistence: () => flushEventPersistence,
  getEventPersistenceInfo: () => getEventPersistenceInfo,
  getEvents: () => getEvents,
  getPersistentQueueStats: () => getPersistentQueueStats,
  getStats: () => getStats,
  initializeEventPersistence: () => initializeEventPersistence,
  isPolling: () => isPolling,
  leaseQueuedEvents: () => leaseQueuedEvents,
  markQueuedEventFailed: () => markQueuedEventFailed,
  markQueuedEventSent: () => markQueuedEventSent,
  recordExternalNetEvent: () => recordExternalNetEvent,
  startEventPolling: () => startEventPolling,
  stopEventPolling: () => stopEventPolling
});
module.exports = __toCommonJS(dlp_events_exports);

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

// main/modules/dlp_events.ts
var import_node_zlib = require("node:zlib");

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
  if ((0, import_node_fs.existsSync)(getSnapshotPath())) {
    try {
      const snapshot = JSON.parse((0, import_node_fs.readFileSync)(getSnapshotPath(), "utf8"));
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
  if ((0, import_node_fs.existsSync)(getJournalPath())) {
    try {
      const lines = (0, import_node_fs.readFileSync)(getJournalPath(), "utf8").split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
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
  (0, import_node_fs.writeFileSync)(getJournalPath(), "", "utf8");
  appendedSinceCompact = 0;
}
function clearPersistentEventQueue() {
  if (!initialized) {
    return;
  }
  try {
    if ((0, import_node_fs.existsSync)(getSnapshotPath())) (0, import_node_fs.unlinkSync)(getSnapshotPath());
  } catch {
  }
  try {
    if ((0, import_node_fs.existsSync)(getJournalPath())) (0, import_node_fs.unlinkSync)(getJournalPath());
  } catch {
  }
  appendedSinceCompact = 0;
}
function getPersistentEventQueueInfo() {
  return {
    directory: getQueueDir(),
    snapshotPath: getSnapshotPath(),
    journalPath: getJournalPath()
  };
}

// main/modules/dlp_events.ts
var MAX_EVENTS = 5e3;
var events = [];
var nextId = 1;
var pollingActive = false;
var pollLoopPromise = null;
var persistenceInitialized = false;
function push(e) {
  events.push(e);
  if (events.length > MAX_EVENTS) events.splice(0, events.length - MAX_EVENTS);
  if (persistenceInitialized) {
    appendPersistentEvent(e, events, nextId, MAX_EVENTS);
  }
}
function normalizeProcessName(ev) {
  if (ev?.processName) return ev.processName;
  if (ev?.processId === 0 || ev?.processId === 4) return "System";
  return "unknown";
}
function toIsoTimestamp(ev) {
  if (typeof ev?.timestampMs === "number" && Number.isFinite(ev.timestampMs) && ev.timestampMs > 0) {
    return new Date(ev.timestampMs).toISOString();
  }
  return (/* @__PURE__ */ new Date()).toISOString();
}
function mirrorFileAudit(addon, ev, processName) {
  try {
    addon?.audit?.file_audit?.logEvent?.(
      ev.type === "file_write" ? 1 : ev.type === "file_delete" ? 2 : ev.type === "file_rename" ? 3 : 0,
      processName,
      ev.fileName || "",
      ev.processId || 0,
      ev.action || "logged",
      ev.timestampMs || 0
    );
  } catch {
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
function decodeCompressedHttpBody(ev) {
  if (!ev?.bodyPreview || !ev?.contentEncoding || !ev?.url?.startsWith?.("BODY ")) {
    return null;
  }
  try {
    const body = Buffer.isBuffer(ev.bodyPreview) ? ev.bodyPreview : Buffer.from(ev.bodyPreview);
    const encoding = String(ev.contentEncoding).toLowerCase();
    let decoded;
    if (encoding === "gzip") decoded = (0, import_node_zlib.gunzipSync)(body);
    else if (encoding === "deflate") decoded = (0, import_node_zlib.inflateSync)(body);
    else return null;
    const text = decoded.toString("utf8").replace(/\s+/g, " ").trim();
    return text ? `${ev.url} | DECODED ${text.slice(0, 512)}` : null;
  } catch {
    return null;
  }
}
function recordFileEvent(addon, ev) {
  const processName = normalizeProcessName(ev);
  mirrorFileAudit(addon, ev, processName);
  push(createPersistedEvent({
    id: nextId++,
    type: ev.type || "file_create",
    action: ev.action || "logged",
    processName,
    pid: ev.processId || 0,
    timestamp: toIsoTimestamp(ev),
    details: ev.fileName || ""
  }));
}
function recordNetEvent(addon, ev) {
  const addr = `${ev.remoteAddress || ""}:${ev.remotePort || 0}`;
  const decodedBody = decodeCompressedHttpBody(ev);
  if (decodedBody) {
    ev.url = decodedBody;
  }
  const processName = normalizeProcessName(ev);
  mirrorNetAudit(addon, ev, processName);
  push(createPersistedEvent({
    id: nextId++,
    type: ev.type || "network_connect",
    action: ev.action || "logged",
    processName,
    pid: ev.processId || 0,
    timestamp: toIsoTimestamp(ev),
    details: ev.url || ev.sniDomain || addr
  }));
}
function drainFileBatch(addon, kc) {
  if (typeof kc.readFileEventsBatch !== "function") {
    return false;
  }
  let any = false;
  for (let i = 0; i < 32; i++) {
    let batch;
    try {
      batch = kc.readFileEventsBatch();
    } catch {
      break;
    }
    if (!batch || !batch.hasEvent || !Array.isArray(batch.events) || batch.events.length === 0) break;
    any = true;
    for (const ev of batch.events) {
      recordFileEvent(addon, ev);
    }
  }
  return any;
}
function drainNetBatch(addon, kc) {
  if (typeof kc.readNetEventsBatch !== "function") {
    return false;
  }
  let any = false;
  for (let i = 0; i < 64; i++) {
    let batch;
    try {
      batch = kc.readNetEventsBatch();
    } catch {
      break;
    }
    if (!batch || !batch.hasEvent || !Array.isArray(batch.events) || batch.events.length === 0) break;
    any = true;
    for (const ev of batch.events) {
      recordNetEvent(addon, ev);
    }
  }
  return any;
}
function drainOnce() {
  const addon = getAddon();
  if (!addon || !isAddonLoaded()) return;
  const kc = addon.dlp.kernel_comm;
  if (!kc) return;
  if (!drainFileBatch(addon, kc)) {
    for (let i = 0; i < 200; i++) {
      let ev;
      try {
        ev = kc.readFileEvent();
      } catch {
        break;
      }
      if (!ev || !ev.hasEvent) break;
      recordFileEvent(addon, ev);
    }
  }
  if (!drainNetBatch(addon, kc)) {
    for (let i = 0; i < 200; i++) {
      let ev;
      try {
        ev = kc.readNetEvent();
      } catch {
        break;
      }
      if (!ev || !ev.hasEvent) break;
      recordNetEvent(addon, ev);
    }
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
function getEventPersistenceInfo() {
  return getPersistentEventQueueInfo();
}
function startEventPolling(intervalMs = 1e3) {
  if (pollingActive) return;
  pollingActive = true;
  pollLoopPromise = (async () => {
    while (pollingActive) {
      try {
        const addon = getAddon();
        const kc = addon?.dlp?.kernel_comm;
        if (addon && isAddonLoaded() && kc && typeof kc.waitForEvents === "function") {
          await kc.waitForEvents(intervalMs);
        } else {
          await new Promise((resolve) => setTimeout(resolve, intervalMs));
        }
      } catch {
        await new Promise((resolve) => setTimeout(resolve, intervalMs));
      }
      if (!pollingActive) break;
      try {
        drainOnce();
      } catch {
      }
    }
  })();
  console.log("[DlpEvents] polling started");
}
function stopEventPolling() {
  pollingActive = false;
  pollLoopPromise = null;
}
function isPolling() {
  return pollingActive;
}
function getEvents(limit) {
  const arr = events.slice().reverse();
  return typeof limit === "number" ? arr.slice(0, limit) : arr;
}
function clearEvents() {
  events.length = 0;
  nextId = 1;
  clearPersistentEventQueue();
}
function getStats() {
  let fileEvents = 0;
  let networkEvents = 0;
  let blockedEvents = 0;
  for (const e of events) {
    if (e.type.startsWith("file")) fileEvents++;
    else if (e.type.startsWith("network") || e.type.startsWith("http") || e.type.startsWith("ftp") || e.type.startsWith("websocket")) networkEvents++;
    if (e.action === "blocked") blockedEvents++;
  }
  return {
    totalEvents: events.length,
    fileEvents,
    networkEvents,
    registryEvents: 0,
    blockedEvents
  };
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
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  clearEvents,
  flushEventPersistence,
  getEventPersistenceInfo,
  getEvents,
  getPersistentQueueStats,
  getStats,
  initializeEventPersistence,
  isPolling,
  leaseQueuedEvents,
  markQueuedEventFailed,
  markQueuedEventSent,
  recordExternalNetEvent,
  startEventPolling,
  stopEventPolling
});
