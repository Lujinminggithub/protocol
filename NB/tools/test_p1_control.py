#!/usr/bin/env python3
import json,os,pathlib,tempfile
import nb_p1_control
def main():
    old=os.environ.get("NB_CONTROL_SIGNING_KEY");os.environ["NB_CONTROL_SIGNING_KEY"]="c"*32
    try:
        with tempfile.TemporaryDirectory(prefix="nb-p1-") as tmp:
            root=pathlib.Path(tmp);policy=root/"policy.json";bundle=root/"bundle"
            policy.write_text(json.dumps({"schema_version":1,"fixed_exit":"kz","tenants":[{"name":"u","max_tcp":2}],"routes":[{"name":"r","host":"127.0.0.1","fixed_exit":"kz"}]}),encoding="utf-8")
            doc=nb_p1_control.prepare(policy,bundle);loaded=json.loads((bundle/"manifest.json").read_text(encoding="utf-8"));nb_p1_control.verify(loaded);assert doc["state"]=="approved"
            assert b"\r\n" not in (bundle/"tenant.conf").read_bytes() and b"\r\n" not in (bundle/"exit_routes.conf").read_bytes()
            assert (bundle/"tenant.conf").read_text(encoding="ascii").strip().endswith(" 1")
            dual={"schema_version":3,"fixed_exit":"kz","tenants":[{"name":"live","max_tcp":2,"max_udp":3,"rate_up_kbps":5000,"rate_down_kbps":8000,"quota_mb":0,"burst_up_bytes":262144,"burst_down_bytes":524288}],"routes":[{"name":"r","host":"127.0.0.1","fixed_exit":"kz"}]}
            tenants,_=nb_p1_control.validate(dual)
            assert tenants.strip()=="tenant-v3 live 2 3 5000 8000 0 262144 524288"
            dual_path=root/"dual.json";dual_path.write_text(json.dumps(dual),encoding="utf-8")
            dual_bundle=root/"dual-bundle";dual_doc=nb_p1_control.prepare(dual_path,dual_bundle)
            assert dual_doc["tenant_fingerprint"]=="3894f0915d74aaef"
            route_bytes=(dual_bundle/"exit_routes.conf").read_bytes()
            deployed_routes=b"# route <name> <H:host:port> <weight> <capacity; 0=unlimited>\n"+route_bytes
            assert nb_p1_control._route_records(route_bytes)==nb_p1_control._route_records(deployed_routes)
            changed_routes=deployed_routes.replace(b"H:127.0.0.1:4443",b"H:127.0.0.1:4444")
            assert nb_p1_control._route_records(route_bytes)!=nb_p1_control._route_records(changed_routes)
            nb_p1_control.apply(bundle,False)
            calls=[]
            original={name:getattr(nb_p1_control,name) for name in
                ("_remote_read","_remote_push","invoke_role","_publish","verify_integrity_after_reload")}
            try:
                nb_p1_control._remote_read=lambda role,path:(
                    (dual_bundle/"tenant.conf").read_bytes() if path.endswith("tenant.conf") else
                    deployed_routes)
                nb_p1_control._remote_push=lambda role,data,path:calls.append(("push",role,path))
                def invoke(role,command):
                    calls.append(("invoke",role,command))
                    if command.startswith("tenant status"):
                        return [{"fingerprint":dual_doc["tenant_fingerprint"]}]
                    return [{"status":"ok"}]
                nb_p1_control.invoke_role=invoke
                nb_p1_control._publish=lambda role,source,target:calls.append(("publish",role,target))
                nb_p1_control.verify_integrity_after_reload=lambda port:calls.append(("probe",port))
                nb_p1_control.apply(dual_bundle,True,1085)
                commits=[call[1] for call in calls if call[0]=="invoke" and call[2].startswith("tenant commit")]
                assert commits==["exit","entry"]
                calls.clear();failed={"done":False}
                def fail_entry_commit(role,command):
                    calls.append(("invoke",role,command))
                    if role=="entry" and command.startswith("tenant commit") and not failed["done"]:
                        failed["done"]=True;raise RuntimeError("injected entry commit failure")
                    if command.startswith("tenant status"):
                        return [{"fingerprint":dual_doc["tenant_fingerprint"]}]
                    return [{"status":"ok"}]
                nb_p1_control.invoke_role=fail_entry_commit
                try:nb_p1_control.apply(dual_bundle,True,1085)
                except RuntimeError as error:assert "injected entry commit failure" in str(error)
                else:raise AssertionError("injected transaction failure was ignored")
                rollback_prepares=[call[1] for call in calls if call[0]=="invoke" and
                    call[2].startswith("tenant prepare") and "rollback-" in call[2]]
                assert rollback_prepares==["entry","exit"]
            finally:
                for name,value in original.items():setattr(nb_p1_control,name,value)
            bad=json.loads(policy.read_text());bad["routes"][0]["fixed_exit"]="other"
            try:nb_p1_control.validate(bad)
            except ValueError:pass
            else:raise AssertionError("fixed exit escape accepted")
    finally:
        if old is None:os.environ.pop("NB_CONTROL_SIGNING_KEY",None)
        else:os.environ["NB_CONTROL_SIGNING_KEY"]=old
    print("RESULT PASS")
if __name__=="__main__":main()
