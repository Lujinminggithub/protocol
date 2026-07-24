#!/usr/bin/env python3
import deploy


class FakeClient:
    def __init__(self):
        self.close_count = 0

    def close(self):
        self.close_count += 1


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
    print("deploy connection lifecycle tests passed")


if __name__ == "__main__":
    main()
