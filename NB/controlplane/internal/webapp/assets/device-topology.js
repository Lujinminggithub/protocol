import * as THREE from "./vendor/three-0.185.1.module.min.js";

const HEALTH_COLORS={ok:0x25a66f,healthy:0x25a66f,degraded:0xd7862d,down:0xc4473d,offline:0xc4473d,unknown:0x9aa7ab};

function deviceHealth(device) { return device.health==="healthy"?"ok":(device.health||"unknown"); }
function escapeHTML(value) { const element=document.createElement("span");element.textContent=String(value??"");return element.innerHTML; }
function computerModel(device) {
  const group=new THREE.Group(),computer=new THREE.Group(),health=deviceHealth(device),body=new THREE.MeshStandardMaterial({color:0xdce4e5,roughness:.58,metalness:.15}),dark=new THREE.MeshStandardMaterial({color:0x173c43,roughness:.4}),base=new THREE.MeshStandardMaterial({color:0x83979a,roughness:.55});
  const monitor=new THREE.Mesh(new THREE.BoxGeometry(11,7,.9),body);monitor.position.y=2.2;computer.add(monitor);
  const screen=new THREE.Mesh(new THREE.BoxGeometry(9.8,5.8,.18),dark);screen.position.set(0,2.2,.55);computer.add(screen);
  const stand=new THREE.Mesh(new THREE.BoxGeometry(1.1,2.2,.8),base);stand.position.y=-2.2;computer.add(stand);
  const foot=new THREE.Mesh(new THREE.BoxGeometry(5,.6,2.2),base);foot.position.y=-3.45;computer.add(foot);
  const light=new THREE.Mesh(new THREE.SphereGeometry(.42,12,8),new THREE.MeshStandardMaterial({color:HEALTH_COLORS[health]||HEALTH_COLORS.unknown,emissive:HEALTH_COLORS[health]||HEALTH_COLORS.unknown,emissiveIntensity:.8}));light.position.set(4.6,-.5,.65);computer.add(light);
  computer.scale.setScalar(2.15);group.add(computer);
  const canvas=document.createElement("canvas"),ctx=canvas.getContext("2d");canvas.width=256;canvas.height=64;ctx.fillStyle="rgba(250,252,252,.92)";ctx.fillRect(0,0,256,64);ctx.fillStyle="#1d2b30";ctx.font="600 22px Segoe UI, sans-serif";ctx.textAlign="center";ctx.fillText(device.name||device.id,128,28);ctx.fillStyle="#66767c";ctx.font="15px Segoe UI, sans-serif";ctx.fillText(device.region||device.id,128,51);
  const label=new THREE.Sprite(new THREE.SpriteMaterial({map:new THREE.CanvasTexture(canvas),transparent:true,depthTest:false}));label.position.set(0,-10.5,0);label.scale.set(18,4.5,1);group.add(label);group.userData.deviceID=device.id;return group;
}
function fallbackMarkup(data) {
  if(!(data.devices||[]).length)return `<div class="topology-empty">尚未登记设备</div>`;
  return `<div class="topology-fallback">${data.devices.map((device)=>`<button type="button" data-fallback-device="${escapeHTML(device.id)}"><span class="computer-icon ${escapeHTML(deviceHealth(device))}"></span><strong>${escapeHTML(device.name||device.id)}</strong><small>${escapeHTML(device.region||"--")}</small></button>`).join("")}</div>`;
}

export class DeviceTopology {
  constructor(root,{onDevice,onLine}) { this.root=root;this.onDevice=onDevice;this.onLine=onLine;this.selectedLine=""; }
  render(source,filters={}) {
    this.source=source||{devices:[],links:[]};this.filters=filters;
    const lineID=filters.lineID||"",region=filters.region||"",role=filters.role||"",health=filters.health||"";
    let links=(this.source.links||[]).filter((link)=>(!lineID||link.line_id===lineID)&&(!role||link.role===role));
    const connected=new Set(links.flatMap((link)=>[link.source.id||link.source,link.target.id||link.target]));
    let devices=(this.source.devices||[]).filter((device)=>(!region||device.region===region)&&(!health||deviceHealth(device)===health)&&(!lineID&&!role||connected.has(device.id)));
    const allowed=new Set(devices.map((device)=>device.id));links=links.filter((link)=>allowed.has(link.source.id||link.source)&&allowed.has(link.target.id||link.target));
    const data={nodes:devices.map((device)=>({...device})),links:links.map((link)=>({...link}))};
    try {
      const canvas=document.createElement("canvas");
      if(!globalThis.ForceGraph3D||!(canvas.getContext("webgl2")||canvas.getContext("webgl")))throw new Error("WebGL unavailable");
      if(!this.graph)this.initialize();
      const saved=JSON.parse(localStorage.getItem("nbTopologyPositions")||"{}");
      data.nodes.forEach((node)=>{const position=saved[node.id];if(position){node.fx=position.x;node.fy=position.y;node.fz=position.z;}});
      this.graph.graphData(data);this.graph.width(this.root.clientWidth).height(this.root.clientHeight);clearTimeout(this.fitTimer);this.fitTimer=setTimeout(()=>{if(this.graph&&data.nodes.length)this.graph.zoomToFit(650,90);},900);
    } catch(error) { this.renderFallback({devices,links}); }
  }
  initialize() {
    this.root.innerHTML="";
    this.linkColor=(link)=>["down","degraded"].includes(link.health)?"#c4473d":link.line_id===this.selectedLine?"#087f8c":"#83a6aa";
    this.linkWidth=(link)=>link.line_id===this.selectedLine?1.2:.35;
    this.graph=globalThis.ForceGraph3D()(this.root).backgroundColor("#f8fafb").showNavInfo(false).nodeThreeObject(computerModel)
      .nodeLabel((node)=>`${node.name||node.id} · ${node.region||"--"} · ${deviceHealth(node)}`)
      .linkColor(this.linkColor).linkWidth(this.linkWidth).linkOpacity(.72)
      .onNodeClick((node)=>this.onDevice&&this.onDevice(node.id)).onLinkClick((link)=>this.focusLine(link.line_id))
      .onEngineStop(()=>this.savePositions());
    this.graph.d3Force("charge").strength(-110);this.graph.d3Force("link").distance(68);
    this.resizeObserver=new ResizeObserver(()=>{if(this.graph)this.graph.width(this.root.clientWidth).height(this.root.clientHeight);});this.resizeObserver.observe(this.root);
  }
  focusLine(lineID) { this.selectedLine=lineID||"";if(this.graph){this.graph.linkColor(this.linkColor).linkWidth(this.linkWidth);}if(lineID&&this.onLine)this.onLine(lineID); }
  savePositions() { if(!this.graph)return;const positions={};for(const node of this.graph.graphData().nodes)if(Number.isFinite(node.x))positions[node.id]={x:node.x,y:node.y,z:node.z};localStorage.setItem("nbTopologyPositions",JSON.stringify(positions)); }
  renderFallback(data) { this.graph=null;this.root.innerHTML=fallbackMarkup(data);this.root.querySelectorAll("[data-fallback-device]").forEach((button)=>button.addEventListener("click",()=>this.onDevice&&this.onDevice(button.dataset.fallbackDevice))); }
  destroy() { clearTimeout(this.fitTimer);if(this.resizeObserver)this.resizeObserver.disconnect();if(this.graph)this.graph._destructor();this.graph=null; }
}
