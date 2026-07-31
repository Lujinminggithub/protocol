#!/usr/bin/env python3
import deploy


class FakeClient:
    def __init__(self):
        self.close_count = 0

    def close(self):
        self.close_count += 1


class FakeServerKey:
    def get_name(self): return "ssh-ed25519"
    def get_base64(self): return "AAAATESTHOSTKEY"


class FakeTransport:
    def get_remote_server_key(self): return FakeServerKey()


class TransferClient(FakeClient):
    def __init__(self, role):
        super().__init__(); self.role = role
    def get_transport(self): return FakeTransport()


class FakeStream:
    def __init__(self, data=b"", status=0):
        self.data = data
        self.status = status
        self.channel = self

    def read(self):
        return self.data

    def recv_exit_status(self):
        return self.status


class FakeExecClient:
    def __init__(self, status):
        self.status = status

    def exec_command(self, _command, timeout=120):
        return None, FakeStream(b"output", self.status), FakeStream(b"error")


def main() -> None:
    target = FakeClient()
    jump = FakeClient()
    deploy._bind_jump_lifecycle(target, jump)
    target.close()
    target.close()
    assert target.close_count == 1
    assert jump.close_count == 1

    direct = FakeClient()
    deploy._bind_jump_lifecycle(direct, None)
    direct.close()
    assert direct.close_count == 1
    assert deploy.ssh_retry_delay(0, jitter=1.0) == 1.0
    assert deploy.ssh_retry_delay(1, jitter=1.0) == 2.0
    assert deploy.ssh_retry_delay(10, cap=12.0, jitter=1.0) == 12.0
    jump_globals = deploy._jump_strategies.__globals__
    original_lab = jump_globals["LAB"]
    try:
        jump_globals["LAB"] = {"entry": {"jump_policy": "direct"},
                               "middle": {"jump_policy": "auto", "jump_candidates": ["entry"], "jump_via": "entry"}}
        assert deploy._jump_strategies("entry") == [None]
        assert deploy._jump_strategies("middle") == [None, "entry"]
    finally:
        jump_globals["LAB"] = original_lab
    assert deploy.checked_run(FakeExecClient(0), "true") == "outputerror"
    try:
        deploy.checked_run(FakeExecClient(7), "false")
        raise AssertionError("checked_run accepted a failing remote command")
    except RuntimeError as error:
        assert "rc=7" in str(error)

    bind_globals = deploy._verify_exit_bind_ip.__globals__
    original_bind = {name: bind_globals[name] for name in ("LAB", "run")}
    try:
        bind_globals["LAB"] = {"exit": {"host": "203.0.113.3", "outip": "203.0.113.30"}}
        checked = []
        bind_globals["run"] = lambda client, command, tmo=120: checked.append(command) or "BIND_IP_OK"
        assert deploy._verify_exit_bind_ip(FakeClient()) == "203.0.113.30"
        assert checked and "grep -Fx -- 203.0.113.30" in checked[0]
        bind_globals["run"] = lambda client, command, tmo=120: ""
        try:
            deploy._verify_exit_bind_ip(FakeClient())
            raise AssertionError("unassigned exit bind address was accepted")
        except RuntimeError as error:
            assert "not assigned" in str(error)
        bind_globals["LAB"]["exit"]["outip"] = "2001:db8::1"
        try:
            deploy._exit_bind_ip()
            raise AssertionError("IPv6 exit bind address was accepted")
        except ValueError as error:
            assert "IPv4" in str(error)
    finally:
        bind_globals.update(original_bind)

    transfer_globals = deploy._copy_release_between_nodes.__globals__
    original_transfer = {name: transfer_globals[name] for name in ("LAB", "INSTANCE_WORK", "run", "push_bytes")}
    commands = []; uploads = []
    source = TransferClient("entry"); target = TransferClient("exit")
    try:
        transfer_globals["LAB"] = {
            "entry": {"host": "192.0.2.1", "port": 22, "user": "root"},
            "exit": {"host": "203.0.113.3", "port": 2273, "user": "root"},
        }
        transfer_globals["INSTANCE_WORK"] = "/etc/NB/instances/test"
        def fake_run(client, command, tmo=120):
            commands.append((client.role, command, tmo))
            if "cat /tmp/nb-release-" in command and command.endswith(".pub"):
                return "ssh-ed25519 AAAATESTKEY generated"
            if "stat -c %s" in command:
                return "0"
            if "echo VERIFIED" in command:
                return "VERIFIED"
            return ""
        transfer_globals["run"] = fake_run
        transfer_globals["push_bytes"] = lambda client, data, path, mode=0o644: uploads.append((client.role, data, path, mode))
        manifest = {
            "release_id": "0123456789abcdef",
            "deployment_id": "0123456789abcdef-aaaaaaaaaaaa",
            "artifact": {"sha256": "b" * 64, "size": 1234},
        }
        address = deploy._copy_release_between_nodes(source, "entry", target, "exit", manifest)
        assert address == "203.0.113.3"
        source_commands = [command for role, command, _ in commands if role == "entry"]
        target_commands = [command for role, command, _ in commands if role == "exit"]
        assert any("tail -c +1" in command and "-p 2273" in command for command in source_commands)
        assert any("authorized_keys" in command for command in target_commands)
        assert any("awk '$NF !=" in command for command in target_commands)
        assert uploads and b"[203.0.113.3]:2273 ssh-ed25519 AAAATESTHOSTKEY" in uploads[0][1]
        assert all("password" not in command.lower() for _, command, _ in commands)
    finally:
        transfer_globals.update(original_transfer)
    print("deploy connection lifecycle tests passed")


if __name__ == "__main__":
    main()
