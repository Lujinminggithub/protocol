#!/usr/bin/env python3
from nb_observe import evaluate


def worker(role, release="a", loss=0.0, age=0, errors=0, rxq_overflow=0):
    return {"role": role, "worker": "0",
        "health": {"status": "ok", "release_id": release, "line_profile": "line", "line_profile_schema": 2},
        "metrics": {"queue_age_max_us": {"down": age, "up": 0, "q2t": 0},
                    "link": {"sent_packets": 200, "effective_loss_max_pct": loss},
                    "udp_errors": {"rx": errors, "tx": 0, "rxq_overflow": rxq_overflow}, "closed": {"error": 0},
                    "event_loop": {"over_20ms": 0}}}


def main():
    snapshot = {"collected_at_utc": "x", "collection_errors": [],
                "workers": [worker("entry", loss=9.0), worker("middle"), worker("exit")]}
    alerts, state = evaluate(snapshot)
    assert any(a["code"] == "effective-loss" and a["severity"] == "critical" and a["streak"] == 1 for a in alerts)
    alerts, _ = evaluate(snapshot, state)
    assert next(a for a in alerts if a["code"] == "effective-loss")["streak"] == 2
    mismatch = {"collected_at_utc": "x", "collection_errors": [],
                "workers": [worker("entry", "a"), worker("middle", "b"), worker("exit", "a")]}
    alerts, _ = evaluate(mismatch)
    assert any(a["code"] == "release-mismatch" for a in alerts)
    errors = {"collected_at_utc": "x", "collection_errors": [{"role": "middle", "error": "down"}],
              "workers": [worker("entry"), worker("exit")]}
    alerts, _ = evaluate(errors)
    assert any(a["code"] == "node-unreachable" for a in alerts)
    baseline = {"collected_at_utc": "x", "collection_errors": [],
                "workers": [worker("entry"), worker("middle"), worker("exit")]}
    _, baseline_state = evaluate(baseline)
    overflow = {"collected_at_utc": "y", "collection_errors": [],
                "workers": [worker("entry", rxq_overflow=3), worker("middle"), worker("exit")]}
    alerts, _ = evaluate(overflow, baseline_state)
    assert any(a["code"] == "udp-rxq-overflow" and a["value"] == 3 for a in alerts)
    print("RESULT PASS")


if __name__ == "__main__": main()
