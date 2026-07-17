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

// main/modules/delivery_test_sink.ts
var delivery_test_sink_exports = {};
__export(delivery_test_sink_exports, {
  clearDeliveryTestSinkRecords: () => clearDeliveryTestSinkRecords,
  getDeliveryTestSinkStatus: () => getDeliveryTestSinkStatus,
  startDeliveryTestSink: () => startDeliveryTestSink,
  stopDeliveryTestSink: () => stopDeliveryTestSink
});
module.exports = __toCommonJS(delivery_test_sink_exports);
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
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  clearDeliveryTestSinkRecords,
  getDeliveryTestSinkStatus,
  startDeliveryTestSink,
  stopDeliveryTestSink
});
