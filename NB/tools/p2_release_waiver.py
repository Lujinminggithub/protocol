#!/usr/bin/env python3
"""Create and verify a signed, evidence-bound P2 restricted-release waiver."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import hmac
import json
import os
import pathlib


POLICY = "p2-restricted-release-v1"
EXCEPTIONS = ["P2-A-ipv6", "P2-B-fec-active", "P2-C-24h-soak"]
RESTRICTIONS = {
    "network_scope": "ipv4-only",
    "fec_mode": "observe-only",
    "fec_active": False,
    "max_public_udp_payload_bytes": 1001,
}


def canonical(value: dict) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_key(path: pathlib.Path | None = None) -> bytes:
    value = os.environ.get("NB_RELEASE_WAIVER_KEY", "").encode("utf-8")
    if not value and path is not None and path.is_file():
        value = path.read_bytes().strip()
    if len(value) < 32:
        raise ValueError("release waiver signing key must contain at least 32 bytes")
    return value


def sign(document: dict, key: bytes) -> None:
    document.pop("signature_hmac_sha256", None)
    document["signature_hmac_sha256"] = hmac.new(
        key, canonical(document), hashlib.sha256).hexdigest()


def create(reports: dict[str, pathlib.Path], approved_by: str, deployment: str,
           expires_at_utc: str, rationale: str, key: bytes,
           now: dt.datetime | None = None) -> dict:
    if not approved_by.strip() or any(ch in approved_by for ch in "\r\n\0"):
        raise ValueError("approved_by must be a non-empty single line")
    if not deployment.strip():
        raise ValueError("deployment is required")
    expires = parse_time(expires_at_utc)
    created = now or dt.datetime.now(dt.timezone.utc)
    if expires <= created:
        raise ValueError("waiver expiry must be in the future")
    evidence = {name: sha256(path) for name, path in sorted(reports.items())}
    document = {
        "schema_version": 1,
        "policy": POLICY,
        "state": "approved",
        "approved_at_utc": iso_time(created),
        "expires_at_utc": iso_time(expires),
        "approved_by": approved_by,
        "deployment": deployment,
        "exceptions": EXCEPTIONS,
        "restrictions": RESTRICTIONS,
        "evidence_sha256": evidence,
        "rationale": rationale,
    }
    document["approval_id"] = hashlib.sha256(canonical(document)).hexdigest()[:16]
    sign(document, key)
    return document


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp must include a timezone")
    return parsed.astimezone(dt.timezone.utc)


def iso_time(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def verify(document: dict, reports: dict[str, pathlib.Path], key: bytes,
           now: dt.datetime | None = None) -> None:
    signature = document.get("signature_hmac_sha256", "")
    unsigned = {name: value for name, value in document.items()
                if name != "signature_hmac_sha256"}
    expected = hmac.new(key, canonical(unsigned), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(signature, expected):
        raise ValueError("release waiver signature is invalid")
    if (document.get("schema_version") != 1 or document.get("policy") != POLICY or
            document.get("state") != "approved"):
        raise ValueError("release waiver policy or state is invalid")
    if document.get("exceptions") != EXCEPTIONS:
        raise ValueError("release waiver exception scope is invalid")
    if document.get("restrictions") != RESTRICTIONS:
        raise ValueError("release waiver restrictions are invalid")
    if parse_time(str(document.get("expires_at_utc", ""))) <= (
            now or dt.datetime.now(dt.timezone.utc)):
        raise ValueError("release waiver has expired")
    actual = {name: sha256(path) for name, path in sorted(reports.items())}
    if document.get("evidence_sha256") != actual:
        raise ValueError("release waiver evidence has drifted")


def main() -> int:
    parser = argparse.ArgumentParser(description="P2 restricted-release waiver")
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("create", "verify"):
        item = sub.add_parser(command)
        for name in ("pmtu", "fec", "fault", "soak"):
            item.add_argument(f"--{name}", type=pathlib.Path, required=True)
        item.add_argument("--key-file", type=pathlib.Path)
        item.add_argument("--waiver", type=pathlib.Path, required=True)
    create_parser = sub.choices["create"]
    create_parser.add_argument("--approved-by", required=True)
    create_parser.add_argument("--deployment", required=True)
    create_parser.add_argument("--expires-at-utc", required=True)
    create_parser.add_argument("--rationale", required=True)
    args = parser.parse_args()
    reports = {name: getattr(args, name) for name in ("pmtu", "fec", "fault", "soak")}
    key = load_key(args.key_file)
    if args.command == "create":
        document = create(reports, args.approved_by, args.deployment,
                          args.expires_at_utc, args.rationale, key)
        args.waiver.parent.mkdir(parents=True, exist_ok=True)
        args.waiver.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
    else:
        document = json.loads(args.waiver.read_text(encoding="utf-8"))
        verify(document, reports, key)
    print(json.dumps({"status": "verified", "approval_id": document["approval_id"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
