#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import deploy_core as deploy  # noqa: E402

INSTANCE = "gz2-hk2-kz-00002_1"
REMOTE = r'''
import glob,json,socket,sys
role,instance=sys.argv[1:3]
out=[]
for path in sorted(glob.glob('/run/nb-'+instance+'-'+role+'-[0-9].ctl')):
 s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect(path);s.sendall(b'metrics\n');v=json.loads(s.recv(524288));s.close()
 f=v.get('fec') or {}
 out.append({'worker':v.get('worker'),'release':v.get('release_id'),'generation':v.get('transport_generation'),
             'udp_adaptive_active':f.get('udp_adaptive_active'),'udp_source_packets':f.get('udp_source_packets'),
             'udp_repairs_sent':f.get('udp_repairs_sent'),'udp_repairs_received':f.get('udp_repairs_received'),
             'udp_recovered':f.get('udp_recovered'),'blocks_recovered':f.get('blocks_recovered')})
print(json.dumps(out,separators=(',',':')))
'''

result = {}
for role in ("entry", "middle", "exit"):
    client = deploy.connect(role)
    try:
        command = "python3 -c " + shlex.quote(REMOTE) + " " + shlex.quote(role) + " " + shlex.quote(INSTANCE)
        result[role] = json.loads(deploy.checked_run(client, command, tmo=30).strip().splitlines()[-1])
    finally:
        client.close()
print(json.dumps(result, separators=(",", ":")))
