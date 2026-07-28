#!/usr/bin/env python3
"""NB deployment build, rollout, and command orchestration."""

from deploy_core import *  # noqa: F403 - deploy_core publishes the CLI integration surface.

PICOQUIC_PATCH_FILES = tuple(ROOT / path for path in (
    "third_party/picoquic/src/picoquic/bbr.c",
    "third_party/picoquic/src/picoquic/cubic.c",
    "third_party/picoquic/src/picoquic/loss_recovery.c",
))
PICOQUIC_SOURCE_DIGEST = "\n".join(
    f"{path.relative_to(ROOT).as_posix()} {nb_release.sha256_file(path)}"
    for path in PICOQUIC_PATCH_FILES) + "\n"
PICOQUIC_STAMP = ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM / "source.sha256"
REBUILD_PICOQUIC = (os.environ.get("NB_REBUILD_PICOQUIC") == "1" or
    not PICOQUIC_STAMP.is_file() or
    PICOQUIC_STAMP.read_text(encoding="ascii") != PICOQUIC_SOURCE_DIGEST)
PICOQUIC_REBUILD_CMD = (
    f"rm -rf {COMPILE_WORK}/third_party/picoquic/prebuilt/{PLATFORM} "
    f"{COMPILE_WORK}/third_party/picoquic/src/build-{PLATFORM} "
    f"{COMPILE_WORK}/third_party/picoquic/src/picotls/build-{PLATFORM}; "
    f"bash {COMPILE_WORK}/third_party/picoquic/build_libs.sh {PLATFORM}; "
) if REBUILD_PICOQUIC else ""
BUILD_CMD = (
    PICOQUIC_REBUILD_CMD +
    f"cd {COMPILE_WORK} && rm -rf build test-build && "
    "cmake -S . -B test-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON >/tmp/nbcmake.log 2>&1 && "
    "ionice -c3 nice -n 10 cmake --build test-build -j2 >>/tmp/nbcmake.log 2>&1 && "
    "ionice -c3 nice -n 10 ctest --test-dir test-build --output-on-failure >>/tmp/nbcmake.log 2>&1 && "
    "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >>/tmp/nbcmake.log 2>&1 && "
    "ionice -c3 nice -n 10 cmake --build build -j2 >>/tmp/nbcmake.log 2>&1 && "
    "test -r tools/nb_supervisor.py -a -r scripts/runtri.sh && "
    f"NB_WORKER_LANE_PORTS=on NB_ENTRY_WORKERS={_effective_workers('entry')} NB_MIDDLE_WORKERS={_effective_workers('middle')} NB_EXIT_WORKERS={_effective_workers('exit')} "
    "NB_KEEP_TMP=1 NB_BIN=" + COMPILE_WORK + "/build/nb_node bash scripts/runtri.sh >>/tmp/nbcmake.log 2>&1; "
    "rc=$?; echo NB_BUILD_GATE_RC=$rc; tail -40 /tmp/nbcmake.log; exit $rc"
)

# CMake 构建集: 自有源码 + CMakeLists + build_libs.sh + 目标平台预编译 .a(自包含, 零依赖 /root/poc)
BUILD_FILES = {
    "VERSION": ROOT / "VERSION",
    "scripts/runtri.sh": ROOT / "scripts" / "runtri.sh",
    "tools/nb_supervisor.py": ROOT / "tools" / "nb_supervisor.py",
    "src/nb_node.c": SRC / "nb_node.c",
    "src/nb_policy.c": SRC / "nb_policy.c",
    "src/nb_policy.h": SRC / "nb_policy.h",
    "src/nb_live.c": SRC / "nb_live.c",
    "src/nb_live.h": SRC / "nb_live.h",
    "src/nb_probe.c": SRC / "nb_probe.c",
    "src/nb_probe.h": SRC / "nb_probe.h",
    "src/nb_lstream.c": SRC / "nb_lstream.c",
    "src/nb_lstream.h": SRC / "nb_lstream.h",
    "src/nb_metrics.c": SRC / "nb_metrics.c",
    "src/nb_metrics.h": SRC / "nb_metrics.h",
    "src/nb_session_index.c": SRC / "nb_session_index.c",
    "src/nb_session_index.h": SRC / "nb_session_index.h",
    "src/nb_session.h": SRC / "nb_session.h",
    "src/nb_runtime.h": SRC / "nb_runtime.h",
    "src/nb_bridge.c": SRC / "nb_bridge.c",
    "src/nb_bridge.h": SRC / "nb_bridge.h",
    "src/nb_send.c": SRC / "nb_send.c",
    "src/nb_send.h": SRC / "nb_send.h",
    "src/nb_path.c": SRC / "nb_path.c",
    "src/nb_path.h": SRC / "nb_path.h",
    "src/nb_pool.h": SRC / "nb_pool.h",
    "src/nb_pool.c": SRC / "nb_pool.c",
    "src/nb_whitelist.c": SRC / "nb_whitelist.c",
    "src/nb_whitelist.h": SRC / "nb_whitelist.h",
    "tools/test_whitelist.c": ROOT / "tools" / "test_whitelist.c",
    "src/nb_pool_health.c": SRC / "nb_pool_health.c",
    "src/nb_pool_health.h": SRC / "nb_pool_health.h",
    "src/nb_pmtu.c": SRC / "nb_pmtu.c",
    "src/nb_pmtu.h": SRC / "nb_pmtu.h",
    "src/nb_dns.c": SRC / "nb_dns.c",
    "src/nb_dns.h": SRC / "nb_dns.h",
    "src/nb_fec_rs.c": SRC / "nb_fec_rs.c",
    "src/nb_fec_rs.h": SRC / "nb_fec_rs.h",
    "src/nb_fec.c": SRC / "nb_fec.c",
    "src/nb_fec_policy.c": SRC / "nb_fec_policy.c",
    "src/nb_fec_policy.h": SRC / "nb_fec_policy.h",
    "tools/test_fec_policy.c": ROOT / "tools" / "test_fec_policy.c",
    "src/nb_fec.h": SRC / "nb_fec.h",
    "src/nb_ring.c": SRC / "nb_ring.c",
    "src/nb_ring.h": SRC / "nb_ring.h",
    "src/nb_auth.c": SRC / "nb_auth.c",
    "src/nb_auth.h": SRC / "nb_auth.h",
    "src/nb_auth_async.c": SRC / "nb_auth_async.c",
    "src/nb_auth_async.h": SRC / "nb_auth_async.h",
    "src/nb_control.c": SRC / "nb_control.c",
    "src/nb_control.h": SRC / "nb_control.h",
    "src/nb_routes.c": SRC / "nb_routes.c",
    "src/nb_routes.h": SRC / "nb_routes.h",
    "src/nb_tenant.c": SRC / "nb_tenant.c",
    "src/nb_tenant.h": SRC / "nb_tenant.h",
    "src/nb_tenant_shared.c": SRC / "nb_tenant_shared.c",
    "src/nb_tenant_shared.h": SRC / "nb_tenant_shared.h",
    "src/nb_udp.c": SRC / "nb_udp.c",
    "src/nb_udp.h": SRC / "nb_udp.h",
    "src/nb_v2_metadata.c": SRC / "nb_v2_metadata.c",
    "src/nb_v2_metadata.h": SRC / "nb_v2_metadata.h",
    "src/nb_udp_io.c": SRC / "nb_udp_io.c",
    "src/nb_udp_io.h": SRC / "nb_udp_io.h",
    "src/nb_udp_lifecycle.c": SRC / "nb_udp_lifecycle.c",
    "src/nb_udp_lifecycle.h": SRC / "nb_udp_lifecycle.h",
    "tools/test_fec.c": ROOT / "tools" / "test_fec.c",
    "tools/test_fec_rs.c": ROOT / "tools" / "test_fec_rs.c",
    "tools/test_ring.c": ROOT / "tools" / "test_ring.c",
    "tools/test_auth.c": ROOT / "tools" / "test_auth.c",
    "tools/test_auth_async.c": ROOT / "tools" / "test_auth_async.c",
    "tools/test_control.c": ROOT / "tools" / "test_control.c",
    "tools/test_routes.c": ROOT / "tools" / "test_routes.c",
    "tools/test_udp.c": ROOT / "tools" / "test_udp.c",
    "tools/test_lstream.c": ROOT / "tools" / "test_lstream.c",
    "tools/test_v2_metadata.c": ROOT / "tools" / "test_v2_metadata.c",
    "tools/test_udp_io.c": ROOT / "tools" / "test_udp_io.c",
    "tools/test_udp_lifecycle.c": ROOT / "tools" / "test_udp_lifecycle.c",
    "tools/test_policy.c": ROOT / "tools" / "test_policy.c",
    "tools/test_live.c": ROOT / "tools" / "test_live.c",
    "tools/test_probe.c": ROOT / "tools" / "test_probe.c",
    "tools/test_metrics.c": ROOT / "tools" / "test_metrics.c",
    "tools/test_session_index.c": ROOT / "tools" / "test_session_index.c",
    "tools/test_bridge.c": ROOT / "tools" / "test_bridge.c",
    "tools/test_send.c": ROOT / "tools" / "test_send.c",
    "tools/test_path.c": ROOT / "tools" / "test_path.c",
    "tools/test_pool_health.c": ROOT / "tools" / "test_pool_health.c",
    "tools/test_pmtu.c": ROOT / "tools" / "test_pmtu.c",
    "tools/test_dns.c": ROOT / "tools" / "test_dns.c",
    "tools/test_tenant.c": ROOT / "tools" / "test_tenant.c",
    "src/log/log4c.c": SRC / "log" / "log4c.c",
    "src/log/log4c.h": SRC / "log" / "log4c.h",
    "CMakeLists.txt": ROOT / "CMakeLists.txt",
    "third_party/picoquic/build_libs.sh": ROOT / "third_party" / "picoquic" / "build_libs.sh",
    # 该文件包含 NB 针对长 RTT 随机丢包的 BBRv3 修正；源码构建验证时覆盖 vendored 基线。
    "third_party/picoquic/src/picoquic/bbr.c": ROOT / "third_party" / "picoquic" / "src" / "picoquic" / "bbr.c",
    "third_party/picoquic/src/picoquic/cubic.c": ROOT / "third_party" / "picoquic" / "src" / "picoquic" / "cubic.c",
    "third_party/picoquic/src/picoquic/loss_recovery.c": ROOT / "third_party" / "picoquic" / "src" / "picoquic" / "loss_recovery.c",
}
for _fragment in sorted(SRC.glob("nb_node_*.inc")):
    BUILD_FILES[_fragment.relative_to(ROOT).as_posix()] = _fragment
for _fragment in sorted((SRC / "log").glob("log4c_*.inc")):
    BUILD_FILES[_fragment.relative_to(ROOT).as_posix()] = _fragment
for _a in sorted((ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM).glob("*.a")):
    BUILD_FILES[f"third_party/picoquic/prebuilt/{PLATFORM}/{_a.name}"] = _a
# prebuilt 路径编译 nb_node.c 需要 picoquic 头文件(CMake 的 3 个 include 目录, 仅头文件不传完整 src)
for _sub in ("src/picoquic", "src/loglib", "src/picotls/include"):
    _base = ROOT / "third_party" / "picoquic" / _sub
    for _hf in _base.rglob("*.h"):
        BUILD_FILES[_hf.relative_to(ROOT).as_posix()] = _hf
if REBUILD_PICOQUIC:
    _source_root = ROOT / "third_party" / "picoquic" / "src"
    for _source_file in _source_root.rglob("*"):
        if not _source_file.is_file():
            continue
        _relative = _source_file.relative_to(_source_root)
        if any(part.startswith("build-") or part == ".git" for part in _relative.parts):
            continue
        BUILD_FILES[_source_file.relative_to(ROOT).as_posix()] = _source_file

RELEASE_INPUTS = dict(BUILD_FILES)
RELEASE_INPUTS["tools/nb_supervisor.py"] = ROOT / "tools" / "nb_supervisor.py"
RELEASE_INPUTS["tools/tiktok_flow_rules.conf"] = ROOT / "tools" / "tiktok_flow_rules.conf"
RELEASE_INPUTS["tools/log4c.runtime.json"] = LOG4C_RUNTIME_CONFIG
RELEASE_INPUTS["tools/nb_p1_control.py"] = ROOT / "tools" / "nb_p1_control.py"
RELEASE_INPUTS["tools/security_rotate.py"] = ROOT / "tools" / "security_rotate.py"
RUNTIME_CONFIGURATION_INPUTS = {
    "tools/nb_supervisor.py": ROOT / "tools" / "nb_supervisor.py",
    "tools/tiktok_flow_rules.conf": ROOT / "tools" / "tiktok_flow_rules.conf",
    "tools/log4c.runtime.json": LOG4C_RUNTIME_CONFIG,
}


def act_recon(roles):
    for r in roles:
        c = connect(r); h = _role_host(r)
        out = run(c, "echo arch=$(uname -m); echo gcc=$(gcc -dumpversion 2>/dev/null||echo NONE); "
                     "echo cmake=$(cmake --version 2>/dev/null|head -1|awk '{print $3}'||echo NONE); "
                     "echo openssl_dev=$(ls /usr/include/openssl/ssl.h 2>/dev/null||echo NONE); "
                     f"echo nb_bin=$(ls {COMPILE_WORK}/build/nb_node 2>/dev/null||echo NONE); "
                     "echo proc=$(pgrep -x nb_node|tr '\\n' ',' || echo none)")
        print(f"### {r}({h['name']}) {h['host']}:{h['port']}\n{out}")
        c.close()


def act_build(roles):
    """CMake + vendored 构建；证书由 security_setup.py 独立管理。"""
    tests = ["test_release.py", "test_deploy_transaction.py", "test_observe.py",
             "test_line_control.py", "test_diag_bundle.py", "test_line_probe.py",
             "test_line_provision.py", "test_line_open.py", "test_supervisor.py"]
    for test in tests:
        result = subprocess.run([sys.executable, str(ROOT / "tools" / test)], cwd=ROOT, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"本地 P0 发布门禁失败: {test}")
    node_source = "\n".join(path.read_text(encoding="utf-8") for path in (
        SRC / "nb_node.c", *sorted(SRC.glob("nb_node_*.inc"))))
    if "picoquic_add_to_stream(" in node_source:
        raise RuntimeError("主节点仍存在 picoquic_add_to_stream 调用")
    c = connect(BUILD_HOST); h = _role_host(BUILD_HOST)
    print(f"### build on {BUILD_HOST}({h['name']}) in {COMPILE_WORK} via CMake + vendored picoquic ...")
    deps = run(c, "for x in gcc g++ make cmake pkg-config; do command -v $x >/dev/null 2>&1 || echo MISSING:$x; done; "
                  "test -f /usr/include/openssl/ssl.h || echo MISSING:libssl-dev")
    if "MISSING:" in deps:
        c.close()
        raise RuntimeError(f"构建机 {h['name']} 缺少依赖: {', '.join(x.split(':', 1)[1] for x in deps.splitlines() if x.startswith('MISSING:'))}")
    build_input_snapshot = nb_release.snapshot_inputs(ROOT, RELEASE_INPUTS)
    run(c, f"rm -rf {COMPILE_WORK}/src {COMPILE_WORK}/third_party {COMPILE_WORK}/CMakeLists.txt {COMPILE_WORK}/build; mkdir -p {COMPILE_WORK}")
    put_tar(c, BUILD_FILES, COMPILE_WORK)
    out = run(c, BUILD_CMD, tmo=300)
    print(out.strip()[-600:] if out.strip() else "(no output)")
    if "NB_BUILD_GATE_RC=0" not in out:
        c.close()
        raise RuntimeError("正式构建或三跳回归未通过，拒绝生成部署产物")
    ok = run(c, f"ls -l {COMPILE_WORK}/build/nb_node 2>/dev/null && echo BUILD_OK || echo BUILD_FAIL")
    print(ok)
    if "BUILD_OK" not in ok:
        c.close(); sys.exit("编译失败, 中止")
    if nb_release.snapshot_inputs(ROOT, RELEASE_INPUTS) != build_input_snapshot:
        c.close()
        raise RuntimeError("构建期间源码或运行时输入发生变化，产物已废弃，请重新构建")
    BUILD_DIR.mkdir(exist_ok=True)
    (BUILD_DIR / "nb_node").write_bytes(fetch_bytes(c, f"{COMPILE_WORK}/build/nb_node"))
    if REBUILD_PICOQUIC:
        local_prebuilt = ROOT / "third_party" / "picoquic" / "prebuilt" / PLATFORM
        local_prebuilt.mkdir(parents=True, exist_ok=True)
        for archive in ("libpicoquic-core.a", "libpicoquic-log.a", "libpicotls-openssl.a",
                        "libpicotls-core.a", "libpicotls-minicrypto.a"):
            (local_prebuilt / archive).write_bytes(fetch_bytes(
                c, f"{COMPILE_WORK}/third_party/picoquic/prebuilt/{PLATFORM}/{archive}"))
        PICOQUIC_STAMP.write_text(PICOQUIC_SOURCE_DIGEST, encoding="ascii")
    manifest = nb_release.create_manifest(
        ROOT, BUILD_DIR / "nb_node", PLATFORM, RELEASE_INPUTS, LAB_FILE,
        LINE_PROFILE if LINE_PROFILE.is_file() else None,
        RUNTIME_CONFIGURATION_INPUTS,
    )
    nb_release.write_manifest(RELEASE_MANIFEST, manifest)
    print(f"产物 -> {BUILD_DIR}: nb_node, release-manifest.json deployment={_deployment_id(manifest)}")
    c.close()


def _distribute(role):
    """把二进制和该角色的安全材料推到节点。"""
    c = connect(role); h = _role_host(role)
    run(c, f"pkill -9 -x nb_node 2>/dev/null; sleep 0.3; rm -f {WORK}/nb_node; mkdir -p {WORK}/logs {WORK}/www; echo ok")
    push_bytes(c, (BUILD_DIR / "nb_node").read_bytes(), f"{WORK}/nb_node", mode=0o755)
    _push_security(c, role)
    print(f"分发 -> {role}({h['name']}) done")
    return c


def act_stop(roles):
    for r in roles:
        c = connect(r); _systemd_stop(c, r); c.close()
    print("三跳 nb_node 已停")


def act_logs(roles):
    for r in roles:
        c = connect(r); h = _role_host(r)
        out = run(c, f"tail -25 {_log_path(r)} 2>/dev/null || echo '(no log)'")
        print(f"### {r}({h['name']}) log\n{out}")
        c.close()


def act_deploy_tri():
    if DEPLOY_INSTANCE:
        raise RuntimeError("deploy-tri cannot target a named instance; use deploy-socks")
    """分发二进制/证书 -> 起 exit(kz)->middle(hk)->entry(gz) -> 从 gz 冒烟(多 stream + md5)。"""
    hk = _role_host("middle"); kz = _role_host("exit")
    hk_ip = hk["host"]; kz_ip = kz["host"]
    _require_local_build();_require_security_material()
    # 1) exit(kz): 起本地 HTTP 目标和安全 NB exit
    ck = _distribute("exit")
    run(ck, f"mkdir -p {WORK}/www {WORK}/logs; "
            f"echo HELLO_NB_TUNNEL_OK>{WORK}/www/test.txt; head -c 300000 /dev/urandom|base64>{WORK}/www/big.txt; "
            f"pkill -9 -x nb_node; pkill -9 -f 'python3 -m http.server'; echo prepared")
    launch(ck, f"cd {WORK}/www && setsid nohup python3 -m http.server 9000 </dev/null >/tmp/http.log 2>&1 &")
    wl_remote = _ensure_remote_whitelist(ck)
    _install_and_restart_role(ck,"exit",_node_command("exit",wl_remote=wl_remote))
    print("exit(kz):", run(ck, "pgrep -x nb_node>/dev/null&&echo NB_EXIT_UP||echo DOWN; tail -3 /tmp/nb_exit.log"))
    # 2) middle(hk): 分发 + 起
    cm = _distribute("middle")
    _install_and_restart_role(cm,"middle",_node_command("middle"))
    print("middle(hk):", run(cm, "pgrep -x nb_node>/dev/null&&echo NB_MIDDLE_UP||echo DOWN; tail -3 /tmp/nb_middle.log"))
    # 3) entry(gz): 分发 + 起; route = 经 middle(kz地址) 到 exit, exit 连本地 http
    cg = _distribute("entry")
    route = f"H:{kz_ip}:4443,T:127.0.0.1:9000"
    sec=_remote_security("entry")
    entry_cmd=(f"{WORK}/nb_node -r entry -l 8080 -n {hk_ip} -N 4443 -R {shlex.quote(route)} "
               f"-c {sec['cert']} -k {sec['key']} -a {sec['ca']}")
    _install_and_restart_role(cg,"entry",entry_cmd)
    time.sleep(2)
    smoke = run(cg,
        "pgrep -x nb_node>/dev/null&&echo NB_ENTRY_UP||echo DOWN\n"
        "for i in 1 2 3 4 5; do curl -s -o /dev/null -w \"try$i first_byte=%{time_starttransfer}s http=%{http_code}\\n\" "
        "http://127.0.0.1:8080/test.txt --max-time 12; done\n"
        "echo '--- md5 300KB ---'; curl -s http://127.0.0.1:8080/big.txt -o /tmp/gb --max-time 20; "
        "echo gz_md5=$(md5sum /tmp/gb 2>/dev/null|awk '{print $1}')\n"
        f"echo '--- entry log ---'; tail -6 {WORK}/logs/nb-entry.log 2>/dev/null", tmo=90)
    print("=== 三跳冒烟(gz entry) ===\n" + smoke)
    print("=== kz big.txt md5 ===", run(ck, f"md5sum {WORK}/www/big.txt|awk '{{print $1}}'"))
    print("=== middle log ===\n" + run(cm, f"tail -8 {WORK}/logs/nb-middle.log 2>/dev/null"))
    print("=== exit log ===\n" + run(ck, f"tail -8 {WORK}/logs/nb-exit.log 2>/dev/null"))
    for c in (cg, cm, ck):
        c.close()


def act_deploy_socks(socks_port=DEFAULT_SOCKS_PORT):
    """预上传版本化产物 -> 原子重启 -> 健康门禁 -> 失败自动回滚。"""
    manifest = _require_local_build()
    _require_security_material()
    if not os.environ.get("NB_SOCKS_USERNAME") or not os.environ.get("NB_SOCKS_PASSWORD"):
        raise RuntimeError("部署前必须设置 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD，以便健康门禁完成端到端冒烟")
    gz = _role_host("entry")
    bindata = (BUILD_DIR / "nb_node").read_bytes()
    roles = ("exit", "middle", "entry")
    clients = {}
    previous = {}
    unit_backups = {}
    state_backups = {}
    activated = []
    locked = []
    deployment_id = _deployment_id(manifest)
    try:
        for role in roles:
            clients[role] = connect(role)
            _acquire_deploy_lock(clients[role], role, deployment_id)
            locked.append(role)
            run(clients[role], f"mkdir -p {INSTANCE_WORK}/logs")
            previous[role] = _stage_release(clients[role], role, manifest, bindata)
            unit_backups[role] = _backup_role_unit(clients[role], role, deployment_id)
            state_backups[role] = _backup_role_state(clients[role], role, deployment_id)
            _append_deploy_audit(clients[role], role, "staged", manifest, previous[role])
            print(f"{role}: deployment={deployment_id} 预上传及哈希校验完成")

        commands = {}
        release_dir = f"{INSTANCE_WORK}/releases/{deployment_id}"
        release_rules = f"{release_dir}/tiktok_flow_rules.conf"
        release_routes = f"{release_dir}/exit_routes.conf"

        ck = clients["exit"]
        _push_security(ck, "exit"); _push_tiktok_rules(ck, release_rules)
        wl_remote = _ensure_remote_whitelist(ck)
        commands["exit"] = _node_command("exit", wl_remote=wl_remote, release_id=deployment_id)

        cm = clients["middle"]
        _push_security(cm, "middle"); _push_tiktok_rules(cm, release_rules)
        commands["middle"] = _node_command("middle", release_id=deployment_id)

        cg = clients["entry"]
        _push_security(cg, "entry"); _push_tiktok_rules(cg, release_rules)
        _push_exit_routes(cg, release_routes);_push_exit_routes(cg, _exit_routes_remote())
        entry_wl = _ensure_remote_whitelist(cg, role="entry")
        commands["entry"] = _node_command(
            "entry", socks_port=socks_port, wl_remote=entry_wl,
            release_id=deployment_id, exit_routes=_exit_routes_remote(),
        )

        for role in roles:
            client = clients[role]
            _activate_release(client, deployment_id)
            activated.append(role)
            _append_deploy_audit(client, role, "activated", manifest, previous[role])
            _install_and_restart_role(client, role, commands[role], release_id=deployment_id)
            health = _verify_release_health(client, role, manifest)
            _append_deploy_audit(client, role, "healthy", manifest, previous[role], health)
            print(f"{role}: {health}")
            print(run(client, f"tail -4 {_log_path(role)} 2>/dev/null"))

        _smoke_socks(socks_port)
        for role in roles:
            _append_deploy_audit(clients[role], role, "completed", manifest, previous[role], "socks-smoke=ok")
            print(f"{role}: release retention {_prune_releases(clients[role], role, previous[role])}")
    except Exception:
        for role in reversed(activated):
            try:
                state = _rollback_release(
                    clients[role], role, previous[role], deployment_id, unit_backups[role],
                    state_backups[role],
                )
                _append_deploy_audit(clients[role], role, "rolled_back", manifest, previous[role], state)
                print(f"{role}: 已回滚到 {previous[role]} ({state})")
            except Exception as rollback_error:
                print(f"{role}: 自动回滚失败: {rollback_error}", file=sys.stderr)
        for role in reversed([item for item in locked if item not in activated]):
            try:
                _restore_role_state(clients[role], role, deployment_id, state_backups[role])
            except Exception as rollback_error:
                print(f"{role}: mutable state restore failed: {rollback_error}", file=sys.stderr)
        raise
    finally:
        for role in locked:
            try:
                _release_deploy_lock(clients[role])
            except Exception as unlock_error:
                retry = None
                try:
                    retry = connect(role)
                    _release_deploy_lock(retry)
                    print(f"{role}: deployment lock released through a fresh SSH connection")
                    continue
                except Exception as retry_error:
                    unlock_error = RuntimeError(f"{unlock_error}; retry: {retry_error}")
                finally:
                    if retry is not None:
                        retry.close()
                print(f"{role}: 释放部署锁失败: {unlock_error}", file=sys.stderr)
        for client in clients.values():
            client.close()

    print(f"\n>>> 部署 {deployment_id} 已通过三节点健康门禁")
    print(f">>> 手机配置: SOCKS5 -> {gz['host']}:{socks_port}，使用 NB_SOCKS_USERNAME 对应凭据")


def _activate_existing_deployment(c, role, deployment_id):
    if not nb_release.RELEASE_NAME_RE.fullmatch(deployment_id) or deployment_id.startswith("legacy-"):
        raise ValueError(f"非法 deployment_id: {deployment_id}")
    service=_service_name(role);directory=f"{INSTANCE_WORK}/releases/{deployment_id}";unit=f"{service}.service"
    check=run(c,f"test -x {shlex.quote(directory + '/nb_node')} && test -f {shlex.quote(directory + '/' + unit)} && echo READY")
    if "READY" not in check:raise RuntimeError(f"{role} 缺少不可变部署 {deployment_id}")
    push_target=f"/etc/systemd/system/{unit}"
    restored=run(c,f"cp -p {shlex.quote(directory + '/' + unit)} {shlex.quote(push_target)} && systemctl daemon-reload && echo UNIT_READY")
    if "UNIT_READY" not in restored:raise RuntimeError(f"{role} 恢复 unit 失败")
    _activate_release(c,deployment_id);_systemd_restart(c,role,warmup=2.0)
    return _verify_deployment_health(c,role,deployment_id,warmup=3)


def act_current_deployment():
    current={}
    for role in ("entry","middle","exit"):
        c=connect(role)
        try:current[role]=_remote_current_deployment(c,role)
        finally:c.close()
    print("CURRENT_JSON="+json.dumps(current,separators=(",",":")))


def act_rollback_socks(deployment_id,socks_port=DEFAULT_SOCKS_PORT):
    if not deployment_id:raise ValueError("rollback-socks 必须提供 --deployment-id")
    if not os.environ.get("NB_SOCKS_USERNAME") or not os.environ.get("NB_SOCKS_PASSWORD"):
        raise RuntimeError("精确回滚需要 SOCKS 凭据完成端到端门禁")
    roles=("exit","middle","entry");clients={};origins={};locked=[];activated=[]
    try:
        for role in roles:
            clients[role]=connect(role);origins[role]=_remote_current_deployment(clients[role],role)
            _acquire_deploy_lock(clients[role],role,deployment_id);locked.append(role)
        for role in roles:
            health=_activate_existing_deployment(clients[role],role,deployment_id)
            _append_exact_rollback_audit(clients[role],role,"exact-rollback-activated",deployment_id,origins[role],health)
            print(f"{role}: {health}")
            activated.append(role)
        _smoke_socks(socks_port)
    except Exception:
        for role in reversed(activated):
            try:
                health=_activate_existing_deployment(clients[role],role,origins[role])
                _append_exact_rollback_audit(clients[role],role,"exact-rollback-aborted",origins[role],deployment_id,health)
            except Exception as error:print(f"{role}: 回滚事务恢复原 deployment 失败: {error}",file=sys.stderr)
        raise
    finally:
        for role in locked:
            try:_release_deploy_lock(clients[role])
            except Exception:pass
        for c in clients.values():c.close()
    print(f">>> 已精确回滚并验证 deployment={deployment_id}")


def act_wl_show():
    c = connect("exit")
    print(run(c, f"cat {_whitelist_remote()}"))
    c.close()


def act_wl_push(local_path: pathlib.Path):
    c = connect("exit")
    _push_whitelist(c, local_path)
    print(run(c, f"tail -6 {_log_path('exit')} 2>/dev/null"))
    c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=[
        "recon", "build", "deploy-tri", "deploy-socks", "stop", "logs",
        "wl-show", "wl-push", "fec-status", "fec-on", "fec-off", "current", "rollback-socks"
    ])
    ap.add_argument("--roles", default="entry,middle,exit")
    ap.add_argument("--socks-port", type=int, default=DEFAULT_SOCKS_PORT)
    ap.add_argument("--whitelist", default=str(WHITELIST_LOCAL))
    ap.add_argument("--deployment-id")
    a = ap.parse_args()
    roles = [r.strip() for r in a.roles.split(",") if r.strip()]
    if a.action == "recon": act_recon(roles)
    elif a.action == "build": act_build(roles)
    elif a.action == "deploy-tri": act_deploy_tri()
    elif a.action == "deploy-socks": act_deploy_socks(a.socks_port)
    elif a.action == "current": act_current_deployment()
    elif a.action == "rollback-socks": act_rollback_socks(a.deployment_id,a.socks_port)
    elif a.action == "stop": act_stop(roles)
    elif a.action == "logs": act_logs(roles)
    elif a.action == "wl-show": act_wl_show()
    elif a.action == "wl-push": act_wl_push(pathlib.Path(a.whitelist))
    elif a.action == "fec-status": act_fec_status()
    elif a.action == "fec-on": _set_fec_roles(True)
    elif a.action == "fec-off": _set_fec_roles(False)


if __name__ == "__main__":
    main()
