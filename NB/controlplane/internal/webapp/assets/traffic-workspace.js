import {TrafficCharts} from "./traffic-charts.js";
import {centeredRange,localInputValue,parseLocalRange,presetRange} from "./traffic-time.js";

export class TrafficWorkspace{
  constructor(root,{token}){
    this.root=root;this.token=token;this.charts=null;this.activePreset="2h";
    this.bind();
  }
  bind(){
    this.closeButton=this.root.querySelector("[data-traffic-close]");
    this.liveButton=this.root.querySelector("[data-traffic-live]");
    this.fromInput=this.root.querySelector("[data-traffic-from]");
    this.toInput=this.root.querySelector("[data-traffic-to]");
    this.status=this.root.querySelector("[data-traffic-status]");
    this.events=this.root.querySelector("[data-traffic-events]");
    this.error=this.root.querySelector("[data-traffic-range-error]");
    this.closeButton.addEventListener("click",()=>this.close());
    this.liveButton.addEventListener("click",()=>this.applyPreset(this.activePreset||"2h"));
    this.root.querySelector("[data-traffic-apply]").addEventListener("click",()=>this.applyCustom());
    this.root.querySelectorAll("[data-traffic-preset]").forEach((button)=>button.addEventListener("click",()=>this.applyPreset(button.dataset.trafficPreset)));
    document.addEventListener("keydown",(event)=>{if(event.key==="Escape"&&!this.root.classList.contains("hidden"))this.close();});
  }
  async open({id,name}){
    this.close();
    this.lineID=id;this.root.querySelector("[data-traffic-title]").textContent=name||id;
    this.root.querySelector("[data-traffic-subtitle]").textContent=`${id} · 分段上下行、任务与事故证据`;
    this.root.classList.remove("hidden");document.body.classList.add("traffic-workspace-open");
    this.charts=new TrafficCharts(this.root.querySelector("[data-traffic-charts]"),{
      lineID:id,token:this.token,onHistory:(history)=>this.renderEvents(history.markers||[]),
      onStatus:(message,online)=>{this.status.textContent=message;this.status.classList.toggle("online",online);}
    });
    this.charts.mount();await this.applyPreset("2h");
  }
  close(){
    this.charts?.destroy();this.charts=null;this.root.classList.add("hidden");
    document.body.classList.remove("traffic-workspace-open");
  }
  setRangeInputs(range){
    this.fromInput.value=localInputValue(range.from);this.toInput.value=localInputValue(range.to);
  }
  async applyPreset(name){
    if(!this.charts)return;
    const range=presetRange(name);this.activePreset=name;this.error.textContent="";
    this.root.querySelectorAll("[data-traffic-preset]").forEach((button)=>button.classList.toggle("active",button.dataset.trafficPreset===name));
    this.liveButton.classList.add("hidden");this.setRangeInputs(range);await this.charts.setRange(range);
  }
  async applyCustom(){
    if(!this.charts)return;
    try{
      const range=parseLocalRange(this.fromInput.value,this.toInput.value);this.error.textContent="";
      this.root.querySelectorAll("[data-traffic-preset]").forEach((button)=>button.classList.remove("active"));
      this.liveButton.classList.remove("hidden");await this.charts.setRange(range);
    }catch(error){this.error.textContent=error.message;}
  }
  async centerMarker(marker){
    const range=centeredRange(marker.observed_at);this.setRangeInputs(range);this.liveButton.classList.remove("hidden");
    this.root.querySelectorAll("[data-traffic-preset]").forEach((button)=>button.classList.remove("active"));
    await this.charts.setRange(range);
  }
  renderEvents(markers){
    this.events.replaceChildren();
    if(!markers.length){
      const empty=document.createElement("p");empty.className="traffic-events-empty";empty.textContent="当前时段没有任务、事故或配置变更";this.events.append(empty);return;
    }
    for(const marker of markers){
      const button=document.createElement("button");button.type="button";button.className=`traffic-event traffic-event-${marker.kind}`;
      const time=document.createElement("time");time.dateTime=marker.observed_at;time.textContent=new Date(marker.observed_at).toLocaleString("zh-CN",{hour12:false});
      const label=document.createElement("strong");label.textContent=marker.label;
      const kind=document.createElement("small");kind.textContent=marker.kind.replace("_"," ");
      button.append(time,label,kind);button.addEventListener("click",()=>this.centerMarker(marker));this.events.append(button);
    }
  }
}
