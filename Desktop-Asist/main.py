#!/usr/bin/env python3
"""
桌面AI小精灵 - 托盘应用程序
功能: AI助手、日历、文件搜索、AIGC、Hermes
"""

import sys
import os

# Add project root to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _fix_frozen_ca_bundle():
    """冻结打包环境下手动修正 certifi CA 包路径。

    PyInstaller onefile 用 sys._MEIPASS。
    Nuitka onefile/standalone 的数据文件与模块 __file__ 同目录，所以这里优先取
    当前文件目录，而不是 sys.executable 所在目录。
    """
    if not getattr(sys, "frozen", False):
        return
    try:
        if hasattr(sys, "_MEIPASS"):
            base_dir = sys._MEIPASS
        else:
            base_dir = os.path.dirname(os.path.abspath(__file__))
        ca_path = os.path.join(base_dir, "certifi", "cacert.pem")
        if os.path.exists(ca_path):
            os.environ["SSL_CERT_FILE"] = ca_path
            os.environ["REQUESTS_CA_BUNDLE"] = ca_path
    except Exception:
        pass  # 兜底失败也不应阻塞启动，后续请求出错时会有明确的错误提示


_fix_frozen_ca_bundle()

from src.tray import TrayIcon
from src.window import MainWindow
from src.setup_dialog import SetupDialog
from src.logger import get_logger


def _install_global_excepthook():
    """安装全局异常兜底。

    PyQt6 里槽函数(按钮点击等)抛出的未捕获异常，默认会直接 abort 整个进程。
    覆盖 sys.excepthook 后 PyQt 不再 abort，而是把异常交给我们记日志并继续运行，
    避免"点某个按钮整个程序就没了"这种糟糕体验(单个功能出错不该拖垮整个应用)。
    """
    def _hook(exc_type, exc_value, exc_tb):
        if issubclass(exc_type, KeyboardInterrupt):
            sys.__excepthook__(exc_type, exc_value, exc_tb)
            return
        try:
            get_logger().error(
                "未捕获异常", exc_info=(exc_type, exc_value, exc_tb)
            )
        except Exception:
            pass

    sys.excepthook = _hook


class App:
    """桌面AI小精灵主应用"""

    def __init__(self):
        from PyQt6.QtWidgets import QApplication, QMessageBox
        from PyQt6.QtCore import Qt, QSharedMemory

        # High DPI support
        QApplication.setHighDpiScaleFactorRoundingPolicy(
            Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
        )

        self.app = QApplication(sys.argv)
        self.app.setStyle("Fusion")
        self.app.setApplicationName("AI小精灵")
        self.app.setApplicationVersion("1.0.0")
        self.app.setOrganizationName("AiSprite")

        # 安装全局异常兜底，避免单个槽函数异常 abort 整个进程
        _install_global_excepthook()

        # ===== 单实例检测 =====
        # 必须在弹出任何窗口/对话框之前完成；共享内存对象需存为 self 属性，
        # 否则函数返回后被 GC 回收，共享内存段随之释放导致锁失效。
        self._shared_mem = QSharedMemory("AiSprite-SingleInstance-Lock-8f2c1a")
        if self._shared_mem.attach():
            self._shared_mem.detach()
            QMessageBox.information(None, "AI小精灵", "程序已在运行，请查看系统托盘。")
            raise SystemExit(0)
        if not self._shared_mem.create(1):
            get_logger().warning(
                "QSharedMemory create failed: %s", self._shared_mem.errorString()
            )

        # 首次启动时弹出 Token 配置引导
        if SetupDialog.should_show():
            dialog = SetupDialog()
            dialog.exec()

        # Create main window and show it immediately
        self.main_window = MainWindow()
        self.main_window.show()
        self.main_window.raise_()
        self.main_window.activateWindow()

        # Create tray icon
        self.tray_icon = TrayIcon(self.main_window)
        self.tray_icon.show()

        # Connect tray signals to window actions
        self.tray_icon.show_requested.connect(self.main_window.show)
        self.tray_icon.show_requested.connect(self.main_window.raise_)
        self.tray_icon.show_requested.connect(self.main_window.activateWindow)
        self.tray_icon.quit_requested.connect(self.app.quit)

        # 窗口关闭(最小化到托盘)时弹出托盘通知 —— MainWindow.closeEvent 里已处理
        # 隐藏逻辑，这里只负责通知，不再用猴子补丁覆盖 closeEvent
        self.main_window.window_hidden.connect(self._on_window_hidden)

        # 日历到期提醒 -> 转发为托盘通知
        self.main_window.calendar_panel.reminder_triggered.connect(
            self.tray_icon.show_notification
        )

    def _on_window_hidden(self):
        """窗口被最小化到托盘时的提示。"""
        self.tray_icon.show_notification(
            "AI小精灵",
            "已最小化到系统托盘，双击托盘图标打开。",
        )

    def run(self):
        """Start the application."""
        sys.exit(self.app.exec())


def main():
    """入口函数，带完整异常捕获。"""
    logger = get_logger()
    try:
        App().run()
    except SystemExit:
        raise
    except Exception:
        import traceback
        from PyQt6.QtWidgets import QApplication, QMessageBox

        logger.exception("Fatal error on startup")
        # 打包为无控制台窗口程序(console=False)后 input() 会挂死/报错，
        # 改用 QMessageBox 弹窗提示，确保用户能看到错误信息。
        if QApplication.instance() is None:
            QApplication(sys.argv)
        QMessageBox.critical(
            None,
            "AI小精灵 - 启动失败",
            f"程序发生错误：\n{traceback.format_exc()[-2000:]}\n\n"
            f"详细日志已保存到日志文件。",
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
