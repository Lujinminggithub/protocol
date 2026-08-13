#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path

import worker_snapshot


class WorkerSnapshotTest(unittest.TestCase):
    def test_role_collection_failure_does_not_create_collector_node(self):
        original = worker_snapshot.query_role

        def query(role):
            if role == "entry":
                raise RuntimeError("temporary entry collection failure")
            return [{
                "path": f"/run/nb-line-1-{role}-0.ctl",
                "health": {"worker": f"{role}-0", "status": "ok"},
                "metrics": {},
            }]

        worker_snapshot.query_role = query
        try:
            samples = worker_snapshot.collect("line-1")
        finally:
            worker_snapshot.query_role = original

        self.assertEqual([sample["role"] for sample in samples], ["middle", "exit"])
        self.assertFalse(any(sample["node_id"].endswith("-collector") for sample in samples))

    def test_protocol_sessions_and_interval_throughput(self):
        record = {
            "path": "/run/nb-entry-0.sock",
            "health": {"node_id": "nb-entry-0", "status": "ok"},
            "metrics": {"sessions": 3, "bytes": {"c2s": 600, "s2c": 400}},
        }
        first = worker_snapshot.snapshot("line-1", "entry", "2026-07-29T00:00:00Z", record)
        self.assertEqual(first["sessions"], 3)

        with tempfile.TemporaryDirectory() as directory:
            state_file = Path(directory) / "counters.json"
            worker_snapshot.apply_throughput([first], state_file)
            self.assertEqual(first["throughput_mbps"], 0)
            self.assertNotIn("_bytes_total", first)

            record["metrics"]["bytes"] = {"c2s": 1_000_600, "s2c": 1_000_400}
            record["metrics"]["udp_errors"] = {"rxq_overflow": 7}
            second = worker_snapshot.snapshot("line-1", "entry", "2026-07-29T00:00:10Z", record)
            worker_snapshot.apply_throughput([second], state_file)
            self.assertAlmostEqual(second["throughput_mbps"], 1.6)
            self.assertAlmostEqual(second["upstream_mbps"], 0.8)
            self.assertAlmostEqual(second["downstream_mbps"], 0.8)
            self.assertEqual(second["health"], "degraded")
            self.assertEqual(second["payload"]["rxq_overflow_delta"], 7)
            persisted = json.loads(state_file.read_text(encoding="utf-8"))
            self.assertEqual(persisted["nb-entry-0"]["bytes_c2s"], 1_000_600)
            self.assertEqual(persisted["nb-entry-0"]["bytes_s2c"], 1_000_400)
            self.assertEqual(persisted["nb-entry-0"]["rxq_overflow_total"], 7)


if __name__ == "__main__":
    unittest.main()
