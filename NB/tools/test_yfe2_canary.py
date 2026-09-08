#!/usr/bin/env python3
from nb_yfe2_canary import CANARY_STEPS, execute_canary, paired_workers


class FakeBackend:
    def __init__(self, fail_at: str = ""):
        self.fail_at = fail_at
        self.actions = []

    def run(self, action: str, role: str, worker: int) -> None:
        self.actions.append((action, role, worker))
        if action == self.fail_at:
            raise RuntimeError("注入失败")


def main() -> None:
    assert paired_workers(0, 0) == 0
    try:
        paired_workers(0, 1)
        raise AssertionError("不匹配 worker 未被拒绝")
    except ValueError as error:
        assert "编号" in str(error)

    backend = FakeBackend()
    result = execute_canary(backend, 0, 0)
    assert result["status"] == "accepted"
    assert [action for action, _, _ in backend.actions] == list(CANARY_STEPS)

    for failed_step in CANARY_STEPS:
        backend = FakeBackend(failed_step)
        try:
            execute_canary(backend, 0, 0)
            raise AssertionError(f"{failed_step} 失败未传播")
        except RuntimeError:
            pass
        actions = [action for action, _, _ in backend.actions]
        restore = actions.index("restore_middle_schema1")
        rollbacks = [index for index, action in enumerate(actions) if action.startswith("rollback_")]
        assert all(restore < index for index in rollbacks)
    print("nb_yfe2_canary tests passed")


if __name__ == "__main__":
    main()
