#!/usr/bin/env python3
from __future__ import annotations

import os
import pathlib
import tempfile

import deploy
import deploy_core


class FakeClient:
    def __init__(self, role): self.role = role
    def close(self): pass


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="nb-security-push-") as security_tmp:
        security = pathlib.Path(security_tmp)
        for name in ("ca.pem", "socks.users", "tenant.conf", "entry.pem", "entry.key",
                     "middle.pem", "middle.key", "exit.pem", "exit.key"):
            (security / name).write_text(name, encoding="ascii")
        saved = {name: getattr(deploy_core, name) for name in
                 ("SECURITY_DIR", "INSTANCE_WORK", "DEPLOY_CERTS", "run", "push_bytes")}
        pushed = []
        try:
            deploy_core.SECURITY_DIR = security
            deploy_core.INSTANCE_WORK = "/runtime"
            deploy_core.DEPLOY_CERTS = "/certs"
            deploy_core.push_bytes = lambda _c, _data, remote, mode: pushed.append((remote, mode))
            deploy_core.run = lambda *_args, **_kwargs: "PRESENT"
            deploy_core._push_security(FakeClient("entry"), "entry")
            assert "/runtime/socks.users" in {path for path, _mode in pushed}
            assert "/runtime/tenant.conf" not in {path for path, _mode in pushed}
            pushed.clear();deploy_core.run = lambda *_args, **_kwargs: ""
            deploy_core._push_security(FakeClient("entry"), "entry")
            assert "/runtime/tenant.conf" in {path for path, _mode in pushed}
        finally:
            for name, value in saved.items():
                setattr(deploy_core, name, value)
    names = [
        "_require_local_build", "_require_security_material", "connect", "run",
        "_acquire_deploy_lock", "_release_deploy_lock", "_stage_release",
        "_stage_entry_release", "_copy_release_between_nodes",
        "_backup_role_unit", "_backup_role_state", "_restore_role_state",
        "_append_deploy_audit", "_push_security",
        "_push_tiktok_rules", "_push_whitelist", "_ensure_remote_whitelist", "_push_exit_routes",
        "_verify_exit_bind_ip",
        "_node_command", "_activate_release", "_install_and_restart_role",
        "_verify_release_health", "_rollback_release", "_smoke_socks", "_prune_releases",
        "_remote_current_deployment", "_activate_existing_deployment",
        "_append_exact_rollback_audit", "BUILD_DIR",
    ]
    original = {name: getattr(deploy, name) for name in names}
    old_user = os.environ.get("NB_SOCKS_USERNAME"); old_password = os.environ.get("NB_SOCKS_PASSWORD")
    events = []
    manifest = {"release_id": "0123456789abcdef", "artifact": {"sha256": "0" * 64}}
    temporary = tempfile.TemporaryDirectory()
    try:
        os.environ["NB_SOCKS_USERNAME"] = "test"; os.environ["NB_SOCKS_PASSWORD"] = "test"
        deploy.BUILD_DIR = pathlib.Path(temporary.name)
        (deploy.BUILD_DIR / "nb_node").write_bytes(b"transaction-test-binary")
        deploy._require_local_build = lambda: manifest
        deploy._require_security_material = lambda: None
        deploy.connect = lambda role: FakeClient(role)
        deploy.run = lambda *args, **kwargs: ""
        deploy._acquire_deploy_lock = lambda c, role, release: events.append(("lock", role))
        deploy._release_deploy_lock = lambda c: events.append(("unlock", c.role))
        deploy._stage_release = lambda c, role, m, data=None: f"releases/old-{role}/nb_node"
        deploy._stage_entry_release = lambda c, m, data: "releases/old-entry/nb_node"
        def copy_release(source, source_role, target, target_role, manifest):
            events.append(("copy", source_role, target_role))
            if source_role == "entry" and target_role == "exit":
                raise RuntimeError("injected direct transfer failure")
            return "test-address"
        deploy._copy_release_between_nodes = copy_release
        deploy._backup_role_unit = lambda *args: True
        deploy._backup_role_state = lambda c, role, release: {"role": role}
        deploy._restore_role_state = lambda c, role, release, state: events.append(("restore", role))
        deploy._append_deploy_audit = lambda *args, **kwargs: None
        deploy._append_exact_rollback_audit = lambda *args, **kwargs: None
        deploy._push_security = deploy._push_tiktok_rules = lambda *args, **kwargs: None
        deploy._push_whitelist = lambda c, path, role: events.append(("whitelist", role)) or "/tmp/wl"
        deploy._ensure_remote_whitelist = lambda *args, **kwargs: "/tmp/wl"
        deploy._push_exit_routes = lambda *args, **kwargs: "/tmp/routes"
        deploy._verify_exit_bind_ip = lambda c: events.append(("bind-ip", c.role)) or "test-address"
        deploy._node_command = lambda role, **kwargs: role
        deploy._activate_release = lambda c, release: events.append(("activate", c.role))
        def install(c, role, command, **kwargs):
            if role == "middle": raise RuntimeError("injected middle failure")
        deploy._install_and_restart_role = install
        deploy._verify_release_health = lambda c, role, m: "ok"
        deploy._rollback_release = lambda c, role, previous, release, had_unit, state: events.append(("rollback", role)) or "ok"
        deploy._smoke_socks = lambda port: None
        deploy._prune_releases = lambda *args: "{}"
        try:
            deploy.act_deploy_socks()
        except RuntimeError as error:
            assert "injected" in str(error)
        else:
            raise AssertionError("故障注入未中止部署")
        assert [item for item in events if item[0] == "rollback"] == [
            ("rollback", "middle"), ("rollback", "exit")
        ]
        assert [item for item in events if item[0] == "restore"] == [("restore", "entry")]
        assert [item for item in events if item[0] == "copy"] == [
            ("copy", "entry", "middle"),
            ("copy", "entry", "exit"),
            ("copy", "middle", "exit"),
        ]
        assert [item for item in events if item[0] == "bind-ip"] == [("bind-ip", "exit")]
        if deploy.LAB_FILE.name == "deployment-hosts.json":
            assert [item for item in events if item[0] == "whitelist"] == [
                ("whitelist", "exit"), ("whitelist", "entry")
            ]
        assert {item[1] for item in events if item[0] == "unlock"} == {"entry", "middle", "exit"}

        events.clear();target="aaaaaaaaaaaaaaaa-bbbbbbbbbbbb"
        deploy._remote_current_deployment=lambda c,role:"cccccccccccccccc-dddddddddddd"
        def activate_existing(c,role,deployment):
            events.append(("exact",role,deployment))
            if role=="middle" and deployment==target:raise RuntimeError("injected exact rollback failure")
            return "ok"
        deploy._activate_existing_deployment=activate_existing
        try:deploy.act_rollback_socks(target)
        except RuntimeError as error:assert "injected" in str(error)
        else:raise AssertionError("精确回滚故障注入未中止")
        assert events[:3]==[("lock","exit"),("lock","middle"),("lock","entry")]
        assert ("exact","exit","cccccccccccccccc-dddddddddddd") in events
    finally:
        for name, value in original.items(): setattr(deploy, name, value)
        temporary.cleanup()
        if old_user is None: os.environ.pop("NB_SOCKS_USERNAME", None)
        else: os.environ["NB_SOCKS_USERNAME"] = old_user
        if old_password is None: os.environ.pop("NB_SOCKS_PASSWORD", None)
        else: os.environ["NB_SOCKS_PASSWORD"] = old_password
    print("RESULT PASS")


if __name__ == "__main__": main()
