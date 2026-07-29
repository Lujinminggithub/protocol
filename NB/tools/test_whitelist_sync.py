#!/usr/bin/env python3
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
    assert "domain_suffix example.org" in rules
    assert "ip 10.0.0.0/8" in rules
    assert "port 443" in rules
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
    print("whitelist SRS conversion tests passed")


if __name__ == "__main__":
    main()
