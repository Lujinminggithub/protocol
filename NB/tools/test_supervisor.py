#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import time
import os
import signal


def main() -> None:
    root=pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="nb-workers-") as tmp:
        child=("import os,pathlib,time;"
            f"p=pathlib.Path({tmp!r},'worker-'+os.environ['NB_WORKER_ID']);"
            "n=int(p.read_text())+1 if p.exists() else 1;p.write_text(str(n));time.sleep(30)")
        proc=subprocess.Popen([sys.executable,str(root/"tools"/"nb_supervisor.py"),"--workers","2","--",sys.executable,"-c",child])
        try:
            deadline=time.time()+5
            while time.time()<deadline:
                if all((pathlib.Path(tmp)/f"worker-{i}").exists() for i in range(2)):break
                time.sleep(0.05)
            else:raise RuntimeError("workers did not start")
            if hasattr(signal,"SIGHUP"):
                os.kill(proc.pid,signal.SIGHUP);deadline=time.time()+5
                while time.time()<deadline:
                    if all((pathlib.Path(tmp)/f"worker-{i}").read_text()=="2" for i in range(2)):break
                    time.sleep(0.05)
                else:raise RuntimeError("workers did not rolling reload")
        finally:
            proc.terminate();proc.wait(timeout=5)
    crash=subprocess.run([sys.executable,str(root/"tools"/"nb_supervisor.py"),"--workers","1",
        "--restart-base","0.01","--crash-limit","3","--",sys.executable,"-c","raise SystemExit(2)"],
        timeout=5,check=False,capture_output=True,text=True)
    assert crash.returncode==75 and "crash-loop" in crash.stderr
    print("RESULT PASS")


if __name__=="__main__":main()
