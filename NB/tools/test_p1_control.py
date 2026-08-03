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
            assert (bundle/"tenant.conf").read_text(encoding="ascii").strip().endswith(" 10")
            nb_p1_control.apply(bundle,False)
            bad=json.loads(policy.read_text());bad["routes"][0]["fixed_exit"]="other"
            try:nb_p1_control.validate(bad)
            except ValueError:pass
            else:raise AssertionError("fixed exit escape accepted")
    finally:
        if old is None:os.environ.pop("NB_CONTROL_SIGNING_KEY",None)
        else:os.environ["NB_CONTROL_SIGNING_KEY"]=old
    print("RESULT PASS")
if __name__=="__main__":main()
