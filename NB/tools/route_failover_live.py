#!/usr/bin/env python3
"""同一固定出口的双 QUIC endpoint 熔断与新会话接管验收。"""
from __future__ import annotations

import argparse,json,pathlib,time
import deploy,line_probe,nb_observe

def routes_snapshot()->dict:
    connection=deploy.connect("entry")
    try:return nb_observe._remote_control_query(connection,"entry")[0]["routes"]
    finally:connection.close()

def report_route(name:str,result:str)->dict:
    connection=deploy.connect("entry")
    try:
        command=f"route-result {name} {result}"
        records=nb_observe._remote_control_query_commands(connection,"entry",(command,))
        response=records[0][command]
        if response.get("status")!="ok":raise RuntimeError(f"路由结果上报失败: {response}")
        return response
    finally:connection.close()

def main()->None:
    parser=argparse.ArgumentParser();parser.add_argument("--apply",action="store_true")
    parser.add_argument("--output",type=pathlib.Path,required=True);args=parser.parse_args()
    if not args.apply:raise SystemExit("必须显式添加 --apply 才会停止主 exit 服务")
    entry=deploy._role_host("entry")["host"];exit_connection=deploy.connect("exit")
    result={"schema_version":1,"before":routes_snapshot(),"attempts":[]}
    try:
        if deploy.run(exit_connection,"systemctl is-active nb-exit-alt.service").strip()!="active":
            raise RuntimeError("备用 exit endpoint 未运行")
        deploy.run(exit_connection,"systemctl stop nb-exit.service; systemctl is-active nb-exit.service || true")
        for attempt in range(1,7):
            try:
                probe=line_probe.run_load_probe(entry,1080,1.0,2,io_timeout=6)
                result["attempts"].append({"attempt":attempt,"passed":True,"probe":probe})
                report_route("kz-primary","success")
            except Exception as error:
                result["attempts"].append({"attempt":attempt,"passed":False,
                    "error":f"{type(error).__name__}: {error}"})
                report_route("kz-primary","failure")
            result["during"]=routes_snapshot()
            primary=result["during"]["routes"][0]
            if not primary["healthy"]:
                takeover=line_probe.run_integrity_probe(entry,1080,io_timeout=8)
                result["attempts"].append({"attempt":"takeover","passed":True,"probe":takeover})
                break
            time.sleep(1)
    finally:
        deploy.run(exit_connection,"systemctl start nb-exit.service")
        deployment=deploy._remote_current_deployment(exit_connection,"exit")
        deploy._verify_deployment_health(exit_connection,"exit",deployment,warmup=4)
        exit_connection.close()
    result["after_restart"]=routes_snapshot()
    failed=any(not item["passed"] for item in result["attempts"])
    failed_count=sum(1 for item in result["attempts"] if isinstance(item["attempt"],int) and not item["passed"])
    took_over=any(item["passed"] and item["attempt"]=="takeover" for item in result["attempts"])
    primary_unhealthy=not result.get("during",{}).get("routes",[{"healthy":True}])[0]["healthy"]
    result["passed"]=failed and failed_count>=3 and took_over and primary_unhealthy
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result,ensure_ascii=False))
    if not result["passed"]:raise SystemExit(1)

if __name__=="__main__":main()
