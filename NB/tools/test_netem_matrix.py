#!/usr/bin/env python3
import json
import pathlib
import tempfile
from types import SimpleNamespace

from netem_matrix import (contract_met, load_scenarios, parse_losses, segment_target, summarize_cases,
                          summarize_observation, validate_cluster, validate_resources)


def snapshot(age: int = 0, loss: float = 0.0, status: str = "ok") -> dict:
    return {
        "collection_errors": [],
        "workers": [{
            "role": "middle",
            "worker": "0",
            "health": {"status": status, "release_id": "r1",
                       "line_profile": "line", "line_profile_schema": 3},
            "metrics": {
                "queue_age_max_us": {"down": age, "up": 0, "q2t": 0},
                "link": {"effective_loss_max_pct": loss, "reorder_delay_max_us": 1200},
                "fec": {"active": 0},
            },
        }],
    }


def main() -> None:
    assert parse_losses("0,1,2.5") == [0.0, 1.0, 2.5]
    try:
        parse_losses("31")
        raise AssertionError("越界 loss 未被拒绝")
    except ValueError:
        pass
    summary = summarize_observation([snapshot(100, 1.0), snapshot(300, 3.0)])
    assert summary["samples"] == 2
    assert summary["queue_age_max_us"] == 300
    assert summary["effective_loss_p95_pct"] == 3.0
    assert not summary["collection_errors"]
    assert segment_target("middle-exit")["receiver_role"] == "exit"
    assert segment_target("entry-middle")["receiver_role"] == "middle"
    cases = [
        {"loss_pct": 1, "delay_ms": 0, "jitter_ms": 0, "reorder_pct": 0,
         "passed": True, "load": {"achieved_mbps": 4}, "observation": {"queue_age_p95_us": 10}},
        {"loss_pct": 1, "delay_ms": 0, "jitter_ms": 0, "reorder_pct": 0,
         "passed": False, "load": {"achieved_mbps": 2}, "observation": {"queue_age_p95_us": 20}},
    ]
    aggregate = summarize_cases(cases)[0]
    assert aggregate["rounds"] == 2 and aggregate["pass_rate"] == 0.5
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "matrix.json"
        path.write_text(json.dumps({"scenarios": [
            {"name": "clean", "loss_pct": 0},
            {"name": "reorder", "delay_ms": 20, "reorder_pct": 3},
            {"name": "boundary", "delay_ms": 80,
             "expected_by_segment": {"entry-middle": "reject"}},
        ]}), encoding="utf-8")
        scenarios = load_scenarios(path, [], SimpleNamespace(delay_ms=0, jitter_ms=0, reorder_pct=0),
                                   "entry-middle")
        assert len(scenarios) == 3 and scenarios[1]["category"] == "custom"
        assert scenarios[2]["expected"] == "reject"
    assert contract_met({"passed": False, "failure_reasons": ["throughput"]}, "reject")
    assert contract_met({"passed": False, "failure_reasons": ["probe-error"],
                         "error": "TimeoutError: timed out"}, "reject")
    assert not contract_met({"passed": False, "failure_reasons": ["collection-error"]}, "reject")
    cluster = {"collection_errors": [], "workers": []}
    for role in ("entry", "middle", "exit"):
        item = snapshot()["workers"][0]
        item["role"] = role
        cluster["workers"].append(item)
    validate_cluster(cluster)
    cluster["workers"][1]["metrics"]["fec"]["active"] = 1
    try:
        validate_cluster(cluster)
        raise AssertionError("active FEC 未被拒绝")
    except RuntimeError:
        pass
    validate_resources({"mem_kb": 1024 * 1024, "sshd": 5, "oom_kill": 2},
                       {"oom_kill": 2}, 256, 64)
    try:
        validate_resources({"mem_kb": 100, "sshd": 5, "oom_kill": 2},
                           {"oom_kill": 2}, 256, 64)
        raise AssertionError("低内存未被拒绝")
    except RuntimeError:
        pass
    print("netem_matrix tests passed")


if __name__ == "__main__":
    main()
