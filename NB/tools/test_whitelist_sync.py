#!/usr/bin/env python3
import pathlib
import tempfile

import whitelist_sync


def main() -> None:
    rules = whitelist_sync.convert({
        "version": 1,
        "rules": [{
            "domain": ["Exact.Example"],
            "domain_suffix": [".example.org"],
            "ip_cidr": ["10.1.2.3/8"],
            "port": [443],
        }],
    })
    assert "domain_exact exact.example" in rules
    assert "domain_exact odr.itunes.apple.com" in rules
    assert "domain_suffix example.org" in rules
    assert "ip 10.0.0.0/8" in rules
    assert "port 443" in rules
    for port in (50000, 50001, 50008, 50009, 50020, 50021):
        assert f"port {port}" in rules
    for address in ("1.1.1.1/32", "8.8.8.8/32", "9.9.9.9/32", "114.114.114.114/32"):
        assert "ip " + address in rules
    assert "domain_suffix tiktok-row.net" in rules
    assert "port 53" in rules

    unrestricted_rules = whitelist_sync.convert({
        "version": 1,
        "rules": [{"domain": ["unrestricted.example"]}],
    })
    assert "port 53" not in unrestricted_rules
    assert not any(f"port {port}" in unrestricted_rules
                   for port in (50000, 50001, 50008, 50009, 50020, 50021))
    scalar_rules = whitelist_sync.convert({
        "version": 3,
        "rules": [{"domain": "scalar.example", "domain_keyword": "bytecdn", "ip_cidr": "192.0.2.8/29", "port": 8443}],
    })
    assert "domain_exact scalar.example" in scalar_rules
    assert "domain_keyword bytecdn" in scalar_rules
    assert "ip 192.0.2.8/29" in scalar_rules
    assert "port 8443" in scalar_rules
    unicode_hyphen_rules = whitelist_sync.convert({
        "version": 1,
        "rules": [{"domain_suffix": ["tiktok\u2011minis.com"]}],
    })
    assert "domain_suffix tiktok-minis.com" in unicode_hyphen_rules
    try:
        whitelist_sync.convert({"version": 1, "rules": [{"domain_suffix": ["tiktok/minis.com"]}]})
        raise AssertionError("unsafe domain punctuation was accepted")
    except ValueError as error:
        assert "cannot be represented" in str(error)
    try:
        whitelist_sync.convert({"version": 1, "rules": [{"domain_regex": [".*"]}]})
        raise AssertionError("unsupported SRS rule was accepted")
    except ValueError as error:
        assert "unsupported" in str(error)
    try:
        whitelist_sync.convert({"version": 1, "rules": [{"ip_cidr": ["2001:db8::/32"]}]})
        raise AssertionError("IPv6 SRS rule was accepted")
    except ValueError as error:
        assert "IPv6" in str(error)

    with tempfile.TemporaryDirectory() as directory:
        output = pathlib.Path(directory) / "whitelist.conf"
        output.write_text("# stale policy\n", encoding="utf-8")
        assert not whitelist_sync.output_has_current_policy(output)
        output.write_text("# nb-whitelist-policy: public-dns-v1\n", encoding="utf-8")
        assert not whitelist_sync.output_has_current_policy(output)
        output.write_text(whitelist_sync.POLICY_MARKER + "\n", encoding="utf-8")
        assert whitelist_sync.output_has_current_policy(output)
    print("whitelist SRS conversion tests passed")


if __name__ == "__main__":
    main()
