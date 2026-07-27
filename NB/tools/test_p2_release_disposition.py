#!/usr/bin/env python3
import json
import pathlib
import tempfile

from p2_release_disposition import build_fec, build_pmtu, build_soak


def write(path: pathlib.Path, value: str) -> pathlib.Path:
    path.write_text(value, encoding="utf-8")
    return path


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        baseline = write(root / "baseline.json", '''{
          "integrity": "ok", "integrity": "count-ok",
          "quic_ip_mtu_proven": 1452, "quic_ip_mtu_proven": 1452,
          "broken": "legacy text?, }''')
        blackhole = write(root / "blackhole.json", json.dumps({
            "status": "passed", "mtu_before": 1424, "mtu_after": 1252,
            "integrity_after": {"integrity": "ok"}}))
        udp = write(root / "udp.json", json.dumps({"status": "failed", "cases": [
            {"payload_bytes": size, "passed": True, "integrity": "ok"}
            for size in (1, 999, 1000, 1001)] + [
            {"payload_bytes": 60000, "passed": False, "integrity": "timeout"}]}))
        pmtu = build_pmtu(baseline, blackhole, udp)
        assert pmtu["status"] == "restricted-passed"

        fec_path = write(root / "fec.json", json.dumps({
            "status": "failed", "restored_fec_observe_only": True,
            "pairs": [{"candidate": {"integrity": "ok"}}]}))
        assert build_fec([fec_path])["status"] == "rejected-for-active"

        chunks = ','.join('{"injected":false,"integrity":"ok","passed":true}'
                          for _ in range(153))
        soak_path = write(root / "soak.json", '{"requested_duration_s":86400,'
            f'"chunks":[{chunks},{{"injected":false,"integrity":"ok","passed":false}}],'
            '"status":"failed","validated_duration_s":45930.949}')
        soak = build_soak(soak_path)
        assert soak["status"] == "waiver-eligible"
        assert soak["passed_chunks"] == 153
    print("p2_release_disposition tests passed")


if __name__ == "__main__":
    main()
