#!/usr/bin/env python3
"""P0 TCP 回程质量统一测量框架 —— 命令行入口（薄 shim）。

纯只读：本地运行，SSH 到三跳只 tail 日志，绝不部署/重启现网，绝不前台跑 xgw。

  python tools/xgw_measure.py collect            # 只读采集现网日志
  python tools/xgw_measure.py analyze            # 离线双向逐跳归因+瓶颈判读
  python tools/xgw_measure.py compare --probe    # 金标准对比
  python tools/xgw_measure.py live               # 周期只读观测
"""

from __future__ import annotations

import sys
from pathlib import Path

# 让 `tools` 成为可导入的包父目录（measure 是其子包）。
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.measure.cli import main  # noqa: E402

if __name__ == "__main__":
    main()
