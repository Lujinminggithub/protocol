#!/usr/bin/env python3
from __future__ import annotations

import pathlib

import deploy


def main() -> None:
    assert deploy.build_execution_mode() == "control-plane"
    assert deploy.controlplane_build_root() == pathlib.Path(
        deploy.os.environ.get("NB_CONTROLPLANE_BUILD_DIR", "/opt/nb-controlplane/data/build"))
    source = pathlib.Path(deploy.__file__).read_text(encoding="utf-8")
    body = source.split("def act_build(roles):", 1)[1].split("def act_prepare_release", 1)[0]
    assert "connect(BUILD_HOST)" not in body
    assert "_act_build_local(git_info)" in body
    print("control-plane local build tests passed")


if __name__ == "__main__":
    main()
