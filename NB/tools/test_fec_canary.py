#!/usr/bin/env python3
from fec_canary import evaluate


def sample() -> dict:
    return {"schema_version": 1, "pairs": [{
        "baseline": {"integrity": "ok", "latency_p95_ms": 100, "wire_bytes": 1000,
                     "cpu_pct": 20, "memory_mb": 100},
        "candidate": {"integrity": "ok", "fec_active": True, "latency_p95_ms": 85,
                      "wire_bytes": 1250, "cpu_pct": 25, "memory_mb": 120,
                      "recovered_packets": 4},
    } for _ in range(3)]}


def main() -> None:
    assert evaluate(sample())["status"] == "admit"
    insufficient = sample();insufficient["pairs"] = insufficient["pairs"][:2]
    assert "insufficient-paired-rounds" in evaluate(insufficient)["reasons"]
    overhead = sample();overhead["pairs"][0]["candidate"]["wire_bytes"] = 1500
    assert "bandwidth-overhead" in evaluate(overhead)["reasons"]
    integrity = sample();integrity["pairs"][1]["candidate"]["integrity"] = "bad"
    assert evaluate(integrity)["status"] == "reject"
    print("fec_canary tests passed")


if __name__ == "__main__":
    main()
