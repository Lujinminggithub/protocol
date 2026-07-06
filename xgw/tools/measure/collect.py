"""采集层：纯只读 dump 三跳 C 层 + 前端日志到本地，供离线重复解析。

红线：所有远端命令必须是 tail / cat /proc/*/fd/1 / pgrep 这类只读命令。
绝不 pkill / nohup / rm / mv / 重定向写远端 / 前台启动 xgw。
"""

from __future__ import annotations

import importlib.util
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Optional

ROOT = Path(__file__).resolve().parents[2]
DEPLOY_FIX = ROOT / "tools" / "deploy_fix.py"
MEASURE_TMP = ROOT / "tmp" / "measure"


def _load_deploy_fix():
    """复用 deploy_fix 的 load_hosts / run_remote / WORK_DIR（含跳板登录逻辑）。"""
    spec = importlib.util.spec_from_file_location("deploy_fix", str(DEPLOY_FIX))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


# C 层各跳 role -> 日志文件名（落地于 deploy_fix.WORK_DIR/logs/）。
C_HOPS = ("ingress", "relay", "egress")
# 前端 xgw-edge-server 跑在 ingress 机上，日志独立。
FRONT_LOG = "hy2front.out.log"


@dataclass
class RawBundle:
    """一次采集的本地落地：run_id + 各跳原始日志路径 + 元信息。"""

    run_id: str
    dir: Path
    paths: Dict[str, Path] = field(default_factory=dict)
    meta: Dict[str, object] = field(default_factory=dict)

    def read(self, hop: str) -> str:
        p = self.paths.get(hop)
        if p is None or not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="ignore")


def _readonly_tail_cmd(proc_pat: str, tail_bytes: int) -> str:
    """动态定位进程真实在写的日志文件再 tail（纯只读，路径自适应）。

    现网部署路径不一（C 层 /opt/xgw/logs、前端 /etc/xgw/logs），硬编码会抓错。
    用 readlink /proc/$PID/fd/1 拿到进程 stdout 实际指向的文件，是唯一可靠来源；
    该文件存在则 tail 文件（含历史），否则直接 tail /proc/$PID/fd/1（实时尾部）。
    """
    return (
        "set +e; "
        f"PID=$(pgrep -f '{proc_pat}' | head -1); "
        '[ -n "$PID" ] || exit 0; '
        'LOG=$(readlink /proc/$PID/fd/1 2>/dev/null); '
        'echo \"# resolved pid=$PID log=$LOG\" >&2; '
        f'if [ -n "$LOG" ] && [ -f "$LOG" ]; then tail -c {tail_bytes} "$LOG" 2>/dev/null; '
        f'else tail -c {tail_bytes} /proc/$PID/fd/1 2>/dev/null; fi'
    )


def collect(
    run_id: str,
    tail_bytes: int = 4_000_000,
    df=None,
) -> RawBundle:
    """从三跳 C 层 + 前端只读拉日志到 tmp/measure/<run_id>/。

    run_id 由调用方提供（脚本里禁用 Date.now，用 CLI 层生成时间戳）。
    """
    if df is None:
        df = _load_deploy_fix()
    hosts = df.load_hosts()
    work = getattr(df, "WORK_DIR", "/etc/xgw")

    out_dir = MEASURE_TMP / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    bundle = RawBundle(run_id=run_id, dir=out_dir)
    sizes: Dict[str, int] = {}

    # C 三跳。
    for role in C_HOPS:
        host = hosts.get(role)
        if host is None:
            continue
        log_path = f"{work}/logs/{role}.out.log"
        cmd = _readonly_tail_cmd(log_path, tail_bytes, f"xgw run .*{role}")
        _, out, _ = df.run_remote(host, cmd, check=False, timeout=120)
        dst = out_dir / f"{role}.log"
        dst.write_text(out, encoding="utf-8")
        bundle.paths[role] = dst
        sizes[role] = len(out)

    # 前端（跑在 ingress 机）。
    front_host = hosts.get("ingress")
    if front_host is not None:
        log_path = f"{work}/logs/{FRONT_LOG}"
        cmd = _readonly_tail_cmd(log_path, tail_bytes, "xgw-edge-server")
        _, out, _ = df.run_remote(front_host, cmd, check=False, timeout=120)
        dst = out_dir / "front.log"
        dst.write_text(out, encoding="utf-8")
        bundle.paths["front"] = dst
        sizes["front"] = len(out)

    bundle.meta = {"run_id": run_id, "tail_bytes": tail_bytes, "sizes": sizes}
    (out_dir / "meta.json").write_text(
        json.dumps(bundle.meta, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return bundle


def load_bundle(run_id: str) -> RawBundle:
    """离线重载一次已采集的样本（不连远端），保证同一 run 可重复解析。"""
    out_dir = MEASURE_TMP / run_id
    if not out_dir.exists():
        raise FileNotFoundError(f"run not found: {out_dir}")
    bundle = RawBundle(run_id=run_id, dir=out_dir)
    for hop, fname in (
        ("ingress", "ingress.log"),
        ("relay", "relay.log"),
        ("egress", "egress.log"),
        ("front", "front.log"),
    ):
        p = out_dir / fname
        if p.exists():
            bundle.paths[hop] = p
    meta_p = out_dir / "meta.json"
    if meta_p.exists():
        bundle.meta = json.loads(meta_p.read_text(encoding="utf-8"))
    return bundle


def latest_run_id() -> Optional[str]:
    if not MEASURE_TMP.exists():
        return None
    runs = sorted(
        (p for p in MEASURE_TMP.iterdir() if p.is_dir()),
        key=lambda p: p.name,
    )
    return runs[-1].name if runs else None
