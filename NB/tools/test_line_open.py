#!/usr/bin/env python3
import pathlib
import os
import subprocess
import tempfile

from line_open import (artifacts_match, baseline_profile, load_checkpoint,
                       configuration_deployment_id, deploy_instance_with_retry,
                       deploy_socks_with_retry,
                       ensure_security_material,
                       load_or_create_client_secret,
                       normalize_hosts, reconcile_socks_user, resolve_instance_id,
                       save_checkpoint, sha256_file)


def main() -> None:
    source = {"machines": [
        {"role": "entry", "host": "192.0.2.1", "password": "e"},
        {"role": "middle", "host": "192.0.2.2", "private_ip": "10.0.0.2", "password": "m"},
        {"role": "exit", "host": "192.0.2.3", "port": 2222, "password": "x"},
    ]}
    hosts, credentials = normalize_hosts(source)
    assert resolve_instance_id("test-line") == "test-line_1"
    assert resolve_instance_id("test-line", "custom_2") == "custom_2"
    try:
        resolve_instance_id("x" * 49)
    except ValueError:
        pass
    else:
        raise AssertionError("unsafe implicit instance id was accepted")
    assert hosts["middle"]["private_ip"] == "10.0.0.2"
    assert "password" not in hosts["entry"]
    assert credentials["NB_SSH_PASSWORD_ENTRY"] == "e"
    assert hosts["exits"][0]["host"] == "192.0.2.3"
    assert hosts["transport"]["exit"]["dns_servers"] == ["1.1.1.1", "8.8.8.8"]
    custom_dns_source = dict(source)
    custom_dns_source["transport"] = {"exit": {"dns_servers": ["9.9.9.9"]}}
    custom_dns, _ = normalize_hosts(custom_dns_source)
    assert custom_dns["transport"]["exit"]["dns_servers"] == ["9.9.9.9"]
    stale = dict(source)
    stale["exits"] = [{"name": "old", "host": "192.0.2.3", "port": 4444,
                       "fixed_exit": "exit-192-0-2-3"}]
    synchronized, _ = normalize_hosts(stale, exit_port=4450)
    assert synchronized["exits"][0]["port"] == 4450
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
        security = root / "new-security"
        def generate_security(command, _env):
            security_staging = pathlib.Path(command[-1])
            security_staging.mkdir()
            for name in ("ca.key", "ca.pem", "socks.users", "tenant.conf",
                         "entry.key", "entry.pem", "middle.key", "middle.pem",
                         "exit.key", "exit.pem"):
                (security_staging / name).write_text(name, encoding="ascii")
        assert ensure_security_material(security, {}, runner=generate_security) == "generated"
        assert (security / "ca.key").is_file()

        auxiliary_security = root / "auxiliary-security"
        auxiliary_security.mkdir()
        known_hosts = auxiliary_security / "known_hosts"
        known_hosts.write_text("pinned-host-key\n", encoding="ascii")
        assert ensure_security_material(auxiliary_security, {}, runner=generate_security) == "generated"
        assert known_hosts.read_text(encoding="ascii") == "pinned-host-key\n"
        assert (auxiliary_security / "entry.pem").is_file()

        (security / "ca.key").unlink()
        def unexpected_generation(_command, _env):
            raise AssertionError("a valid runtime identity was regenerated")
        assert ensure_security_material(security, {}, runner=unexpected_generation) == "reused"

        (security / "entry.pem").unlink()
        try:
            ensure_security_material(security, {}, runner=unexpected_generation)
        except SystemExit as error:
            assert "entry.pem" in str(error)
        else:
            raise AssertionError("an incomplete runtime identity was accepted")

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
        hosts_path = root / "hosts.json"
        profile_path = root / "profile.json"
        hosts_path.write_text('{"entry":"gz"}\n', encoding="utf-8")
        profile_path.write_text('{"schema_version":1}\n', encoding="utf-8")
        config_deployment = configuration_deployment_id(hosts_path, profile_path)
        assert config_deployment.startswith("cfg-") and len(config_deployment) == 20
        assert config_deployment == configuration_deployment_id(hosts_path, profile_path)
        instance_attempts = []
        deploy_instance_with_retry(config_deployment, 1082, {},
            runner=lambda command, _env: instance_attempts.append(command), delays=())
        assert len(instance_attempts) == 1
        command = instance_attempts[0]
        assert command[1:3] == ["tools/deploy.py", "deploy-instance"]
        assert command[-4:] == ["--deployment-id", config_deployment, "--socks-port", "1082"]
        assert all(value not in command for value in ("build", "prepare-release", "deploy-socks"))
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
