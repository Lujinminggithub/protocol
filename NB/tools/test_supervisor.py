#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import time


def main() -> None:
    root=pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="nb-workers-") as tmp:
        child=(
            "import os,pathlib,time;"
            f"pathlib.Path({tmp!r},'worker-'+os.environ['NB_WORKER_ID']).touch();"
            "time.sleep(30)"
        )
        proc=subprocess.Popen([sys.executable,str(root/"tools"/"nb_supervisor.py"),"--workers","2","--",sys.executable,"-c",child])
        try:
            deadline=time.time()+5
            while time.time()<deadline:
                if all((pathlib.Path(tmp)/f"worker-{i}").exists() for i in range(2)):break
                time.sleep(0.05)
            else:raise RuntimeError("workers did not start")
        finally:
            proc.terminate();proc.wait(timeout=5)
    print("RESULT PASS")


if __name__=="__main__":main()
