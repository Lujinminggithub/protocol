export const RANGE_SECONDS={"30m":1800,"2h":7200,"6h":21600,"24h":86400,"7d":604800,"30d":2592000};

export function presetRange(name,now=new Date()) {
  const seconds=RANGE_SECONDS[name];
  if(!seconds)throw new Error("未知时间范围");
  return {from:new Date(now.getTime()-seconds*1000),to:new Date(now),live:true};
}

export function parseLocalRange(fromValue,toValue) {
  const from=new Date(fromValue),to=new Date(toValue);
  if(!Number.isFinite(from.getTime())||!Number.isFinite(to.getTime())||from>=to)throw new Error("开始时间必须早于结束时间");
  if(to-from>365*24*60*60*1000)throw new Error("单次查询不能超过 365 天");
  return {from,to,live:false};
}

export function centeredRange(observedAt,minutes=15) {
  const center=new Date(observedAt);
  if(!Number.isFinite(center.getTime()))throw new Error("事件时间无效");
  const radius=minutes*60*1000;
  return {from:new Date(center.getTime()-radius),to:new Date(center.getTime()+radius),live:false};
}

export function localInputValue(value) {
  const date=value instanceof Date?value:new Date(value);
  if(!Number.isFinite(date.getTime()))return "";
  const local=new Date(date.getTime()-date.getTimezoneOffset()*60000);
  return local.toISOString().slice(0,16);
}
