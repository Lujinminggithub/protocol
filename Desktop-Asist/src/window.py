"""
主窗口 - 包含侧边栏导航和所有面板

窗口为 Frameless(无边框)，自绘标题栏支持拖动/双击最大化，
四边支持鼠标拖动缩放(委托给 Qt6 内建的 startSystemMove/startSystemResize，
由操作系统处理，DPI缩放和多屏场景更可靠)。
"""

import sys
from PyQt6.QtWidgets import (
    QMainWindow,
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QStackedWidget,
    QFrame,
)
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QFont

from src.styles import STYLE_SHEET
from src.ai_panel import AIPanel
from src.calendar_panel import CalendarPanel
from src.search_panel import SearchPanel
from src.aigc_panel import AIGCPanel
from src.hermes_panel import HermesPanel
from src.script_video_panel import ScriptVideoPanel


# 侧边栏导航项定义 —— 全应用唯一数据源(标签文案/图标/tooltip 都从这里取，
# 不要在别处再维护一份重复列表)
NAV_ITEMS = [
    {"key": "ai", "icon": "🤖", "label": "AI助手", "tooltip": "与AI助手对话"},
    {"key": "calendar", "icon": "📅", "label": "日历", "tooltip": "查看日程安排"},
    {"key": "search", "icon": "🔍", "label": "文件搜索", "tooltip": "快速搜索文件"},
    {"key": "aigc", "icon": "🎨", "label": "AIGC", "tooltip": "AI生成内容"},
    {"key": "script_video", "icon": "🎬", "label": "剧本成片", "tooltip": "剧本一键生成多场景配音视频"},
    {"key": "hermes", "icon": "⚡", "label": "Hermes", "tooltip": "系统工具与信息"},
]


class Sidebar(QFrame):
    """侧边栏导航。"""

    # 切换面板信号
    tab_changed = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("sidebar")
        self.setFixedWidth(200)
        self._init_ui()

    def _init_ui(self):
        """初始化UI。"""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(6)

        # Logo区域
        logo = QLabel("✨ AI小精灵")
        logo.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        logo.setStyleSheet("""
            color: #cba6f7;
            padding: 8px 0;
            border-bottom: 1px solid #313244;
        """)
        logo.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(logo)

        # 分隔线
        sep = QLabel()
        sep.setStyleSheet("border-top: 1px solid #313244; margin: 8px 0;")
        layout.addWidget(sep)

        # 导航按钮
        self.buttons = []
        for item in NAV_ITEMS:
            btn = QPushButton(f"{item['icon']} {item['label']}")
            btn.setCheckable(True)
            btn.setToolTip(item["tooltip"])
            index = len(self.buttons)
            btn.clicked.connect(lambda checked, idx=index: self.tab_changed.emit(idx))
            btn.setStyleSheet("""
                QPushButton {
                    text-align: left;
                    padding: 10px 14px;
                    border-radius: 8px;
                    background-color: transparent;
                    color: #a6adc8;
                    font-size: 13px;
                }
                QPushButton:hover {
                    background-color: #313244;
                    color: #cdd6f4;
                }
                QPushButton:checked {
                    background-color: #45475a;
                    color: #cba6f7;
                    font-weight: bold;
                }
            """)
            self.buttons.append(btn)
            layout.addWidget(btn)

        layout.addStretch()

        # 底部版本信息
        version = QLabel("v1.0.0")
        version.setAlignment(Qt.AlignmentFlag.AlignCenter)
        version.setStyleSheet("color: #45475a; font-size: 11px; padding: 8px;")
        layout.addWidget(version)

    def set_active(self, index: int):
        """设置当前激活的导航项。"""
        for i, btn in enumerate(self.buttons):
            btn.setChecked(i == index)


class TitleBar(QWidget):
    """自绘标题栏 —— 支持拖动移动窗口、双击最大化/还原。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("titlebar")
        self.setFixedHeight(44)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(12, 0, 0, 0)

        self.title_label = QLabel("AI助手")
        layout.addWidget(self.title_label)
        layout.addStretch()

        # 最小化按钮
        min_btn = QPushButton("_")
        min_btn.setFixedSize(36, 28)
        min_btn.clicked.connect(lambda: self.window().showMinimized())
        min_btn.setStyleSheet(self._btn_style())
        layout.addWidget(min_btn)

        # 最大化/还原按钮
        self.max_btn = QPushButton("□")
        self.max_btn.setFixedSize(36, 28)
        self.max_btn.clicked.connect(self._toggle_maximize)
        self.max_btn.setStyleSheet(self._btn_style())
        layout.addWidget(self.max_btn)

        # 关闭按钮
        close_btn = QPushButton("✕")
        close_btn.setFixedSize(36, 28)
        close_btn.clicked.connect(lambda: self.window().close())
        close_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                border-radius: 4px;
                color: #a6adc8;
                font-size: 14px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #f38ba8; color: #1e1e2e; }
        """)
        layout.addWidget(close_btn)

    def _btn_style(self) -> str:
        return """
            QPushButton {
                background-color: #313244;
                border-radius: 4px;
                color: #a6adc8;
                font-size: 16px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #45475a; color: #cdd6f4; }
        """

    def _toggle_maximize(self):
        win = self.window()
        if win.isMaximized():
            win.showNormal()
        else:
            win.showMaximized()

    def mousePressEvent(self, event):
        """按住标题栏拖动窗口(交给操作系统处理，DPI/多屏更可靠)。"""
        if event.button() == Qt.MouseButton.LeftButton:
            wh = self.window().windowHandle()
            if wh is not None:
                wh.startSystemMove()
                return
        super().mousePressEvent(event)

    def mouseDoubleClickEvent(self, event):
        """双击标题栏切换最大化/还原。"""
        self._toggle_maximize()
        super().mouseDoubleClickEvent(event)


class MainWindow(QMainWindow):
    """主窗口。"""

    # 关闭窗口(最小化到托盘)时发出，main.py 订阅该信号弹出托盘通知
    window_hidden = pyqtSignal()

    RESIZE_MARGIN = 6

    def __init__(self):
        super().__init__()
        self.setWindowTitle("AI小精灵")
        self.setMinimumSize(800, 600)
        self.resize(900, 650)

        # Frameless 无边框窗口，配合自绘标题栏，避免系统原生标题栏与
        # 自绘标题栏同时出现的双标题栏问题
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        self.setMouseTracking(True)

        # 应用样式
        self.setStyleSheet(STYLE_SHEET)

        # 中央组件
        central = QWidget()
        central.setMouseTracking(True)
        self.setCentralWidget(central)
        main_layout = QHBoxLayout(central)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # ===== 侧边栏 =====
        self.sidebar = Sidebar()
        self.sidebar.tab_changed.connect(self.switch_to_tab)
        main_layout.addWidget(self.sidebar)

        # ===== 右侧内容区 =====
        content_area = QWidget()
        content_layout = QVBoxLayout(content_area)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(0)

        # 标题栏
        self.titlebar = TitleBar()
        content_layout.addWidget(self.titlebar)

        # 面板堆叠
        self.stack = QStackedWidget()
        self._build_panels()
        content_layout.addWidget(self.stack)

        # AI 助手通过 Function Calling 增改日程后，让日历面板重新读盘刷新
        self.ai_panel.calendar_modified.connect(self.calendar_panel.reload_from_disk)

        main_layout.addWidget(content_area, 1)

        # 默认选中第一个面板
        self.sidebar.set_active(0)

        # Qt 的悬停 mouseMoveEvent 只投递给鼠标所在的最内层控件(且该控件需要
        # 自己开启 mouse tracking，不会像 mousePressEvent 那样向上冒泡)。
        # 边缘缩放区域会被 Sidebar/TitleBar/Stack 等子控件覆盖，因此需要给
        # 所有子控件也打开 mouse tracking，悬停缩放光标反馈才能生效。
        for w in self.findChildren(QWidget):
            w.setMouseTracking(True)

    def _build_panels(self):
        """构建所有功能面板(具名属性，供 main.py 跨面板信号连线)。
        注意: 添加顺序必须与 NAV_ITEMS 顺序一致(靠索引对应)。"""
        self.ai_panel = AIPanel()
        self.calendar_panel = CalendarPanel()
        self.search_panel = SearchPanel()
        self.aigc_panel = AIGCPanel()
        self.script_video_panel = ScriptVideoPanel()
        self.hermes_panel = HermesPanel()

        for panel in (
            self.ai_panel,
            self.calendar_panel,
            self.search_panel,
            self.aigc_panel,
            self.script_video_panel,
            self.hermes_panel,
        ):
            panel.setWindowTitle("")
            self.stack.addWidget(panel)

    def switch_to_tab(self, index: int):
        """切换到指定索引的面板。"""
        self.stack.setCurrentIndex(index)
        self.sidebar.set_active(index)
        self.titlebar.title_label.setText(NAV_ITEMS[index]["label"])

    def closeEvent(self, event):
        """处理窗口关闭事件 - 最小化到托盘而非退出。"""
        event.ignore()
        self.hide()
        self.window_hidden.emit()

    # ===== 边缘拖动缩放 =====

    def mousePressEvent(self, event):
        edge = self._edge_at(event.position().toPoint())
        if edge and event.button() == Qt.MouseButton.LeftButton:
            wh = self.windowHandle()
            if wh is not None:
                wh.startSystemResize(edge)
                return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        edge = self._edge_at(event.position().toPoint())
        self.setCursor(self._cursor_for(edge) if edge else Qt.CursorShape.ArrowCursor)
        super().mouseMoveEvent(event)

    def _edge_at(self, pos):
        r, m = self.rect(), self.RESIZE_MARGIN
        left = pos.x() <= m
        right = pos.x() >= r.width() - m
        top = pos.y() <= m
        bottom = pos.y() >= r.height() - m
        if top and left:
            return Qt.Edge.TopEdge | Qt.Edge.LeftEdge
        if top and right:
            return Qt.Edge.TopEdge | Qt.Edge.RightEdge
        if bottom and left:
            return Qt.Edge.BottomEdge | Qt.Edge.LeftEdge
        if bottom and right:
            return Qt.Edge.BottomEdge | Qt.Edge.RightEdge
        if left:
            return Qt.Edge.LeftEdge
        if right:
            return Qt.Edge.RightEdge
        if top:
            return Qt.Edge.TopEdge
        if bottom:
            return Qt.Edge.BottomEdge
        return None

    def _cursor_for(self, edge):
        if edge in (
            Qt.Edge.TopEdge | Qt.Edge.LeftEdge,
            Qt.Edge.BottomEdge | Qt.Edge.RightEdge,
        ):
            return Qt.CursorShape.SizeFDiagCursor
        if edge in (
            Qt.Edge.TopEdge | Qt.Edge.RightEdge,
            Qt.Edge.BottomEdge | Qt.Edge.LeftEdge,
        ):
            return Qt.CursorShape.SizeBDiagCursor
        if edge in (Qt.Edge.LeftEdge, Qt.Edge.RightEdge):
            return Qt.CursorShape.SizeHorCursor
        if edge in (Qt.Edge.TopEdge, Qt.Edge.BottomEdge):
            return Qt.CursorShape.SizeVerCursor
        return Qt.CursorShape.ArrowCursor
