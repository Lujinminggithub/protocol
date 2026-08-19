import * as THREE from "./vendor/three-0.185.1.module.min.js";
import {healthTone,initialPositions} from "./topology-layout.js";

const PALETTES={
  normal:{body:0xdce4e5,screen:0x173c43,base:0x83979a,light:0x25a66f},
  warning:{body:0xe7ad45,screen:0x754600,base:0xb97416,light:0xffbd45},
  critical:{body:0xd65349,screen:0x661b1b,base:0xa53131,light:0xff5147}
};
function valueID(value){return typeof value==="object"?value.id:value;}
function deviceHealth(device){return device.health==="healthy"?"ok":(device.health||"unknown");}
function escapeHTML(value){const element=document.createElement("span");element.textContent=String(value??"");return element.innerHTML;}
function computerModel(device){
  const group=new THREE.Group(),computer=new THREE.Group();
  const body=new THREE.MeshStandardMaterial({roughness:.58,metalness:.15});
  const dark=new THREE.MeshStandardMaterial({roughness:.4});
  const base=new THREE.MeshStandardMaterial({roughness:.55});
  const lightMaterial=new THREE.MeshStandardMaterial({emissiveIntensity:.8});
  const monitor=new THREE.Mesh(new THREE.BoxGeometry(11,7,.9),body);monitor.position.y=2.2;computer.add(monitor);
  const screen=new THREE.Mesh(new THREE.BoxGeometry(9.8,5.8,.18),dark);screen.position.set(0,2.2,.55);computer.add(screen);
  const stand=new THREE.Mesh(new THREE.BoxGeometry(1.1,2.2,.8),base);stand.position.y=-2.2;computer.add(stand);
  const foot=new THREE.Mesh(new THREE.BoxGeometry(5,.6,2.2),base);foot.position.y=-3.45;computer.add(foot);
  const light=new THREE.Mesh(new THREE.SphereGeometry(.42,12,8),lightMaterial);light.position.set(4.6,-.5,.65);computer.add(light);
  computer.scale.setScalar(2.15);group.add(computer);
  const canvas=document.createElement("canvas"),ctx=canvas.getContext("2d");canvas.width=256;canvas.height=64;
  ctx.fillStyle="rgba(250,252,252,.94)";ctx.fillRect(0,0,256,64);ctx.fillStyle="#1d2b30";ctx.font="600 22px Segoe UI, sans-serif";ctx.textAlign="center";ctx.fillText(device.name||device.id,128,28);
  ctx.fillStyle="#66767c";ctx.font="15px Segoe UI, sans-serif";ctx.fillText(device.region||device.id,128,51);
  const label=new THREE.Sprite(new THREE.SpriteMaterial({map:new THREE.CanvasTexture(canvas),transparent:true,depthTest:false}));
  label.position.set(0,-10.5,0);label.scale.set(18,4.5,1);group.add(label);
  group.userData.applyHealth=(health)=>{
    const palette=PALETTES[healthTone(health)];
    body.color.setHex(palette.body);dark.color.setHex(palette.screen);base.color.setHex(palette.base);
    lightMaterial.color.setHex(palette.light);lightMaterial.emissive.setHex(palette.light);
  };
  group.userData.applyHealth(deviceHealth(device));device.__model=group;return group;
}
function fallbackMarkup(data){
  if(!(data.devices||[]).length)return `<div class="topology-empty">尚未登记设备</div>`;
  return `<div class="topology-fallback">${data.devices.map((device)=>`<button type="button" data-fallback-device="${escapeHTML(device.id)}"><span class="computer-icon ${escapeHTML(deviceHealth(device))}"></span><strong>${escapeHTML(device.name||device.id)}</strong><small>${escapeHTML(device.region||"--")}</small></button>`).join("")}</div>`;
}

export class DeviceTopology{
  constructor(root,{onDevice,onLine,onSaveLayout}){
    this.root=root;this.onDevice=onDevice;this.onLine=onLine;this.onSaveLayout=onSaveLayout;this.selectedLine="";this.signature="";
  }
  filtered(source,filters){
    const lineID=filters.lineID||"",region=filters.region||"",role=filters.role||"",health=filters.health||"";
    let links=(source.links||[]).filter((link)=>(!lineID||link.line_id===lineID)&&(!role||link.role===role));
    const connected=new Set(links.flatMap((link)=>[valueID(link.source),valueID(link.target)]));
    const devices=(source.devices||[]).filter((device)=>(!region||device.region===region)&&(!health||deviceHealth(device)===health)&&(!lineID&&!role||connected.has(device.id)));
    const allowed=new Set(devices.map((device)=>device.id));
    links=links.filter((link)=>allowed.has(valueID(link.source))&&allowed.has(valueID(link.target)));
    return {devices,links};
  }
  render(source,filters={}){
    this.source=source||{devices:[],links:[]};this.filters=filters;
    const filtered=this.filtered(this.source,filters);
    const signature=JSON.stringify({filters,nodes:filtered.devices.map((item)=>item.id).sort(),links:filtered.links.map((item)=>item.id).sort()});
    try{
      const canvas=document.createElement("canvas");
      if(!globalThis.ForceGraph3D||!(canvas.getContext("webgl2")||canvas.getContext("webgl")))throw new Error("WebGL unavailable");
      if(!this.graph)this.initialize();
      if(signature===this.signature){this.mergeStatus(filtered);return;}
      this.signature=signature;
      const nodes=filtered.devices.map((device)=>({...device})),links=filtered.links.map((link)=>({...link,source:valueID(link.source),target:valueID(link.target)}));
      const positions=initialPositions(nodes,links);
      for(const node of nodes){const position=positions[node.id];node.fx=position.x;node.fy=position.y;node.fz=position.z;}
      this.graph.graphData({nodes,links}).width(this.root.clientWidth).height(this.root.clientHeight);
      clearTimeout(this.fitTimer);this.fitTimer=setTimeout(()=>{if(this.graph&&nodes.length)this.graph.zoomToFit(500,70);},500);
    }catch(error){this.renderFallback(filtered);}
  }
  mergeStatus(filtered){
    if(!this.graph)return;
    const incoming=new Map(filtered.devices.map((item)=>[item.id,item]));
    for(const node of this.graph.graphData().nodes){
      const latest=incoming.get(node.id);if(!latest)continue;
      const position={x:node.x,y:node.y,z:node.z,fx:node.fx,fy:node.fy,fz:node.fz,__model:node.__model};
      Object.assign(node,latest,position);node.__model?.userData.applyHealth(deviceHealth(node));
    }
    const links=new Map(filtered.links.map((item)=>[item.id,item]));
    for(const link of this.graph.graphData().links){const latest=links.get(link.id);if(latest)Object.assign(link,latest,{source:link.source,target:link.target});}
    this.graph.linkColor(this.linkColor).linkWidth(this.linkWidth);
  }
  initialize(){
    this.root.innerHTML="";
    this.linkColor=(link)=>["down","degraded"].includes(link.health)?"#c4473d":link.line_id===this.selectedLine?"#087f8c":"#83a6aa";
    this.linkWidth=(link)=>link.line_id===this.selectedLine?1:.28;
    this.graph=globalThis.ForceGraph3D()(this.root).backgroundColor("#f8fafb").showNavInfo(false).nodeThreeObject(computerModel)
      .nodeLabel((node)=>`${node.name||node.id} · ${node.region||"--"} · ${deviceHealth(node)}`)
      .linkColor(this.linkColor).linkWidth(this.linkWidth).linkOpacity(.68)
      .onNodeClick((node)=>this.onDevice?.(node.id)).onLinkClick((link)=>this.focusLine(link.line_id))
      .onNodeDragEnd((node)=>this.persistNode(node));
    this.graph.d3Force("charge").strength(-105);this.graph.d3Force("link").distance(70);
    this.resizeObserver=new ResizeObserver(()=>this.graph?.width(this.root.clientWidth).height(this.root.clientHeight));this.resizeObserver.observe(this.root);
  }
  persistNode(node){
    if(!Number.isFinite(node.x)||!Number.isFinite(node.y)||!Number.isFinite(node.z))return;
    node.fx=node.x;node.fy=node.y;node.fz=node.z;
    clearTimeout(this.saveTimer);this.saveTimer=setTimeout(()=>this.onSaveLayout?.([{device_id:node.id,x:node.x,y:node.y,z:node.z}]),250);
  }
  reset(source=this.source,filters=this.filters){
    this.signature="";this.resizeObserver?.disconnect();if(this.graph){this.graph._destructor();this.graph=null;}this.render(source,filters);
  }
  focusLine(lineID){this.selectedLine=lineID||"";if(this.graph)this.graph.linkColor(this.linkColor).linkWidth(this.linkWidth);if(lineID)this.onLine?.(lineID);}
  renderFallback(data){this.resizeObserver?.disconnect();if(this.graph){this.graph._destructor();this.graph=null;}this.root.innerHTML=fallbackMarkup(data);this.root.querySelectorAll("[data-fallback-device]").forEach((button)=>button.addEventListener("click",()=>this.onDevice?.(button.dataset.fallbackDevice)));}
  destroy(){clearTimeout(this.fitTimer);clearTimeout(this.saveTimer);this.resizeObserver?.disconnect();this.graph?._destructor();this.graph=null;}
}
