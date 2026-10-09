import {TrafficCharts} from "./traffic-charts.js";
import {DeviceTopology} from "./device-topology.js";
import {TrafficWorkspace} from "./traffic-workspace.js";
import {lineDeletionAction} from "./line-actions.js";

const state = {
  token: sessionStorage.getItem("nbSessionToken") || "",
  dashboard: null, devices: [], lines: [], governance: [], details: {}, topology: {devices:[],links:[]}, operations: [], incidents: [], executors: [], nodeRelease: null, platformRelease: null, platformUpgradePreview: null, view: "overview", openOperationID: "",
  trafficCharts: null, overviewTopology: null, deviceTopology: null,
  pendingDeviceHostKey: null, selectedOperations: new Set(), mustChangePassword: false, nodeUploadRequest: null
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const escapeHTML = (value = "") => String(value).replace(/[&<>'"]/g, (char) => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;",'"':"&quot;"})[char]);
const formatTime = (value) => value ? new Intl.DateTimeFormat("zh-CN", {month:"2-digit", day:"2-digit", hour:"2-digit", minute:"2-digit", second:"2-digit"}).format(new Date(value)) : "--";
const number = (value, digits = 0) => Number(value || 0).toLocaleString("zh-CN", {maximumFractionDigits:digits});
const formatDurationUS = (value) => { const us=Number(value||0); if(us<1000)return `${number(us)} μs`; if(us<1000000)return `${number(us/1000,2)} ms`; return `${number(us/1000000,2)} s`; };
const statusText = {ok:"健康",healthy:"健康",degraded:"异常",down:"离线",unreachable:"不可达",unhealthy:"异常",unknown:"待采集",ready:"已就绪",provisioning:"开线中",qualification_pending:"开线中",qualification_failed:"维护",offline:"离线",retired:"退役",active:"运行中",draft:"草稿",validating:"验证中",maintenance:"维护",disabled:"停用",deleting:"清理中",queued:"排队中",dispatched:"已下发",running:"执行中",succeeded:"成功",failed:"失败",cancelled:"已取消",rolled_back:"已回滚",open:"未处理",firing:"告警中",resolved:"已恢复",critical:"严重",warning:"警告",info:"信息",pending:"等待",skipped:"跳过"};
const deviceStatusText = {ready:"已启用",provisioning:"初始化",maintenance:"维护",offline:"停用",retired:"退役"};
const environmentText = {production:"生产",test:"测试"};
function environmentBadge(value) { const env=value||"production"; return `<span class="badge ${env === "test" ? "warning" : "active"}">${escapeHTML(environmentText[env]||env)}</span>`; }
function healthState(value) { return value === "ok" ? "healthy" : (value || "unknown"); }
const kindText = {"line.open":"开通线路","line.validate":"验证线路","line.optimize":"验证并调优","line.rollback":"回滚","line.disable":"停用","line.tune":"协议调优","node.release.build":"构建平台候选","platform.upgrade":"平台升级","platform.rollback":"平台回滚"};
const roleText = {entry:"Entry",relay:"Relay",exit:"Exit"};
const stageText = {prepare:"准备",build:"构建",provision:"开线部署",validate:"线路验证",deploy:"部署",rollback:"回滚",stop:"停用",whitelist:"白名单下发","whitelist-fetch":"白名单更新"};
const legacyMessageText = {"preparing operation":"正在准备任务","operation prepared":"任务准备完成","step started":"步骤开始执行","step completed":"步骤执行完成","operation completed":"任务执行完成","dynamic plan exceeds assigned port limits":"线路端口超出 worker 授权范围"};
function operationMessage(value) { return legacyMessageText[value] || value; }

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("Authorization", `Bearer ${state.token}`);
  if (options.body) headers.set("Content-Type", "application/json");
  let response;
  try {
    response = await fetch(path, {...options, headers});
  } catch (cause) {
    const error = new Error("无法连接中央服务，请确认 9091 端口和 NB 统一控制面进程正在运行");
    error.cause = cause;
    error.network = true;
    throw error;
  }
  const body = await response.json().catch(() => ({}));
  if (!response.ok) { const error = new Error(body.error || `请求失败 (${response.status})`); error.status = response.status; throw error; }
  return body;
}

function uploadNodeChunk(path, offset, chunk, total, onProgress) {
  return new Promise((resolve, reject) => {
    const request=new XMLHttpRequest();state.nodeUploadRequest=request;request.open("PUT",path);request.timeout=30000;request.setRequestHeader("Authorization",`Bearer ${state.token}`);request.setRequestHeader("Content-Type","application/octet-stream");request.setRequestHeader("X-Upload-Offset",String(offset));
    request.upload.onprogress=(event)=>{if(event.lengthComputable&&onProgress)onProgress(offset+event.loaded,total,"正在上传源码分块");};
    request.onload=()=>{state.nodeUploadRequest=null;let payload={};try{payload=JSON.parse(request.responseText||"{}");}catch{}if(request.status>=200&&request.status<300)resolve(payload);else{const error=new Error(payload.error||`分块上传失败（HTTP ${request.status}）`);error.status=request.status;error.received=payload.received;reject(error);}};
    request.onerror=()=>{state.nodeUploadRequest=null;const error=new Error("源码分块上传网络失败");error.network=true;reject(error);};request.onabort=()=>{state.nodeUploadRequest=null;const error=new Error("源码包上传已取消");error.aborted=true;reject(error);};request.send(chunk);
    request.ontimeout=()=>{state.nodeUploadRequest=null;const error=new Error("源码分块上传超时，正在重试");error.network=true;reject(error);};
  });
}

async function uploadNodeArchive(data, onProgress) {
  const file=data.get("archive"),requestedBy=String(data.get("requested_by")||"operator");
  if(!(file instanceof File)||!file.size)throw new Error("请选择源码包");
  const uploadKey=`${file.name}:${file.size}:${file.lastModified}`,initialized=await api("/api/v1/platform-releases/uploads",{method:"POST",body:JSON.stringify({filename:file.name,size:file.size,upload_key:uploadKey})});
  let offset=Number(initialized.received||0);const chunkSize=4*1024*1024;onProgress(offset,file.size,"正在恢复上传状态");
  while(offset<file.size){const end=Math.min(file.size,offset+chunkSize),chunk=file.slice(offset,end);let failure;
    for(let attempt=0;attempt<5;attempt++){try{const result=await uploadNodeChunk(`/api/v1/platform-releases/uploads/${encodeURIComponent(initialized.upload_id)}`,offset,chunk,file.size,onProgress);offset=Number(result.received);failure=null;break;}catch(error){failure=error;if(error.aborted)throw error;if(error.status===409&&Number.isFinite(Number(error.received))){offset=Number(error.received);failure=null;break;}await new Promise((resolve)=>setTimeout(resolve,Math.min(8000,1000*2**attempt)));}}
    if(failure&&offset<end)throw failure;onProgress(offset,file.size,"正在上传源码分块");
  }
  onProgress(file.size,file.size,"上传完成，正在校验并创建构建任务");
  return api(`/api/v1/platform-releases/uploads/${encodeURIComponent(initialized.upload_id)}/complete`,{method:"POST",body:JSON.stringify({requested_by:requestedBy})});
}

function badge(value) { return `<span class="badge ${escapeHTML(value)}">${escapeHTML(statusText[value] || value || "未知")}</span>`; }
function toast(message) { const el = $("#toast"); el.textContent = message; el.classList.add("show"); clearTimeout(toast.timer); toast.timer = setTimeout(() => el.classList.remove("show"), 2800); }
async function saveTopologyLayouts(layouts) {
  try { await api("/api/v1/topology/layout",{method:"PUT",body:JSON.stringify({updated_by:"operator",layouts})}); }
  catch(error) { toast(`拓扑布局保存失败：${error.message}`); throw error; }
}
function setConnected(ok) { $("#apiDot").classList.toggle("online", ok); $("#apiStatus").textContent = ok ? "中央服务在线" : "未连接"; }
function metric(label, value, note) { return `<div class="metric"><span class="metric-label">${label}</span><strong class="metric-value">${value}</strong><span class="metric-note">${note}</span></div>`; }

function renderMetrics() {
  const d = state.dashboard || {};
  $("#metrics").innerHTML = [
    metric("设备总数", number(state.devices.length), `${state.devices.filter((x) => x.last_health === "healthy").length} 台在线`),
    metric("线路总数", number(d.lines_total), `${number(d.lines_healthy)} 条健康`),
    metric("异常线路", number(d.lines_degraded), d.lines_degraded ? "需要处理" : "当前无异常"),
    metric("总容量", `${number(d.capacity_mbps)} Mbps`, "已登记线路容量"),
    metric("当前吞吐", `${number(d.throughput_mbps, 2)} Mbps`, `上行 ${number(d.upstream_mbps,2)} / 下行 ${number(d.downstream_mbps,2)} Mbps`),
    metric("执行中任务", number(d.active_operations), `${number(d.open_incidents)} 条未恢复告警`)
  ].join("");
}

function operationAvailability(lineID, kind) {
  if (kind === "line.open" && !(state.details[lineID]?.spec?.nodes || []).length) return {enabled:false, reason:"需要先配置设备和部署规格"};
  const exact = state.executors.flatMap((executor) => (executor.lines || []).filter((line) => line.line_id === lineID).map((line) => ({executor,line})));
  const candidates = exact.length ? exact : state.executors.flatMap((executor) => (executor.lines || []).filter((line) => line.line_id === "*").map((line) => ({executor,line})));
  if (candidates.some(({executor,line}) => executor.online && (line.operations || []).includes(kind))) return {enabled:true, reason:""};
  const reason = candidates.find(({line}) => line.reason)?.line.reason;
  return {enabled:false, reason:reason || (candidates.length ? "执行器离线或未授权此操作" : "等待执行器")};
}

function snapshotForRole(detail, role) {
  const snapshotRole = role === "relay" ? "middle" : role;
  const items = (detail?.snapshots || []).filter((item) => item.role === snapshotRole || item.role === role);
  if (!items.length) return null;
  return {
    health: items.every((item) => healthState(item.health) === "healthy") ? "healthy" : "down",
    throughput_mbps: items.reduce((sum,item) => sum + Number(item.throughput_mbps || 0), 0),
    upstream_mbps: items.reduce((sum,item) => sum + Number(item.upstream_mbps || 0), 0),
    downstream_mbps: items.reduce((sum,item) => sum + Number(item.downstream_mbps || 0), 0),
    sessions: items.reduce((sum,item) => sum + Number(item.sessions || 0), 0),
    queue_age_p95_us: Math.max(...items.map((item) => Number(item.queue_age_p95_us || 0))),
    effective_loss_pct: Math.max(...items.map((item) => Number(item.effective_loss_pct || 0)))
  };
}

function topology(detail, compact = false) {
  const spec = detail?.spec;
  if (!spec?.nodes?.length) return `<div class="topology-empty">该线路尚未配置设备拓扑</div>`;
  const order = {entry:0, relay:1, exit:2};
  const nodes = [...spec.nodes].sort((a,b) => order[a.role] - order[b.role] || a.ordinal - b.ordinal);
  return `<div class="topology-flex ${compact ? "compact" : ""}">${nodes.map((node, index) => {
    const device = node.device || {};
    const snap = snapshotForRole(detail, node.role);
    const health = healthState(snap?.health || device.last_health);
    const port = node.role === "entry" ? spec.socks_port : node.role === "relay" ? spec.relay_port : spec.exit_port;
    const connector = index ? `<div class="topology-link"><span></span><small>${escapeHTML(nodes[index - 1].next_hop_device_id ? "下一跳" : "链路")}</small></div>` : "";
    return `${connector}<button class="topology-node ${escapeHTML(health)}" data-device-detail="${escapeHTML(node.device_id)}"><span class="computer-icon" aria-hidden="true"></span><span class="node-role">${roleText[node.role]}</span><strong>${escapeHTML(device.name || node.device_id)}</strong><span>${escapeHTML(device.host || "--")}:${port}</span><small>${escapeHTML(device.region || "--")} · ${escapeHTML(device.provider || "--")}</small>${snap ? `<em>↑ ${number(snap.upstream_mbps,2)} / ↓ ${number(snap.downstream_mbps,2)} Mbps · ${number(snap.sessions)} 会话</em>` : `<em>等待运行快照</em>`}</button>`;
  }).join("")}</div>`;
}

function renderOverviewTopology() {
  const select = $("#topologyLine");
  const previous = select.value;
  select.innerHTML = `<option value="">全部线路</option>${state.lines.map((line) => `<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`).join("")}`;
  if (state.lines.some((line) => line.id === previous) || previous === "") select.value = previous;
  if (!state.overviewTopology) state.overviewTopology = new DeviceTopology($("#overviewTopology"), {onDevice:showDeviceDetail,onLine:showLineDetail,onSaveLayout:saveTopologyLayouts});
  state.overviewTopology.render(state.topology,{lineID:select.value});
}

function renderDeviceTopology() {
  const lineSelect=$("#deviceTopologyLine"),regionSelect=$("#deviceTopologyRegion"),previousLine=lineSelect.value,previousRegion=regionSelect.value;
  lineSelect.innerHTML=`<option value="">全部线路</option>${state.lines.map((line)=>`<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`).join("")}`;
  if(state.lines.some((line)=>line.id===previousLine))lineSelect.value=previousLine;
  const regions=[...new Set(state.devices.map((device)=>device.region).filter(Boolean))].sort();regionSelect.innerHTML=`<option value="">全部区域</option>${regions.map((region)=>`<option value="${escapeHTML(region)}">${escapeHTML(region)}</option>`).join("")}`;
  if(regions.includes(previousRegion))regionSelect.value=previousRegion;
  if(!state.deviceTopology)state.deviceTopology=new DeviceTopology($("#deviceTopology"),{onDevice:showDeviceDetail,onLine:showLineDetail,onSaveLayout:saveTopologyLayouts});
  state.deviceTopology.render(state.topology,{lineID:lineSelect.value,region:regionSelect.value,role:$("#deviceTopologyRole").value,health:$("#deviceTopologyHealth").value});
}

function overviewRow(line) {
  const d = state.details[line.id];
  const boundary = (d?.snapshots || []).filter((item) => item.role === "entry");
  const summary = boundary.reduce((sum, item) => ({upstream:sum.upstream + Number(item.upstream_mbps || 0), downstream:sum.downstream + Number(item.downstream_mbps || 0), sessions:sum.sessions + Number(item.sessions || 0), queue:Math.max(sum.queue, Number(item.queue_age_p95_us || 0)), loss:Math.max(sum.loss, Number(item.effective_loss_pct || 0))}), {upstream:0,downstream:0,sessions:0,queue:0,loss:0});
  const health = d?.snapshots?.some((x) => healthState(x.health) !== "healthy") ? "degraded" : d?.snapshots?.length ? "healthy" : "unknown";
  return `<tr class="clickable" data-line-detail="${escapeHTML(line.id)}"><td><div class="line-name">${escapeHTML(line.name)}</div><div class="line-id">${escapeHTML(line.id)}</div></td><td>${badge(health)}</td><td><strong>↑ ${number(summary.upstream,2)} / ↓ ${number(summary.downstream,2)}</strong> Mbps</td><td>${number(summary.sessions)}</td><td>${formatDurationUS(summary.queue)}</td><td>${number(summary.loss,3)}%</td><td><span class="mono">${escapeHTML((line.active_deployment || "未部署").slice(0,20))}</span></td></tr>`;
}

function renderOverview() {
  $("#overviewLines").innerHTML = state.lines.length ? state.lines.map(overviewRow).join("") : `<tr><td class="empty" colspan="7">尚未登记线路</td></tr>`;
  $("#recentOperations").innerHTML = state.operations.slice(0,8).map((item) => `<button class="activity-item" data-operation-detail="${escapeHTML(item.id)}"><span>${escapeHTML(kindText[item.kind] || item.kind)}</span><strong>${escapeHTML(item.line_id)}</strong><small>${badge(item.status)} ${formatTime(item.updated_at)}</small></button>`).join("") || `<div class="topology-empty">暂无任务</div>`;
  renderOverviewTopology(); bindDetails();
}

function deviceRow(item) {
  const health = item.last_health || "unknown";
  const environment = item.environment || "production";
  return `<tr data-device-search="${escapeHTML(`${item.id} ${item.name} ${item.host} ${item.region} ${item.provider}`.toLowerCase())}" data-device-status="${escapeHTML(item.status)}" data-device-environment="${escapeHTML(environment)}"><td><div class="device-name"><span class="health-dot ${escapeHTML(health)}"></span><div><strong>${escapeHTML(item.name)}</strong><small>${escapeHTML(item.id)}</small></div></div></td><td>${environmentBadge(environment)}</td><td><span class="badge ${escapeHTML(item.status)}">${escapeHTML(deviceStatusText[item.status]||item.status||"未知")}</span></td><td><span class="mono">${escapeHTML(item.ssh_user)}@${escapeHTML(item.host)}:${item.ssh_port}</span></td><td>${escapeHTML(item.region || "--")}<div class="subtext">${escapeHTML(item.provider || "--")}</div></td><td>${escapeHTML(item.os || "--")}<div class="subtext">${escapeHTML(item.arch || "--")}</div></td><td>${badge(health)}<div class="subtext">最近成功 ${formatTime(item.last_seen_at)}</div></td><td><div class="actions"><button class="action-button" data-probe-device="${escapeHTML(item.id)}">探测</button><button class="action-button" data-edit-device="${escapeHTML(item.id)}">编辑</button><button class="action-button danger-action" data-delete-device="${escapeHTML(item.id)}">删除</button></div></td></tr>`;
}

function renderDevices() {
  $("#devicesTable").innerHTML = state.devices.length ? state.devices.map(deviceRow).join("") : `<tr><td class="empty" colspan="8">尚未录入设备</td></tr>`;
  filterDevices();
  $$('[data-probe-device]').forEach((b) => b.addEventListener("click", () => probeDevice(b.dataset.probeDevice)));
  $$('[data-edit-device]').forEach((b) => b.addEventListener("click", () => editDevice(b.dataset.editDevice)));
  $$('[data-delete-device]').forEach((b) => b.addEventListener("click", () => deleteDevice(b.dataset.deleteDevice)));
  const options = `<option value="">选择设备</option>${state.devices.filter((d) => d.status !== "retired").map((d) => `<option value="${escapeHTML(d.id)}">${escapeHTML(d.name)} · ${escapeHTML(d.host)}</option>`).join("")}`;
  const signature = state.devices.filter((d) => d.status !== "retired").map((d) => `${d.id}:${d.name}:${d.host}`).join("|");
  const lineEditorOpen = !$("#lineModal").classList.contains("hidden");
  for (const name of ["entry_device","relay_device","exit_device"]) {
    const select = $(`#lineForm [name="${name}"]`);
    if (lineEditorOpen || select.dataset.optionsSignature === signature) continue;
    const selected = select.value;
    select.innerHTML = options;
    select.dataset.optionsSignature = signature;
    if ([...select.options].some((option) => option.value === selected)) select.value = selected;
  }
}

function filterDevices() { const q = $("#deviceSearch").value.trim().toLowerCase(), environment = $("#deviceEnvironment").value, status = $("#deviceStatus").value; $$("#devicesTable tr[data-device-search]").forEach((row) => row.hidden = (q && !row.dataset.deviceSearch.includes(q)) || (environment && row.dataset.deviceEnvironment !== environment) || (status && row.dataset.deviceStatus !== status)); }
async function probeDevice(id) { try { toast("正在探测 SSH 管理端口"); await api(`/api/v1/devices/${encodeURIComponent(id)}/probe`, {method:"POST"}); await loadAll(); toast("设备探测完成"); } catch (error) { toast(error.message); } }
function clearDeviceHostKeyConfirmation() { state.pendingDeviceHostKey=null; $("#deviceHostKeyConfirmation").classList.add("hidden"); $("#deviceHostKeyEndpoint").textContent=""; $("#deviceHostKeyType").textContent=""; $("#deviceHostKeyFingerprint").textContent=""; }
function editDevice(id) { const item = state.devices.find((x) => x.id === id); if (!item) return; const form = $("#deviceForm"); form.reset(); clearDeviceHostKeyConfirmation(); for (const [key,value] of Object.entries(item)) if (form.elements[key] && key !== "labels" && key !== "password") form.elements[key].value = value ?? ""; form.dataset.originalHost=item.host; form.dataset.originalPort=String(item.ssh_port); form.dataset.hostKeyStatus=item.ssh_host_key_status||"pending"; form.elements.id.readOnly = true; form.elements.password.required = false; $("#devicePasswordLabel").textContent = "更换 SSH 密码"; $("#devicePasswordHint").textContent = "留空表示保持当前密码"; $("#deviceError").textContent = ""; $("#scanDeviceHostKey").textContent="保存设备"; $("#deviceModal").classList.remove("hidden"); }
async function deleteDevice(id) { if (!confirm(`确认删除设备 ${id}？`)) return; try { await api(`/api/v1/devices/${encodeURIComponent(id)}`, {method:"DELETE"}); await loadAll(); toast("设备已删除"); } catch (error) { toast(error.message); } }

const governanceText={maintenance_required:"设备环境需整改"};
function governanceSection(detail){const findings=detail?.governance||[];if(!findings.length)return "";return `<div class="detail-section governance-findings"><h3>生产治理</h3>${findings.map((item)=>`<div class="governance-finding"><div><strong>${escapeHTML(governanceText[item.code]||item.code)}</strong><span class="mono">${escapeHTML(item.resource_id||item.line_id)}</span></div><p>${escapeHTML(item.message)}</p><small>${escapeHTML(item.required_action)}</small></div>`).join("")}</div>`;}

function lineRow(line) {
  const detail = state.details[line.id];
  const nodes = detail?.spec?.nodes || [];
  const route = nodes.map((n) => n.device?.name || n.device_id).join(" → ") || `${line.entry_region} → ${line.exit_region}`;
  const definitions = line.status === "deleting" ? [] : line.status === "draft" || line.status === "disabled" ? [["line.open","开线","primary-action"]] : [["line.optimize","验证并调优","primary-action"],["line.rollback","回滚",""] ,["line.disable","停用","danger-action"]];
  const actions = definitions.map(([kind,label,style]) => { const a = operationAvailability(line.id,kind); return `<button class="action-button ${style}" ${a.enabled ? `data-action="${kind}" data-line="${escapeHTML(line.id)}"` : `disabled title="${escapeHTML(a.reason)}"`}>${label}</button>`; }).join("");
  const hasActiveOperation=state.operations.some((item)=>item.line_id===line.id&&["queued","dispatched","running"].includes(item.status));
  const hasFailedDisable=state.operations.some((item)=>item.line_id===line.id&&item.kind==="line.disable"&&item.status==="failed");
  const hasNodeProblem=nodes.some((node)=>["unreachable","down","degraded","unhealthy","offline"].includes(node.device?.last_health));
  const deletion=lineDeletionAction(line.status,nodes.length>0,hasActiveOperation,hasNodeProblem||hasFailedDisable);
  const deleteAction=deletion.mode==="normal"?`<button class="action-button danger-action" data-delete-line="${escapeHTML(line.id)}">${deletion.label}</button>`:
    deletion.mode==="force"?`<button class="action-button danger-action" data-force-delete-line="${escapeHTML(line.id)}">${deletion.label}</button>`:
    `<button class="action-button danger-action" disabled title="${escapeHTML(deletion.reason)}">${deletion.label}</button>`;
  const upstream=detail?.spec?.upstream_mbps||detail?.spec?.bandwidth_mbps||line.capacity_mbps,downstream=detail?.spec?.downstream_mbps||detail?.spec?.bandwidth_mbps||line.capacity_mbps;
  const environment = detail?.spec?.environment || line.environment || "production";
  const environmentAction = line.status === "deleting" || line.status === "archived" ? "" : `<button class="action-button" data-edit-line-environment="${escapeHTML(line.id)}">改环境</button>`;
  return `<tr data-line-search="${escapeHTML(`${line.id} ${line.name} ${route}`.toLowerCase())}" data-line-status="${escapeHTML(line.status)}" data-line-environment="${escapeHTML(environment)}"><td><button class="text-button" data-line-detail="${escapeHTML(line.id)}"><strong>${escapeHTML(line.name)}</strong><small>${escapeHTML(line.id)}</small></button></td><td>${environmentBadge(environment)}</td><td>${badge(line.status)}</td><td><div class="route-summary">${escapeHTML(route)}</div></td><td>↑ ${number(upstream)} / ↓ ${number(downstream)} Mbps</td><td>${detail?.spec?.socks_port || "--"}</td><td><span class="mono">${escapeHTML(line.profile || "--")}</span></td><td><div class="actions">${environmentAction}${actions}${deleteAction}</div></td></tr>`;
}

function openLineEnvironmentEdit(id) {
  const line = state.lines.find((item) => item.id === id), detail = state.details[id], form = $("#lineEnvironmentForm");
  if (!line || !form) return;
  form.reset(); form.elements.line_id.value = id; form.elements.environment.value = detail?.spec?.environment || line.environment || "production";
  $("#lineEnvironmentError").textContent = ""; $("#lineEnvironmentModal").classList.remove("hidden");
}

async function deleteLine(id) {
  const detail = state.details[id], incomplete = !(detail?.spec?.nodes || []).length;
  if (!confirm(`确认删除线路 ${id}？系统将先清理三端实例和端口，确认完成后再删除线路记录。`)) return;
  try {
    const result=await api(`/api/v1/lines/${encodeURIComponent(id)}`, {method:"DELETE", body:JSON.stringify({requested_by:"operator", reason:incomplete?"remove incomplete discovered line":"operator requested line deletion"})});
    await loadAll(); toast(result?.kind==="line.disable"?"节点清理任务已创建，完成后自动删除线路":"线路已删除");
  } catch (error) { toast(error.message); }
}

function openForceDeleteLine(id) {
  const form=$("#forceDeleteLineForm"),line=state.lines.find((item)=>item.id===id);
  form.reset();form.elements.line_id.value=id;
  $("#forceDeleteLineTarget").textContent=line?`${line.name} · ${id}`:id;
  $("#forceDeleteLineHint").textContent=`请输入完整线路 ID：${id}`;
  $("#forceDeleteLineError").textContent="";
  $("#forceDeleteLineModal").classList.remove("hidden");
}

function renderLines() {
  $("#linesTable").innerHTML = state.lines.length ? state.lines.map(lineRow).join("") : `<tr><td class="empty" colspan="8">尚未登记线路</td></tr>`;
  filterLines();
  $$('[data-action]').forEach((button) => button.addEventListener("click", () => openOperation(button.dataset.line, button.dataset.action)));
  $$('[data-delete-line]').forEach((button) => button.addEventListener("click", () => deleteLine(button.dataset.deleteLine)));
  $$('[data-force-delete-line]').forEach((button) => button.addEventListener("click", () => openForceDeleteLine(button.dataset.forceDeleteLine)));
  $$('[data-edit-line-environment]').forEach((button) => button.addEventListener("click", () => openLineEnvironmentEdit(button.dataset.editLineEnvironment)));
  $("#operationLine").innerHTML = `<option value="">全部线路</option>${state.lines.map((line) => `<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`).join("")}`;
  bindDetails();
}
function filterLines() { const q = $("#lineSearch").value.trim().toLowerCase(), environment = $("#lineEnvironment").value, status = $("#lineStatus").value; $$("#linesTable tr[data-line-search]").forEach((row) => row.hidden = (q && !row.dataset.lineSearch.includes(q)) || (environment && row.dataset.lineEnvironment !== environment) || (status && row.dataset.lineStatus !== status)); }

function renderOperations() {
  const available=new Set(state.operations.map((item)=>item.id));for(const id of state.selectedOperations)if(!available.has(id))state.selectedOperations.delete(id);
  $("#operationsTable").innerHTML = state.operations.length ? state.operations.map((item) => {const cleanable=["succeeded","failed","cancelled","rolled_back"].includes(item.status),selected=state.selectedOperations.has(item.id),rollback=item.kind==="platform.upgrade"&&item.status==="succeeded"?`<button class="action-button" data-platform-rollback="${escapeHTML(item.id)}">回滚</button>`:"";return `<tr class="clickable" data-operation-detail="${escapeHTML(item.id)}"><td class="selection-cell"><input class="selection-checkbox" type="checkbox" data-operation-select="${escapeHTML(item.id)}" ${selected?"checked":""} ${cleanable?"":"disabled"} aria-label="选择任务 ${escapeHTML(item.id)}"></td><td><span class="mono">${escapeHTML(item.id)}</span></td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(kindText[item.kind] || item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.requested_by)}</td><td>${formatTime(item.created_at)}</td><td>${formatTime(item.updated_at)}</td><td>${rollback}<button class="action-button danger-action" data-delete-operation="${escapeHTML(item.id)}" ${cleanable?"":"disabled title=\"执行中的任务不能清理\""}>清理</button></td></tr>`;}).join("") : `<tr><td class="empty" colspan="9">暂无操作任务</td></tr>`;
  syncOperationSelection();
  $$('[data-operation-select]').forEach((box)=>box.addEventListener("click",(event)=>event.stopPropagation()));
  $$('[data-operation-select]').forEach((box)=>box.addEventListener("change",()=>{box.checked?state.selectedOperations.add(box.dataset.operationSelect):state.selectedOperations.delete(box.dataset.operationSelect);syncOperationSelection();}));
  $$('[data-delete-operation]').forEach((button)=>button.addEventListener("click",(event)=>{event.stopPropagation();cleanupOperations([button.dataset.deleteOperation]);}));
  $$('[data-platform-rollback]').forEach((button)=>button.addEventListener("click",async(event)=>{event.stopPropagation();const id=button.dataset.platformRollback;if(!confirm(`确认回滚平台升级 ${id}？`))return;try{await api(`/api/v1/platform-upgrades/${encodeURIComponent(id)}/rollback`,{method:"POST",headers:{"Idempotency-Key":`rollback-${crypto.randomUUID()}`},body:JSON.stringify({requested_by:"operator",confirmation:id})});await loadAll();toast("平台回滚任务已排队");}catch(error){toast(`平台回滚失败：${error.message}`);}}));
  bindDetails();
}
function syncOperationSelection(){const boxes=$$('[data-operation-select]:not(:disabled)'),selected=boxes.filter((box)=>state.selectedOperations.has(box.dataset.operationSelect));const all=$("#operationSelectAll");all.checked=boxes.length>0&&selected.length===boxes.length;all.indeterminate=selected.length>0&&selected.length<boxes.length;$("#deleteSelectedOperations").disabled=state.selectedOperations.size===0;}
async function cleanupOperations(ids){const unique=[...new Set(ids)].filter(Boolean);if(!unique.length)return;if(!confirm(`确认从任务列表清理 ${unique.length} 条已结束任务？成功结果、执行证据和原始监控事件会保留。`))return;try{const options=unique.length===1?{method:"DELETE"}:{method:"DELETE",body:JSON.stringify({ids:unique})};await api(unique.length===1?`/api/v1/operations/${encodeURIComponent(unique[0])}`:"/api/v1/operations",options);for(const id of unique)state.selectedOperations.delete(id);await loadAll();toast(`已从列表清理 ${unique.length} 条任务`);}catch(error){toast(`任务清理失败：${error.message}`);}}
function renderIncidents() { $("#incidentsTable").innerHTML = state.incidents.length ? state.incidents.map((item) => `<tr><td>${badge(item.severity)}</td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.message)}</td><td>${formatTime(item.observed_at)}</td></tr>`).join("") : `<tr><td class="empty" colspan="6">当前无告警</td></tr>`; }

function renderUpgrades(){
  const release=state.platformRelease,node=release?.nb_node||{},web=release?.nb_web||{},worker=release?.nb_web_worker||{},upgrader=release?.nb_upgrader||{};
  const cells=[
    ["脚本快照",release?.scripts?.digest||"--",release?"候选已构建":"尚无候选"],
    ["Web",web.sha256||"--",web.size?`${number(web.size/1048576,1)} MiB`:"等待构建"],
    ["Worker",worker.sha256||"--",worker.size?`${number(worker.size/1048576,1)} MiB`:"等待构建"],
    ["Upgrader",upgrader.sha256||"--",upgrader.size?`${number(upgrader.size/1048576,1)} MiB`:"等待构建"],
    ["Node",node.version?`${node.version.product} (${node.version.semantic})`:"--",node.sha256||"等待构建"],
  ];
  $("#platformVersionMatrix").innerHTML=cells.map(([name,value,note])=>`<div class="version-cell"><span>${escapeHTML(name)}</span><strong>${escapeHTML(value.length>28?value.slice(0,16):value)}</strong><small>${escapeHTML(note)}</small></div>`).join("");
  $("#platformReleaseSelect").innerHTML=release?`<option value="${escapeHTML(release.release_id)}">${escapeHTML(release.release_id)}</option>`:`<option value="">尚无可用候选</option>`;
  const active=state.lines.filter((line)=>line.status==="active"||line.status==="maintenance");
  $("#platformUpgradeLine").innerHTML=active.length?active.map((line)=>`<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)} · ${escapeHTML(line.id)}</option>`).join(""):`<option value="">没有运行中线路</option>`;
  $("#previewPlatformUpgrade").disabled=!release||!active.length;
  if(!state.platformUpgradePreview)$("#startPlatformUpgrade").disabled=true;
}

function showPlatformPreview(preview){
  state.platformUpgradePreview=preview;
  $("#platformUpgradePreview").innerHTML=`<strong>${escapeHTML(preview.unit_id)} · ${number(preview.devices?.length)} 个设备角色</strong><small>${escapeHTML(preview.warning)}</small><ul>${(preview.affected_lines||[]).map((line)=>`<li>${escapeHTML(line)}</li>`).join("")}</ul>`;
  $("#startPlatformUpgrade").disabled=!$("#confirmPlatformInterrupt").checked;
}

async function loadAll() {
  try {
    const [dashboard,devices,lines,governance,topologyData,operations,incidents,executors,nodeRelease,platformRelease] = await Promise.all([api("/api/v1/dashboard"),api("/api/v1/devices"),api("/api/v1/lines"),api("/api/v1/governance/production-lines"),api("/api/v1/topology"),api("/api/v1/operations?limit=100"),api("/api/v1/incidents?limit=100"),api("/api/v1/executors"),api("/api/v1/node-releases/status"),api("/api/v1/platform-releases/status")]);
    state.dashboard=dashboard; state.devices=devices.devices||[]; state.lines=lines.lines||[]; state.governance=governance.findings||[]; state.topology=topologyData||{devices:[],links:[]}; state.operations=operations.operations||[]; state.incidents=incidents.incidents||[]; state.executors=executors.executors||[]; state.nodeRelease=nodeRelease.available?nodeRelease.release:null;state.platformRelease=platformRelease.available?platformRelease.release:null;
    const details = await Promise.all(state.lines.map((line) => api(`/api/v1/lines/${encodeURIComponent(line.id)}/detail`).catch(() => ({line}))));
    state.details = Object.fromEntries(details.map((detail) => {detail.governance=state.governance.filter((item)=>item.line_id===detail.line.id);return [detail.line.id,detail];}));
    renderMetrics(); renderDevices(); renderLines(); renderOperations(); renderIncidents(); renderOverview(); renderDeviceTopology(); renderUpgrades(); if($("#nodeReleaseStatus"))$("#nodeReleaseStatus").textContent=state.platformRelease?`平台候选 ${state.platformRelease.release_id}`:"尚无平台候选";
    $("#updatedAt").textContent = `更新于 ${new Date().toLocaleTimeString("zh-CN", {hour:"2-digit",minute:"2-digit",second:"2-digit"})}`; setConnected(true); $("#authModal").classList.add("hidden");
  } catch (error) { setConnected(false); if (error.status === 401) { state.token=""; sessionStorage.removeItem("nbSessionToken"); $("#authModal").classList.remove("hidden"); } else toast(error.message); throw error; }
}

const viewMeta = {overview:["运营总览","线路拓扑、节点健康与执行任务"],devices:["设备管理","服务器库存、管理端口和健康状态"],lines:["线路管理","设备角色、上下行能力、端口与部署状态"],operations:["操作任务","构建、上传、分发、启动和验证进度"],upgrades:["版本升级","脚本、控制面和共享 Node 的统一事务"],incidents:["告警中心","线路和设备异常汇总"]};
function switchView(name) { state.view=name; $$(".nav-item").forEach((item) => item.classList.toggle("active",item.dataset.view===name)); $$(".view").forEach((view) => view.classList.toggle("active",view.id===`${name}View`)); [$("#viewTitle").textContent,$("#viewSubtitle").textContent]=viewMeta[name]; const action=$("#contextAction"); action.hidden=!['devices','lines','overview'].includes(name); action.textContent=name==='devices'?"录入设备":"新增线路"; }

function upgradeAffectedLines(lineID) { const nodes=state.details[lineID]?.spec?.nodes||[],keys=new Set(nodes.map((node)=>`${node.device_id}:${node.role}`));return state.lines.filter((line)=>(state.details[line.id]?.spec?.nodes||[]).some((node)=>keys.has(`${node.device_id}:${node.role}`))).map((line)=>line.id).sort(); }
function openOperation(lineID,kind) { const line=state.lines.find((x)=>x.id===lineID),form=$("#operationForm"); form.reset(); form.elements.line_id.value=lineID; form.elements.kind.value=kind; form.elements.requested_by.value="operator"; $("#operationTitle").textContent=kindText[kind]||"创建任务"; const affected=kind==="line.upgrade"?upgradeAffectedLines(lineID):[],pending=kind==="line.open"&&state.nodeRelease?` · 节点升级待处理: ${state.nodeRelease.release_id}`:""; $("#operationTarget").textContent=(line?`${line.name} · ${line.id}`:lineID)+(affected.length?` · 影响线路: ${affected.join(", ")}`:"")+pending; $("#operationError").textContent=""; $("#operationModal").classList.remove("hidden"); }
function bindDetails() { $$('[data-line-detail]').forEach((b) => b.onclick=()=>showLineDetail(b.dataset.lineDetail)); $$('[data-operation-detail]').forEach((b) => b.onclick=()=>showOperationDetail(b.dataset.operationDetail)); }
function showDeviceDetail(id) { const item=state.devices.find((x)=>x.id===id); if(!item)return; state.openOperationID=""; $("#detailTitle").textContent=item.name; $("#detailSubtitle").textContent=item.id; $("#detailBody").innerHTML=`<div class="detail-grid"><dl><dt>环境</dt><dd>${environmentBadge(item.environment)}</dd><dt>SSH</dt><dd class="mono">${escapeHTML(item.ssh_user)}@${escapeHTML(item.host)}:${item.ssh_port}</dd><dt>内网地址</dt><dd>${escapeHTML(item.private_ip||"--")}</dd><dt>区域 / 运营商</dt><dd>${escapeHTML(item.region||"--")} / ${escapeHTML(item.provider||"--")}</dd></dl><dl><dt>管理状态</dt><dd><span class="badge ${escapeHTML(item.status)}">${escapeHTML(deviceStatusText[item.status]||item.status||"未知")}</span></dd><dt>连通状态</dt><dd>${badge(item.last_health||"unknown")}</dd><dt>系统</dt><dd>${escapeHTML(item.os||"--")} · ${escapeHTML(item.arch||"--")}</dd><dt>最近成功连通</dt><dd>${formatTime(item.last_seen_at)}</dd></dl></div>`; $("#detailModal").classList.remove("hidden"); }
function showLineDetail(id) { const detail=state.details[id]; if(!detail)return; if(state.trafficCharts){state.trafficCharts.destroy();state.trafficCharts=null;} state.openOperationID=""; const spec=detail.spec||{},upstream=spec.upstream_mbps||spec.bandwidth_mbps||detail.line.capacity_mbps,downstream=spec.downstream_mbps||spec.bandwidth_mbps||detail.line.capacity_mbps,dns=(spec.dns_servers||["1.1.1.1","8.8.8.8"]).join(", "); $("#detailTitle").textContent=detail.line.name; $("#detailSubtitle").textContent=`${environmentText[spec.environment||"production"]||"生产"} · ${detail.line.id} · ${statusText[detail.line.status]||detail.line.status}`; $("#detailBody").innerHTML=`<div class="detail-section"><h3>设备拓扑</h3><div class="topology-canvas">${topology(detail)}</div></div><div class="detail-grid"><dl><dt>环境</dt><dd>${environmentBadge(spec.environment)}</dd><dt>资源组 / 实例</dt><dd>${escapeHTML(spec.resource_group||"--")} / ${escapeHTML(spec.instance_id||"--")}</dd><dt>平均速率</dt><dd>上行 ${number(upstream)} / 下行 ${number(downstream)} Mbps</dd><dt>出口 IP</dt><dd>${escapeHTML(spec.exit_bind_ip||"默认路由")}</dd><dt>出口 DNS</dt><dd>${escapeHTML(dns)}</dd><dt>端口规划</dt><dd>SOCKS ${spec.socks_port||"--"} · Relay ${spec.relay_port||"--"} · Exit ${spec.exit_port||"--"}</dd></dl><dl><dt>UDP 池</dt><dd>${spec.udp_port_min||"--"} - ${spec.udp_port_max||"--"}</dd><dt>构建 / 跳板</dt><dd>${escapeHTML(spec.build_mode||"--")} / ${escapeHTML(spec.jump_policy||"--")}</dd><dt>Deployment</dt><dd class="mono">${escapeHTML(detail.line.active_deployment||"--")}</dd></dl></div>${governanceSection(detail)}${clientConfigSection(detail.client_operation)}<div class="detail-section traffic-section"><h3>实时与历史流量</h3><div id="lineTrafficCharts" class="traffic-charts"></div></div><div class="detail-section"><h3>节点快照</h3>${snapshotTable(detail.snapshots||[])}</div>`; $("#detailModal").classList.remove("hidden"); $$('[data-device-detail]').forEach((b)=>b.onclick=()=>showDeviceDetail(b.dataset.deviceDetail)); hydrateClientConfig($("#detailBody .client-delivery")); requestAnimationFrame(()=>{state.trafficCharts=new TrafficCharts($("#lineTrafficCharts"),{lineID:id,token:state.token});state.trafficCharts.mount();}); }
function snapshotTable(items) { if(!items.length)return `<div class="topology-empty">等待 worker 采集运行快照</div>`; return `<div class="table-wrap"><table class="detail-table"><thead><tr><th>节点</th><th>角色</th><th>健康</th><th>上行</th><th>下行</th><th>会话</th><th>队列最大等待</th><th>有效丢包</th></tr></thead><tbody>${items.map((x)=>{const health=healthState(x.health);return `<tr><td><span class="snapshot-node"><span class="computer-icon small ${escapeHTML(health)}" aria-hidden="true"></span>${escapeHTML(x.node_id)}</span></td><td>${escapeHTML(x.role)}</td><td>${badge(health)}</td><td>${number(x.upstream_mbps,2)} Mbps</td><td>${number(x.downstream_mbps,2)} Mbps</td><td>${number(x.sessions)}</td><td>${formatDurationUS(x.queue_age_p95_us)}</td><td>${number(x.effective_loss_pct,3)}%</td></tr>`;}).join("")}</tbody></table></div>`; }
function clientConfigSection(operation) {
  const clientURL=operation?.result?.client_url;
  if(!clientURL)return "";
  return `<div class="detail-section client-delivery" data-client-operation="${escapeHTML(operation.id)}"><div class="client-qr"><img alt="客户端配置二维码" width="196" height="196" hidden><span>二维码生成中...</span></div><div class="client-access"><h3>客户端配置</h3><p>支持 SOCKS5 URL 的客户端可直接扫码或导入。</p><code>${escapeHTML(clientURL)}</code><button class="secondary" type="button" data-copy-client-url>复制 URL</button></div></div>`;
}
async function hydrateClientConfig(container) {
  if(!container)return;
  const operationID=container.dataset.clientOperation,img=container.querySelector("img"),status=container.querySelector(".client-qr span");
  container.querySelector("[data-copy-client-url]").onclick=async()=>{const value=container.querySelector("code").textContent;try{await navigator.clipboard.writeText(value);toast("客户端 URL 已复制");}catch{toast("复制失败，请手动选择 URL");}};
  try{const result=await api(`/api/v1/operations/${encodeURIComponent(operationID)}/client-qr`);img.src=`data:${result.media_type};base64,${result.data}`;img.hidden=false;status.remove();}
  catch(error){status.textContent=`二维码生成失败：${error.message}`;}
}
function operationTimeline(operation, events) {
  if (events.length) return events.map((event)=>`<div class="timeline-item ${escapeHTML(event.status)}"><span>${event.sequence}</span><div><strong>${escapeHTML(stageText[event.stage]||event.stage)}</strong><small>${escapeHTML(operationMessage(event.message)||statusText[event.status]||event.status)} · ${formatTime(event.created_at)}</small></div></div>`).join("");
  if (operation.status === "failed") return `<div class="timeline-item failed"><span>!</span><div><strong>准备</strong><small>${escapeHTML(operationMessage(operation.result?.message) || "任务在执行准备阶段失败")}</small></div></div>`;
  return `<div class="topology-empty">任务等待执行器领取</div>`;
}
function tuneResultSection(operation) {
  if(operation.kind!=="line.tune"&&operation.kind!=="line.optimize")return "";
  const result=operation.result||{},profile=result.transport_profile||operation.request?.transport_profile;
  const rollout=result.transport_rollout||{},generation=result.transport_generation||profile?.generation;
  if(!profile&&!generation)return `<div class="detail-section"><h3>调优结果</h3><div class="topology-empty">任务尚未生成协议参数</div></div>`;
  const segmentNames={entry_middle:"Entry → Middle",middle_exit:"Middle → Exit"},endpointNames={source:"发送端",target:"接收端"};
  const rows=[];
  for(const [segmentID,segment] of Object.entries(profile?.segments||{}))for(const endpoint of ["source","target"]){
    const link=segment?.[endpoint];if(!link)continue;
    const cwin=Number(link.cwin_max_bytes||0),fec=link.fec_active?"已启用":link.fec_observe?"仅观察":"关闭";
    rows.push(`<tr><td><strong>${escapeHTML(segmentNames[segmentID]||segmentID)}</strong><small>${endpointNames[endpoint]}</small></td><td>${escapeHTML(link.cc||"--")}</td><td>${number(link.target_mbps,2)} Mbps</td><td>${cwin?`${number(cwin/1024)} KiB`:"自适应"}</td><td>${number(link.mtu_max)} B</td><td>${number(link.reorder_gap)} 包 / ${formatDurationUS(link.reorder_delay_us)}</td><td>${escapeHTML(fec)}</td><td>${escapeHTML(link.confidence||"--")}</td></tr>`);
  }
  const roleNames={entry:"Entry",middle:"Middle",exit:"Exit"};
  const roleRows=Object.entries(rollout.roles||{}).map(([role,item])=>`<tr><td>${roleNames[role]||escapeHTML(role)}</td><td>${item.prepared?"完成":"--"}</td><td>${item.committed?"完成":"--"}</td><td>${item.readback?"一致":"--"}</td><td class="mono">${escapeHTML(item.fingerprint||"--")}</td></tr>`).join("");
  const status=rollout.status||(operation.status==="succeeded"?"committed":operation.status),prepare=(rollout.prepare_order||["entry","middle","exit"]).map(x=>roleNames[x]||x).join(" → "),commit=(rollout.commit_order||["exit","middle","entry"]).map(x=>roleNames[x]||x).join(" → ");
  return `<div class="detail-section tune-result"><h3>调优结果</h3><div class="detail-grid"><dl><dt>Generation</dt><dd><strong>${number(generation)}</strong></dd><dt>Profile Schema</dt><dd>${number(profile?.schema_version)}</dd><dt>事务状态</dt><dd>${escapeHTML(status)}</dd></dl><dl><dt>Prepare 顺序</dt><dd>${escapeHTML(prepare)}</dd><dt>Commit 顺序</dt><dd>${escapeHTML(commit)}</dd><dt>线路 Profile</dt><dd class="mono">${escapeHTML(result.profile||operation.line_id||"--")}</dd></dl></div>${rows.length?`<div class="table-wrap"><table class="detail-table tune-table"><thead><tr><th>链路端点</th><th>拥塞控制</th><th>目标速率</th><th>CWin</th><th>MTU</th><th>重排容忍</th><th>FEC</th><th>证据</th></tr></thead><tbody>${rows.join("")}</tbody></table></div>`:""}${roleRows?`<div class="table-wrap"><table class="detail-table rollout-table"><thead><tr><th>角色</th><th>Prepare</th><th>Commit</th><th>读回</th><th>Fingerprint</th></tr></thead><tbody>${roleRows}</tbody></table></div>`:`<p class="subtext">历史任务未保存逐角色事务摘要；generation 来自任务结果。</p>`}</div>`;
}
function failureResultSection(operation){
  const result=operation.result||{},failure=result.failure;
  if(operation.status!=="failed"&&!failure)return "";
  const summary=failure?.summary||result.message||"任务执行失败",stage=failure?.stage||"--",root=failure?.root_cause||summary,excerpt=failure?.log_excerpt||"",logFile=failure?.log_file||result.log_file||"";
  return `<div class="detail-section failure-result"><h3>失败原因</h3><strong>${escapeHTML(summary)}</strong><dl><dt>失败阶段</dt><dd>${escapeHTML(stageText[stage]||stage)}</dd><dt>根因</dt><dd>${escapeHTML(root)}</dd>${logFile?`<dt>执行日志</dt><dd class="mono">${escapeHTML(logFile)}</dd>`:""}</dl>${excerpt?`<details><summary>查看脱敏技术详情</summary><pre>${escapeHTML(excerpt)}</pre></details>`:""}</div>`;
}
async function showOperationDetail(id) {
  try {
    const data=await api(`/api/v1/operations/${encodeURIComponent(id)}`),op=data.operation;
    $("#detailTitle").textContent=kindText[op.kind]||op.kind;
    $("#detailSubtitle").textContent=`${op.id} · ${op.line_id}`;
    const plan=op.request?.plan||{};
    const affected=(op.request?.affected_lines||[]).join(", ");
    $("#detailBody").innerHTML=`<div class="detail-grid"><dl><dt>状态</dt><dd>${badge(op.status)}</dd><dt>发起人</dt><dd>${escapeHTML(op.requested_by)}</dd><dt>时间</dt><dd>${formatTime(op.created_at)} · ${formatTime(op.updated_at)}</dd></dl><dl><dt>资源组</dt><dd>${escapeHTML(plan.resource_group||"--")}</dd><dt>实例</dt><dd>${escapeHTML(plan.instance_id||"--")}</dd><dt>构建策略</dt><dd>${escapeHTML(plan.build_mode||"--")}</dd>${affected?`<dt>影响线路</dt><dd>${escapeHTML(affected)}</dd>`:""}</dl></div>${failureResultSection(op)}${clientConfigSection(op)}${tuneResultSection(op)}<div class="detail-section"><h3>执行流程</h3><div class="timeline">${operationTimeline(op,data.events||[])}</div></div>`;
    state.openOperationID=["queued","dispatched","running"].includes(op.status)?id:"";
    $("#detailModal").classList.remove("hidden");
    hydrateClientConfig($("#detailBody .client-delivery"));
    const timeline=$("#detailBody .timeline"); if(timeline)timeline.scrollTop=timeline.scrollHeight;
  } catch(error) { toast(error.message); }
}

$("#authForm").addEventListener("submit",async(event)=>{
  event.preventDefault();
  const username=$("#usernameInput").value.trim(),password=$("#passwordInput").value,button=$("#authSubmit"),errorBox=$("#authError");
  if(!username||!password){errorBox.textContent="请输入用户名和密码";return;}
  errorBox.textContent="";button.disabled=true;button.textContent="登录中...";
  try{const result=await api("/api/v1/auth/login",{method:"POST",body:JSON.stringify({username,password})});state.token=result.token;state.mustChangePassword=!!result.must_change_password;sessionStorage.setItem("nbSessionToken",state.token);$("#authModal").classList.add("hidden");if(state.mustChangePassword){$("#currentPasswordInput").value=password;$("#passwordChangeModal").classList.remove("hidden");}else await loadAll();}
  catch(error){state.token="";sessionStorage.removeItem("nbSessionToken");errorBox.textContent=error.status===401?"用户名或密码错误":`登录失败：${error.message}`;}
  finally{button.disabled=false;button.textContent="登录";}
});
$("#passwordChangeForm").addEventListener("submit",async(event)=>{event.preventDefault();const current=$("#currentPasswordInput").value,next=$("#newPasswordInput").value,confirm=$("#confirmPasswordInput").value,button=$("#passwordChangeSubmit"),errorBox=$("#passwordChangeError");errorBox.textContent="";if(next!==confirm){errorBox.textContent="两次输入的新密码不一致";return;}button.disabled=true;try{await api("/api/v1/auth/password",{method:"POST",body:JSON.stringify({current_password:current,new_password:next})});state.mustChangePassword=false;$("#passwordChangeModal").classList.add("hidden");$("#passwordChangeForm").reset();await loadAll();}catch(error){errorBox.textContent=error.message;}finally{button.disabled=false;}});
$("#deviceForm").addEventListener("submit",async(event)=>{
  event.preventDefault();
  const form=event.currentTarget,v=Object.fromEntries(new FormData(form));v.ssh_port=Number(v.ssh_port);v.labels={};
  const unchanged=form.elements.id.readOnly&&form.dataset.originalHost===v.host&&form.dataset.originalPort===String(v.ssh_port)&&form.dataset.hostKeyStatus==="trusted";
  $("#deviceError").textContent="";
  try{
    if(unchanged){await api("/api/v1/devices",{method:"POST",body:JSON.stringify(v)});$("#deviceModal").classList.add("hidden");form.reset();form.elements.id.readOnly=false;clearDeviceHostKeyConfirmation();await loadAll();toast("设备已保存");return;}
    const scanned=await api("/api/v1/devices/host-key/scan",{method:"POST",body:JSON.stringify({host:v.host,ssh_port:v.ssh_port})});
    state.pendingDeviceHostKey={device:v,scanned};
    $("#deviceHostKeyEndpoint").textContent=`${scanned.host}:${scanned.ssh_port}`;
    $("#deviceHostKeyType").textContent=scanned.ssh_host_key_type;
    $("#deviceHostKeyFingerprint").textContent=scanned.ssh_host_key_sha256;
    $("#deviceHostKeyConfirmation").classList.remove("hidden");
  }catch(error){clearDeviceHostKeyConfirmation();$("#deviceError").textContent=error.message;}
});
$("#confirmDeviceHostKey").addEventListener("click",async()=>{
  const pending=state.pendingDeviceHostKey;if(!pending)return;
  const button=$("#confirmDeviceHostKey");button.disabled=true;$("#deviceError").textContent="";
  try{
    const v={...pending.device,ssh_host_key:pending.scanned.ssh_host_key,ssh_host_key_type:pending.scanned.ssh_host_key_type,ssh_host_key_sha256:pending.scanned.ssh_host_key_sha256,host_key_confirmation_token:pending.scanned.confirmation_token};
    await api("/api/v1/devices",{method:"POST",body:JSON.stringify(v)});const form=$("#deviceForm");$("#deviceModal").classList.add("hidden");form.reset();form.elements.id.readOnly=false;clearDeviceHostKeyConfirmation();await loadAll();toast("设备主机密钥已确认并保存");
  }catch(error){$("#deviceError").textContent=error.message;}finally{button.disabled=false;}
});
for(const name of ["host","ssh_port"]){$("#deviceForm").elements[name].addEventListener("input",clearDeviceHostKeyConfirmation);}
function syncLineTopologyForm(){const form=$("#lineForm"),single=form.elements.topology_mode.value==="single_hk";$$('[data-trihop-only]').forEach((item)=>item.classList.toggle("hidden",single));form.elements.relay_device.required=!single;form.elements.exit_device.required=!single;$("#entryRoleLabel").textContent=single?"香港 Entry / Exit":"Entry";$("#singleHKDeviceNote").classList.toggle("hidden",!single);if(single){form.elements.exit_region.value=form.elements.entry_region.value||"HK";form.elements.jump_policy.value="direct";}}
$("#lineForm").elements.topology_mode.addEventListener("change",syncLineTopologyForm);
$("#lineForm").elements.entry_region.addEventListener("input",()=>{if($("#lineForm").elements.topology_mode.value==="single_hk")$("#lineForm").elements.exit_region.value=$("#lineForm").elements.entry_region.value;});
$("#lineForm").addEventListener("submit",async(event)=>{event.preventDefault();const form=event.currentTarget,submit=form.querySelector("[type=submit]"),v=Object.fromEntries(new FormData(form));if(submit.disabled)return;submit.disabled=true;const numeric=["upstream_mbps","downstream_mbps","socks_port"];numeric.forEach((key)=>v[key]=Number(v[key]||0));v.bandwidth_mbps=Math.max(v.upstream_mbps,v.downstream_mbps);const single=v.topology_mode==="single_hk",line={id:v.id,name:v.name,status:"draft",environment:v.environment,entry_region:v.entry_region,exit_region:single?v.entry_region:v.exit_region,provider:"mixed",capacity_mbps:v.bandwidth_mbps,active_deployment:"",profile:"",secret_ref:""};const dns=(v.dns_servers||"1.1.1.1,8.8.8.8").split(/[\s,]+/).map(x=>x.trim()).filter(Boolean),nodes=single?[{device_id:v.entry_device,role:"entry",ordinal:0,next_hop_device_id:v.entry_device,jump_candidates:[],config:{}},{device_id:v.entry_device,role:"exit",ordinal:0,next_hop_device_id:"",jump_candidates:[],config:{}}]:[{device_id:v.entry_device,role:"entry",ordinal:0,next_hop_device_id:v.relay_device,jump_candidates:[],config:{}},{device_id:v.relay_device,role:"relay",ordinal:0,next_hop_device_id:v.exit_device,jump_candidates:[v.entry_device],config:{}},{device_id:v.exit_device,role:"exit",ordinal:0,next_hop_device_id:"",jump_candidates:[v.relay_device,v.entry_device],config:{}}];const spec={line_id:v.id,environment:v.environment,topology_mode:v.topology_mode,service_profile:v.service_profile,instance_id:v.instance_id,bandwidth_mbps:v.bandwidth_mbps,upstream_mbps:v.upstream_mbps,downstream_mbps:v.downstream_mbps,socks_port:v.socks_port,relay_port:0,exit_port:0,exit_bind_ip:(v.exit_bind_ip||"").trim(),dns_servers:dns,udp_port_min:0,udp_port_max:0,whitelist:v.whitelist.split(/\r?\n/).map(x=>x.trim()).filter(Boolean),build_mode:v.build_mode,artifact_ref:v.artifact_ref,source_ref:v.source_ref,srs_ref:v.srs_ref,jump_policy:single?"direct":v.jump_policy,nodes};let lineCreated=false,specSaved=false;try{await api("/api/v1/lines",{method:"POST",body:JSON.stringify(line)});lineCreated=true;await api(`/api/v1/lines/${encodeURIComponent(v.id)}/spec`,{method:"PUT",body:JSON.stringify(spec)});specSaved=true;await api("/api/v1/operations",{method:"POST",headers:{"Idempotency-Key":`web-${crypto.randomUUID()}`},body:JSON.stringify({line_id:v.id,kind:"line.open",requested_by:"operator",request:{note:"created from topology editor"}})});$("#lineModal").classList.add("hidden");form.reset();syncLineTopologyForm();await loadAll();switchView("operations");toast("线路已登记，开线任务已排队");}catch(error){if(lineCreated&&!specSaved){await api(`/api/v1/lines/${encodeURIComponent(v.id)}`,{method:"DELETE",body:JSON.stringify({requested_by:"operator",reason:"rollback incomplete web line creation"})}).catch(()=>{});}$("#lineError").textContent=error.message;await loadAll().catch(()=>{});}finally{submit.disabled=false;}});
$("#operationForm").addEventListener("submit",async(event)=>{event.preventDefault();const v=Object.fromEntries(new FormData(event.currentTarget));try{await api("/api/v1/operations",{method:"POST",headers:{"Idempotency-Key":`web-${crypto.randomUUID()}`},body:JSON.stringify({line_id:v.line_id,kind:v.kind,requested_by:v.requested_by,request:{deployment:v.deployment,note:v.note}})});$("#operationModal").classList.add("hidden");await loadAll();switchView("operations");toast("任务已进入队列");}catch(error){$("#operationError").textContent=error.message;}});
$("#forceDeleteLineForm").addEventListener("submit",async(event)=>{
  event.preventDefault();const form=event.currentTarget,v=Object.fromEntries(new FormData(form)),button=form.querySelector("[type=submit]");
  $("#forceDeleteLineError").textContent="";button.disabled=true;
  try{
    await api(`/api/v1/lines/${encodeURIComponent(v.line_id)}`,{method:"DELETE",body:JSON.stringify({requested_by:"operator",reason:v.reason.trim(),force:true,confirmation:v.confirmation.trim(),acknowledge_orphans:v.acknowledge_orphans==="on"})});
    $("#forceDeleteLineModal").classList.add("hidden");await loadAll();toast("线路已强制删除，审计记录已保留");
  }catch(error){$("#forceDeleteLineError").textContent=error.message;}finally{button.disabled=false;}
});
$("#lineEnvironmentForm").addEventListener("submit",async(event)=>{event.preventDefault();const form=event.currentTarget,v=Object.fromEntries(new FormData(form)),submit=form.querySelector("[type=submit]");submit.disabled=true;try{await api(`/api/v1/lines/${encodeURIComponent(v.line_id)}`,{method:"PATCH",body:JSON.stringify({environment:v.environment})});$("#lineEnvironmentModal").classList.add("hidden");await loadAll();toast("线路环境已更新");}catch(error){$("#lineEnvironmentError").textContent=error.message;}finally{submit.disabled=false;}});
$("#nodeSourceForm").addEventListener("submit",async(event)=>{event.preventDefault();const form=event.currentTarget,submit=form.querySelector("[type=submit]"),data=new FormData(form),panel=$("#nodeSourceProgress"),bar=$("#nodeSourceProgressBar"),text=$("#nodeSourceProgressText"),percent=$("#nodeSourceProgressPercent");submit.disabled=true;$("#nodeSourceError").textContent="";panel.classList.remove("hidden");bar.value=0;text.textContent="正在初始化分块上传";percent.textContent="0%";try{const payload=await uploadNodeArchive(data,(loaded,total,status)=>{const value=total?Math.min(100,Math.round(loaded/total*100)):0;bar.value=value;percent.textContent=`${value}%`;text.textContent=`${status} · ${number(loaded/1048576,1)} / ${number(total/1048576,1)} MiB`;});bar.value=100;percent.textContent="100%";$("#nodeSourceModal").classList.add("hidden");form.reset();panel.classList.add("hidden");await loadAll();switchView("operations");toast(`平台源码已上传，候选构建任务 ${payload.operation.id} 已排队`);}catch(error){$("#nodeSourceError").textContent=error.message;text.textContent=error.message;}finally{submit.disabled=false;}});

$$('.nav-item').forEach((item)=>item.addEventListener("click",()=>switchView(item.dataset.view)));
$("#contextAction").addEventListener("click",()=>{if(state.view==="devices"){const form=$("#deviceForm");form.reset();delete form.dataset.originalHost;delete form.dataset.originalPort;delete form.dataset.hostKeyStatus;clearDeviceHostKeyConfirmation();form.elements.id.readOnly=false;form.elements.password.required=true;$("#devicePasswordLabel").textContent="SSH 密码";$("#devicePasswordHint").textContent="首次登记必须输入，保存后不会回显";$("#scanDeviceHostKey").textContent="扫描主机密钥";$("#deviceError").textContent="";$("#deviceModal").classList.remove("hidden");}else{$("#lineError").textContent="";syncLineTopologyForm();$("#lineModal").classList.remove("hidden");}});
$("#refreshButton").addEventListener("click",()=>loadAll().catch(()=>{}));
$$('.close-device').forEach((x)=>x.addEventListener("click",()=>{clearDeviceHostKeyConfirmation();$("#deviceModal").classList.add("hidden");}));$$('.close-line').forEach((x)=>x.addEventListener("click",()=>$("#lineModal").classList.add("hidden")));$$('.close-line-environment').forEach((x)=>x.addEventListener("click",()=>$("#lineEnvironmentModal").classList.add("hidden")));$$('.close-operation').forEach((x)=>x.addEventListener("click",()=>$("#operationModal").classList.add("hidden")));$$('.close-force-delete-line').forEach((x)=>x.addEventListener("click",()=>$("#forceDeleteLineModal").classList.add("hidden")));$$('.close-detail').forEach((x)=>x.addEventListener("click",()=>{state.openOperationID="";if(state.trafficCharts){state.trafficCharts.destroy();state.trafficCharts=null;}$("#detailModal").classList.add("hidden");}));
$("#uploadNodeSource").addEventListener("click",()=>switchView("upgrades"));
$("#buildPlatformRelease").addEventListener("click",()=>{$("#nodeSourceError").textContent="";$("#nodeSourceProgress").classList.add("hidden");$("#nodeSourceModal").classList.remove("hidden");});
$$('.close-node-source').forEach((x)=>x.addEventListener("click",()=>{if(state.nodeUploadRequest)state.nodeUploadRequest.abort();$("#nodeSourceModal").classList.add("hidden");}));
$("#previewPlatformUpgrade").addEventListener("click",async()=>{const release=$("#platformReleaseSelect").value,line=$("#platformUpgradeLine").value;$("#platformUpgradeError").textContent="";state.platformUpgradePreview=null;$("#startPlatformUpgrade").disabled=true;try{const preview=await api(`/api/v1/platform-upgrades/preview?release_id=${encodeURIComponent(release)}&line_id=${encodeURIComponent(line)}`);showPlatformPreview(preview);}catch(error){$("#platformUpgradeError").textContent=error.message;}});
$("#confirmPlatformInterrupt").addEventListener("change",()=>{$("#startPlatformUpgrade").disabled=!state.platformUpgradePreview||!$("#confirmPlatformInterrupt").checked;});
for(const selector of ["#platformReleaseSelect","#platformUpgradeLine"])$(selector).addEventListener("change",()=>{state.platformUpgradePreview=null;$("#confirmPlatformInterrupt").checked=false;$("#startPlatformUpgrade").disabled=true;$("#platformUpgradePreview").innerHTML=`<span class="topology-empty">范围已变化，请重新预览</span>`;});
$("#startPlatformUpgrade").addEventListener("click",async()=>{const preview=state.platformUpgradePreview;if(!preview||!$("#confirmPlatformInterrupt").checked)return;const button=$("#startPlatformUpgrade");button.disabled=true;$("#platformUpgradeError").textContent="";try{const operation=await api("/api/v1/platform-upgrades",{method:"POST",headers:{"Idempotency-Key":`platform-${crypto.randomUUID()}`},body:JSON.stringify({release_id:preview.release_id,line_id:preview.seed_line_id,preview_digest:preview.preview_digest,requested_by:"operator",confirm_interrupt:true})});await loadAll();switchView("operations");toast(`平台升级任务 ${operation.id} 已排队`);}catch(error){$("#platformUpgradeError").textContent=error.message;button.disabled=false;}});
$("#deviceSearch").addEventListener("input",filterDevices);$("#deviceEnvironment").addEventListener("change",filterDevices);$("#deviceStatus").addEventListener("change",filterDevices);$("#exportDevices").addEventListener("click",async()=>{try{const response=await fetch("/api/v1/devices/export",{headers:{Authorization:`Bearer ${state.token}`}});if(!response.ok)throw new Error(`导出失败（HTTP ${response.status}）`);const blob=await response.blob(),url=URL.createObjectURL(blob),anchor=document.createElement("a");anchor.href=url;anchor.download="nb-devices.csv";anchor.click();URL.revokeObjectURL(url);toast("设备信息已导出");}catch(error){toast(error.message);}});$("#lineSearch").addEventListener("input",filterLines);$("#lineEnvironment").addEventListener("change",filterLines);$("#lineStatus").addEventListener("change",filterLines);$("#topologyLine").addEventListener("change",renderOverviewTopology);
for(const selector of ["#deviceTopologyLine","#deviceTopologyRegion","#deviceTopologyRole","#deviceTopologyHealth"])$(selector).addEventListener("change",renderDeviceTopology);
const resetTopologyButton=document.createElement("button");
resetTopologyButton.type="button";resetTopologyButton.className="secondary topology-reset";resetTopologyButton.textContent="重置布局";
resetTopologyButton.addEventListener("click",async()=>{
  try{
    await api("/api/v1/topology/layout",{method:"DELETE"});state.topology=await api("/api/v1/topology");
    state.deviceTopology?.reset(state.topology,{lineID:$("#deviceTopologyLine").value,region:$("#deviceTopologyRegion").value,role:$("#deviceTopologyRole").value,health:$("#deviceTopologyHealth").value});
    state.overviewTopology?.reset(state.topology,{lineID:$("#topologyLine").value});toast("拓扑布局已重置");
  }catch(error){toast(`重置布局失败：${error.message}`);}
});
$(".topology-filters").append(resetTopologyButton);
let trafficWorkspace=null;
document.addEventListener("nb:open-traffic",(event)=>{
  const lineID=event.detail?.lineID,detail=state.details[lineID];if(!lineID)return;
  if(!trafficWorkspace)trafficWorkspace=new TrafficWorkspace($("#trafficWorkspace"),{token:state.token});
  trafficWorkspace.token=state.token;
  $("#detailModal").classList.add("hidden");
  trafficWorkspace.open({id:lineID,name:detail?.line?.name||lineID});
});
$("#operationLine").addEventListener("change",async()=>{const id=$("#operationLine").value,data=await api(`/api/v1/operations?limit=100${id?`&line_id=${encodeURIComponent(id)}`:""}`);state.operations=data.operations||[];renderOperations();});$("#reloadOperations").addEventListener("click",()=>$("#operationLine").dispatchEvent(new Event("change")));
$("#operationSelectAll").addEventListener("change",(event)=>{for(const box of $$('[data-operation-select]:not(:disabled)')){box.checked=event.currentTarget.checked;event.currentTarget.checked?state.selectedOperations.add(box.dataset.operationSelect):state.selectedOperations.delete(box.dataset.operationSelect);}syncOperationSelection();});
$("#deleteSelectedOperations").addEventListener("click",()=>cleanupOperations([...state.selectedOperations]));
async function restoreSession(){if(!state.token){$("#authModal").classList.remove("hidden");return;}try{const user=await api("/api/v1/auth/me");$("#authModal").classList.add("hidden");state.mustChangePassword=!!user.must_change_password;if(state.mustChangePassword){$("#passwordChangeModal").classList.remove("hidden");return;}await loadAll();}catch{state.token="";state.mustChangePassword=false;sessionStorage.removeItem("nbSessionToken");$("#authModal").classList.remove("hidden");}}
restoreSession();
setInterval(()=>{if(state.token&&!state.mustChangePassword&&document.visibilityState==="visible")loadAll().catch(()=>{});},10000);
setInterval(()=>{if(state.token&&!state.mustChangePassword&&state.openOperationID&&!$("#detailModal").classList.contains("hidden")&&document.visibilityState==="visible")showOperationDetail(state.openOperationID).catch(()=>{});},2000);
