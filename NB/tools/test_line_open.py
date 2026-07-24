#!/usr/bin/env python3
from line_open import baseline_profile, normalize_hosts


def main() -> None:
    source = {"machines": [
        {"role": "entry", "host": "192.0.2.1", "password": "e"},
        {"role": "middle", "host": "192.0.2.2", "private_ip": "10.0.0.2", "password": "m"},
        {"role": "exit", "host": "192.0.2.3", "port": 2222, "password": "x"},
    ]}
    hosts, credentials = normalize_hosts(source)
    assert hosts["middle"]["private_ip"] == "10.0.0.2"
    assert "password" not in hosts["entry"]
    assert credentials["NB_SSH_PASSWORD_ENTRY"] == "e"
    assert hosts["exits"][0]["host"] == "192.0.2.3"
    profile = baseline_profile(hosts, "test-line")
    assert profile["transport"]["entry_middle"]["address"] == "10.0.0.2:4443"
    assert profile["fixed_exit"] == hosts["exit"]["name"]
    legacy = {"edges": [{"host": "1.1.1.1"}], "relays": [{"host": "2.2.2.2"}],
              "terminals": [{"host": "3.3.3.3"}]}
    converted, _ = normalize_hosts(legacy)
    assert converted["entry"]["host"] == "1.1.1.1"
    print("line_open tests passed")


if __name__ == "__main__":
    main()
