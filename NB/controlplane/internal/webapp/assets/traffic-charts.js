const COLORS = {upstream:"#087f8c",downstream:"#d36b32",grid:"#e5ebed",text:"#58656d"};
const ROLE_LABELS = {entry:"Entry",middle:"Middle",exit:"Exit"};
const RANGE_SECONDS = {"30m":1800,"2h":7200,"6h":21600,"24h":86400,"7d":604800,"30d":2592000};

function pointTime(point) { return new Date(point.observed_at).getTime(); }
function addGaps(points, field, resolution) {
  const result=[];
  for(let index=0;index<points.length;index++) {
    const point=points[index],at=pointTime(point);
    if(index&&at-pointTime(points[index-1])>resolution*2000) result.push([pointTime(points[index-1])+resolution*1000,null]);
    result.push([at,Number(point[field]||0)]);
  }
  return result;
}
function roleSeries(history,role) { return (history.roles||[]).find((item)=>item.role===role)||{role,points:[]}; }
function chartOption(title,series,resolution,overview=false,markers=[]) {
  const points=series.points||[];
  const markLines=markers.map((marker)=>({
    xAxis:new Date(marker.observed_at).getTime(),
    name:marker.label,
    lineStyle:{color:marker.kind==="operation"?"#8a989d":"#087f8c",type:"dashed",width:1},
    label:{show:true,formatter:marker.label,position:"insideEndTop",fontSize:9,color:"#58656d"}
  }));
  return {
    animation:false,
    color:[COLORS.upstream,COLORS.downstream],
    title:{text:title,left:8,top:4,textStyle:{fontSize:12,fontWeight:650,color:"#26343a"}},
    legend:{top:3,right:8,itemWidth:14,textStyle:{fontSize:11,color:COLORS.text}},
    grid:{left:48,right:18,top:38,bottom:overview?52:28},
    tooltip:{trigger:"axis",axisPointer:{type:"cross"},valueFormatter:(value)=>value==null?"无采样":`${Number(value).toFixed(2)} Mbps`},
    axisPointer:{link:[{xAxisIndex:"all"}]},
    xAxis:{type:"time",axisLabel:{fontSize:10,color:COLORS.text},axisLine:{lineStyle:{color:COLORS.grid}},splitLine:{show:false}},
    yAxis:{type:"value",min:0,name:"Mbps",nameTextStyle:{fontSize:10,color:COLORS.text},axisLabel:{fontSize:10,color:COLORS.text},splitLine:{lineStyle:{color:COLORS.grid}}},
    dataZoom:overview?[{type:"inside",xAxisIndex:0},{type:"slider",height:16,bottom:5,borderColor:"transparent",backgroundColor:"#eef2f3",fillerColor:"rgba(8,127,140,.14)"}]:[{type:"inside",xAxisIndex:0}],
    series:[
      {name:"上行",type:"line",showSymbol:false,connectNulls:false,lineStyle:{width:2},areaStyle:overview?{opacity:.07}:undefined,data:addGaps(points,"upstream_mbps",resolution),markLine:overview&&markLines.length?{silent:true,symbol:"none",data:markLines}:undefined},
      {name:"下行",type:"line",showSymbol:false,connectNulls:false,lineStyle:{width:2},areaStyle:overview?{opacity:.05}:undefined,data:addGaps(points,"downstream_mbps",resolution)}
    ]
  };
}

export class TrafficCharts {
  constructor(root,{lineID,token}) {
    this.root=root; this.lineID=lineID; this.token=token; this.range="2h"; this.charts=[]; this.abortController=null; this.reloadTimer=0;
  }
  mount() {
    this.root.innerHTML=`<div class="traffic-toolbar"><div class="segmented" role="group" aria-label="曲线时间范围">${Object.keys(RANGE_SECONDS).map((range)=>`<button type="button" data-traffic-range="${range}" class="${range===this.range?"active":""}">${range}</button>`).join("")}</div><span class="traffic-live-status"><i></i><span>连接实时数据</span></span></div><div class="traffic-error" role="alert"></div><div class="traffic-overview" data-chart="overview"></div><div class="traffic-hop-grid"><div data-chart="entry"></div><div data-chart="middle"></div><div data-chart="exit"></div></div>`;
    this.root.querySelectorAll("[data-traffic-range]").forEach((button)=>button.addEventListener("click",()=>{this.range=button.dataset.trafficRange;this.root.querySelectorAll("[data-traffic-range]").forEach((item)=>item.classList.toggle("active",item===button));this.load();}));
    for(const key of ["overview","entry","middle","exit"]) {
      const chart=globalThis.echarts.init(this.root.querySelector(`[data-chart="${key}"]`),null,{renderer:"canvas"});
      chart.group=`traffic-${this.lineID}`; this.charts.push({key,chart});
    }
    globalThis.echarts.connect(`traffic-${this.lineID}`);
    this.resizeObserver=new ResizeObserver(()=>this.charts.forEach(({chart})=>chart.resize()));
    this.resizeObserver.observe(this.root);
    this.load(); this.connectStream();
  }
  async load() {
    const seconds=RANGE_SECONDS[this.range]||7200,to=new Date(),from=new Date(to.getTime()-seconds*1000);
    try {
      const response=await fetch(`/api/v1/lines/${encodeURIComponent(this.lineID)}/traffic?from=${encodeURIComponent(from.toISOString())}&to=${encodeURIComponent(to.toISOString())}&resolution=auto`,{headers:{Authorization:`Bearer ${this.token}`}});
      const body=await response.json().catch(()=>({})); if(!response.ok)throw new Error(body.error||`请求失败 (${response.status})`);
      this.history=body; this.render(); this.root.querySelector(".traffic-error").textContent="";
    } catch(error) { if(error.name!=="AbortError")this.root.querySelector(".traffic-error").textContent=`曲线加载失败：${error.message}`; }
  }
  render() {
    if(!this.history)return;
    const resolution=Number(this.history.resolution_s||15),entry=roleSeries(this.history,"entry");
    for(const {key,chart} of this.charts) {
      const series=key==="overview"?entry:roleSeries(this.history,key);
      chart.setOption(chartOption(key==="overview"?"线路边界总流量":ROLE_LABELS[key],series,resolution,key==="overview",this.history.markers||[]),true);
    }
  }
  setLiveStatus(text,online=false) { const status=this.root.querySelector(".traffic-live-status"); if(!status)return;status.classList.toggle("online",online);status.querySelector("span").textContent=text; }
  scheduleReload() { clearTimeout(this.reloadTimer);this.reloadTimer=setTimeout(()=>this.load(),350); }
  async connectStream() {
    if(this.abortController)this.abortController.abort(); this.abortController=new AbortController();
    while(!this.abortController.signal.aborted) {
      try {
        this.setLiveStatus("正在连接实时数据");
        const response=await fetch(`/api/v1/lines/${encodeURIComponent(this.lineID)}/traffic/stream`,{headers:{Authorization:`Bearer ${this.token}`},signal:this.abortController.signal});
        if(!response.ok||!response.body)throw new Error(`HTTP ${response.status}`);
        this.setLiveStatus("实时数据已连接",true);
        const reader=response.body.getReader(),decoder=new TextDecoder(); let buffer="";
        while(true) {
          const {done,value}=await reader.read(); if(done)break; buffer+=decoder.decode(value,{stream:true});
          const events=buffer.split("\n\n");buffer=events.pop()||"";
          for(const event of events)if(event.includes("event: snapshot"))this.scheduleReload();
        }
      } catch(error) { if(this.abortController.signal.aborted)return;this.setLiveStatus("实时数据重连中"); }
      await new Promise((resolve)=>setTimeout(resolve,2000));
    }
  }
  destroy() {
    clearTimeout(this.reloadTimer);if(this.abortController)this.abortController.abort();if(this.resizeObserver)this.resizeObserver.disconnect();
    this.charts.forEach(({chart})=>chart.dispose());this.charts=[];
  }
}
