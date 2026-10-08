#!/usr/bin/env python3
"""Transactional installation of fixed NB shard workers."""
from __future__ import annotations

import os
import pathlib
import shlex
import time

import nb_shard_deploy


def verify_all_controls(c, role, *, work, run):
    root = nb_shard_deploy.shard_root(work)
    script = ("import pathlib,stat;"
        f"root=pathlib.Path({str(root + '/configs/' + role)!r});"
        "configs=list(root.glob('*/*.conf'));paths=[];"
        "[(paths.append(next(x.split('=',1)[1] for x in p.read_text().splitlines() "
        "if x.startswith('control_path=')))) for p in configs];"
        "missing=[p for p in paths if not pathlib.Path(p).exists() or "
        "not stat.S_ISSOCK(pathlib.Path(p).stat().st_mode)];"
        "assert not missing,(len(configs),missing);print('SHARD_CONTROLS_OK',len(paths))")
    result = run(c, f"python3 -c {shlex.quote(script)}", tmo=20)
    if "SHARD_CONTROLS_OK" not in result:
        raise RuntimeError(f"{role} shard control socket gate failed: {result.strip()}")
    return result.strip()


def active_sessions(c, role, *, work, run) -> int:
    root = nb_shard_deploy.shard_root(work)
    script = ("import json,pathlib,socket;"
        f"root=pathlib.Path({str(root + '/configs/' + role)!r});total=0;"
        "configs=list(root.glob('*/*.conf'));"
        "paths=[next(x.split('=',1)[1] for x in p.read_text().splitlines() "
        "if x.startswith('control_path=')) for p in configs];"
        "responses=[];"
        "[(lambda s,p:(s.settimeout(2),s.connect(p),s.sendall(b'health\\n'),"
        "responses.append(json.loads(s.recv(8192).decode())),s.close()))"
        "(socket.socket(socket.AF_UNIX),p) for p in paths];"
        "print(sum(int(x.get('resources',{}).get('sessions',0)) for x in responses))")
    output = run(c, f"python3 -c {shlex.quote(script)}", tmo=20).strip().splitlines()
    try:
        return int(output[-1])
    except (IndexError, ValueError) as error:
        raise RuntimeError(f"{role} shard session drain query failed") from error


def require_idle_for_binary_change(c, role, *, work, run) -> None:
    deadline = time.monotonic() + max(0, min(600, int(os.environ.get("NB_SHARD_DRAIN_SECONDS", "30"))))
    while True:
        sessions = active_sessions(c, role, work=work, run=run)
        if sessions == 0:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"{role} shard binary rollout refused: {sessions} active sessions; "
                "retry after drain to preserve line isolation")
        time.sleep(2)


def _reuse_current_binary_when_busy(c, role, *, work, run) -> bool:
    """Keep the active shard binary when a line open must not disrupt traffic."""
    if os.environ.get("NB_ALLOW_BINARY_REUSE_WHEN_BUSY") != "1":
        return False
    sessions = active_sessions(c, role, work=work, run=run)
    if sessions <= 0:
        return False
    print(f">>> {role} shard has {sessions} active sessions; reuse current binary and apply instance config")
    return True


def _memory_limits(lab, role):
    defaults = {"entry": (640, 896), "middle": (512, 768), "exit": (640, 896)}
    configured = lab.get("shards", {}).get(role, {})
    high = int(configured.get("memory_high_mb", defaults[role][0]))
    maximum = int(configured.get("memory_max_mb", defaults[role][1]))
    if high < 128 or maximum < high or maximum > 8192:
        raise ValueError(f"shards.{role} memory limits are invalid")
    return high, maximum


def install_instance(c, role, command, environment, deployment_id, *, work,
                     instance_work, deploy_instance, lab, run, push_bytes,
                     effective_workers):
    """Install one line config without changing the shared shard runtime."""
    workers = effective_workers(role)
    if (not deploy_instance or not nb_shard_deploy.SAFE_ID.fullmatch(deploy_instance) or
            not nb_shard_deploy.SAFE_ID.fullmatch(deployment_id)):
        raise ValueError("named instance deployment requires safe instance and deployment IDs")
    root = nb_shard_deploy.shard_root(work)
    services = [nb_shard_deploy.shard_service(role, worker) for worker in range(workers)]
    ready = (f"test -x {shlex.quote(root + '/nb_node')} && "
             f"test -f {shlex.quote('/etc/systemd/system/nb-' + role + '-shard@.service')} && " +
             " && ".join(f"systemctl is-active --quiet {shlex.quote(service)}" for service in services) +
             " && echo RUNTIME_READY || echo RUNTIME_MISSING")
    if "RUNTIME_READY" not in run(c, ready, tmo=20):
        raise RuntimeError(f"runtime-preflight: {role} shared shard runtime is not ready")

    release_dir = f"{instance_work}/releases/{deployment_id}"
    run(c, f"mkdir -p {shlex.quote(release_dir)}")
    marker = nb_shard_deploy.current_deployment_marker(instance_work)
    previous_marker = nb_shard_deploy.saved_previous_deployment_marker(instance_work, deployment_id)
    run(c, f"if test -f {shlex.quote(marker)}; then cp -p {shlex.quote(marker)} "
        f"{shlex.quote(previous_marker)}; printf 'present\\n' > {shlex.quote(previous_marker + '.state')}; "
        f"else printf 'absent\\n' > {shlex.quote(previous_marker + '.state')}; fi")
    backups = []
    try:
        for worker in range(workers):
            directory = nb_shard_deploy.shard_config_dir(work, role, worker)
            path = f"{directory}/{deploy_instance}.conf"
            candidate = path + ".next"
            backup = path + ".rollback"
            state = run(c, f"mkdir -p {shlex.quote(directory)}; "
                f"if test -f {shlex.quote(path)}; then cp -p {shlex.quote(path)} {shlex.quote(backup)}; "
                "echo CONFIG_PRESENT; else echo CONFIG_ABSENT; fi")
            existed = "CONFIG_PRESENT" in state
            saved_previous = nb_shard_deploy.saved_previous_instance_config(
                instance_work, deployment_id, role, worker)
            saved_state = nb_shard_deploy.saved_previous_instance_state(
                instance_work, deployment_id, role, worker)
            if existed:
                run(c, f"cp -p {shlex.quote(path)} {shlex.quote(saved_previous)}; "
                    f"printf 'present\\n' > {shlex.quote(saved_state)}")
            else:
                run(c, f"printf 'absent\\n' > {shlex.quote(saved_state)}")
            config = nb_shard_deploy.render_instance_config(
                deploy_instance, role, worker, command, environment,
                int(os.environ.get("NB_INSTANCE_MAX_SESSIONS", "1024")),
                int(os.environ.get("NB_INSTANCE_MAX_QUEUE_BYTES", str(64 * 1024 * 1024))),
            )
            push_bytes(c, config.encode("utf-8"), candidate, mode=0o600)
            run(c, f"chmod 0600 {shlex.quote(candidate)} && mv -f {shlex.quote(candidate)} {shlex.quote(path)}")
            backups.append((worker, path, candidate, backup, existed))

        for service in services:
            run(c, f"systemctl reload {shlex.quote(service)}")
        controls = [nb_shard_deploy.line_control_path(deploy_instance, role, worker)
                    for worker in range(workers)]
        condition = " && ".join(f"test -S {shlex.quote(path)}" for path in controls)
        gate = run(c, f"for i in $(seq 1 40); do if {condition}; then echo INSTANCE_CONTROL_OK; "
            "exit 0; fi; sleep 0.25; done; echo INSTANCE_CONTROL_MISSING; exit 1", tmo=20)
        if "INSTANCE_CONTROL_OK" not in gate:
            raise RuntimeError(f"{role} instance control socket gate failed")
        for worker, path, _candidate, _backup, _existed in backups:
            saved = nb_shard_deploy.saved_instance_config(instance_work, deployment_id, role, worker)
            run(c, f"cp -p {shlex.quote(path)} {shlex.quote(saved)}")
        run(c, f"printf '%s\\n' {shlex.quote(deployment_id)} > {shlex.quote(marker)}; chmod 0600 {shlex.quote(marker)}")
    except Exception:
        for _worker, path, candidate, backup, existed in backups:
            if existed:
                run(c, f"mv -f {shlex.quote(backup)} {shlex.quote(path)}")
            else:
                run(c, f"rm -f {shlex.quote(path)} {shlex.quote(backup)}")
            run(c, f"rm -f {shlex.quote(candidate)}")
        for service in services:
            run(c, f"systemctl reload {shlex.quote(service)} 2>/dev/null || true")
        raise
    finally:
        for _worker, _path, candidate, backup, _existed in backups:
            run(c, f"rm -f {shlex.quote(candidate)} {shlex.quote(backup)}")
    return f"instance controls=ok workers={workers} deployment={deployment_id}"


def rollback_instance(c, role, deployment_id, *, work, instance_work,
                      deploy_instance, run, effective_workers):
    """Restore only one line's configs saved by install_instance."""
    workers = effective_workers(role)
    if (not deploy_instance or not nb_shard_deploy.SAFE_ID.fullmatch(deploy_instance) or
            not nb_shard_deploy.SAFE_ID.fullmatch(deployment_id)):
        raise ValueError("instance rollback requires safe instance and deployment IDs")
    restored_present = []
    for worker in range(workers):
        path = f"{nb_shard_deploy.shard_config_dir(work, role, worker)}/{deploy_instance}.conf"
        saved = nb_shard_deploy.saved_previous_instance_config(instance_work, deployment_id, role, worker)
        state_path = nb_shard_deploy.saved_previous_instance_state(instance_work, deployment_id, role, worker)
        state = run(c, f"cat {shlex.quote(state_path)} 2>/dev/null || true").strip()
        if state == "present":
            run(c, f"cp -p {shlex.quote(saved)} {shlex.quote(path + '.next')} && "
                f"mv -f {shlex.quote(path + '.next')} {shlex.quote(path)}")
            restored_present.append(worker)
        elif state == "absent":
            run(c, f"rm -f {shlex.quote(path)} {shlex.quote(path + '.next')}")
        else:
            raise RuntimeError(f"{role} instance rollback state is unavailable")
    marker = nb_shard_deploy.current_deployment_marker(instance_work)
    previous_marker = nb_shard_deploy.saved_previous_deployment_marker(instance_work, deployment_id)
    marker_state = run(c, f"cat {shlex.quote(previous_marker + '.state')} 2>/dev/null || true").strip()
    if marker_state == "present":
        run(c, f"cp -p {shlex.quote(previous_marker)} {shlex.quote(marker)}")
    elif marker_state == "absent":
        run(c, f"rm -f {shlex.quote(marker)}")
    else:
        raise RuntimeError(f"{role} deployment marker rollback state is unavailable")
    for worker in range(workers):
        run(c, f"systemctl reload {shlex.quote(nb_shard_deploy.shard_service(role, worker))}")
    controls = [nb_shard_deploy.line_control_path(deploy_instance, role, worker)
                for worker in range(workers)]
    conditions = []
    for worker, control in enumerate(controls):
        conditions.append(("test -S " if worker in restored_present else "test ! -S ") + shlex.quote(control))
    condition = " && ".join(conditions)
    gate = run(c, f"for i in $(seq 1 40); do if {condition}; then echo INSTANCE_ROLLBACK_OK; "
        "exit 0; fi; sleep 0.25; done; echo INSTANCE_ROLLBACK_FAILED; exit 1", tmo=20)
    if "INSTANCE_ROLLBACK_OK" not in gate:
        raise RuntimeError(f"{role} instance rollback control gate failed")
    return f"instance rollback=ok workers={workers} deployment={deployment_id}"


def install_role(c, role, command, environment, release_id, binary_release_id, warmup, *,
                 work, instance_work, deploy_instance, lab, run, push_bytes,
                 effective_workers, legacy_service_name):
    workers = effective_workers(role)
    if not deploy_instance or not release_id or not binary_release_id:
        raise ValueError("named shard deployment requires instance, deployment and binary release IDs")
    root = nb_shard_deploy.shard_root(work)
    source = f"{instance_work}/releases/{release_id}/nb_node"
    shared_release = f"{root}/releases/{binary_release_id}/nb_node"
    run(c, f"mkdir -p {shlex.quote(root + '/releases/' + binary_release_id)}")
    staged = run(c, f"src=$(sha256sum {shlex.quote(source)}|awk '{{print $1}}'); "
        f"dst=$(test -f {shlex.quote(shared_release)} && sha256sum {shlex.quote(shared_release)}|awk '{{print $1}}' || true); "
        f"if test -n \"$dst\" && test \"$src\" != \"$dst\"; then echo COLLISION; "
        f"elif test -z \"$dst\"; then cp -p {shlex.quote(source)} {shlex.quote(shared_release)} && "
        "chmod 0755 " + shlex.quote(shared_release) + " && echo STAGED; else echo REUSED; fi")
    if "COLLISION" in staged or not ("STAGED" in staged or "REUSED" in staged):
        raise RuntimeError(f"{role} shared shard release staging failed")
    previous_target = run(c, f"readlink -f {shlex.quote(root + '/nb_node')} 2>/dev/null || true").strip()
    previous_target_exists = bool(previous_target) and "PRESENT" in run(
        c, f"if test -x {shlex.quote(previous_target)}; then echo PRESENT; else echo ABSENT; fi"
    )
    # Deployment IDs are intentionally unique per operation. Compare the binary
    # contents instead of the release path so an identical binary does not
    # restart shared shards or trip the active-session isolation gate.
    previous_sha = ""
    if previous_target_exists:
        previous_sha = run(
            c, f"sha256sum {shlex.quote(previous_target)} | awk '{{print $1}}'"
        ).strip()
    shared_sha = run(c, f"sha256sum {shlex.quote(shared_release)} | awk '{{print $1}}'").strip()
    binary_changed = not previous_target_exists or previous_sha != shared_sha
    binary_reused = False
    effective_binary_release_id = binary_release_id
    if previous_target_exists and binary_changed:
        if _reuse_current_binary_when_busy(c, role, work=work, run=run):
            binary_changed = False
            binary_reused = True
            effective_binary_release_id = pathlib.PurePosixPath(previous_target).parent.name
        else:
            require_idle_for_binary_change(c, role, work=work, run=run)
    high, maximum = _memory_limits(lab, role)
    unit = nb_shard_deploy.render_systemd_unit(work, role, effective_binary_release_id, high, maximum)
    push_bytes(c, unit.encode("utf-8"), f"/etc/systemd/system/nb-{role}-shard@.service", mode=0o644)
    for worker in range(workers):
        run(c, f"mkdir -p {shlex.quote(nb_shard_deploy.shard_config_dir(work, role, worker))}")
    if binary_changed:
        temporary = f"{root}/.nb_node.next"
        run(c, f"ln -sfn {shlex.quote('releases/' + binary_release_id + '/nb_node')} "
            f"{shlex.quote(temporary)} && mv -Tf {shlex.quote(temporary)} {shlex.quote(root + '/nb_node')}")
    run(c, "systemctl daemon-reload")
    for worker in range(workers):
        service = nb_shard_deploy.shard_service(role, worker)
        active = run(c, f"systemctl is-active {shlex.quote(service)} 2>/dev/null || true").strip() == "active"
        action = "restart" if active and binary_changed else "start"
        state = run(c, f"systemctl enable {shlex.quote(service)} >/dev/null; "
            f"systemctl {action} {shlex.quote(service)}; sleep {warmup}; "
            f"systemctl is-active {shlex.quote(service)}").strip().splitlines()
        if "active" not in state:
            detail = run(c, f"systemctl status {shlex.quote(service)} --no-pager -l; "
                f"journalctl -u {shlex.quote(service)} -n 40 --no-pager")
            raise RuntimeError(f"{service} failed to start:\n{detail}")
    if binary_changed:
        verify_all_controls(c, role, work=work, run=run)

    legacy = legacy_service_name(role)
    legacy_was_active = run(c, f"systemctl is-active {shlex.quote(legacy)} 2>/dev/null || true").strip() == "active"
    backups = []
    try:
        if legacy_was_active:
            run(c, f"systemctl stop {shlex.quote(legacy)}")
        max_sessions = int(os.environ.get("NB_INSTANCE_MAX_SESSIONS", "1024"))
        max_queue = int(os.environ.get("NB_INSTANCE_MAX_QUEUE_BYTES", str(64 * 1024 * 1024)))
        for worker in range(workers):
            directory = nb_shard_deploy.shard_config_dir(work, role, worker)
            path = f"{directory}/{deploy_instance}.conf"
            backup = f"{path}.rollback"
            existed = "PRESENT" in run(c, f"if test -f {shlex.quote(path)}; then "
                f"cp -p {shlex.quote(path)} {shlex.quote(backup)}; echo PRESENT; else echo ABSENT; fi")
            config = nb_shard_deploy.render_instance_config(deploy_instance, role, worker, command,
                environment, max_sessions, max_queue)
            push_bytes(c, config.encode("utf-8"), path, mode=0o600)
            backups.append((path, backup, existed))
        for worker in range(workers):
            run(c, f"systemctl reload {shlex.quote(nb_shard_deploy.shard_service(role, worker))}")
        time.sleep(warmup)
        verify_all_controls(c, role, work=work, run=run)
        for worker, (path, _backup, _existed) in enumerate(backups):
            saved = nb_shard_deploy.saved_instance_config(instance_work, release_id, role, worker)
            run(c, f"cp -p {shlex.quote(path)} {shlex.quote(saved)}")
        marker = nb_shard_deploy.saved_binary_release(instance_work, release_id, role)
        run(c, f"printf '%s\\n' {shlex.quote(effective_binary_release_id)} > {shlex.quote(marker)}; "
            f"chmod 0600 {shlex.quote(marker)}")
    except Exception:
        for path, backup, existed in backups:
            if existed:
                run(c, f"mv -f {shlex.quote(backup)} {shlex.quote(path)}")
            else:
                run(c, f"rm -f {shlex.quote(path)} {shlex.quote(backup)}")
        for worker in range(workers):
            run(c, f"systemctl reload {shlex.quote(nb_shard_deploy.shard_service(role, worker))} 2>/dev/null || true")
        if legacy_was_active:
            run(c, f"systemctl start {shlex.quote(legacy)}")
        raise
    finally:
        for _path, backup, _existed in backups:
            run(c, f"rm -f {shlex.quote(backup)}")
    reused = " binary_reused=true" if binary_reused else ""
    return f"shard workers={workers} binary_changed={str(binary_changed).lower()}{reused} controls=ok"
