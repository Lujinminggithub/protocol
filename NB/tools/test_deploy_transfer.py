#!/usr/bin/env python3
from __future__ import annotations

import deploy_transfer


class ServerKey:
    def get_name(self): return "ssh-ed25519"
    def get_base64(self): return "AAAAtransfer-test"


class Transport:
    def get_remote_server_key(self): return ServerKey()


class Client:
    def __init__(self, role): self.role = role
    def get_transport(self): return Transport()


def main() -> None:
    source, target = Client("middle"), Client("exit")
    manifest = {
        "deployment_id": "0123456789abcdef-0123456789ab",
        "release_id": "0123456789abcdef",
        "artifact": {"sha256": "a" * 64, "size": 1024},
    }
    checked = []

    def run(client, command, **_kwargs):
        if "sha256sum" in command and client is target:
            return ""
        return ""

    def checked_run(client, command, **_kwargs):
        checked.append((client.role, command))
        if client is source and command.startswith("cat "):
            return "ssh-ed25519 AAAAtransfer-test marker"
        if client is target and "authorized_keys" in command:
            raise RuntimeError("remote command failed rc=1: No space left on device")
        return ""

    try:
        deploy_transfer.copy_release(
            source, "middle", target, "exit", manifest,
            instance_work="/etc/NB/instances/test", role_host=lambda _role: {
                "host": "192.0.2.1", "port": 22, "user": "root"},
            run=run, checked_run=checked_run,
            push_bytes=lambda *_args, **_kwargs: None,
        )
    except RuntimeError as error:
        assert "No space left on device" in str(error)
        assert "Permission denied" not in str(error)
    else:
        raise AssertionError("target key installation failure was ignored")
    assert any(role == "exit" and "authorized_keys" in command for role, command in checked)
    print("RESULT PASS")


if __name__ == "__main__":
    main()
