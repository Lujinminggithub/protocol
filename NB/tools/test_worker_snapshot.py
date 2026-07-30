#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path

import worker_snapshot


class WorkerSnapshotTest(unittest.TestCase):
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
            second = worker_snapshot.snapshot("line-1", "entry", "2026-07-29T00:00:10Z", record)
            worker_snapshot.apply_throughput([second], state_file)
            self.assertAlmostEqual(second["throughput_mbps"], 1.6)
            persisted = json.loads(state_file.read_text(encoding="utf-8"))
            self.assertEqual(persisted["nb-entry-0"]["bytes_total"], 2_001_000)


if __name__ == "__main__":
    unittest.main()
