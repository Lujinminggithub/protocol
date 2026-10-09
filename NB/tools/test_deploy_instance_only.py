#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import os

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import deploy_shard_runtime as runtime
import deploy


class Remote:
    def __init__(self, ready: bool = True) -> None:
        self.ready = ready
        self.commands: list[str] = []
        self.uploads: list[str] = []

    def run(self, _client, command: str, **_kwargs) -> str:
        self.commands.append(command)
        if "RUNTIME_READY" in command:
            return "RUNTIME_READY\n" if self.ready else "RUNTIME_MISSING\n"
        if "CONFIG_PRESENT" in command:
            return "CONFIG_ABSENT\n"
        if "INSTANCE_CONTROL_OK" in command:
            return "INSTANCE_CONTROL_OK\n"
        return ""

    def push(self, _client, _data: bytes, path: str, **_kwargs) -> None:
        self.uploads.append(path)


def install(remote: Remote) -> str:
    return runtime.install_instance(
        None,
        "entry",
        "/etc/NB/instances/line-a/nb_node -r entry -l 1082 -n 10.0.0.2 -N 4445 -S",
        {"NB_CC": "bbr"},
        "cfg-0123456789abcdef",
        work="/etc/NB",
        instance_work="/etc/NB/instances/line-a",
        deploy_instance="line-a",
        lab={},
        run=remote.run,
        push_bytes=remote.push,
        effective_workers=lambda _role: 2,
    )


def main() -> None:
    remote = Remote()
    result = install(remote)
    commands = "\n".join(remote.commands)
    uploads = "\n".join(remote.uploads)
    assert "instance controls=ok" in result
    assert any(path.endswith("/line-a.conf.next") for path in remote.uploads)
    assert "systemctl reload nb-entry-shard@0" in commands
    assert "INSTANCE_CONTROL_OK" in commands
    for forbidden in ("systemctl restart", "systemctl stop", "ln -sfn", ".nb_node.next"):
        assert forbidden not in commands, forbidden
    for forbidden in ("/etc/systemd/system/", "/shards/nb_node"):
        assert forbidden not in uploads, forbidden

    missing = Remote(ready=False)
    try:
        install(missing)
    except RuntimeError as error:
        assert "runtime-preflight" in str(error)
    else:
        raise AssertionError("missing shared runtime was accepted")
    assert not missing.uploads
    assert not any(".conf.next" in command for command in missing.commands)

    names = (
        "DEPLOY_INSTANCE",
        "connect", "_acquire_deploy_lock", "_release_deploy_lock", "_require_security_material",
        "_push_security", "_push_tiktok_rules", "_push_whitelist", "_ensure_remote_whitelist",
        "_push_exit_routes", "_verify_exit_bind_ip", "_node_command", "_install_instance_role",
        "_rollback_instance_role", "_smoke_socks",
    )
    original = {name: getattr(deploy, name) for name in names}
    previous_user = os.environ.get("NB_SOCKS_USERNAME")
    previous_password = os.environ.get("NB_SOCKS_PASSWORD")
    events: list[tuple] = []

    class Client:
        def __init__(self, role: str) -> None:
            self.role = role

        def close(self) -> None:
            events.append(("close", self.role))

    try:
        os.environ["NB_SOCKS_USERNAME"] = "user"
        os.environ["NB_SOCKS_PASSWORD"] = "password"
        deploy.DEPLOY_INSTANCE = "line-a"
        deploy.connect = lambda role: Client(role)
        deploy._acquire_deploy_lock = lambda _client, role, _deployment: events.append(("lock", role))
        deploy._release_deploy_lock = lambda client: events.append(("unlock", client.role))
        deploy._require_security_material = lambda: None
        deploy._push_security = deploy._push_tiktok_rules = lambda *_args, **_kwargs: None
        deploy._push_whitelist = lambda _client, _path, role: f"/line/{role}/whitelist.conf"
        deploy._ensure_remote_whitelist = lambda _client, role="exit": f"/line/{role}/whitelist.conf"
        deploy._push_exit_routes = lambda *_args, **_kwargs: "/line/entry/routes.conf"
        deploy._verify_exit_bind_ip = lambda _client: "198.51.100.9"
        deploy._node_command = lambda role, **_kwargs: role

        def install_role(client, role, _command, *, deployment_id):
            events.append(("install", role, deployment_id))
            if role == "middle":
                raise RuntimeError("injected instance failure")
            return "ok"

        deploy._install_instance_role = install_role
        deploy._rollback_instance_role = lambda client, role, deployment_id: events.append(
            ("rollback", role, deployment_id))
        deploy._smoke_socks = lambda _port: events.append(("smoke",))
        try:
            deploy.act_deploy_instance("cfg-0123456789abcdef", 1082)
        except RuntimeError as error:
            assert "injected instance failure" in str(error)
        else:
            raise AssertionError("injected instance failure did not abort deployment")
        assert [item[:2] for item in events if item[0] == "install"] == [
            ("install", "exit"), ("install", "middle")]
        assert [item[:2] for item in events if item[0] == "rollback"] == [("rollback", "exit")]
        assert {item[1] for item in events if item[0] == "unlock"} == {"entry", "middle", "exit"}
    finally:
        for name, value in original.items():
            setattr(deploy, name, value)
        if previous_user is None:
            os.environ.pop("NB_SOCKS_USERNAME", None)
        else:
            os.environ["NB_SOCKS_USERNAME"] = previous_user
        if previous_password is None:
            os.environ.pop("NB_SOCKS_PASSWORD", None)
        else:
            os.environ["NB_SOCKS_PASSWORD"] = previous_password

    rollback_names = ("DEPLOY_INSTANCE", "INSTANCE_WORK", "WORK", "run", "_effective_workers",
                      "_verify_deployment_health", "_activate_release")
    rollback_original = {name: getattr(deploy, name) for name in rollback_names}
    rollback_commands: list[str] = []
    try:
        deploy.DEPLOY_INSTANCE = "line-a"
        deploy.INSTANCE_WORK = "/etc/NB/instances/line-a"
        deploy.WORK = "/etc/NB"
        deploy._effective_workers = lambda _role: 2

        def rollback_run(_client, command: str, **_kwargs) -> str:
            rollback_commands.append(command)
            if "CONFIG_DEPLOYMENT_READY" in command:
                return "CONFIG_DEPLOYMENT_READY\n"
            return ""

        deploy.run = rollback_run
        deploy._verify_deployment_health = lambda *_args, **_kwargs: "config-health=ok"
        deploy._activate_release = lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("config rollback activated a binary release"))
        result = deploy._activate_existing_deployment(
            Client("entry"), "entry", "cfg-0123456789abcdef")
        assert result == "config-health=ok"
        commands = "\n".join(rollback_commands)
        assert "systemctl reload nb-entry-shard@0" in commands
        assert "systemctl restart" not in commands and "systemctl stop" not in commands
    finally:
        for name, value in rollback_original.items():
            setattr(deploy, name, value)
    print("deploy instance-only tests passed")


if __name__ == "__main__":
    main()
