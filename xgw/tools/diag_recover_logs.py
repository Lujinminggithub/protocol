#!/usr/bin/env python3
"""恢复被 rm 但进程仍持有 fd 的日志（/proc/<pid>/fd），并采集前端/ingress 现场。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    hosts = df.load_hosts()
    ingress = hosts["ingress"]
    cmd = (
        "set +e; "
        "FRONT=$(pgrep -f xgw-edge-server | head -1); "
        "CING=$(pgrep -f '/etc/xgw/xgw run /etc/xgw/configs/ingress.conf' | head -1); "
        "echo \"FRONT_PID=$FRONT CING_PID=$CING\"; "
        "echo '=== FRONT open fds (deleted logs still held) ==='; "
        "ls -la /proc/$FRONT/fd 2>/dev/null | grep -iE 'log|deleted' ; "
        "echo '=== CING open fds ==='; "
        "ls -la /proc/$CING/fd 2>/dev/null | grep -iE 'log|deleted' ; "
        "echo '=== FRONT deleted-log tails ==='; "
        "for fd in /proc/$FRONT/fd/*; do "
        "  tgt=$(readlink \"$fd\" 2>/dev/null); "
        "  case \"$tgt\" in *log*deleted*|*hy2front*|*.log*) "
        "    echo \"--- $fd -> $tgt ---\"; tail -c 8000 \"$fd\" 2>/dev/null; echo; ;; esac; "
        "done; "
        "echo '=== CING deleted-log tails ==='; "
        "for fd in /proc/$CING/fd/*; do "
        "  tgt=$(readlink \"$fd\" 2>/dev/null); "
        "  case \"$tgt\" in *log*deleted*|*ingress*|*.log*) "
        "    echo \"--- $fd -> $tgt ---\"; tail -c 8000 \"$fd\" 2>/dev/null; echo; ;; esac; "
        "done; "
        "echo '=== bridge-ring mtime / size now ==='; ls -la --time-style=full-iso /run/xgw/ 2>/dev/null; "
        "echo '=== date now ==='; date -u +%FT%TZ; "
    )
    code, out, err = df.run_remote(ingress, cmd, check=False, timeout=120)
    print(out)
    if err.strip():
        print("STDERR:", err)


if __name__ == "__main__":
    main()
