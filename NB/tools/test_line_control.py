#!/usr/bin/env python3
import json,os,pathlib,tempfile
import nb_line_control as control


def candidate():
    rec={"confidence":"load-qualified","cc":"bbr","reorder_gap":32,"reorder_delay_us":300000,"mtu_max":1452,
         "current":{"cc":"bbr","reorder_gap":16,"reorder_delay_us":200000},
         "mtu_evidence":{"confidence":"quic-and-df"}}
    segment={"candidate":rec,"quic":{"packets_observed":12000,"windows_valid":6}}
    return {"schema_version":2,"status":"candidate","probe_mode":"active-quic","line":"test","fixed_exit":"kz",
            "baseline_hosts_sha256":control._sha256(control.DEFAULT_HOSTS),"baseline_profile_sha256":control._sha256(control.DEFAULT_PROFILE),
            "active_probe":{"integrity":{"integrity":"ok"},"load":{"integrity":"count-ok"}},
            "admission":{"status":"admitted","reasons":[]},
            "segments":{"entry_middle":segment,"middle_exit":segment}}


def main():
    old=os.environ.get("NB_PROFILE_SIGNING_KEY");os.environ["NB_PROFILE_SIGNING_KEY"]="x"*32
    try:
        with tempfile.TemporaryDirectory(prefix="nb-control-") as tmp:
            root=pathlib.Path(tmp);source=root/"candidate.json";approved=root/"approved.json"
            source.write_text(json.dumps(candidate()),encoding="utf-8")
            doc=control.approve(source,approved,"tester");control.verify_approval(doc)
            hosts,profile,state=control.prepare_canary(approved,root/"canary")
            assert hosts.is_file() and profile.is_file() and state["state"]=="canary-ready"
            control.verify_canary_bundle(state)
            snapshots=[{"alerts":[],"workers":[{"metrics":{"queue_age_max_us":{"down":100000,"up":0,"q2t":0}}}]} for _ in range(6)]
            assert control.evaluate_canary(snapshots)["accepted"]
            snapshots[0]["alerts"]=[{"severity":"critical"}]
            assert not control.evaluate_canary(snapshots)["accepted"]
            snapshots[0]["alerts"]=[{"severity":"critical","code":"node-unreachable","streak":1}]
            assert control.evaluate_canary(snapshots)["accepted"]
            snapshots[0]["alerts"][0]["streak"]=2
            assert not control.evaluate_canary(snapshots)["accepted"]
            canary_profile=json.loads(profile.read_text(encoding="utf-8"))
            readback={"collection_errors":[],"workers":[{"role":"entry","worker":"0","health":{
                "line_profile":canary_profile["line_id"],"line_profile_schema":canary_profile["schema_version"]}}]}
            control.verify_readback(readback,canary_profile)
            control._verify_active_probe({"integrity":{"integrity":"ok"},
                "load":{"integrity":"count-ok"}})
            try: control._verify_active_probe({"error":"connection aborted"})
            except RuntimeError: pass
            else: raise AssertionError("failed active probe was accepted")
            unsafe=candidate();unsafe["segments"]["entry_middle"]["candidate"]["reorder_gap"]=8
            try: control.validate_candidate(unsafe)
            except ValueError: pass
            else: raise AssertionError("unsafe reorder reduction was accepted")
            rejected=candidate();rejected["admission"]={"status":"rejected","reasons":["insufficient-throughput"]}
            try: control.validate_candidate(rejected)
            except ValueError: pass
            else: raise AssertionError("rejected line admission was accepted")
            doc["approved_by"]="tampered"
            try: control.verify_approval(doc)
            except ValueError: pass
            else: raise AssertionError("篡改审批未被拒绝")

            old_hosts,old_profile=control.DEFAULT_HOSTS,control.DEFAULT_PROFILE
            old_current=control._current_deployment
            try:
                control.DEFAULT_HOSTS=root/"active-hosts.json"
                control.DEFAULT_PROFILE=root/"active-profile.json"
                control.DEFAULT_HOSTS.write_bytes(old_hosts.read_bytes())
                control.DEFAULT_PROFILE.write_bytes(old_profile.read_bytes())
                source2=root/"candidate2.json";approved2=root/"approved2.json"
                source2.write_text(json.dumps(candidate()),encoding="utf-8")
                control.approve(source2,approved2,"tester")
                _,_,state2=control.prepare_canary(approved2,root/"promote")
                state2["state"]="accepted";state2["canary_deployment_id"]="deployment-test"
                control._resign(state2)
                (root/"promote"/"state.json").write_text(json.dumps(state2),encoding="utf-8")
                control._current_deployment=lambda: "deployment-test"
                promoted=control.promote_canary(approved2,root/"promote")
                assert promoted["state"]=="promoted"
                active=json.loads(control.DEFAULT_PROFILE.read_text(encoding="utf-8"))
                assert active["status"]=="active-baseline"
            finally:
                control.DEFAULT_HOSTS,control.DEFAULT_PROFILE=old_hosts,old_profile
                control._current_deployment=old_current
    finally:
        if old is None:os.environ.pop("NB_PROFILE_SIGNING_KEY",None)
        else:os.environ["NB_PROFILE_SIGNING_KEY"]=old
    print("RESULT PASS")


if __name__=="__main__":main()
