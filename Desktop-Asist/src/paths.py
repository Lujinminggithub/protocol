"""
路径管理 - 统一管理配置目录与资源路径

统一原来散落在各模块里的 os.path.dirname(__file__) 拼接方式，
并兼容 PyInstaller / Nuitka 等冻结打包环境。
"""

import os
import shutil
import sys


def resource_path(*parts) -> str:
    """解析资源文件路径，兼容开发环境与冻结打包环境。

    开发环境: 相对于项目根目录(src 的上一级)。
    PyInstaller onefile: 相对于 sys._MEIPASS。
    Nuitka onefile / onedir: 对于打包进程序内部的数据文件，使用模块 __file__ 所在目录。
    这与 Nuitka 官方文档对 onefile 数据文件定位方式一致：onefile 内部数据文件
    应从 os.path.dirname(__file__) 查找，而不是 sys.executable 所在目录。
    """
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        base = sys._MEIPASS
    elif "__compiled__" in globals():
        base = os.path.dirname(os.path.abspath(__file__))
    elif getattr(sys, "frozen", False):
        base = os.path.dirname(os.path.abspath(__file__))
    else:
        base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, *parts)


def application_dir() -> str:
    """Return the launched exe directory, or the project root in source mode."""
    if "__compiled__" in globals() or getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.argv[0]))
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ===== 用户数据目录 =====
USER_DATA_DIR = os.path.join(application_dir(), ".aisprite")
LEGACY_USER_DATA_DIR = os.path.join(os.path.expanduser("~"), ".aisprite")
CONFIG_FILE = os.path.join(USER_DATA_DIR, "config.json")
CHAT_HISTORY_FILE = os.path.join(USER_DATA_DIR, "chat_history.json")
CALENDAR_FILE = os.path.join(USER_DATA_DIR, "calendar.json")
LOG_DIR = os.path.join(USER_DATA_DIR, "logs")
LOG_FILE = os.path.join(LOG_DIR, "aisprite.log")


def ensure_user_data_dir():
    """Ensure portable data exists, copying legacy data once when possible."""
    should_migrate = (
        os.path.normcase(os.path.abspath(USER_DATA_DIR))
        != os.path.normcase(os.path.abspath(LEGACY_USER_DATA_DIR))
        and not os.path.exists(USER_DATA_DIR)
        and os.path.isdir(LEGACY_USER_DATA_DIR)
    )
    os.makedirs(USER_DATA_DIR, exist_ok=True)
    if should_migrate:
        for filename in ("config.json", "chat_history.json", "calendar.json"):
            source = os.path.join(LEGACY_USER_DATA_DIR, filename)
            target = os.path.join(USER_DATA_DIR, filename)
            if os.path.isfile(source) and not os.path.exists(target):
                try:
                    shutil.copy2(source, target)
                except OSError:
                    pass
