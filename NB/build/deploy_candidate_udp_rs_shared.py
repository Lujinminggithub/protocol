#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "build"))

import deploy_core as deploy  # noqa: E402
import deploy_candidate_shared_binary as shared  # noqa: E402


ROLES = ("entry", "middle", "exit")
EXPECTED_PREVIOUS = {"c57f28878d62f943", "0d054baa72c7c2f1", "392a577126921aa8"}


def main() -> None:
    binary = (ROOT / "build" / "nb_node").read_bytes()
    digest = hashlib.sha256(binary).hexdigest()
    release_id = digest[:16]
    if release_id != "f771b837da64bcb7":
        raise RuntimeError(f"unexpected RS candidate release: {release_id}")
    clients = {}
    states = {}
    activated = []
    try:
        for role in ROLES:
            clients[role] = deploy.connect(role)
            states[role] = shared.stage(clients[role], role, binary, release_id, digest)
            previous = pathlib.PurePosixPath(states[role]["previous"]).parent.name
            if previous not in EXPECTED_PREVIOUS:
                raise RuntimeError(f"{role} 当前共享版本不是受信任的 RS 基线：{previous}")
        results = {}
        for role in ROLES:
            activated.append(role)
            results[role] = shared.activate(clients[role], role, states[role], release_id, digest)
        print(json.dumps({"status": "deployed", "release_id": release_id,
                          "sha256": digest, "previous": sorted(EXPECTED_PREVIOUS),
                          "roles": results}, separators=(",", ":")))
    except Exception:
        for role in reversed(activated):
            try:
                shared.rollback(clients[role], role, states[role])
            except Exception as error:
                print(f"回滚失败 role={role} error={error}", file=sys.stderr)
        raise
    finally:
        for client in clients.values():
            client.close()


if __name__ == "__main__":
    main()
