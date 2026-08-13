#!/usr/bin/env python3
"""P1 租户/路由策略签名、原子下发和失败回滚。"""
from __future__ import annotations
import argparse,datetime as dt,hashlib,hmac,ipaddress,json,os,pathlib,re,secrets,shlex,time
import deploy,line_probe,nb_shard_deploy

NAME=re.compile(r"^[A-Za-z0-9_.-]{1,63}$")
def key()->bytes:
    value=os.environ.get("NB_CONTROL_SIGNING_KEY","").encode()
    if not value:
        path=pathlib.Path(os.environ.get("NB_CONTROL_SIGNING_KEY_FILE",
            str(deploy.SECURITY_DIR/"control-signing.key")))
        if path.is_file():value=path.read_bytes().strip()
    if len(value)<32:raise ValueError("控制面签名密钥至少需要 32 字节，请设置环境变量或密钥文件")
    return value
def canonical(v:dict)->bytes:return json.dumps(v,sort_keys=True,separators=(",",":"),ensure_ascii=False).encode()
def sign(v:dict)->None:v.pop("signature",None);v["signature"]=hmac.new(key(),canonical(v),hashlib.sha256).hexdigest()
def verify(v:dict)->None:
    sig=v.get("signature","");unsigned={k:x for k,x in v.items() if k!="signature"}
    if not hmac.compare_digest(sig,hmac.new(key(),canonical(unsigned),hashlib.sha256).hexdigest()):raise ValueError("策略签名无效")

def _fnv_u64(value:int,h:int)->int:
    for shift in range(0,64,8):
        h^=(value>>shift)&0xff;h=(h*1099511628211)&0xffffffffffffffff
    return h

def tenant_fingerprint(policy:dict)->int:
    h=1469598103934665603
    for tenant in policy["tenants"]:
        for byte in str(tenant["name"]).encode("ascii"):
            h^=byte;h=(h*1099511628211)&0xffffffffffffffff
        up=int(tenant.get("rate_up_kbps",tenant.get("rate_kbps",0)))*1000//8
        down=int(tenant.get("rate_down_kbps",tenant.get("rate_kbps",0)))*1000//8
        if int(policy["schema_version"])==3:
            burst_up=int(tenant.get("burst_up_bytes",0));burst_down=int(tenant.get("burst_down_bytes",0))
        else:
            burst_up=up*int(tenant.get("burst_up_seconds",tenant.get("burst_seconds",1)))
            burst_down=down*int(tenant.get("burst_down_seconds",tenant.get("burst_seconds",1)))
        values=(int(tenant.get("max_tcp",0)),int(tenant.get("max_udp",0)),up,down,
                burst_up,burst_down,int(tenant.get("quota_mb",0))*1024*1024)
        for value in values:h=_fnv_u64(value,h)
    return h

def validate(policy:dict)->tuple[str,str]:
    schema=int(policy.get("schema_version",0))
    if schema not in (1,2,3) or not NAME.fullmatch(str(policy.get("fixed_exit",""))):raise ValueError("schema/fixed_exit 非法")
    tenants=policy.get("tenants");routes=policy.get("routes")
    if not isinstance(tenants,list)or not tenants or not isinstance(routes,list)or not routes:raise ValueError("tenants/routes 不能为空")
    tlines=[];seen=set()
    for t in tenants:
        name=str(t.get("name",""));max_tcp=int(t.get("max_tcp",0));max_udp=int(t.get("max_udp",0));quota=int(t.get("quota_mb",0))
        if schema==3:
            up=int(t.get("rate_up_kbps",0));down=int(t.get("rate_down_kbps",0))
            up_burst=int(t.get("burst_up_bytes",0));down_burst=int(t.get("burst_down_bytes",0))
            valid_rates=(0<=up<=100000000 and 0<=down<=100000000 and
                0<=up_burst<=1073741824 and 0<=down_burst<=1073741824 and
                (up==0 or up_burst>0) and (down==0 or down_burst>0))
            rendered=f"tenant-v3 {name} {max_tcp} {max_udp} {up} {down} {quota} {up_burst} {down_burst}"
        elif "rate_up_kbps" in t or "rate_down_kbps" in t:
            up=int(t.get("rate_up_kbps",0));down=int(t.get("rate_down_kbps",0));up_burst=int(t.get("burst_up_seconds",1));down_burst=int(t.get("burst_down_seconds",1))
            valid_rates=0<=up<=100000000 and 0<=down<=100000000 and 1<=up_burst<=60 and 1<=down_burst<=60
            rendered=f"tenant {name} {max_tcp} {max_udp} {up} {down} {quota} {up_burst} {down_burst}"
        else:
            rate=int(t.get("rate_kbps",0));burst=int(t.get("burst_seconds",1));valid_rates=0<=rate<=100000000 and 1<=burst<=60
            rendered=f"tenant {name} {max_tcp} {max_udp} {rate} {quota} {burst}"
        if not NAME.fullmatch(name)or name in seen or not(0<=max_tcp<=100000 and 0<=max_udp<=100000 and 0<=quota<=10**9 and valid_rates):raise ValueError(f"非法 tenant: {t}")
        seen.add(name);tlines.append(rendered)
    rlines=[];seen=set();fixed=str(policy["fixed_exit"])
    for r in routes:
        name=str(r.get("name",""));host=str(r.get("host",""));port=int(r.get("port",4443));weight=int(r.get("weight",1));capacity=int(r.get("capacity",0))
        try:ipaddress.ip_address(host)
        except ValueError:
            if not NAME.fullmatch(host):raise ValueError(f"非法 route host: {host}")
        if r.get("fixed_exit")!=fixed or not NAME.fullmatch(name)or name in seen or not(1<=port<=65535 and 1<=weight<=1000 and 0<=capacity<=100000):raise ValueError(f"非法 route: {r}")
        seen.add(name);rlines.append(f"route {name} H:{host}:{port} {weight} {capacity}")
    return "\n".join(tlines)+"\n","\n".join(rlines)+"\n"

def prepare(policy_path:pathlib.Path,output:pathlib.Path)->dict:
    policy=json.loads(policy_path.read_text(encoding="utf-8"));tenants,routes=validate(policy);output.mkdir(parents=True,exist_ok=True)
    tenant_bytes=tenants.encode("ascii");route_bytes=routes.encode("ascii")
    (output/"tenant.conf").write_bytes(tenant_bytes);(output/"exit_routes.conf").write_bytes(route_bytes)
    fingerprint=tenant_fingerprint(policy)
    doc={"schema_version":2,"state":"approved","policy_id":hashlib.sha256(canonical(policy)).hexdigest()[:16],"created_at_utc":dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z"),"fixed_exit":policy["fixed_exit"],"tenant_sha256":hashlib.sha256(tenant_bytes).hexdigest(),"tenant_fingerprint":f"{fingerprint:016x}","routes_sha256":hashlib.sha256(route_bytes).hexdigest()};sign(doc);(output/"manifest.json").write_text(json.dumps(doc,ensure_ascii=False,indent=2)+"\n",encoding="utf-8");return doc

def _remote_control_command(control:str,command:str)->str:
    script=("import socket;"+f"s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect({control!r});"+
        f"s.sendall(({command!r}+'\\n').encode());data=s.recv(32768);s.close();print(data.decode(),end='')")
    return "python3 -c "+shlex.quote(script)

def invoke_role(role:str,command:str)->list[dict]:
    connection=deploy.connect(role);responses=[]
    try:
        for worker in range(deploy._effective_workers(role)):
            control=nb_shard_deploy.line_control_path(deploy.DEPLOY_INSTANCE,role,worker)
            raw=deploy.checked_run(connection,_remote_control_command(control,command),tmo=15).strip()
            response=json.loads(raw.splitlines()[-1])
            if response.get("error"):raise RuntimeError(f"{role}[{worker}] 拒绝限速策略: {response['error']}")
            responses.append(response)
    finally:connection.close()
    return responses

def _remote_read(role:str,path:str)->bytes:
    connection=deploy.connect(role)
    try:return deploy.fetch_bytes(connection,path)
    finally:connection.close()

def _remote_push(role:str,data:bytes,path:str)->None:
    connection=deploy.connect(role)
    try:deploy.push_bytes(connection,data,path,0o600)
    finally:connection.close()

def _publish(role:str,source:str,target:str)->None:
    connection=deploy.connect(role);temporary=target+".next"
    try:deploy.checked_run(connection," && ".join((f"test -f {shlex.quote(source)}",
        f"ln -sfn {shlex.quote(source)} {shlex.quote(temporary)}",
        f"mv -Tf {shlex.quote(temporary)} {shlex.quote(target)}")),tmo=15)
    finally:connection.close()

def _verify_role(role:str,fingerprint:str)->None:
    responses=invoke_role(role,"tenant status")
    mismatched=[index for index,response in enumerate(responses)
        if str(response.get("fingerprint","")).lower()!=fingerprint.lower()]
    if mismatched:raise RuntimeError(f"{role} 限速策略指纹读回不一致 workers={mismatched}")

def _rollback_role(role:str,old:bytes,transaction:int,root:str)->None:
    rollback=f"{root}/rollback-{role}.conf";target=f"{deploy.INSTANCE_WORK}/tenant.conf"
    _remote_push(role,old,rollback);invoke_role(role,f"tenant prepare {transaction} {rollback}")
    _publish(role,rollback,target);invoke_role(role,f"tenant commit {transaction}")

def verify_integrity_after_reload(socks_port:int)->None:
    last_error=None
    for attempt in range(3):
        try:
            line_probe.run_entry_local_probe("integrity",socks_port)
            return
        except (OSError,RuntimeError,TimeoutError) as error:
            last_error=error
            if attempt<2:
                delay=2*(attempt+1)
                print(f"策略热加载后的完整性探针暂时不可用，{delay} 秒后重试",flush=True)
                time.sleep(delay)
    raise RuntimeError(f"策略热加载后完整性探针连续失败: {last_error}")

def apply(bundle:pathlib.Path,execute:bool,socks_port:int=1080)->None:
    doc=json.loads((bundle/"manifest.json").read_text(encoding="utf-8"));verify(doc)
    tenant=(bundle/"tenant.conf").read_bytes();routes=(bundle/"exit_routes.conf").read_bytes()
    if hashlib.sha256(tenant).hexdigest()!=doc["tenant_sha256"]or hashlib.sha256(routes).hexdigest()!=doc["routes_sha256"]:raise ValueError("策略文件哈希不匹配")
    if not execute:print(json.dumps({"preflight":"ok","policy_id":doc["policy_id"]},ensure_ascii=False));return
    work=deploy.INSTANCE_WORK;root=f"{work}/configs/{doc['policy_id']}";transaction=int(doc["policy_id"],16)
    old_t={role:_remote_read(role,f"{work}/tenant.conf") for role in ("exit","entry")}
    old_r=_remote_read("entry",f"{work}/exit_routes.conf")
    if routes!=old_r:raise ValueError("路由变更必须走完整原子部署，限速热更新只接受租户策略")
    prepared=[];published=[]
    try:
        for role in ("exit","entry"):_remote_push(role,tenant,f"{root}/tenant.conf")
        for role in ("exit","entry"):
            invoke_role(role,f"tenant prepare {transaction} {root}/tenant.conf");prepared.append(role)
        for role in ("exit","entry"):
            _publish(role,f"{root}/tenant.conf",f"{work}/tenant.conf");published.append(role)
            invoke_role(role,f"tenant commit {transaction}");_verify_role(role,doc["tenant_fingerprint"])
        os.environ.setdefault("NB_SOCKS_USERNAME","");verify_integrity_after_reload(socks_port)
    except Exception as original:
        for role in reversed(prepared):
            try:invoke_role(role,f"tenant abort {transaction}")
            except Exception:pass
        rollback_transaction=(transaction+1)&0xffffffffffffffff or 1;errors=[]
        for role in reversed(published):
            try:_rollback_role(role,old_t[role],rollback_transaction,root)
            except Exception as error:errors.append(f"{role}: {error}")
        if errors:raise RuntimeError(f"限速策略提交失败且回滚不完整: {original}; {'; '.join(errors)}") from original
        raise
    doc["state"]="active";doc["activated_at_utc"]=dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z");sign(doc);(bundle/"manifest.json").write_text(json.dumps(doc,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")

def keygen(output:pathlib.Path)->None:
    if output.exists():raise ValueError(f"签名密钥已存在，拒绝覆盖: {output}")
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(secrets.token_hex(32)+"\n",encoding="ascii")
    try:output.chmod(0o600)
    except OSError:pass
    print(f"控制面签名密钥已生成: {output}")

def main()->None:
    parser=argparse.ArgumentParser();sub=parser.add_subparsers(dest="command",required=True)
    k=sub.add_parser("keygen");k.add_argument("--output",type=pathlib.Path,default=deploy.SECURITY_DIR/"control-signing.key")
    p=sub.add_parser("prepare");p.add_argument("policy",type=pathlib.Path);p.add_argument("output",type=pathlib.Path)
    a=sub.add_parser("apply");a.add_argument("bundle",type=pathlib.Path);a.add_argument("--execute",action="store_true");a.add_argument("--socks-port",type=int,default=1080)
    args=parser.parse_args()
    if args.command=="keygen":keygen(args.output.resolve())
    elif args.command=="prepare":print(json.dumps(prepare(args.policy,args.output),ensure_ascii=False,indent=2))
    else:apply(args.bundle,args.execute,args.socks_port)
if __name__=="__main__":main()
