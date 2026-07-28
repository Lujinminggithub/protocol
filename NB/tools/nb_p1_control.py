#!/usr/bin/env python3
"""P1 租户/路由策略签名、原子下发和失败回滚。"""
from __future__ import annotations
import argparse,datetime as dt,hashlib,hmac,ipaddress,json,os,pathlib,re,secrets,shlex,time
import deploy,line_probe

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

def validate(policy:dict)->tuple[str,str]:
    if policy.get("schema_version")!=1 or not NAME.fullmatch(str(policy.get("fixed_exit",""))):raise ValueError("schema/fixed_exit 非法")
    tenants=policy.get("tenants");routes=policy.get("routes")
    if not isinstance(tenants,list)or not tenants or not isinstance(routes,list)or not routes:raise ValueError("tenants/routes 不能为空")
    tlines=[];seen=set()
    for t in tenants:
        name=str(t.get("name",""));values=[int(t.get(k,0)) for k in ("max_tcp","max_udp","rate_kbps","quota_mb")]
        if not NAME.fullmatch(name)or name in seen or not(0<=values[0]<=100000 and 0<=values[1]<=100000 and 0<=values[2]<=100000000 and 0<=values[3]<=10**9):raise ValueError(f"非法 tenant: {t}")
        seen.add(name);tlines.append(f"tenant {name} {' '.join(map(str,values))}")
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
    doc={"schema_version":1,"state":"approved","policy_id":hashlib.sha256(canonical(policy)).hexdigest()[:16],"created_at_utc":dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z"),"fixed_exit":policy["fixed_exit"],"tenant_sha256":hashlib.sha256(tenant_bytes).hexdigest(),"routes_sha256":hashlib.sha256(route_bytes).hexdigest()};sign(doc);(output/"manifest.json").write_text(json.dumps(doc,ensure_ascii=False,indent=2)+"\n",encoding="utf-8");return doc

def apply(bundle:pathlib.Path,execute:bool,socks_port:int=1080)->None:
    doc=json.loads((bundle/"manifest.json").read_text(encoding="utf-8"));verify(doc)
    tenant=(bundle/"tenant.conf").read_bytes();routes=(bundle/"exit_routes.conf").read_bytes()
    if hashlib.sha256(tenant).hexdigest()!=doc["tenant_sha256"]or hashlib.sha256(routes).hexdigest()!=doc["routes_sha256"]:raise ValueError("策略文件哈希不匹配")
    if not execute:print(json.dumps({"preflight":"ok","policy_id":doc["policy_id"]},ensure_ascii=False));return
    work=deploy.INSTANCE_WORK;service=deploy._service_name("entry")
    c=deploy.connect("entry");root=f"{work}/configs/{doc['policy_id']}";old_t=deploy.fetch_bytes(c,f"{work}/tenant.conf");old_r=deploy.fetch_bytes(c,f"{work}/exit_routes.conf")
    try:
        deploy.push_bytes(c,tenant,f"{root}/tenant.conf",0o600);deploy.push_bytes(c,routes,f"{root}/exit_routes.conf",0o600)
        deploy.run(c,f"ln -sfn {shlex.quote(root+'/tenant.conf')} {shlex.quote(work+'/tenant.conf.next')}; mv -Tf {shlex.quote(work+'/tenant.conf.next')} {shlex.quote(work+'/tenant.conf')}; ln -sfn {shlex.quote(root+'/exit_routes.conf')} {shlex.quote(work+'/exit_routes.conf.next')}; mv -Tf {shlex.quote(work+'/exit_routes.conf.next')} {shlex.quote(work+'/exit_routes.conf')}; systemctl kill -s HUP {shlex.quote(service + '.service')}")
        deployment=deploy._remote_current_deployment(c,"entry");deploy._verify_deployment_health(c,"entry",deployment,warmup=4)
        os.environ.setdefault("NB_SOCKS_USERNAME","");line_probe.run_integrity_probe(deploy._role_host("entry")["host"],socks_port)
    except Exception:
        deploy.push_bytes(c,old_t,f"{work}/tenant.conf",0o600);deploy.push_bytes(c,old_r,f"{work}/exit_routes.conf",0o600);deploy.run(c,f"systemctl kill -s HUP {shlex.quote(service + '.service')}");raise
    finally:c.close()
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
