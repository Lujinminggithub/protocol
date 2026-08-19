const COLORS={upstream:"#087f8c",downstream:"#d36b32",grid:"#e5ebed",text:"#58656d"};
const ROLE_LABELS={entry:"Entry 接入",middle:"Middle 中继",exit:"Exit 出口"};

function pointTime(point){return new Date(point.observed_at).getTime();}
function roleSeries(history,role){return (history.roles||[]).find((item)=>item.role===role)||{role,points:[]};}
function addGaps(points,field,resolution){
  const result=[];
  for(let index=0;index<points.length;index++){
    const point=points[index],at=pointTime(point);
    if(index&&at-pointTime(points[index-1])>resolution*2000)result.push([pointTime(points[index-1])+resolution*1000,null]);
    result.push([at,Number(point[field]||0)]);
  }
  return result;
}
function markerColor(kind){
  if(kind==="incident")return "#b33a3a";
  if(kind==="operation_event")return "#a26100";
  if(kind==="operation")return "#66767c";
  return "#087f8c";
}
function chartOption(title,series,resolution,overview,markers){
  const points=series.points||[],markLines=(markers||[]).map((marker)=>({
    xAxis:new Date(marker.observed_at).getTime(),name:marker.label,
    lineStyle:{color:markerColor(marker.kind),type:"dashed",width:1},label:{show:false}
  }));
  return {
    animation:false,color:[COLORS.upstream,COLORS.downstream],
    title:{text:title,left:12,top:7,textStyle:{fontSize:13,fontWeight:650,color:"#26343a"}},
    legend:{top:6,right:14,itemWidth:16,textStyle:{fontSize:11,color:COLORS.text}},
    grid:{left:58,right:24,top:44,bottom:overview?58:34},
    tooltip:{trigger:"axis",axisPointer:{type:"cross"},confine:true,valueFormatter:(value)=>value==null?"无采样":`${Number(value).toFixed(2)} Mbps`},
    axisPointer:{link:[{xAxisIndex:"all"}]},
    xAxis:{type:"time",axisLabel:{fontSize:10,color:COLORS.text,hideOverlap:true},axisLine:{lineStyle:{color:COLORS.grid}},splitLine:{show:false}},
    yAxis:{type:"value",min:0,axisLabel:{fontSize:10,color:COLORS.text,formatter:"{value} Mbps"},splitLine:{lineStyle:{color:COLORS.grid}}},
    dataZoom:overview?[{type:"inside",xAxisIndex:0},{type:"slider",height:18,bottom:8,borderColor:"transparent",backgroundColor:"#eef2f3",fillerColor:"rgba(8,127,140,.14)"}]:[{type:"inside",xAxisIndex:0}],
    series:[
      {name:"上行",type:"line",showSymbol:false,connectNulls:false,lineStyle:{width:2},areaStyle:overview?{opacity:.07}:undefined,data:addGaps(points,"upstream_mbps",resolution),markLine:overview&&markLines.length?{silent:true,symbol:"none",data:markLines}:undefined},
      {name:"下行",type:"line",showSymbol:false,connectNulls:false,lineStyle:{width:2},areaStyle:overview?{opacity:.05}:undefined,data:addGaps(points,"downstream_mbps",resolution)}
    ]
  };
}

export class TrafficCharts{
  constructor(root,{lineID,token,onHistory,onStatus}){
    this.root=root;this.lineID=lineID;this.token=token;this.onHistory=onHistory;this.onStatus=onStatus;
    this.charts=[];this.abortController=null;this.reloadTimer=0;this.live=true;this.range=null;
  }
  mount(){
    if(this.root.id==="lineTrafficCharts"){this.mountSummary();return;}
    this.root.innerHTML=`<div class="traffic-error" role="alert"></div><div class="traffic-chart-panel traffic-overview" data-chart="overview"></div><div class="traffic-hop-stack"><div class="traffic-chart-panel" data-chart="entry"></div><div class="traffic-chart-panel" data-chart="middle"></div><div class="traffic-chart-panel" data-chart="exit"></div></div>`;
    for(const key of ["overview","entry","middle","exit"]){
      const chart=globalThis.echarts.init(this.root.querySelector(`[data-chart="${key}"]`),null,{renderer:"canvas"});
      chart.group=`traffic-${this.lineID}`;this.charts.push({key,chart});
    }
    globalThis.echarts.connect(`traffic-${this.lineID}`);
    this.resizeObserver=new ResizeObserver(()=>this.charts.forEach(({chart})=>chart.resize()));
    this.resizeObserver.observe(this.root);this.connectStream();
  }
  async mountSummary(){
    this.root.classList.add("traffic-summary");
    this.root.innerHTML=`<div><strong>正在读取最近流量</strong><small>线路分段流量与事件证据将在独立分析页展示</small></div><button class="primary" type="button">打开流量分析</button>`;
    this.root.querySelector("button").addEventListener("click",()=>this.root.dispatchEvent(new CustomEvent("nb:open-traffic",{bubbles:true,detail:{lineID:this.lineID}})));
    const to=new Date(),from=new Date(to.getTime()-30*60*1000);
    try{
      const query=new URLSearchParams({from:from.toISOString(),to:to.toISOString(),resolution:"auto"});
      const response=await fetch(`/api/v1/lines/${encodeURIComponent(this.lineID)}/traffic?${query}`,{headers:{Authorization:`Bearer ${this.token}`}});
      const body=await response.json();if(!response.ok)throw new Error(body.error||`HTTP ${response.status}`);
      const points=roleSeries(body,"entry").points||[],latest=points.at(-1);
      const strong=this.root.querySelector("strong");
      strong.textContent=latest?`最近上行 ${Number(latest.upstream_mbps||0).toFixed(2)} / 下行 ${Number(latest.downstream_mbps||0).toFixed(2)} Mbps`:"最近 30 分钟无流量采样";
    }catch(error){this.root.querySelector("strong").textContent="最近流量读取失败";}
  }
  async setRange({from,to,live=false}){
    this.range={from:new Date(from),to:new Date(to)};this.live=Boolean(live);return this.load();
  }
  currentRange(){
    if(!this.range)return null;
    if(!this.live)return this.range;
    const span=this.range.to-this.range.from,to=new Date();
    return {from:new Date(to.getTime()-span),to};
  }
  async load(){
    const range=this.currentRange();if(!range)return;
    this.onStatus?.(this.live?"正在加载实时数据":"正在加载历史数据",false);
    try{
      const query=new URLSearchParams({from:range.from.toISOString(),to:range.to.toISOString(),resolution:"auto"});
      const response=await fetch(`/api/v1/lines/${encodeURIComponent(this.lineID)}/traffic?${query}`,{headers:{Authorization:`Bearer ${this.token}`}});
      const body=await response.json().catch(()=>({}));if(!response.ok)throw new Error(body.error||`请求失败 (${response.status})`);
      this.history=body;this.render();this.root.querySelector(".traffic-error").textContent="";
      this.onHistory?.(body);this.onStatus?.(this.live?"实时数据已连接":"历史数据已加载",true);
    }catch(error){
      this.root.querySelector(".traffic-error").textContent=`曲线加载失败：${error.message}`;
      this.onStatus?.("数据加载失败",false);
    }
  }
  render(){
    if(!this.history)return;
    const resolution=Number(this.history.resolution_s||15),entry=roleSeries(this.history,"entry");
    for(const {key,chart} of this.charts){
      const series=key==="overview"?entry:roleSeries(this.history,key);
      chart.setOption(chartOption(key==="overview"?"线路边界总流量":ROLE_LABELS[key],series,resolution,key==="overview",this.history.markers),true);
    }
  }
  scheduleReload(){if(!this.live)return;clearTimeout(this.reloadTimer);this.reloadTimer=setTimeout(()=>this.load(),350);}
  async connectStream(){
    if(this.abortController)this.abortController.abort();this.abortController=new AbortController();
    while(!this.abortController.signal.aborted){
      try{
        const response=await fetch(`/api/v1/lines/${encodeURIComponent(this.lineID)}/traffic/stream`,{headers:{Authorization:`Bearer ${this.token}`},signal:this.abortController.signal});
        if(!response.ok||!response.body)throw new Error(`HTTP ${response.status}`);
        const reader=response.body.getReader(),decoder=new TextDecoder();let buffer="";
        while(true){
          const {done,value}=await reader.read();if(done)break;buffer+=decoder.decode(value,{stream:true});
          const events=buffer.split("\n\n");buffer=events.pop()||"";
          for(const event of events)if(event.includes("event: snapshot"))this.scheduleReload();
        }
      }catch(error){if(this.abortController.signal.aborted)return;this.onStatus?.("实时数据正在重连",false);}
      await new Promise((resolve)=>setTimeout(resolve,2000));
    }
  }
  destroy(){
    clearTimeout(this.reloadTimer);this.abortController?.abort();this.resizeObserver?.disconnect();
    this.charts.forEach(({chart})=>chart.dispose());this.charts=[];
  }
}
