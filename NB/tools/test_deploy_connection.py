#!/usr/bin/env python3
import deploy


class FakeClient:
    def __init__(self):
        self.close_count = 0

    def close(self):
        self.close_count += 1


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
    print("deploy connection lifecycle tests passed")


if __name__ == "__main__":
    main()
