#!/usr/bin/env python3
"""Fetch, verify and safely convert a sing-box SRS whitelist for NB."""
from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import pathlib
import re
import subprocess
import tempfile
import urllib.parse
import urllib.request


MD5_RE = re.compile(r"^[0-9a-fA-F]{32}$")
MAX_METADATA = 8192
MAX_SRS = 64 * 1024 * 1024
SUPPORTED_FIELDS = {"domain", "domain_suffix", "domain_keyword", "ip_cidr", "port"}
DOMAIN_LABEL_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
DNS_HYPHEN_TRANSLATION = str.maketrans({"\u2010": "-", "\u2011": "-"})
CONNECTIVITY_TEST_RULES = {"domain_exact odr.itunes.apple.com"}
REQUIRED_PUBLIC_DNS_RULES = {
    "ip 1.1.1.1/32",
    "ip 8.8.8.8/32",
    "ip 9.9.9.9/32",
    "ip 114.114.114.114/32",
}
POLICY_MARKER = "# nb-whitelist-policy: public-dns-v1"


def fetch(url: str, limit: int) -> tuple[bytes, str]:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or not parsed.hostname:
        raise ValueError("whitelist source must use HTTPS")
    request = urllib.request.Request(url, headers={"User-Agent": "NB-whitelist-sync/1.0"})
    with urllib.request.urlopen(request, timeout=30) as response:
        final_url = response.geturl()
        if urllib.parse.urlparse(final_url).scheme != "https":
            raise ValueError("whitelist redirect must remain HTTPS")
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("whitelist response exceeds size limit")
    return data, final_url


def resolve_source(source_url: str, mode: str, current_digest: str = "") -> tuple[bytes | None, str]:
    first, final_url = fetch(source_url, MAX_SRS if mode in ("direct", "auto") else MAX_METADATA)
    text = first.decode("utf-8", "replace").strip()
    metadata = text.split("|", 1)
    is_metadata = len(metadata) == 2 and MD5_RE.fullmatch(metadata[0].strip())
    if mode == "metadata" and not is_metadata:
        raise ValueError("metadata endpoint did not return MD5|URL")
    if mode == "direct" or (mode == "auto" and not is_metadata):
        return first, hashlib.md5(first).hexdigest().lower()
    expected = metadata[0].strip().lower()
    if current_digest == expected:
        return None, expected
    download_url = metadata[1].strip()
    if urllib.parse.urlparse(download_url).hostname != urllib.parse.urlparse(final_url).hostname:
        raise ValueError("metadata download URL must use the same host")
    payload, _ = fetch(download_url, MAX_SRS)
    actual = hashlib.md5(payload).hexdigest().lower()
    if actual != expected:
        raise ValueError("downloaded whitelist MD5 does not match metadata")
    return payload, actual


def values(rule: dict, field: str) -> list:
    value = rule.get(field, [])
    if value is None:
        return []
    if isinstance(value, (str, int)) and not isinstance(value, bool):
        return [value]
    if not isinstance(value, list):
        raise ValueError(f"SRS field {field} must be a scalar or array")
    return value


def normalize_domain(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("invalid SRS domain")
    domain = value.translate(DNS_HYPHEN_TRANSLATION).lower().lstrip(".").rstrip(".")
    try:
        domain = domain.encode("idna").decode("ascii")
    except UnicodeError as error:
        raise ValueError("SRS domain cannot be represented by NB") from error
    labels = domain.split(".")
    if not domain or len(domain) > 127 or any(not DOMAIN_LABEL_RE.fullmatch(label) for label in labels):
        raise ValueError("SRS domain cannot be represented by NB")
    return domain


def normalize_domain_keyword(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("invalid SRS domain keyword")
    keyword = value.lower()
    if not keyword or len(keyword) > 127 or not re.fullmatch(r"[a-z0-9._-]+", keyword):
        raise ValueError("SRS domain keyword cannot be represented by NB")
    return keyword


def convert(source: dict) -> list[str]:
    if source.get("version") not in (1, 2, 3):
        raise ValueError("unsupported sing-box rule-set version")
    rules = source.get("rules")
    if not isinstance(rules, list) or not rules:
        raise ValueError("SRS contains no rules")
    output = set(CONNECTIVITY_TEST_RULES)
    for rule in rules:
        if not isinstance(rule, dict):
            raise ValueError("invalid SRS rule")
        unsupported = set(rule) - SUPPORTED_FIELDS
        if unsupported:
            raise ValueError("SRS contains unsupported fields: " + ",".join(sorted(unsupported)))
        for domain in values(rule, "domain"):
            output.add("domain_exact " + normalize_domain(domain))
        for domain in values(rule, "domain_suffix"):
            output.add("domain_suffix " + normalize_domain(domain))
        for keyword in values(rule, "domain_keyword"):
            output.add("domain_keyword " + normalize_domain_keyword(keyword))
        for cidr in values(rule, "ip_cidr"):
            network = ipaddress.ip_network(cidr, strict=False)
            if network.version != 4:
                raise ValueError("IPv6 whitelist rules are not enabled")
            output.add("ip " + str(network))
        for port in values(rule, "port"):
            if not isinstance(port, int) or not 1 <= port <= 65535:
                raise ValueError("invalid whitelist port")
            output.add("port " + str(port))
    output.update(REQUIRED_PUBLIC_DNS_RULES)
    if any(item.startswith("port ") for item in output):
        output.add("port 53")
    if not any(item.startswith(("domain_", "ip ")) for item in output):
        raise ValueError("SRS produced no NB address rules")
    for prefix in ("domain_exact ", "domain_suffix ", "domain_keyword ", "ip ", "port "):
        if sum(item.startswith(prefix) for item in output) > 8192:
            raise ValueError("SRS exceeds the NB whitelist capacity")
    return sorted(output)


def output_has_current_policy(path: pathlib.Path) -> bool:
    if not path.is_file():
        return False
    try:
        with path.open("r", encoding="utf-8") as handle:
            return any(line.rstrip("\r\n") == POLICY_MARKER for line in handle)
    except (OSError, UnicodeError):
        return False


def decompile(binary: str, payload: bytes, directory: pathlib.Path) -> dict:
    srs = directory / "download.srs"
    source = directory / "source.json"
    srs.write_bytes(payload)
    result = subprocess.run([binary, "rule-set", "decompile", "-o", str(source), str(srs)],
                            capture_output=True, text=True, timeout=60, check=False)
    if result.returncode != 0 or not source.is_file():
        raise RuntimeError("sing-box failed to decompile the SRS whitelist")
    return json.loads(source.read_text(encoding="utf-8"))


def atomic_write(path: pathlib.Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(data, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-env", required=True)
    parser.add_argument("--mode", choices=("auto", "metadata", "direct"), default="auto")
    parser.add_argument("--sing-box", default="sing-box")
    parser.add_argument("--state-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    source_url = os.environ.get(args.source_env, "")
    if not source_url:
        raise SystemExit("whitelist source environment variable is unavailable")
    digest_path = args.state_dir / "whitelist.md5"
    current_digest = (digest_path.read_text(encoding="ascii").strip().lower()
                      if digest_path.is_file() and output_has_current_policy(args.output) else "")
    payload, digest = resolve_source(source_url, args.mode, current_digest)
    if payload is None or current_digest == digest:
        print("WHITELIST_SYNC_CHANGED=0")
        return
    args.state_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.state_dir) as temporary:
        source = decompile(args.sing_box, payload, pathlib.Path(temporary))
    rules = convert(source)
    content = POLICY_MARKER + "\n# generated from verified sing-box SRS\n" + "\n".join(rules) + "\n"
    atomic_write(args.output, content)
    atomic_write(digest_path, digest + "\n")
    print(f"WHITELIST_SYNC_CHANGED=1 rules={len(rules)}")


if __name__ == "__main__":
    main()
