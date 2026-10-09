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
    local_build = source.split("def _act_build_local(git_info):", 1)[1].split("def act_build(roles):", 1)[0]
    rebuild = local_build.split("if REBUILD_PICOQUIC:", 1)[1]
    assert "snapshot = nb_release.snapshot_inputs(ROOT, NODE_RELEASE_INPUTS)" in rebuild
    environment = deploy.local_build_environment({
        "PATH": "test", "NB_DEPLOY_INSTANCE": "line-1", "NB_SOCKS_PORT": "1082",
        "NB_SSH_PASSWORD_ENTRY": "secret",
    })
    assert environment["PATH"] == "test"
    assert "NB_DEPLOY_INSTANCE" not in environment and "NB_SOCKS_PORT" not in environment
    assert "NB_SSH_PASSWORD_ENTRY" not in environment
    print("control-plane local build tests passed")


if __name__ == "__main__":
    main()
