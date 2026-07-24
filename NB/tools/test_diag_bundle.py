#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import tempfile

import nb_diag


class FakeClient:
    def close(self):
        pass


def main() -> None:
    original_connect = nb_diag.deploy.connect
    original_run = nb_diag.deploy.run
    original_workers = nb_diag.deploy._effective_workers
    try:
        nb_diag.deploy.connect = lambda role: (_ for _ in ()).throw(RuntimeError("unreachable")) if role == "middle" else FakeClient()
        nb_diag.deploy.run = lambda client, command, tmo=120: "UTC=2026-07-20T00:00:00Z\nHEALTH_OK\n"
        nb_diag.deploy._effective_workers = lambda role: 1
        with tempfile.TemporaryDirectory(prefix="nb-diag-") as temporary:
            directory, archive, summary = nb_diag.incident_bundle(50, pathlib.Path(temporary))
            assert directory.is_dir() and archive.is_file()
            assert (directory / "entry.txt").is_file()
            assert summary["roles"]["middle"]["status"] == "failed"
            assert summary["roles"]["exit"]["status"] == "collected"
    finally:
        nb_diag.deploy.connect = original_connect
        nb_diag.deploy.run = original_run
        nb_diag.deploy._effective_workers = original_workers
    print("RESULT PASS")


if __name__ == "__main__":
    main()
