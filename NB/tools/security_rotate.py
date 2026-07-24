#!/usr/bin/env python3
"""NB 节点证书重叠轮换：prepare -> trust -> identity -> retire。"""
from __future__ import annotations

import argparse,datetime as dt,hashlib,hmac,json,os,pathlib,secrets,shlex,shutil,subprocess,time
import deploy

ROOT=pathlib.Path(__file__).resolve().parents[1]

def run(*args:str)->None: subprocess.run(args,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def sha(path:pathlib.Path)->str:return hashlib.sha256(path.read_bytes()).hexdigest()
def key()->bytes:
    value=os.environ.get("NB_ROTATION_SIGNING_KEY","").encode()
    if not value:
        path=pathlib.Path(os.environ.get("NB_ROTATION_SIGNING_KEY_FILE",
            str(deploy.SECURITY_DIR/"rotation-signing.key")))
        if path.is_file():value=path.read_bytes().strip()
    if len(value)<32:raise ValueError("轮换签名密钥至少需要 32 字节，请设置环境变量或密钥文件")
    return value
def canonical(value:dict)->bytes:return json.dumps(value,sort_keys=True,separators=(",",":"),ensure_ascii=False).encode()
def sign(value:dict)->None:value.pop("signature",None);value["signature"]=hmac.new(key(),canonical(value),hashlib.sha256).hexdigest()
def verify(value:dict)->None:
    signature=value.get("signature","");unsigned={k:v for k,v in value.items() if k!="signature"}
    if not hmac.compare_digest(signature,hmac.new(key(),canonical(unsigned),hashlib.sha256).hexdigest()):raise ValueError("轮换工件签名无效")

def prepare(current:pathlib.Path,output:pathlib.Path)->dict:
    openssl=shutil.which("openssl")
    if not openssl:raise RuntimeError("未找到 openssl")
    if output.exists() and any(output.iterdir()):raise ValueError(f"输出目录非空: {output}")
    output.mkdir(parents=True,mode=0o700,exist_ok=True);new=output/"new";new.mkdir()
    run(openssl,"genpkey","-algorithm","EC","-pkeyopt","ec_paramgen_curve:P-256","-out",str(new/"ca.key"))
    run(openssl,"req","-x509","-new","-sha256","-key",str(new/"ca.key"),"-days","3650","-subj","/CN=NB Private CA next","-out",str(new/"ca.pem"))
    ext=output/"node.ext";ext.write_text("subjectAltName=DNS:nb.internal\nextendedKeyUsage=serverAuth,clientAuth\nkeyUsage=digitalSignature\n",encoding="ascii")
    for role in ("entry","middle","exit"):
        run(openssl,"genpkey","-algorithm","EC","-pkeyopt","ec_paramgen_curve:P-256","-out",str(new/f"{role}.key"))
        run(openssl,"req","-new","-key",str(new/f"{role}.key"),"-subj",f"/CN=nb-{role}","-out",str(new/f"{role}.csr"))
        run(openssl,"x509","-req","-sha256","-in",str(new/f"{role}.csr"),"-CA",str(new/"ca.pem"),"-CAkey",str(new/"ca.key"),"-CAcreateserial","-days","825","-extfile",str(ext),"-out",str(new/f"{role}.pem"));(new/f"{role}.csr").unlink()
    ext.unlink();(output/"ca-overlap.pem").write_bytes((current/"ca.pem").read_bytes()+(new/"ca.pem").read_bytes())
    rotation_id=dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")+"-"+sha(new/"ca.pem")[:12]
    document={"schema_version":1,"rotation_id":rotation_id,"state":"prepared","created_at_utc":dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z"),"old_ca_sha256":sha(current/"ca.pem"),"new_ca_sha256":sha(new/"ca.pem"),"roles":{r:{"cert_sha256":sha(new/f"{r}.pem"),"key_sha256":sha(new/f"{r}.key")} for r in ("entry","middle","exit")}}
    sign(document);(output/"manifest.json").write_text(json.dumps(document,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    for path in new.glob("*.key"):path.chmod(0o600)
    return document

def audit(bundle:pathlib.Path,event:str,role:str|None,detail:dict)->None:
    record={"at_utc":dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z"),"event":event,"role":role,"detail":detail}
    with (bundle/"audit.jsonl").open("a",encoding="utf-8") as stream:stream.write(json.dumps(record,ensure_ascii=False,separators=(",",":"))+"\n")

def sync_local_identity(bundle:pathlib.Path,destination:pathlib.Path)->None:
    destination.mkdir(parents=True,exist_ok=True)
    for name in ("ca.key","ca.pem","entry.key","entry.pem","middle.key",
        "middle.pem","exit.key","exit.pem"):
        source=bundle/"new"/name;temporary=destination/(name+".next")
        shutil.copy2(source,temporary);temporary.replace(destination/name)
    for path in (destination/"ca.key",destination/"entry.key",
        destination/"middle.key",destination/"exit.key"):
        try:path.chmod(0o600)
        except OSError:pass

def apply_phase(bundle:pathlib.Path,phase:str,execute:bool)->None:
    manifest=json.loads((bundle/"manifest.json").read_text(encoding="utf-8"));verify(manifest)
    required={"trust":"prepared","identity":"trusted-overlap","retire":"identity-switched"}[phase]
    if manifest.get("state")!=required:raise ValueError(f"phase={phase} 要求 state={required}，当前为 {manifest.get('state')}")
    if not execute:print(json.dumps({"preflight":"ok","phase":phase,"rotation_id":manifest["rotation_id"]},ensure_ascii=False));return
    remote_root=f"{deploy.WORK}/certs";backup=f"{remote_root}/rotation-{manifest['rotation_id']}-{phase}"
    attempted=[]
    try:
        for role in ("exit","middle","entry"):
            c=None
            try:
                c=deploy.connect(role);deployment=deploy._remote_current_deployment(c,role)
                deploy.run(c,f"mkdir -p {shlex.quote(backup)}; cp -p {remote_root}/ca.pem {remote_root}/{role}.pem {remote_root}/{role}.key {shlex.quote(backup)}/")
                attempted.append(role)
                if phase=="trust":deploy.push_bytes(c,(bundle/"ca-overlap.pem").read_bytes(),f"{remote_root}/ca.pem",0o644)
                elif phase=="identity":
                    deploy.push_bytes(c,(bundle/"new"/f"{role}.pem").read_bytes(),f"{remote_root}/{role}.pem",0o644);deploy.push_bytes(c,(bundle/"new"/f"{role}.key").read_bytes(),f"{remote_root}/{role}.key",0o600)
                elif phase=="retire":
                    deploy.push_bytes(c,(bundle/"new"/"ca.pem").read_bytes(),f"{remote_root}/ca.pem",0o644);deploy.push_bytes(c,(manifest["old_ca_sha256"]+"\n").encode(),f"{remote_root}/revoked-ca.sha256",0o600)
                else:raise ValueError("phase 必须为 trust/identity/retire")
                deploy.run(c,f"systemctl kill -s HUP nb-{role}.service");health=deploy._verify_deployment_health(c,role,deployment,warmup=4)
                audit(bundle,f"{phase}-applied",role,{"health":health})
            finally:
                if c is not None:c.close()
    except Exception as error:
        for role in reversed(attempted):
            c=None
            try:
                c=deploy.connect(role);deploy.run(c,f"cp -pf {shlex.quote(backup)}/ca.pem {remote_root}/ca.pem; cp -pf {shlex.quote(backup)}/{role}.pem {remote_root}/{role}.pem; cp -pf {shlex.quote(backup)}/{role}.key {remote_root}/{role}.key; systemctl kill -s HUP nb-{role}.service")
            except Exception:pass
            finally:
                if c is not None:c.close()
        audit(bundle,f"{phase}-rollback",None,{"error":f"{type(error).__name__}: {error}"});raise
    manifest["state"]={"trust":"trusted-overlap","identity":"identity-switched","retire":"complete"}[phase];manifest[f"{phase}_at_utc"]=dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00","Z");sign(manifest);(bundle/"manifest.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    if phase=="retire":sync_local_identity(bundle,deploy.SECURITY_DIR)

def keygen(output:pathlib.Path)->None:
    if output.exists():raise ValueError(f"签名密钥已存在，拒绝覆盖: {output}")
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(secrets.token_hex(32)+"\n",encoding="ascii")
    try:output.chmod(0o600)
    except OSError:pass
    print(f"轮换签名密钥已生成: {output}")

def main()->None:
    parser=argparse.ArgumentParser();sub=parser.add_subparsers(dest="command",required=True)
    k=sub.add_parser("keygen");k.add_argument("--output",type=pathlib.Path,default=deploy.SECURITY_DIR/"rotation-signing.key")
    p=sub.add_parser("prepare");p.add_argument("--current",type=pathlib.Path,default=deploy.SECURITY_DIR);p.add_argument("--output",type=pathlib.Path,required=True)
    a=sub.add_parser("apply");a.add_argument("bundle",type=pathlib.Path);a.add_argument("--phase",choices=("trust","identity","retire"),required=True);a.add_argument("--execute",action="store_true")
    args=parser.parse_args()
    if args.command=="keygen":keygen(args.output.resolve())
    elif args.command=="prepare":print(json.dumps(prepare(args.current.resolve(),args.output.resolve()),ensure_ascii=False,indent=2))
    else:apply_phase(args.bundle.resolve(),args.phase,args.execute)
if __name__=="__main__":main()
