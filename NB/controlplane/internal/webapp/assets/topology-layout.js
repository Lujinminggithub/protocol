export function healthTone(value) {
  const health=value==="healthy"?"ok":(value||"unknown");
  if(["down","offline","unhealthy"].includes(health))return "critical";
  if(["degraded","unknown"].includes(health))return "warning";
  return "normal";
}

export function initialPositions(devices,links) {
  const ids=new Set(devices.map((item)=>item.id));
  const incoming=new Map(devices.map((item)=>[item.id,0]));
  for(const link of links)if(ids.has(link.source)&&ids.has(link.target))incoming.set(link.target,(incoming.get(link.target)||0)+1);
  const depth=new Map(),queue=[];
  for(const device of devices)if((incoming.get(device.id)||0)===0){depth.set(device.id,0);queue.push(device.id);}
  if(!queue.length&&devices.length){depth.set(devices[0].id,0);queue.push(devices[0].id);}
  while(queue.length){
    const current=queue.shift(),base=depth.get(current)||0;
    for(const link of links)if(link.source===current&&ids.has(link.target)&&!depth.has(link.target)){
      depth.set(link.target,base+1);queue.push(link.target);
    }
  }
  const max=Math.max(0,...depth.values()),columns=new Map();
  for(const device of devices){const column=depth.get(device.id)??0;if(!columns.has(column))columns.set(column,[]);columns.get(column).push(device.id);}
  const result={};
  for(const [column,columnIDs] of columns)columnIDs.sort().forEach((id,index)=>{
    const stored=devices.find((item)=>item.id===id)?.layout;
    result[id]=stored?{x:stored.x,y:stored.y,z:stored.z}:{x:(column-max/2)*150,y:(index-(columnIDs.length-1)/2)*95,z:0};
  });
  return result;
}
