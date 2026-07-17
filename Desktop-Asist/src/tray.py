"""
托盘图标模块 - 系统托盘管理与交互
"""

import sys
import os
import platform

from PyQt6.QtWidgets import (
    QSystemTrayIcon,
    QMenu,
    QMessageBox,
)
from PyQt6.QtGui import QAction, QIcon, QPainter, QColor, QPixmap
from PyQt6.QtCore import Qt, pyqtSignal

from src.paths import resource_path


def _create_default_icon() -> QIcon:
    """创建一个默认的AI小精灵图标(256x256 pixmap)。"""
    size = 256
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    # 渐变背景 - 紫色主题
    gradient = QColor(100, 50, 180)
    gradient_end = QColor(60, 20, 120)
    painter.setBrush(gradient)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.drawEllipse(0, 0, size, size)

    # 绘制AI符号 - 大脑/星星
    painter.setPen(QColor(255, 255, 255))
    painter.setBrush(QColor(255, 255, 255))

    # 中心圆
    painter.drawEllipse(size // 2 - 20, size // 2 - 20, 40, 40)

    # 简化版: 绘制一个精灵帽子形状
    painter.setBrush(QColor(255, 200, 100))
    painter.drawEllipse(size // 2 - 50, size // 2 - 70, 100, 50)
    painter.setBrush(QColor(255, 255, 255))
    painter.drawEllipse(size // 2 + 45, size // 2 - 75, 15, 15)

    painter.end()

    return QIcon(pixmap)


class TrayIcon(QSystemTrayIcon):
    """系统托盘图标。"""

    # 信号: 用户点击托盘图标
    icon_clicked = pyqtSignal()
    # 信号: 用户选择"打开"
    show_requested = pyqtSignal()
    # 信号: 退出请求
    quit_requested = pyqtSignal()

    def __init__(self, parent_window=None):
        super().__init__(parent_window)
        self.parent_window = parent_window

        # 设置图标 - 优先使用资源文件,否则用默认图标
        # 使用 resource_path 兼容 PyInstaller 打包后的 sys._MEIPASS 路径
        icon_path = resource_path("assets", "icons", "sprite.ico")
        if os.path.exists(icon_path):
            self.setIcon(QIcon(icon_path))
        else:
            self.setIcon(_create_default_icon())

        # 设置tooltip
        self.setToolTip("AI小精灵 - 双击打开")

        # 构建菜单
        self._build_menu()

        # 连接双击事件
        self.activated.connect(self._on_activated)

    def _build_menu(self):
        """构建右键菜单。"""
        menu = QMenu()

        # 打开窗口
        show_action = QAction("打开 AI小精灵", menu)
        show_action.triggered.connect(self._on_show)
        menu.addAction(show_action)

        menu.addSeparator()

        # 快捷功能
        ai_action = QAction("AI 助手", menu)
        ai_action.triggered.connect(self._on_quick_ai)
        menu.addAction(ai_action)

        calendar_action = QAction("日历", menu)
        calendar_action.triggered.connect(self._on_quick_calendar)
        menu.addAction(calendar_action)

        menu.addSeparator()

        # 设置
        settings_action = QAction("设置", menu)
        settings_action.triggered.connect(self._on_settings)
        menu.addAction(settings_action)

        # 关于
        about_action = QAction("关于", menu)
        about_action.triggered.connect(self._on_about)
        menu.addAction(about_action)

        menu.addSeparator()

        # 退出
        quit_action = QAction("退出", menu)
        quit_action.triggered.connect(self._on_quit)
        menu.addAction(quit_action)

        self.setContextMenu(menu)

    def _on_activated(self, reason):
        """处理托盘图标激活事件。"""
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self.icon_clicked.emit()
            self.show_requested.emit()

    def _on_show(self):
        """显示主窗口。"""
        self.show_requested.emit()

    def _on_quick_ai(self):
        """快捷切换到AI助手。"""
        self.show_requested.emit()
        if self.parent_window:
            self.parent_window.switch_to_tab(0)

    def _on_quick_calendar(self):
        """快捷切换到日历。"""
        self.show_requested.emit()
        if self.parent_window:
            self.parent_window.switch_to_tab(1)

    def _on_about(self):
        """显示关于对话框。"""
        QMessageBox.information(
            None,
            "关于 AI小精灵",
            "桌面AI小精灵 v1.0\n\n"
            "集成AI助手、日历、文件搜索、\n"
            "AIGC、Hermes的多功能桌面工具。\n\n"
            "© 2026 AiSprite",
        )

    def _on_settings(self):
        """打开设置对话框。"""
        from src.setup_dialog import SetupDialog
        dialog = SetupDialog(self.parent(), is_setup=False)
        dialog.exec()

    def _on_quit(self):
        """退出应用。"""
        self.quit_requested.emit()

    def show_notification(self, title: str, message: str, icon_type=QSystemTrayIcon.MessageIcon.Information, duration: int = 3000):
        """显示系统通知。"""
        super().showMessage(title, message, icon_type, duration)