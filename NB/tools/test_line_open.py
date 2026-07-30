#!/usr/bin/env python3
import pathlib
import os
import subprocess
import tempfile

from line_open import (artifacts_match, baseline_profile, load_checkpoint,
                       deploy_socks_with_retry,
                       load_or_create_client_secret,
                       normalize_hosts, reconcile_socks_user, save_checkpoint, sha256_file)


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
    attempts, delays = [], []
    def transient_runner(command, _env):
        attempts.append(command)
        if len(attempts) < 3:
            raise subprocess.CalledProcessError(1, command)
    deploy_socks_with_retry(1082, {}, runner=transient_runner, sleeper=delays.append,
                            delays=(1, 2))
    assert len(attempts) == 3 and delays == [1, 2]
    legacy = {"edges": [{"host": "1.1.1.1"}], "relays": [{"host": "2.2.2.2"}],
              "terminals": [{"host": "3.3.3.3"}]}
    converted, _ = normalize_hosts(legacy)
    assert converted["entry"]["host"] == "1.1.1.1"
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        secret_path = root / "bootstrap-client-secret.json"
        old_socks, old_client = os.environ.get("NB_SOCKS_PASSWORD"), os.environ.get("NB_CLIENT_PASSWORD")
        try:
            os.environ["NB_SOCKS_PASSWORD"] = "line-scoped"
            os.environ["NB_CLIENT_PASSWORD"] = "process-global"
            secret = load_or_create_client_secret(secret_path, "nbmobile")
            assert secret["password"] == "line-scoped"
            os.environ["NB_SOCKS_PASSWORD"] = "changed"
            assert load_or_create_client_secret(secret_path, "nbmobile")["password"] == "line-scoped"
            try:
                load_or_create_client_secret(secret_path, "another-user")
            except RuntimeError:
                pass
            else:
                raise AssertionError("incompatible existing client secret was accepted")
            users_path = root / "socks.users"
            users_path.write_text("nbmobile:300000:" + "00" * 16 + ":" + "00" * 32 + "\n", encoding="ascii")
            assert reconcile_socks_user(users_path, "nbmobile", "line-scoped")
            assert not reconcile_socks_user(users_path, "nbmobile", "line-scoped")
            record = users_path.read_text(encoding="ascii").strip().split(":")
            assert len(record) == 4 and record[0] == "nbmobile"
        finally:
            if old_socks is None:
                os.environ.pop("NB_SOCKS_PASSWORD", None)
            else:
                os.environ["NB_SOCKS_PASSWORD"] = old_socks
            if old_client is None:
                os.environ.pop("NB_CLIENT_PASSWORD", None)
            else:
                os.environ["NB_CLIENT_PASSWORD"] = old_client
        artifact = root / "stable.json"
        artifact.write_text('{"status":"stable"}\n', encoding="utf-8")
        checkpoint_path = root / "open-checkpoint.json"
        checkpoint = load_checkpoint(checkpoint_path, "fingerprint-a")
        save_checkpoint(checkpoint_path, checkpoint, "qualification", {
            "status": "complete", "sha256": {"profile": sha256_file(artifact)}})
        resumed = load_checkpoint(checkpoint_path, "fingerprint-a")
        assert artifacts_match(resumed["stages"]["qualification"], {"profile": artifact})
        artifact.write_text('{"status":"changed"}\n', encoding="utf-8")
        assert not artifacts_match(resumed["stages"]["qualification"], {"profile": artifact})
        reset = load_checkpoint(checkpoint_path, "fingerprint-b")
        assert reset["stages"] == {}
        assert list((root / "checkpoint-history").glob("open-checkpoint-*.json"))
    print("line_open tests passed")


if __name__ == "__main__":
    main()
