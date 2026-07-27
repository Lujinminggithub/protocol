#!/usr/bin/env python3
from fec_canary_run import percentile, qdisc_bytes


def main() -> None:
    assert percentile([3, 1, 2], 0.95) == 3
    assert qdisc_bytes("qdisc netem 1: root\n Sent 12345 bytes 17 pkt") == 12345
    try:
        qdisc_bytes("no counter")
    except RuntimeError:
        pass
    else:
        raise AssertionError("missing qdisc counter accepted")
    print("fec_canary_run tests passed")


if __name__ == "__main__":
    main()
