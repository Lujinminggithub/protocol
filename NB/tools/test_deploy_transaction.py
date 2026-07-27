#!/usr/bin/env python3
from __future__ import annotations

import os

import deploy


class FakeClient:
    def __init__(self, role): self.role = role
    def close(self): pass


def main() -> None:
    names = [
        "_require_local_build", "_require_security_material", "connect", "run",
        "_acquire_deploy_lock", "_release_deploy_lock", "_stage_release",
        "_backup_role_unit", "_backup_role_state", "_restore_role_state",
        "_append_deploy_audit", "_push_security",
        "_push_tiktok_rules", "_ensure_remote_whitelist", "_push_exit_routes",
        "_node_command", "_activate_release", "_install_and_restart_role",
        "_verify_release_health", "_rollback_release", "_smoke_socks", "_prune_releases",
        "_remote_current_deployment", "_activate_existing_deployment",
        "_append_exact_rollback_audit",
    ]
    original = {name: getattr(deploy, name) for name in names}
    old_user = os.environ.get("NB_SOCKS_USERNAME"); old_password = os.environ.get("NB_SOCKS_PASSWORD")
    events = []
    manifest = {"release_id": "0123456789abcdef", "artifact": {"sha256": "0" * 64}}
    try:
        os.environ["NB_SOCKS_USERNAME"] = "test"; os.environ["NB_SOCKS_PASSWORD"] = "test"
        deploy._require_local_build = lambda: manifest
        deploy._require_security_material = lambda: None
        deploy.connect = lambda role: FakeClient(role)
        deploy.run = lambda *args, **kwargs: ""
        deploy._acquire_deploy_lock = lambda c, role, release: events.append(("lock", role))
        deploy._release_deploy_lock = lambda c: events.append(("unlock", c.role))
        deploy._stage_release = lambda c, role, m, data: f"releases/old-{role}/nb_node"
        deploy._backup_role_unit = lambda *args: True
        deploy._backup_role_state = lambda c, role, release: {"role": role}
        deploy._restore_role_state = lambda c, role, release, state: events.append(("restore", role))
        deploy._append_deploy_audit = lambda *args, **kwargs: None
        deploy._append_exact_rollback_audit = lambda *args, **kwargs: None
        deploy._push_security = deploy._push_tiktok_rules = lambda *args, **kwargs: None
        deploy._ensure_remote_whitelist = lambda *args, **kwargs: "/tmp/wl"
        deploy._push_exit_routes = lambda *args, **kwargs: "/tmp/routes"
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
        assert {role for event, role in events if event == "unlock"} == {"entry", "middle", "exit"}

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
        if old_user is None: os.environ.pop("NB_SOCKS_USERNAME", None)
        else: os.environ["NB_SOCKS_USERNAME"] = old_user
        if old_password is None: os.environ.pop("NB_SOCKS_PASSWORD", None)
        else: os.environ["NB_SOCKS_PASSWORD"] = old_password
    print("RESULT PASS")


if __name__ == "__main__": main()
