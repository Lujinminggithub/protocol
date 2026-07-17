"""
系统资源信息工具 - 纯函数版，供 Hermes 面板的 SystemInfoWorker 与
Function Calling 共用，避免同一段 psutil 采集逻辑维护两份。
"""

import json
import os
import socket
from datetime import datetime


def collect_system_info() -> dict:
    """采集当前系统的 CPU/内存/磁盘等信息，返回原始字典(供 UI 或工具调用两处复用)。"""
    try:
        import psutil

        return {
            "cpu_percent": psutil.cpu_percent(interval=0.5),
            "memory_percent": psutil.virtual_memory().percent,
            "disk_percent": psutil.disk_usage("/").percent,
            "memory_total_gb": psutil.virtual_memory().total / (1024 ** 3),
            "memory_used_gb": psutil.virtual_memory().used / (1024 ** 3),
            "disk_total_gb": psutil.disk_usage("/").total / (1024 ** 3),
            "disk_used_gb": psutil.disk_usage("/").used / (1024 ** 3),
            "cpu_count": psutil.cpu_count(logical=True),
            "hostname": socket.gethostname(),
            "boot_time": datetime.fromtimestamp(psutil.boot_time()),
        }
    except ImportError:
        return {
            "cpu_percent": 0,
            "memory_percent": 0,
            "disk_percent": 0,
            "memory_total_gb": 0,
            "memory_used_gb": 0,
            "disk_total_gb": 0,
            "disk_used_gb": 0,
            "cpu_count": os.cpu_count() or 1,
            "hostname": socket.gethostname(),
            "boot_time": datetime.now(),
        }


def run(args: dict) -> str:
    info = collect_system_info()
    return json.dumps(info, ensure_ascii=False, default=str)
