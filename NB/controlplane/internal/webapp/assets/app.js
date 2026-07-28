const state = { token: sessionStorage.getItem("nbAdminToken") || "", dashboard: null, lines: [], operations: [], incidents: [], executors: [] };

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const escapeHTML = (value = "") => String(value).replace(/[&<>'"]/g, (char) => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;",'"':"&quot;"})[char]);
const formatTime = (value) => value ? new Intl.DateTimeFormat("zh-CN", {month:"2-digit", day:"2-digit", hour:"2-digit", minute:"2-digit", second:"2-digit"}).format(new Date(value)) : "--";
const formatNumber = (value, digits = 0) => Number(value || 0).toLocaleString("zh-CN", {maximumFractionDigits: digits});
const statusText = {healthy:"健康", degraded:"异常", unknown:"待采集", active:"运行中", draft:"草稿", validating:"验证中", maintenance:"维护", disabled:"停用", queued:"排队中", dispatched:"已下发", running:"执行中", succeeded:"成功", failed:"失败", cancelled:"已取消", rolled_back:"已回滚", open:"未处理", firing:"告警中", resolved:"已恢复", critical:"严重", warning:"警告", info:"信息"};
const kindText = {"line.open":"开通线路", "line.validate":"验证线路", "line.upgrade":"升级", "line.rollback":"回滚", "line.disable":"停用"};

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("Authorization", `Bearer ${state.token}`);
  if (options.body) headers.set("Content-Type", "application/json");
  const response = await fetch(path, {...options, headers});
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    const error = new Error(body.error || `请求失败 (${response.status})`);
    error.status = response.status;
    throw error;
  }
  return body;
}

function badge(value) {
  return `<span class="badge ${escapeHTML(value)}">${escapeHTML(statusText[value] || value || "未知")}</span>`;
}

function toast(message) {
  const element = $("#toast");
  element.textContent = message;
  element.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => element.classList.remove("show"), 2600);
}

function setConnected(connected) {
  $("#apiDot").classList.toggle("online", connected);
  $("#apiStatus").textContent = connected ? "中央服务在线" : "未连接";
}

function metric(label, value, note) {
  return `<div class="metric"><span class="metric-label">${label}</span><strong class="metric-value">${value}</strong><span class="metric-note">${note}</span></div>`;
}

function renderMetrics() {
  const d = state.dashboard || {};
  $("#metrics").innerHTML = [
    metric("线路总数", formatNumber(d.lines_total), `${formatNumber(d.lines_healthy)} 条健康`),
    metric("异常线路", formatNumber(d.lines_degraded), d.lines_degraded ? "需要处理" : "当前无异常"),
    metric("总容量", `${formatNumber(d.capacity_mbps)} Mbps`, "已登记线路容量"),
    metric("当前吞吐", `${formatNumber(d.throughput_mbps, 2)} Mbps`, "最新 worker 汇总"),
    metric("执行中任务", formatNumber(d.active_operations), "排队、下发与执行中"),
    metric("未恢复告警", formatNumber(d.open_incidents), "open / firing")
  ].join("");
}

function overviewRow(line) {
  const ratio = line.capacity_mbps > 0 ? Math.min(100, line.throughput_mbps / line.capacity_mbps * 100) : 0;
  const deployment = line.active_deployment || "未部署";
  return `<tr>
    <td><div class="line-name">${escapeHTML(line.name)}</div><div class="line-id">${escapeHTML(line.id)} · ${formatNumber(line.workers)} workers</div></td>
    <td>${badge(line.health)}</td>
    <td>${escapeHTML(line.entry_region)} → ${escapeHTML(line.exit_region)}<div class="subtext">${escapeHTML(line.provider || "--")}</div></td>
    <td class="capacity"><strong>${formatNumber(line.throughput_mbps, 2)}</strong> / ${formatNumber(line.capacity_mbps)} Mbps<div class="capacity-bar"><span style="width:${ratio}%"></span></div></td>
    <td>${formatNumber(line.sessions)}</td>
    <td>${formatNumber(line.queue_age_p95_us, 0)} μs</td>
    <td>${formatNumber(line.effective_loss_pct, 3)}%</td>
    <td><span class="mono">${escapeHTML(deployment.slice(0, 16))}</span><div class="subtext">${formatTime(line.last_observed_at)}</div></td>
  </tr>`;
}

function renderOverview() {
  const lines = state.dashboard?.lines || [];
  $("#overviewLines").innerHTML = lines.length ? lines.map(overviewRow).join("") : `<tr><td class="empty" colspan="8">尚未登记线路</td></tr>`;
}

function lineRow(line) {
  const definitions = line.status === "draft" || line.status === "disabled"
    ? [["line.open", "开线", "primary-action"]]
    : [["line.validate", "验证", ""], ["line.upgrade", "升级", ""], ["line.rollback", "回滚", ""], ["line.disable", "停用", "danger-action"]];
  const states = definitions.map(([kind]) => operationAvailability(line.id, kind));
  const actions = definitions.map(([kind, label, style], index) => {
    const availability = states[index];
    return availability.enabled
      ? `<button class="action-button ${style}" data-action="${kind}" data-line="${escapeHTML(line.id)}">${label}</button>`
      : `<button class="action-button ${style}" disabled title="${escapeHTML(availability.reason)}">${label}</button>`;
  }).join("");
  const blocked = states.every((item) => !item.enabled) ? `<span class="executor-note">${escapeHTML(states[0]?.reason || "等待执行器")}</span>` : "";
  return `<tr data-search="${escapeHTML(`${line.id} ${line.name} ${line.entry_region} ${line.exit_region} ${line.provider}`.toLowerCase())}" data-status="${escapeHTML(line.status)}">
    <td><div class="line-name">${escapeHTML(line.name)}</div><div class="line-id">${escapeHTML(line.id)}</div></td>
    <td>${badge(line.status)}</td><td>${escapeHTML(line.entry_region)}</td><td>${escapeHTML(line.exit_region)}</td>
    <td>${escapeHTML(line.provider || "--")}</td><td>${formatNumber(line.capacity_mbps)} Mbps</td>
    <td><span class="mono">${escapeHTML(line.profile || "--")}</span></td>
    <td><div class="actions">${actions}</div>${blocked}</td>
  </tr>`;
}

function operationAvailability(lineID, kind) {
  const matching = state.executors.flatMap((executor) => (executor.lines || [])
    .filter((line) => line.line_id === lineID).map((line) => ({executor, line})));
  const available = matching.find(({executor, line}) => executor.online && (line.operations || []).includes(kind));
  if (available) return {enabled:true, reason:""};
  const reason = matching.find(({line}) => line.reason)?.line.reason;
  if (reason) return {enabled:false, reason};
  if (matching.length) return {enabled:false, reason:"执行器离线或未授权此操作"};
  return {enabled:false, reason:"等待执行器"};
}

function renderLines() {
  $("#linesTable").innerHTML = state.lines.length ? state.lines.map(lineRow).join("") : `<tr><td class="empty" colspan="8">尚未登记线路</td></tr>`;
  filterLines();
  $$('[data-action]').forEach((button) => button.addEventListener("click", () => openOperation(button.dataset.line, button.dataset.action)));
  const options = [`<option value="">全部线路</option>`, ...state.lines.map((line) => `<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`)];
  $("#operationLine").innerHTML = options.join("");
}

function filterLines() {
  const search = $("#lineSearch").value.trim().toLowerCase();
  const status = $("#lineStatus").value;
  $$("#linesTable tr[data-search]").forEach((row) => {
    row.hidden = (search && !row.dataset.search.includes(search)) || (status && row.dataset.status !== status);
  });
}

function renderOperations() {
  const rows = state.operations.map((item) => `<tr><td><span class="mono">${escapeHTML(item.id)}</span></td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(kindText[item.kind] || item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.requested_by)}</td><td>${formatTime(item.created_at)}</td><td>${formatTime(item.updated_at)}</td></tr>`);
  $("#operationsTable").innerHTML = rows.length ? rows.join("") : `<tr><td class="empty" colspan="7">暂无操作任务</td></tr>`;
}

function renderIncidents() {
  const rows = state.incidents.map((item) => `<tr><td>${badge(item.severity)}</td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.message)}</td><td>${formatTime(item.observed_at)}</td></tr>`);
  $("#incidentsTable").innerHTML = rows.length ? rows.join("") : `<tr><td class="empty" colspan="6">当前无告警</td></tr>`;
}

async function loadAll() {
  try {
    const [dashboard, lines, operations, incidents, executors] = await Promise.all([
      api("/api/v1/dashboard"), api("/api/v1/lines"), api("/api/v1/operations?limit=100"), api("/api/v1/incidents?limit=100"), api("/api/v1/executors")
    ]);
    state.dashboard = dashboard; state.lines = lines.lines || []; state.operations = operations.operations || []; state.incidents = incidents.incidents || []; state.executors = executors.executors || [];
    renderMetrics(); renderOverview(); renderLines(); renderOperations(); renderIncidents();
    $("#updatedAt").textContent = `更新于 ${new Date().toLocaleTimeString("zh-CN", {hour:"2-digit", minute:"2-digit", second:"2-digit"})}`;
    setConnected(true); $("#authModal").classList.add("hidden");
  } catch (error) {
    setConnected(false);
    if (error.status === 401) { sessionStorage.removeItem("nbAdminToken"); $("#authModal").classList.remove("hidden"); }
    else toast(error.message);
    throw error;
  }
}

const viewMeta = {
  overview: ["线路总览", "统一查看线路容量、健康与运行状态"],
  lines: ["线路管理", "登记线路并发起验证、升级与回滚"],
  operations: ["操作任务", "跟踪中央控制面下发给线路 agent 的任务"],
  incidents: ["告警中心", "集中查看所有线路的异常与恢复状态"]
};

function switchView(name) {
  $$(".nav-item").forEach((item) => item.classList.toggle("active", item.dataset.view === name));
  $$(".view").forEach((view) => view.classList.toggle("active", view.id === `${name}View`));
  [$("#viewTitle").textContent, $("#viewSubtitle").textContent] = viewMeta[name];
}

function openOperation(lineID, kind = "line.open") {
  const line = state.lines.find((item) => item.id === lineID);
  const form = $("#operationForm");
  form.reset(); form.elements.line_id.value = lineID; form.elements.kind.value = kind; form.elements.requested_by.value = "operator";
  $("#operationTitle").textContent = kindText[kind] || "创建任务";
  $("#operationTarget").textContent = line ? `${line.name} · ${line.id}` : lineID;
  $("#operationError").textContent = ""; $("#operationModal").classList.remove("hidden");
}

$("#authForm").addEventListener("submit", async (event) => {
  event.preventDefault(); state.token = $("#tokenInput").value;
  try { await loadAll(); sessionStorage.setItem("nbAdminToken", state.token); $("#authError").textContent = ""; }
  catch (error) { $("#authError").textContent = error.message; }
});

$("#lineForm").addEventListener("submit", async (event) => {
  event.preventDefault(); const form = event.currentTarget; const values = Object.fromEntries(new FormData(form)); values.capacity_mbps = Number(values.capacity_mbps); values.active_deployment = "";
  try { await api("/api/v1/lines", {method:"POST", body:JSON.stringify(values)}); $("#lineModal").classList.add("hidden"); form.reset(); await loadAll(); toast("线路已保存"); }
  catch (error) { $("#lineError").textContent = error.message; }
});

$("#operationForm").addEventListener("submit", async (event) => {
  event.preventDefault(); const values = Object.fromEntries(new FormData(event.currentTarget));
  const body = {line_id:values.line_id, kind:values.kind, requested_by:values.requested_by, request:{deployment:values.deployment, note:values.note}};
  try { await api("/api/v1/operations", {method:"POST", headers:{"Idempotency-Key":`web-${crypto.randomUUID()}`}, body:JSON.stringify(body)}); $("#operationModal").classList.add("hidden"); await loadAll(); switchView("operations"); toast("任务已进入队列"); }
  catch (error) { $("#operationError").textContent = error.message; }
});

$$('.nav-item').forEach((item) => item.addEventListener("click", () => switchView(item.dataset.view)));
$("#refreshButton").addEventListener("click", () => loadAll().catch(() => {}));
$("#newLineButton").addEventListener("click", () => { $("#lineError").textContent = ""; $("#lineModal").classList.remove("hidden"); });
$$('.close-modal').forEach((item) => item.addEventListener("click", () => $("#lineModal").classList.add("hidden")));
$$('.close-operation').forEach((item) => item.addEventListener("click", () => $("#operationModal").classList.add("hidden")));
$("#lineSearch").addEventListener("input", filterLines); $("#lineStatus").addEventListener("change", filterLines);
$("#operationLine").addEventListener("change", async () => { const value = $("#operationLine").value; const data = await api(`/api/v1/operations?limit=100${value ? `&line_id=${encodeURIComponent(value)}` : ""}`); state.operations = data.operations || []; renderOperations(); });
$("#reloadOperations").addEventListener("click", () => $("#operationLine").dispatchEvent(new Event("change")));

if (state.token) loadAll().catch(() => {}); else $("#authModal").classList.remove("hidden");
setInterval(() => { if (state.token && document.visibilityState === "visible") loadAll().catch(() => {}); }, 30000);
