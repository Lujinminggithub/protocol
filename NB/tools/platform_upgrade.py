#!/usr/bin/env python3
"""Transactional scripts, control-plane, and shared Node upgrade runner."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
from typing import Protocol

import platform_release


ROLES = ("exit", "middle", "entry")
STAGE_ROLES = ("entry", "middle", "exit")


def shared_release_materialize_command(work: str, instance_work: str, release_id: str,
                                       deployment_id: str, expected_sha: str) -> str:
    shared = f"{work}/shards/releases/{release_id}/nb_node"
    destination = f"{instance_work}/releases/{deployment_id}/nb_node"
    temporary = destination + ".materialize"
    parent = str(pathlib.PurePosixPath(destination).parent)
    return (f"test \"$(sha256sum {shlex.quote(shared)} | awk '{{print $1}}')\" = {shlex.quote(expected_sha)}; "
            f"mkdir -p {shlex.quote(parent)}; "
            f"if test -f {shlex.quote(destination)}; then "
            f"test \"$(sha256sum {shlex.quote(destination)} | awk '{{print $1}}')\" = {shlex.quote(expected_sha)}; "
            f"else cp -p {shlex.quote(shared)} {shlex.quote(temporary)} && chmod 0755 {shlex.quote(temporary)} && "
            f"mv -f {shlex.quote(temporary)} {shlex.quote(destination)}; fi; "
            "echo MIDDLE_SHARED_SOURCE_VERIFIED")


class UpgradeAdapter(Protocol):
    def switch_scripts(self) -> None: ...
    def rollback_scripts(self) -> None: ...
    def switch_controlplane(self, service: str) -> None: ...
    def rollback_controlplane(self, service: str) -> None: ...
    def stage_node(self, role: str) -> None: ...
    def activate_node(self, role: str, worker: int, deadline_seconds: float) -> dict: ...
    def rollback_node(self, role: str, worker: int, deadline_seconds: float) -> None: ...
    def smoke(self) -> None: ...


class CompositeUpgradeAdapter:
    """Runs one transaction across every unique physical device-role target."""

    def __init__(self, members: list[tuple[dict[str, str], UpgradeAdapter]]):
        if not members:
            raise ValueError("platform upgrade requires at least one affected line")
        self.members = members
        self.primary = members[0][1]

    def _unique(self, role: str) -> list[tuple[str, UpgradeAdapter]]:
        selected: dict[str, UpgradeAdapter] = {}
        for devices, adapter in self.members:
            device_id = str(devices.get(role, ""))
            if not device_id:
                raise RuntimeError(f"affected line is missing {role} device identity")
            selected.setdefault(device_id, adapter)
        return list(selected.items())

    def switch_scripts(self) -> None:
        self.primary.switch_scripts()

    def rollback_scripts(self) -> None:
        self.primary.rollback_scripts()

    def switch_controlplane(self, service: str) -> None:
        self.primary.switch_controlplane(service)

    def rollback_controlplane(self, service: str) -> None:
        self.primary.rollback_controlplane(service)

    def stage_node(self, role: str) -> None:
        for _device_id, adapter in self._unique(role):
            adapter.stage_node(role)

    def activate_node(self, role: str, worker: int, deadline_seconds: float) -> dict:
        targets = []
        for device_id, adapter in self._unique(role):
            try:
                result = adapter.activate_node(role, worker, deadline_seconds)
            except Exception as error:
                raise RuntimeError(f"{device_id} {role}[{worker}] activation failed: {error}") from error
            targets.append({"device_id": device_id, **result})
        return {
            "sessions_interrupted": sum(int(item.get("sessions_interrupted", 0)) for item in targets),
            "downtime_ms": max((int(item.get("downtime_ms", 0)) for item in targets), default=0),
            "configs": sum(int(item.get("configs", 0)) for item in targets),
            "targets": targets,
        }

    def rollback_node(self, role: str, worker: int, deadline_seconds: float) -> None:
        for _device_id, adapter in reversed(self._unique(role)):
            adapter.rollback_node(role, worker, deadline_seconds)

    def smoke(self) -> None:
        for _devices, adapter in self.members:
            adapter.smoke()

    def close(self) -> None:
        for _devices, adapter in self.members:
            close = getattr(adapter, "close", None)
            if callable(close):
                close()


def load_isolated_deploy(root: pathlib.Path, namespace: str):
    tools = root / "tools"
    suffix = re.sub(r"[^A-Za-z0-9_]", "_", namespace)
    core_name = "nb_deploy_core_" + suffix
    deploy_name = "nb_deploy_" + suffix
    core_spec = importlib.util.spec_from_file_location(core_name, tools / "deploy_core.py")
    deploy_spec = importlib.util.spec_from_file_location(deploy_name, tools / "deploy.py")
    if core_spec is None or core_spec.loader is None or deploy_spec is None or deploy_spec.loader is None:
        raise RuntimeError("failed to load isolated deployment modules")
    core_module = importlib.util.module_from_spec(core_spec)
    deploy_module = importlib.util.module_from_spec(deploy_spec)
    previous_core = sys.modules.get("deploy_core")
    added_path = str(tools) not in sys.path
    if added_path:
        sys.path.insert(0, str(tools))
    try:
        core_spec.loader.exec_module(core_module)
        sys.modules["deploy_core"] = core_module
        deploy_spec.loader.exec_module(deploy_module)
    finally:
        if previous_core is None:
            sys.modules.pop("deploy_core", None)
        else:
            sys.modules["deploy_core"] = previous_core
        if added_path:
            sys.path.remove(str(tools))
    return deploy_module


def execute_transaction(adapter: UpgradeAdapter, *, workers: int,
                        deadline_seconds: float = 2.0) -> dict:
    if workers < 1 or workers > 32 or not 0.5 <= deadline_seconds <= 2.0:
        raise ValueError("invalid platform upgrade worker count or deadline")
    activated: list[tuple[str, int]] = []
    controls: list[str] = []
    evidence: list[dict] = []
    scripts_switched = False
    try:
        adapter.switch_scripts()
        scripts_switched = True
        for service in ("nb-web-worker", "nb-web"):
            controls.append(service)
            adapter.switch_controlplane(service)
        for role in STAGE_ROLES:
            adapter.stage_node(role)
        for role in ROLES:
            for worker in range(workers):
                activated.append((role, worker))
                result = adapter.activate_node(role, worker, deadline_seconds)
                evidence.append({"role": role, "worker": worker, **result})
        adapter.smoke()
        return {
            "status": "succeeded",
            "sessions_interrupted": sum(int(item.get("sessions_interrupted", 0)) for item in evidence),
            "workers": evidence,
        }
    except Exception as original:
        rollback_errors = []
        for role, worker in reversed(activated):
            try:
                adapter.rollback_node(role, worker, deadline_seconds)
            except Exception as error:
                rollback_errors.append(f"{role}[{worker}]: {error}")
        for service in reversed(controls):
            try:
                adapter.rollback_controlplane(service)
            except Exception as error:
                rollback_errors.append(f"{service}: {error}")
        if scripts_switched:
            try:
                adapter.rollback_scripts()
            except Exception as error:
                rollback_errors.append(f"scripts: {error}")
        if rollback_errors:
            raise RuntimeError(f"{original}; rollback errors: {'; '.join(rollback_errors)}") from original
        raise
    finally:
        close = getattr(adapter, "close", None)
        if callable(close):
            close()


def execute_rollback(adapter: UpgradeAdapter, *, workers: int,
                     deadline_seconds: float = 2.0) -> dict:
    if workers < 1 or workers > 32 or not 0.5 <= deadline_seconds <= 2.0:
        raise ValueError("invalid platform rollback worker count or deadline")
    try:
        errors = []
        for role in reversed(ROLES):
            for worker in reversed(range(workers)):
                try:
                    adapter.rollback_node(role, worker, deadline_seconds)
                except Exception as error:
                    errors.append(f"{role}[{worker}]: {error}")
        for service in ("nb-web", "nb-web-worker"):
            try:
                adapter.rollback_controlplane(service)
            except Exception as error:
                errors.append(f"{service}: {error}")
        try:
            adapter.rollback_scripts()
        except Exception as error:
            errors.append(f"scripts: {error}")
        if errors:
            raise RuntimeError("platform rollback incomplete: " + "; ".join(errors))
        return {"status": "rolled_back"}
    finally:
        close = getattr(adapter, "close", None)
        if callable(close):
            close()


class SystemAdapter:
    def __init__(self, manifest_path: pathlib.Path, line_id: str, operation_id: str):
        self.manifest_path = manifest_path.resolve()
        self.manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        self.release_id = str(self.manifest["release_id"])
        self.line_id = line_id
        self.operation_id = operation_id
        self.current_root = pathlib.Path(os.environ.get("NB_CONTROLPLANE_ROOT", "/opt/nb-controlplane/repo"))
        self.candidate_root = pathlib.Path(self.manifest["candidate_root"])
        self.install_root = self.current_root.parent
        self.source_backup: pathlib.Path | None = None
        self.control_backups: dict[str, pathlib.Path] = {}
        self.previous_nodes: dict[str, str] = {}
        self.clients = {}
        self.deploy = None
        self.node_manifest = None
        self.node_binary = None
        self.runtime_plan = None
        self.units_installed = False

    def _rollback_record(self, release_id: str | None = None,
                         upgrade_operation_id: str | None = None,
                         line_id: str | None = None) -> pathlib.Path:
        name = "-".join((release_id or self.release_id,
                         upgrade_operation_id or self.operation_id,
                         line_id or self.line_id)) + ".json"
        return self.install_root / "data" / "upgrader" / "releases" / name

    def load_rollback_record(self, release_id: str, upgrade_operation_id: str, line_id: str) -> None:
        record = json.loads(self._rollback_record(release_id, upgrade_operation_id, line_id).read_text(encoding="utf-8"))
        self.release_id = release_id
        self.line_id = record["line_id"]
        self.source_backup = pathlib.Path(record["source_backup"]) if record.get("source_backup") else None
        self.control_backups = {key: pathlib.Path(value) for key, value in record["control_backups"].items()}
        self.previous_nodes = {key: str(value) for key, value in record["previous_nodes"].items()}

    def _verify_scripts(self) -> None:
        for record in self.manifest["scripts"]["files"]:
            path = self.candidate_root / record["path"]
            if not path.is_file():
                raise RuntimeError(f"candidate script missing: {record['path']}")
            if platform_release.sha256_file(path) != record["sha256"]:
                raise RuntimeError(f"candidate script hash mismatch: {record['path']}")

    def switch_scripts(self) -> None:
        self._verify_scripts()
        if self.candidate_root.resolve() == self.current_root.resolve():
            return
        releases = self.install_root / "source-releases"
        releases.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.source_backup = releases / f"repo-before-{self.release_id}"
        activation = releases / f"repo-next-{self.operation_id}"
        if self.source_backup.exists():
            shutil.rmtree(self.source_backup)
        if activation.exists():
            shutil.rmtree(activation)
        shutil.copytree(self.candidate_root, activation)
        os.replace(self.current_root, self.source_backup)
        try:
            os.replace(activation, self.current_root)
        except Exception:
            os.replace(self.source_backup, self.current_root)
            self.source_backup = None
            if activation.exists():
                shutil.rmtree(activation)
            raise
        self.manifest_path = self.current_root / "build" / "platform-release.json"

    def rollback_scripts(self) -> None:
        if self.source_backup is None or not self.source_backup.exists():
            return
        failed = self.install_root / "source-releases" / f"repo-failed-{self.operation_id}"
        if failed.exists():
            shutil.rmtree(failed)
        os.replace(self.current_root, failed)
        os.replace(self.source_backup, self.current_root)

    def _wait_service(self, service: str, deadline: float, expected_sha: str | None = None,
                      started_at: float | None = None) -> int:
        started = time.monotonic() if started_at is None else started_at
        while time.monotonic() - started <= deadline:
            active = subprocess.run(["systemctl", "is-active", "--quiet", service], check=False).returncode == 0
            healthy = active
            if active and expected_sha:
                pid = subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", service],
                    check=False, capture_output=True, text=True, encoding="utf-8").stdout.strip()
                executable = pathlib.Path("/proc") / pid / "exe"
                try:
                    healthy = executable.is_file() and platform_release.sha256_file(executable) == expected_sha
                except OSError:
                    healthy = False
            if active and service == "nb-web":
                health_url = os.environ.get("NB_WEB_BASE_URL", "http://127.0.0.1:19091").rstrip("/") + "/healthz"
                healthy = subprocess.run(["curl", "-fsS", "--max-time", ".3",
                    health_url], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, check=False).returncode == 0
            if healthy:
                return int((time.monotonic() - started) * 1000)
            time.sleep(0.05)
        raise RuntimeError(f"{service} did not recover within {int(deadline * 1000)}ms")

    @staticmethod
    def _force_restart(service: str) -> float:
        started = time.monotonic()
        subprocess.run(["systemctl", "stop", service], check=True, timeout=1)
        subprocess.run(["systemctl", "start", "--no-block", service], check=True, timeout=2)
        return started

    def _install_control_units(self) -> None:
        if self.units_installed:
            return
        for service in ("nb-web", "nb-web-worker", "nb-upgrader"):
            source = self.current_root / "controlplane" / "linux" / (service + ".service")
            destination = pathlib.Path("/etc/systemd/system") / (service + ".service")
            temporary = destination.with_suffix(".service.next")
            shutil.copy2(source, temporary)
            temporary.chmod(0o644)
            os.replace(temporary, destination)
        subprocess.run(["systemctl", "daemon-reload"], check=True)
        self.units_installed = True

    def switch_controlplane(self, service: str) -> None:
        self._install_control_units()
        name = service
        source = self.current_root / "build" / "platform" / name
        expected = self.manifest[name.replace("-", "_")]["sha256"]
        if not source.is_file() or platform_release.sha256_file(source) != expected:
            raise RuntimeError(f"{service} candidate hash mismatch")
        destination = self.install_root / "bin" / name
        backup = self.install_root / "releases" / self.release_id / (name + ".previous")
        backup.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            shutil.copy2(destination, backup)
        temporary = destination.with_suffix(".next")
        shutil.copy2(source, temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        self.control_backups[service] = backup
        started = self._force_restart(service)
        self._wait_service(service, 2.0, expected, started)

    def rollback_controlplane(self, service: str) -> None:
        backup = self.control_backups.get(service)
        if backup is None or not backup.is_file():
            return
        destination = self.install_root / "bin" / service
        temporary = destination.with_suffix(".rollback")
        shutil.copy2(backup, temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        started = self._force_restart(service)
        self._wait_service(service, 2.0, started_at=started)

    def _load_deploy(self):
        if self.deploy is not None:
            return
        state = self.install_root / "data" / "worker" / "lines" / self.line_id
        self.runtime_plan = json.loads((state / "runtime-plan.json").read_text(encoding="utf-8"))
        os.environ["NB_HOSTS_FILE"] = str(state / "source-machines.json")
        os.environ["NB_KNOWN_HOSTS"] = str(state / "security" / "known_hosts")
        os.environ["NB_DEPLOY_INSTANCE"] = str(self.runtime_plan["instance_id"])
        os.environ["NB_LINE_PROFILE_FILE"] = str(state / "transport" / "active.json")
        os.environ["NB_SECURITY_DIR"] = str(state / "security")
        client_secret = json.loads((state / "bootstrap-client-secret.json").read_text(encoding="utf-8"))
        os.environ["NB_SOCKS_USERNAME"] = str(client_secret["username"])
        os.environ["NB_SOCKS_PASSWORD"] = str(client_secret["password"])
        secrets = json.loads((self.install_root / "data" / "secrets" / "device-secrets.json").read_text(encoding="utf-8"))
        inventory = json.loads((state / "source-machines.json").read_text(encoding="utf-8"))
        aliases = {"entry": ("ENTRY", "edges"), "middle": ("MIDDLE", "relays"), "exit": ("EXIT", "terminals")}
        for role, (environment_role, list_key) in aliases.items():
            device = inventory.get(role)
            if not isinstance(device, dict):
                values = inventory.get(list_key) or []
                device = values[0] if len(values) == 1 else {}
            device_id = str(device.get("id") or device.get("name") or "")
            secret = secrets.get("device:" + device_id) or secrets.get(device_id) or {}
            os.environ["NB_SSH_PASSWORD_" + environment_role] = str(secret.get("password") or "")
        self.deploy = load_isolated_deploy(self.current_root, self.operation_id + "-" + self.line_id)
        self.node_manifest = json.loads((self.current_root / "build" / "release-manifest.json").read_text(encoding="utf-8"))
        self.node_binary = (self.current_root / "build" / "nb_node").read_bytes()
        self.clients = {role: self.deploy.connect(role) for role in ("entry", "middle", "exit")}

    def stage_node(self, role: str) -> None:
        self._load_deploy()
        if role == "entry":
            self.deploy._stage_entry_release(self.clients["entry"], self.node_manifest, self.node_binary)
        elif role == "middle":
            self.deploy._copy_release_between_nodes(self.clients["entry"], "entry", self.clients["middle"], "middle", self.node_manifest)
            self.deploy._stage_release(self.clients["middle"], "middle", self.node_manifest)
        elif role == "exit":
            materialize = shared_release_materialize_command(
                self.deploy.WORK, self.deploy.INSTANCE_WORK, self.node_manifest["release_id"],
                self.node_manifest["deployment_id"], self.node_manifest["artifact"]["sha256"])
            if "MIDDLE_SHARED_SOURCE_VERIFIED" not in self.deploy.checked_run(
                    self.clients["middle"], materialize):
                raise RuntimeError("middle shared Node source verification failed")
            self.deploy._copy_release_between_nodes(self.clients["middle"], "middle", self.clients["exit"], "exit", self.node_manifest)
            self.deploy._stage_release(self.clients["exit"], "exit", self.node_manifest)
        else:
            raise ValueError("invalid Node role")
        deployment = self.node_manifest["deployment_id"]
        release = self.node_manifest["release_id"]
        source = f"{self.deploy.INSTANCE_WORK}/releases/{deployment}/nb_node"
        target = f"{self.deploy.WORK}/shards/releases/{release}/nb_node"
        expected = self.node_manifest["artifact"]["sha256"]
        result = self.deploy.run(self.clients[role],
            f"mkdir -p {shlex.quote(str(pathlib.PurePosixPath(target).parent))}; "
            f"cp -p {shlex.quote(source)} {shlex.quote(target)}; chmod 0755 {shlex.quote(target)}; "
            f"test $(sha256sum {shlex.quote(target)}|awk '{{print $1}}') = {shlex.quote(expected)} && echo STAGED")
        if "STAGED" not in result:
            raise RuntimeError(f"{role} shared Node staging failed")

    def _worker_state(self, role: str, worker: int) -> tuple[int, int]:
        root = f"{self.deploy.WORK}/shards/configs/{role}/{worker}"
        code = ("import json,pathlib,socket;"
            f"root=pathlib.Path({root!r});paths=[];"
            "configs=list(root.glob('*.conf'));"
            "[paths.append(next(x.split('=',1)[1] for x in p.read_text().splitlines() if x.startswith('control_path='))) for p in configs];"
            "responses=[];"
            "[(lambda s,p:(s.settimeout(1),s.connect(p),s.sendall(b'health\\n'),responses.append(json.loads(s.recv(8192).decode())),s.close()))(socket.socket(socket.AF_UNIX),p) for p in paths];"
            "print(json.dumps({'configs':len(configs),'sessions':sum(int(x.get('resources',{}).get('sessions',0)) for x in responses)}))")
        output = self.deploy.checked_run(self.clients[role], "python3 -c " + shlex.quote(code), tmo=10)
        state = json.loads(output.strip().splitlines()[-1])
        return int(state["configs"]), int(state["sessions"])

    def activate_node(self, role: str, worker: int, deadline_seconds: float) -> dict:
        self._load_deploy()
        client = self.clients[role]
        root = f"{self.deploy.WORK}/shards"
        release = self.node_manifest["release_id"]
        if role not in self.previous_nodes:
            previous = self.deploy.run(client, f"readlink -f {shlex.quote(root + '/nb_node')}").strip()
            if not previous:
                raise RuntimeError(f"{role} has no rollback Node")
            self.previous_nodes[role] = previous
            temporary = root + "/.nb_node.next"
            self.deploy.run(client, f"ln -sfn {shlex.quote('releases/' + release + '/nb_node')} {shlex.quote(temporary)}; "
                f"mv -Tf {shlex.quote(temporary)} {shlex.quote(root + '/nb_node')}")
        configs, sessions = self._worker_state(role, worker)
        service = f"nb-{role}-shard@{worker}"
        control_root = f"{root}/configs/{role}/{worker}"
        gate = ("import json,pathlib,socket;"
            f"root=pathlib.Path({control_root!r});configs=list(root.glob('*.conf'));assert len(configs)=={configs};"
            "paths=[next(x.split('=',1)[1] for x in p.read_text().splitlines() if x.startswith('control_path=')) for p in configs];"
            "responses=[];"
            "[(lambda s,p:(s.settimeout(.2),s.connect(p),s.sendall(b'health\\n'),responses.append(json.loads(s.recv(8192).decode())),s.close()))(socket.socket(socket.AF_UNIX),p) for p in paths];"
            "assert len(responses)==len(paths) and all(x.get('status') in ('ok','starting') for x in responses)")
        deadline_ms = int(deadline_seconds * 1000)
        expected_sha = self.node_manifest["artifact"]["sha256"]
        command = (f"started=$(date +%s%3N); systemctl stop --no-block {shlex.quote(service)} || true; "
            f"systemctl kill --kill-who=all --signal=KILL {shlex.quote(service)} 2>/dev/null || true; "
            f"systemctl reset-failed {shlex.quote(service)} 2>/dev/null || true; "
            f"systemctl start --no-block {shlex.quote(service)} || exit 31; "
            f"for i in $(seq 1 20); do pid=$(systemctl show -p MainPID --value {shlex.quote(service)}); "
            f"if systemctl is-active --quiet {shlex.quote(service)} && test -n \"$pid\" && "
            f"test \"$(sha256sum /proc/$pid/exe 2>/dev/null|awk '{{print $1}}')\" = {shlex.quote(expected_sha)} && "
            f"python3 -c {shlex.quote(gate)} 2>/dev/null; "
            f"then ended=$(date +%s%3N); elapsed=$((ended-started)); test $elapsed -le {deadline_ms} || exit 32; "
            "echo UPGRADE_OK elapsed_ms=$elapsed; exit 0; fi; sleep .1; done; exit 33")
        output = self.deploy.checked_run(client, command, tmo=10)
        match = re.search(r"elapsed_ms=(\d+)", output)
        if not match:
            raise RuntimeError(f"{role}[{worker}] health evidence missing")
        return {"sessions_interrupted": sessions, "downtime_ms": int(match.group(1)), "configs": configs}

    def rollback_node(self, role: str, worker: int, deadline_seconds: float) -> None:
        previous = self.previous_nodes.get(role)
        if not previous:
            return
        client = self.clients[role]
        root = f"{self.deploy.WORK}/shards"
        temporary = root + "/.nb_node.rollback"
        relative = str(pathlib.PurePosixPath(previous).relative_to(root))
        self.deploy.run(client, f"ln -sfn {shlex.quote(relative)} {shlex.quote(temporary)}; "
            f"mv -Tf {shlex.quote(temporary)} {shlex.quote(root + '/nb_node')}; "
            f"service={shlex.quote('nb-' + role + '-shard@' + str(worker))}; "
            f"systemctl stop --no-block $service || true; "
            f"systemctl kill --kill-who=all --signal=KILL $service 2>/dev/null || true; "
            f"systemctl reset-failed $service 2>/dev/null || true; systemctl start --no-block $service; "
            f"for i in $(seq 1 20); do systemctl is-active --quiet $service && exit 0; sleep .1; done; exit 1", tmo=10)

    def smoke(self) -> None:
        self._load_deploy()
        socks_port = int(self.runtime_plan["socks_port"])
        self.deploy._smoke_socks(socks_port)
        environment = os.environ.copy()
        result = subprocess.run([sys.executable, str(self.current_root / "tools" / "probe_cleanup.py")],
            cwd=self.current_root, env=environment, check=False, capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).strip()[-4000:])
        record = {
            "release_id": self.release_id,
            "line_id": self.line_id,
            "source_backup": str(self.source_backup) if self.source_backup else "",
            "control_backups": {key: str(value) for key, value in self.control_backups.items()},
            "previous_nodes": self.previous_nodes,
        }
        path = self._rollback_record()
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(record, separators=(",", ":")) + "\n", encoding="utf-8")
        temporary.replace(path)

    def close(self) -> None:
        for client in self.clients.values():
            client.close()
        self.clients = {}


def line_device_ids(install_root: pathlib.Path, line_id: str) -> dict[str, str]:
    inventory_path = install_root / "data" / "worker" / "lines" / line_id / "source-machines.json"
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    aliases = {"entry": "edges", "middle": "relays", "exit": "terminals"}
    result: dict[str, str] = {}
    for role, list_key in aliases.items():
        device = inventory.get(role)
        if not isinstance(device, dict):
            values = inventory.get(list_key) or []
            device = values[0] if len(values) == 1 else {}
        device_id = str(device.get("id") or device.get("name") or "")
        if not device_id:
            raise RuntimeError(f"line {line_id} is missing {role} device identity")
        result[role] = device_id
    return result


def system_composite(manifest: pathlib.Path, line_ids: list[str], operation_id: str) -> CompositeUpgradeAdapter:
    members = []
    for line_id in dict.fromkeys(line_ids):
        adapter = SystemAdapter(manifest, line_id, operation_id)
        members.append((line_device_ids(adapter.install_root, line_id), adapter))
    return CompositeUpgradeAdapter(members)


def rollback_composite(manifest: pathlib.Path, release_id: str, upgrade_operation_id: str,
                       operation_id: str) -> CompositeUpgradeAdapter:
    root = pathlib.Path(os.environ.get("NB_CONTROLPLANE_ROOT", "/opt/nb-controlplane/repo")).parent
    records = []
    directory = root / "data" / "upgrader" / "releases"
    for path in directory.glob(f"{release_id}-{upgrade_operation_id}-*.json"):
        record = json.loads(path.read_text(encoding="utf-8"))
        records.append(record)
    if not records:
        raise RuntimeError("platform rollback record is unavailable")
    records.sort(key=lambda item: (not bool(item.get("source_backup")), str(item.get("line_id", ""))))
    members = []
    for record in records:
        line_id = str(record["line_id"])
        adapter = SystemAdapter(manifest, line_id, operation_id)
        adapter.load_rollback_record(release_id, upgrade_operation_id, line_id)
        members.append((line_device_ids(adapter.install_root, line_id), adapter))
    return CompositeUpgradeAdapter(members)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("upgrade", "rollback"))
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--line-id", action="append")
    parser.add_argument("--release-id")
    parser.add_argument("--upgrade-operation-id")
    parser.add_argument("--operation-id", required=True)
    parser.add_argument("--workers", type=int, default=2)
    args = parser.parse_args()
    if args.action == "upgrade":
        if args.manifest is None or not args.line_id:
            parser.error("upgrade requires --manifest and --line-id")
        adapter = system_composite(args.manifest, args.line_id, args.operation_id)
        result = execute_transaction(adapter, workers=args.workers, deadline_seconds=2.0)
    else:
        if not args.release_id or not args.upgrade_operation_id:
            parser.error("rollback requires --release-id and --upgrade-operation-id")
        root = pathlib.Path(os.environ.get("NB_CONTROLPLANE_ROOT", "/opt/nb-controlplane/repo"))
        manifest = root / "build" / "platform-release.json"
        adapter = rollback_composite(manifest, args.release_id, args.upgrade_operation_id, args.operation_id)
        result = execute_rollback(adapter, workers=args.workers, deadline_seconds=2.0)
    print("PLATFORM_UPGRADE_JSON=" + json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()
