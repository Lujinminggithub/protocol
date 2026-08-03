#!/usr/bin/env python3
import json,os,pathlib,tempfile
import security_rotate

class FakeConnection:
    def __init__(self,role,closed):self.role=role;self.closed=closed
    def close(self):self.closed.append(self.role)

def main():
    old=os.environ.get("NB_ROTATION_SIGNING_KEY");os.environ["NB_ROTATION_SIGNING_KEY"]="r"*32
    try:
        with tempfile.TemporaryDirectory(prefix="nb-rotation-") as tmp:
            generated=pathlib.Path(tmp)/"rotation-signing.key";security_rotate.keygen(generated)
            try:security_rotate.keygen(generated)
            except ValueError:pass
            else:raise AssertionError("keygen overwrote an existing key")
            current=pathlib.Path(tmp)/"current";current.mkdir()
            (current/"ca.pem").write_text("test-current-ca\n",encoding="ascii")
            out=pathlib.Path(tmp)/"bundle";doc=security_rotate.prepare(current,out)
            loaded=json.loads((out/"manifest.json").read_text(encoding="utf-8"));security_rotate.verify(loaded)
            assert doc["state"]=="prepared" and (out/"ca-overlap.pem").is_file()
            loaded["state"]="tampered"
            try:security_rotate.verify(loaded)
            except ValueError:pass
            else:raise AssertionError("tampered rotation accepted")
            synced=pathlib.Path(tmp)/"synced";security_rotate.sync_local_identity(out,synced)
            assert all((synced/name).is_file() for name in ("ca.pem","entry.pem","middle.pem","exit.pem"))
            (out/"manifest.json").write_text(json.dumps(doc),encoding="utf-8")
            try:security_rotate.apply_phase(out,"identity",False)
            except ValueError:pass
            else:raise AssertionError("out-of-order rotation accepted")
            calls=[];closed=[]
            originals=(security_rotate.deploy.connect,security_rotate.deploy.run,security_rotate.deploy.push_bytes,security_rotate.deploy._remote_current_deployment,security_rotate.deploy._verify_deployment_health)
            security_rotate.deploy.connect=lambda role:FakeConnection(role,closed)
            security_rotate.deploy.run=lambda conn,command,**kwargs:calls.append(("run",conn.role,command)) or "ok"
            security_rotate.deploy.push_bytes=lambda conn,data,path,mode=0o644:calls.append(("push",conn.role,path))
            security_rotate.deploy._remote_current_deployment=lambda conn,role:"deployment"
            def health(conn,role,deployment,warmup=0):
                if role=="middle":raise RuntimeError("injected health failure")
                return {"status":"ok"}
            security_rotate.deploy._verify_deployment_health=health
            try:
                try:security_rotate.apply_phase(out,"trust",True)
                except RuntimeError:pass
                else:raise AssertionError("injected rotation failure accepted")
                rollback_roles=[role for kind,role,command in calls if kind=="run" and "cp -pf" in command]
                assert rollback_roles==["middle","exit"],rollback_roles
                assert closed.count("middle")==2 and closed.count("exit")==2,closed
            finally:
                (security_rotate.deploy.connect,security_rotate.deploy.run,security_rotate.deploy.push_bytes,security_rotate.deploy._remote_current_deployment,security_rotate.deploy._verify_deployment_health)=originals
    finally:
        if old is None:os.environ.pop("NB_ROTATION_SIGNING_KEY",None)
        else:os.environ["NB_ROTATION_SIGNING_KEY"]=old
    print("RESULT PASS")
if __name__=="__main__":main()
