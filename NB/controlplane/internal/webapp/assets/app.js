const state = {
  token: sessionStorage.getItem("nbAdminToken") || "",
  dashboard: null, devices: [], lines: [], details: {}, operations: [], incidents: [], executors: [], view: "overview"
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const escapeHTML = (value = "") => String(value).replace(/[&<>'"]/g, (char) => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;",'"':"&quot;"})[char]);
const formatTime = (value) => value ? new Intl.DateTimeFormat("zh-CN", {month:"2-digit", day:"2-digit", hour:"2-digit", minute:"2-digit", second:"2-digit"}).format(new Date(value)) : "--";
const number = (value, digits = 0) => Number(value || 0).toLocaleString("zh-CN", {maximumFractionDigits:digits});
const statusText = {ok:"健康",healthy:"健康",degraded:"异常",down:"离线",unknown:"待采集",ready:"可用",provisioning:"初始化",offline:"离线",retired:"退役",active:"运行中",draft:"草稿",validating:"验证中",maintenance:"维护",disabled:"停用",queued:"排队中",dispatched:"已下发",running:"执行中",succeeded:"成功",failed:"失败",cancelled:"已取消",rolled_back:"已回滚",open:"未处理",firing:"告警中",resolved:"已恢复",critical:"严重",warning:"警告",info:"信息",pending:"等待",skipped:"跳过"};
function healthState(value) { return value === "ok" ? "healthy" : (value || "unknown"); }
const kindText = {"line.open":"开通线路","line.validate":"验证线路","line.upgrade":"升级","line.rollback":"回滚","line.disable":"停用"};
const roleText = {entry:"Entry",relay:"Relay",exit:"Exit"};

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("Authorization", `Bearer ${state.token}`);
  if (options.body) headers.set("Content-Type", "application/json");
  const response = await fetch(path, {...options, headers});
  const body = await response.json().catch(() => ({}));
  if (!response.ok) { const error = new Error(body.error || `请求失败 (${response.status})`); error.status = response.status; throw error; }
  return body;
}

function badge(value) { return `<span class="badge ${escapeHTML(value)}">${escapeHTML(statusText[value] || value || "未知")}</span>`; }
function toast(message) { const el = $("#toast"); el.textContent = message; el.classList.add("show"); clearTimeout(toast.timer); toast.timer = setTimeout(() => el.classList.remove("show"), 2800); }
function setConnected(ok) { $("#apiDot").classList.toggle("online", ok); $("#apiStatus").textContent = ok ? "中央服务在线" : "未连接"; }
function metric(label, value, note) { return `<div class="metric"><span class="metric-label">${label}</span><strong class="metric-value">${value}</strong><span class="metric-note">${note}</span></div>`; }

function renderMetrics() {
  const d = state.dashboard || {};
  $("#metrics").innerHTML = [
    metric("设备总数", number(state.devices.length), `${state.devices.filter((x) => x.last_health === "healthy").length} 台在线`),
    metric("线路总数", number(d.lines_total), `${number(d.lines_healthy)} 条健康`),
    metric("异常线路", number(d.lines_degraded), d.lines_degraded ? "需要处理" : "当前无异常"),
    metric("总容量", `${number(d.capacity_mbps)} Mbps`, "已登记线路容量"),
    metric("当前吞吐", `${number(d.throughput_mbps, 2)} Mbps`, "最新节点快照汇总"),
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

function snapshotsByDevice(detail) {
  const result = {};
  for (const item of detail?.snapshots || []) result[item.node_id] = item;
  return result;
}

function topology(detail, compact = false) {
  const spec = detail?.spec;
  if (!spec?.nodes?.length) return `<div class="topology-empty">该线路尚未配置设备拓扑</div>`;
  const snapshots = snapshotsByDevice(detail);
  const order = {entry:0, relay:1, exit:2};
  const nodes = [...spec.nodes].sort((a,b) => order[a.role] - order[b.role] || a.ordinal - b.ordinal);
  return `<div class="topology-flex ${compact ? "compact" : ""}">${nodes.map((node, index) => {
    const device = node.device || {};
    const snap = snapshots[node.device_id];
    const health = healthState(snap?.health || device.last_health);
    const port = node.role === "entry" ? spec.socks_port : node.role === "relay" ? spec.relay_port : spec.exit_port;
    const connector = index ? `<div class="topology-link"><span></span><small>${escapeHTML(nodes[index - 1].next_hop_device_id ? "下一跳" : "链路")}</small></div>` : "";
    return `${connector}<button class="topology-node" data-device-detail="${escapeHTML(node.device_id)}"><span class="node-role">${roleText[node.role]}</span><span class="node-health ${escapeHTML(health)}"></span><strong>${escapeHTML(device.name || node.device_id)}</strong><span>${escapeHTML(device.host || "--")}:${port}</span><small>${escapeHTML(device.region || "--")} · ${escapeHTML(device.provider || "--")}</small>${snap ? `<em>${number(snap.throughput_mbps,2)} Mbps · ${number(snap.queue_age_p95_us)} μs</em>` : `<em>等待运行快照</em>`}</button>`;
  }).join("")}</div>`;
}

function renderOverviewTopology() {
  const select = $("#topologyLine");
  const previous = select.value;
  select.innerHTML = state.lines.map((line) => `<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`).join("");
  if (state.lines.some((line) => line.id === previous)) select.value = previous;
  const id = select.value || state.lines[0]?.id;
  $("#overviewTopology").innerHTML = id ? topology(state.details[id]) : `<div class="topology-empty">尚未登记线路</div>`;
  $$('[data-device-detail]').forEach((button) => button.addEventListener("click", () => showDeviceDetail(button.dataset.deviceDetail)));
}

function overviewRow(line) {
  const d = state.details[line.id];
  const summary = d?.snapshots?.reduce((sum, item) => ({throughput:sum.throughput + Number(item.throughput_mbps || 0), sessions:sum.sessions + Number(item.sessions || 0), queue:Math.max(sum.queue, Number(item.queue_age_p95_us || 0)), loss:Math.max(sum.loss, Number(item.effective_loss_pct || 0))}), {throughput:0,sessions:0,queue:0,loss:0}) || {};
  const health = d?.snapshots?.some((x) => healthState(x.health) !== "healthy") ? "degraded" : d?.snapshots?.length ? "healthy" : "unknown";
  return `<tr class="clickable" data-line-detail="${escapeHTML(line.id)}"><td><div class="line-name">${escapeHTML(line.name)}</div><div class="line-id">${escapeHTML(line.id)}</div></td><td>${badge(health)}</td><td><strong>${number(summary.throughput,2)}</strong> / ${number(line.capacity_mbps)} Mbps</td><td>${number(summary.sessions)}</td><td>${number(summary.queue)} μs</td><td>${number(summary.loss,3)}%</td><td><span class="mono">${escapeHTML((line.active_deployment || "未部署").slice(0,20))}</span></td></tr>`;
}

function renderOverview() {
  $("#overviewLines").innerHTML = state.lines.length ? state.lines.map(overviewRow).join("") : `<tr><td class="empty" colspan="7">尚未登记线路</td></tr>`;
  $("#recentOperations").innerHTML = state.operations.slice(0,8).map((item) => `<button class="activity-item" data-operation-detail="${escapeHTML(item.id)}"><span>${escapeHTML(kindText[item.kind] || item.kind)}</span><strong>${escapeHTML(item.line_id)}</strong><small>${badge(item.status)} ${formatTime(item.updated_at)}</small></button>`).join("") || `<div class="topology-empty">暂无任务</div>`;
  renderOverviewTopology(); bindDetails();
}

function deviceRow(item) {
  const health = item.last_health || "unknown";
  return `<tr data-device-search="${escapeHTML(`${item.id} ${item.name} ${item.host} ${item.region} ${item.provider}`.toLowerCase())}" data-device-status="${escapeHTML(item.status)}"><td><div class="device-name"><span class="health-dot ${escapeHTML(health)}"></span><div><strong>${escapeHTML(item.name)}</strong><small>${escapeHTML(item.id)}</small></div></div></td><td>${badge(item.status)}</td><td><span class="mono">${escapeHTML(item.ssh_user)}@${escapeHTML(item.host)}:${item.ssh_port}</span></td><td>${escapeHTML(item.region || "--")}<div class="subtext">${escapeHTML(item.provider || "--")}</div></td><td>${escapeHTML(item.os || "--")}<div class="subtext">${escapeHTML(item.arch || "--")}</div></td><td>${badge(health)}<div class="subtext">${formatTime(item.last_seen_at)}</div></td><td><div class="actions"><button class="action-button" data-probe-device="${escapeHTML(item.id)}">探测</button><button class="action-button" data-edit-device="${escapeHTML(item.id)}">编辑</button><button class="action-button danger-action" data-delete-device="${escapeHTML(item.id)}">删除</button></div></td></tr>`;
}

function renderDevices() {
  $("#devicesTable").innerHTML = state.devices.length ? state.devices.map(deviceRow).join("") : `<tr><td class="empty" colspan="7">尚未录入设备</td></tr>`;
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

function filterDevices() { const q = $("#deviceSearch").value.trim().toLowerCase(), status = $("#deviceStatus").value; $$("#devicesTable tr[data-device-search]").forEach((row) => row.hidden = (q && !row.dataset.deviceSearch.includes(q)) || (status && row.dataset.deviceStatus !== status)); }
async function probeDevice(id) { try { toast("正在探测 SSH 管理端口"); await api(`/api/v1/devices/${encodeURIComponent(id)}/probe`, {method:"POST"}); await loadAll(); toast("设备探测完成"); } catch (error) { toast(error.message); } }
function editDevice(id) { const item = state.devices.find((x) => x.id === id); if (!item) return; const form = $("#deviceForm"); form.reset(); for (const [key,value] of Object.entries(item)) if (form.elements[key] && key !== "labels") form.elements[key].value = value ?? ""; form.elements.id.readOnly = true; $("#deviceError").textContent = ""; $("#deviceModal").classList.remove("hidden"); }
async function deleteDevice(id) { if (!confirm(`确认删除设备 ${id}？`)) return; try { await api(`/api/v1/devices/${encodeURIComponent(id)}`, {method:"DELETE"}); await loadAll(); toast("设备已删除"); } catch (error) { toast(error.message); } }

function lineRow(line) {
  const detail = state.details[line.id];
  const nodes = detail?.spec?.nodes || [];
  const route = nodes.map((n) => n.device?.name || n.device_id).join(" → ") || `${line.entry_region} → ${line.exit_region}`;
  const definitions = line.status === "draft" || line.status === "disabled" ? [["line.open","开线","primary-action"]] : [["line.validate","验证",""] ,["line.upgrade","升级",""] ,["line.rollback","回滚",""] ,["line.disable","停用","danger-action"]];
  const actions = definitions.map(([kind,label,style]) => { const a = operationAvailability(line.id,kind); return `<button class="action-button ${style}" ${a.enabled ? `data-action="${kind}" data-line="${escapeHTML(line.id)}"` : `disabled title="${escapeHTML(a.reason)}"`}>${label}</button>`; }).join("");
  const canDelete = !nodes.length || ["draft","disabled","archived"].includes(line.status);
  const deleteAction = canDelete ? `<button class="action-button danger-action" data-delete-line="${escapeHTML(line.id)}">删除</button>` : "";
  return `<tr data-line-search="${escapeHTML(`${line.id} ${line.name} ${route}`.toLowerCase())}" data-line-status="${escapeHTML(line.status)}"><td><button class="text-button" data-line-detail="${escapeHTML(line.id)}"><strong>${escapeHTML(line.name)}</strong><small>${escapeHTML(line.id)}</small></button></td><td>${badge(line.status)}</td><td><div class="route-summary">${escapeHTML(route)}</div></td><td>${number(detail?.spec?.bandwidth_mbps || line.capacity_mbps)} Mbps</td><td>${detail?.spec?.socks_port || "--"}</td><td><span class="mono">${escapeHTML(line.profile || "--")}</span></td><td><div class="actions">${actions}${deleteAction}</div></td></tr>`;
}

async function deleteLine(id) {
  const detail = state.details[id], incomplete = !(detail?.spec?.nodes || []).length;
  if (!confirm(`确认删除线路 ${id}？任务和监控历史将从当前视图移除，删除审计会保留。`)) return;
  try {
    await api(`/api/v1/lines/${encodeURIComponent(id)}`, {method:"DELETE", body:JSON.stringify({requested_by:"operator", reason:incomplete?"remove incomplete discovered line":"operator requested line deletion"})});
    await loadAll(); toast("线路已删除");
  } catch (error) { toast(error.message); }
}

function renderLines() {
  $("#linesTable").innerHTML = state.lines.length ? state.lines.map(lineRow).join("") : `<tr><td class="empty" colspan="7">尚未登记线路</td></tr>`;
  filterLines();
  $$('[data-action]').forEach((button) => button.addEventListener("click", () => openOperation(button.dataset.line, button.dataset.action)));
  $$('[data-delete-line]').forEach((button) => button.addEventListener("click", () => deleteLine(button.dataset.deleteLine)));
  $("#operationLine").innerHTML = `<option value="">全部线路</option>${state.lines.map((line) => `<option value="${escapeHTML(line.id)}">${escapeHTML(line.name)}</option>`).join("")}`;
  bindDetails();
}
function filterLines() { const q = $("#lineSearch").value.trim().toLowerCase(), status = $("#lineStatus").value; $$("#linesTable tr[data-line-search]").forEach((row) => row.hidden = (q && !row.dataset.lineSearch.includes(q)) || (status && row.dataset.lineStatus !== status)); }

function renderOperations() {
  $("#operationsTable").innerHTML = state.operations.length ? state.operations.map((item) => `<tr class="clickable" data-operation-detail="${escapeHTML(item.id)}"><td><span class="mono">${escapeHTML(item.id)}</span></td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(kindText[item.kind] || item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.requested_by)}</td><td>${formatTime(item.created_at)}</td><td>${formatTime(item.updated_at)}</td></tr>`).join("") : `<tr><td class="empty" colspan="7">暂无操作任务</td></tr>`;
  bindDetails();
}
function renderIncidents() { $("#incidentsTable").innerHTML = state.incidents.length ? state.incidents.map((item) => `<tr><td>${badge(item.severity)}</td><td>${escapeHTML(item.line_id)}</td><td>${escapeHTML(item.kind)}</td><td>${badge(item.status)}</td><td>${escapeHTML(item.message)}</td><td>${formatTime(item.observed_at)}</td></tr>`).join("") : `<tr><td class="empty" colspan="6">当前无告警</td></tr>`; }

async function loadAll() {
  try {
    const [dashboard,devices,lines,operations,incidents,executors] = await Promise.all([api("/api/v1/dashboard"),api("/api/v1/devices"),api("/api/v1/lines"),api("/api/v1/operations?limit=100"),api("/api/v1/incidents?limit=100"),api("/api/v1/executors")]);
    state.dashboard=dashboard; state.devices=devices.devices||[]; state.lines=lines.lines||[]; state.operations=operations.operations||[]; state.incidents=incidents.incidents||[]; state.executors=executors.executors||[];
    const details = await Promise.all(state.lines.map((line) => api(`/api/v1/lines/${encodeURIComponent(line.id)}/detail`).catch(() => ({line}))));
    state.details = Object.fromEntries(details.map((detail) => [detail.line.id,detail]));
    renderMetrics(); renderDevices(); renderLines(); renderOperations(); renderIncidents(); renderOverview();
    $("#updatedAt").textContent = `更新于 ${new Date().toLocaleTimeString("zh-CN", {hour:"2-digit",minute:"2-digit",second:"2-digit"})}`; setConnected(true); $("#authModal").classList.add("hidden");
  } catch (error) { setConnected(false); if (error.status === 401) { sessionStorage.removeItem("nbAdminToken"); $("#authModal").classList.remove("hidden"); } else toast(error.message); throw error; }
}

const viewMeta = {overview:["运营总览","线路拓扑、节点健康与执行任务"],devices:["设备管理","服务器库存、管理端口和健康状态"],lines:["线路管理","设备角色、容量、端口与部署状态"],operations:["操作任务","构建、上传、分发、启动和验证进度"],incidents:["告警中心","线路和设备异常汇总"]};
function switchView(name) { state.view=name; $$(".nav-item").forEach((item) => item.classList.toggle("active",item.dataset.view===name)); $$(".view").forEach((view) => view.classList.toggle("active",view.id===`${name}View`)); [$("#viewTitle").textContent,$("#viewSubtitle").textContent]=viewMeta[name]; const action=$("#contextAction"); action.hidden=!['devices','lines','overview'].includes(name); action.textContent=name==='devices'?"录入设备":"新增线路"; }

function openOperation(lineID,kind) { const line=state.lines.find((x)=>x.id===lineID),form=$("#operationForm"); form.reset(); form.elements.line_id.value=lineID; form.elements.kind.value=kind; form.elements.requested_by.value="operator"; $("#operationTitle").textContent=kindText[kind]||"创建任务"; $("#operationTarget").textContent=line?`${line.name} · ${line.id}`:lineID; $("#operationError").textContent=""; $("#operationModal").classList.remove("hidden"); }
function bindDetails() { $$('[data-line-detail]').forEach((b) => b.onclick=()=>showLineDetail(b.dataset.lineDetail)); $$('[data-operation-detail]').forEach((b) => b.onclick=()=>showOperationDetail(b.dataset.operationDetail)); }
function showDeviceDetail(id) { const item=state.devices.find((x)=>x.id===id); if(!item)return; $("#detailTitle").textContent=item.name; $("#detailSubtitle").textContent=item.id; $("#detailBody").innerHTML=`<div class="detail-grid"><dl><dt>SSH</dt><dd class="mono">${escapeHTML(item.ssh_user)}@${escapeHTML(item.host)}:${item.ssh_port}</dd><dt>内网地址</dt><dd>${escapeHTML(item.private_ip||"--")}</dd><dt>区域 / 运营商</dt><dd>${escapeHTML(item.region||"--")} / ${escapeHTML(item.provider||"--")}</dd></dl><dl><dt>状态</dt><dd>${badge(item.status)} ${badge(item.last_health||"unknown")}</dd><dt>系统</dt><dd>${escapeHTML(item.os||"--")} · ${escapeHTML(item.arch||"--")}</dd><dt>最近在线</dt><dd>${formatTime(item.last_seen_at)}</dd></dl></div>`; $("#detailModal").classList.remove("hidden"); }
function showLineDetail(id) { const detail=state.details[id]; if(!detail)return; const spec=detail.spec||{}; $("#detailTitle").textContent=detail.line.name; $("#detailSubtitle").textContent=`${detail.line.id} · ${statusText[detail.line.status]||detail.line.status}`; $("#detailBody").innerHTML=`<div class="detail-section"><h3>设备拓扑</h3><div class="topology-canvas">${topology(detail)}</div></div><div class="detail-grid"><dl><dt>资源组 / 实例</dt><dd>${escapeHTML(spec.resource_group||"--")} / ${escapeHTML(spec.instance_id||"--")}</dd><dt>带宽</dt><dd>${number(spec.bandwidth_mbps||detail.line.capacity_mbps)} Mbps</dd><dt>端口规划</dt><dd>SOCKS ${spec.socks_port||"--"} · Relay ${spec.relay_port||"--"} · Exit ${spec.exit_port||"--"}</dd></dl><dl><dt>UDP 池</dt><dd>${spec.udp_port_min||"--"} - ${spec.udp_port_max||"--"}</dd><dt>构建 / 跳板</dt><dd>${escapeHTML(spec.build_mode||"--")} / ${escapeHTML(spec.jump_policy||"--")}</dd><dt>Deployment</dt><dd class="mono">${escapeHTML(detail.line.active_deployment||"--")}</dd></dl></div><div class="detail-section"><h3>节点快照</h3>${snapshotTable(detail.snapshots||[])}</div>`; $("#detailModal").classList.remove("hidden"); $$('[data-device-detail]').forEach((b)=>b.onclick=()=>showDeviceDetail(b.dataset.deviceDetail)); }
function snapshotTable(items) { if(!items.length)return `<div class="topology-empty">等待 worker 采集运行快照</div>`; return `<div class="table-wrap"><table class="detail-table"><thead><tr><th>节点</th><th>角色</th><th>健康</th><th>吞吐</th><th>会话</th><th>队列 P95</th><th>有效丢包</th></tr></thead><tbody>${items.map((x)=>`<tr><td>${escapeHTML(x.node_id)}</td><td>${escapeHTML(x.role)}</td><td>${badge(healthState(x.health))}</td><td>${number(x.throughput_mbps,2)} Mbps</td><td>${number(x.sessions)}</td><td>${number(x.queue_age_p95_us)} μs</td><td>${number(x.effective_loss_pct,3)}%</td></tr>`).join("")}</tbody></table></div>`; }
async function showOperationDetail(id) { try { const data=await api(`/api/v1/operations/${encodeURIComponent(id)}`),op=data.operation; $("#detailTitle").textContent=kindText[op.kind]||op.kind; $("#detailSubtitle").textContent=`${op.id} · ${op.line_id}`; const plan=op.request?.plan||{}; $("#detailBody").innerHTML=`<div class="detail-grid"><dl><dt>状态</dt><dd>${badge(op.status)}</dd><dt>发起人</dt><dd>${escapeHTML(op.requested_by)}</dd><dt>时间</dt><dd>${formatTime(op.created_at)} · ${formatTime(op.updated_at)}</dd></dl><dl><dt>资源组</dt><dd>${escapeHTML(plan.resource_group||"--")}</dd><dt>实例</dt><dd>${escapeHTML(plan.instance_id||"--")}</dd><dt>构建策略</dt><dd>${escapeHTML(plan.build_mode||"--")}</dd></dl></div><div class="detail-section"><h3>执行流程</h3><div class="timeline">${(data.events||[]).map((event)=>`<div class="timeline-item ${escapeHTML(event.status)}"><span>${event.sequence}</span><div><strong>${escapeHTML(event.stage)}</strong><small>${escapeHTML(event.message||statusText[event.status]||event.status)} · ${formatTime(event.created_at)}</small></div></div>`).join("")||`<div class="topology-empty">任务等待执行器领取</div>`}</div></div>`; $("#detailModal").classList.remove("hidden"); } catch(error){toast(error.message);} }

$("#authForm").addEventListener("submit",async(event)=>{event.preventDefault();state.token=$("#tokenInput").value;try{await loadAll();sessionStorage.setItem("nbAdminToken",state.token);$("#authError").textContent="";}catch(error){$("#authError").textContent=error.message;}});
$("#deviceForm").addEventListener("submit",async(event)=>{event.preventDefault();const form=event.currentTarget,v=Object.fromEntries(new FormData(form));v.ssh_port=Number(v.ssh_port);v.labels={};try{await api("/api/v1/devices",{method:"POST",body:JSON.stringify(v)});$("#deviceModal").classList.add("hidden");form.reset();form.elements.id.readOnly=false;await loadAll();toast("设备已保存");}catch(error){$("#deviceError").textContent=error.message;}});
$("#lineForm").addEventListener("submit",async(event)=>{event.preventDefault();const form=event.currentTarget,v=Object.fromEntries(new FormData(form));const numeric=["bandwidth_mbps","socks_port"];numeric.forEach((key)=>v[key]=Number(v[key]));const line={id:v.id,name:v.name,status:"draft",entry_region:v.entry_region,exit_region:v.exit_region,provider:"mixed",capacity_mbps:v.bandwidth_mbps,active_deployment:"",profile:"",secret_ref:""};const spec={line_id:v.id,resource_group:v.resource_group,instance_id:v.instance_id,bandwidth_mbps:v.bandwidth_mbps,socks_port:v.socks_port,relay_port:0,exit_port:0,udp_port_min:0,udp_port_max:0,whitelist:v.whitelist.split(/\r?\n/).map(x=>x.trim()).filter(Boolean),build_mode:v.build_mode,artifact_ref:v.artifact_ref,source_ref:v.source_ref,srs_ref:v.srs_ref,jump_policy:v.jump_policy,nodes:[{device_id:v.entry_device,role:"entry",ordinal:0,next_hop_device_id:v.relay_device,jump_candidates:[],config:{}},{device_id:v.relay_device,role:"relay",ordinal:0,next_hop_device_id:v.exit_device,jump_candidates:[v.entry_device],config:{}},{device_id:v.exit_device,role:"exit",ordinal:0,next_hop_device_id:"",jump_candidates:[v.relay_device,v.entry_device],config:{}}]};let lineCreated=false,specSaved=false;try{await api("/api/v1/lines",{method:"POST",body:JSON.stringify(line)});lineCreated=true;await api(`/api/v1/lines/${encodeURIComponent(v.id)}/spec`,{method:"PUT",body:JSON.stringify(spec)});specSaved=true;await api("/api/v1/operations",{method:"POST",headers:{"Idempotency-Key":`web-${crypto.randomUUID()}`},body:JSON.stringify({line_id:v.id,kind:"line.open",requested_by:"operator",request:{note:"created from topology editor"}})});$("#lineModal").classList.add("hidden");form.reset();await loadAll();switchView("operations");toast("线路已登记，内部端口已分配，开线任务已排队");}catch(error){if(lineCreated&&!specSaved){await api(`/api/v1/lines/${encodeURIComponent(v.id)}`,{method:"DELETE",body:JSON.stringify({requested_by:"operator",reason:"rollback incomplete web line creation"})}).catch(()=>{});}$("#lineError").textContent=error.message;await loadAll().catch(()=>{});}});
$("#operationForm").addEventListener("submit",async(event)=>{event.preventDefault();const v=Object.fromEntries(new FormData(event.currentTarget));try{await api("/api/v1/operations",{method:"POST",headers:{"Idempotency-Key":`web-${crypto.randomUUID()}`},body:JSON.stringify({line_id:v.line_id,kind:v.kind,requested_by:v.requested_by,request:{deployment:v.deployment,note:v.note}})});$("#operationModal").classList.add("hidden");await loadAll();switchView("operations");toast("任务已进入队列");}catch(error){$("#operationError").textContent=error.message;}});

$$('.nav-item').forEach((item)=>item.addEventListener("click",()=>switchView(item.dataset.view)));
$("#contextAction").addEventListener("click",()=>{if(state.view==="devices"){const form=$("#deviceForm");form.reset();form.elements.id.readOnly=false;$("#deviceError").textContent="";$("#deviceModal").classList.remove("hidden");}else{$("#lineError").textContent="";$("#lineModal").classList.remove("hidden");}});
$("#refreshButton").addEventListener("click",()=>loadAll().catch(()=>{}));
$$('.close-device').forEach((x)=>x.addEventListener("click",()=>$("#deviceModal").classList.add("hidden")));$$('.close-line').forEach((x)=>x.addEventListener("click",()=>$("#lineModal").classList.add("hidden")));$$('.close-operation').forEach((x)=>x.addEventListener("click",()=>$("#operationModal").classList.add("hidden")));$$('.close-detail').forEach((x)=>x.addEventListener("click",()=>$("#detailModal").classList.add("hidden")));
$("#deviceSearch").addEventListener("input",filterDevices);$("#deviceStatus").addEventListener("change",filterDevices);$("#lineSearch").addEventListener("input",filterLines);$("#lineStatus").addEventListener("change",filterLines);$("#topologyLine").addEventListener("change",renderOverviewTopology);
$("#operationLine").addEventListener("change",async()=>{const id=$("#operationLine").value,data=await api(`/api/v1/operations?limit=100${id?`&line_id=${encodeURIComponent(id)}`:""}`);state.operations=data.operations||[];renderOperations();});$("#reloadOperations").addEventListener("click",()=>$("#operationLine").dispatchEvent(new Event("change")));
if(state.token)loadAll().catch(()=>{});else $("#authModal").classList.remove("hidden");
setInterval(()=>{if(state.token&&document.visibilityState==="visible")loadAll().catch(()=>{});},10000);
