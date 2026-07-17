"""
路径管理 - 统一管理配置目录与资源路径

统一原来散落在各模块里的 os.path.dirname(__file__) 拼接方式，
并提供 PyInstaller 打包后的资源路径解析(sys._MEIPASS)。
"""

import os
import sys


def resource_path(*parts) -> str:
    """解析资源文件路径，兼容开发环境与 PyInstaller 打包环境。

    开发环境: 相对于项目根目录(src 的上一级)。
    打包环境: 相对于 PyInstaller 解压的临时目录 sys._MEIPASS。
    """
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        base = sys._MEIPASS
    else:
        base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, *parts)


# ===== 用户数据目录 =====
USER_DATA_DIR = os.path.join(os.path.expanduser("~"), ".aisprite")
CONFIG_FILE = os.path.join(USER_DATA_DIR, "config.json")
CHAT_HISTORY_FILE = os.path.join(USER_DATA_DIR, "chat_history.json")
CALENDAR_FILE = os.path.join(USER_DATA_DIR, "calendar.json")
LOG_DIR = os.path.join(USER_DATA_DIR, "logs")
LOG_FILE = os.path.join(LOG_DIR, "aisprite.log")


def ensure_user_data_dir():
    """确保用户数据目录存在。"""
    os.makedirs(USER_DATA_DIR, exist_ok=True)
