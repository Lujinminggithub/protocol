"""
文件搜索面板 - 本地文件快速搜索
"""

import os
import fnmatch

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QFileDialog,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
from PyQt6.QtGui import QFont


class FileSearchWorker(QThread):
    """后台文件搜索线程。"""

    # 搜索结果信号
    found = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(self, search_dir: str, pattern: str):
        super().__init__()
        self.search_dir = search_dir
        self.pattern = pattern
        self._stopped = False

    def run(self):
        """执行文件搜索。"""
        try:
            pattern_lower = self.pattern.lower()
            for root, dirs, files in os.walk(self.search_dir):
                if self._stopped:
                    break
                for filename in files:
                    if self._stopped:
                        break
                    if fnmatch.fnmatch(filename.lower(), pattern_lower):
                        filepath = os.path.join(root, filename)
                        self.found.emit(filepath)
        finally:
            if not self._stopped:
                self.finished.emit()

    def stop(self):
        self._stopped = True


class SearchPanel(QWidget):
    """文件搜索面板。"""

    MAX_RESULTS = 2000

    def __init__(self, parent=None):
        super().__init__(parent)
        self.worker = None
        self.results = []
        self.search_dir = None  # 实例变量存储目录(不再依赖解析 status_label 文本)
        self._pending = []  # 待落地到 UI 的结果缓冲区

        self._flush_timer = QTimer(self)
        self._flush_timer.setInterval(100)  # 100ms 批量落地一次，减少逐条插入卡顿
        self._flush_timer.timeout.connect(self._flush_results)

        self._init_ui()

    def _init_ui(self):
        """初始化UI。"""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(12)

        # ===== 标题 =====
        title = QLabel("🔍 文件搜索")
        title.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #cba6f7;")
        layout.addWidget(title)

        # ===== 搜索控制区 =====
        control_row = QHBoxLayout()

        self.dir_button = QPushButton("📂 选择文件夹")
        self.dir_button.clicked.connect(self._select_directory)
        self.dir_button.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                border-radius: 6px;
                padding: 8px 14px;
            }
            QPushButton:hover { background-color: #45475a; }
        """)
        control_row.addWidget(self.dir_button)

        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText(
            "输入搜索关键词(支持通配符 *, ?; 无通配符时自动按子串匹配)..."
        )
        self.search_input.setObjectName("search-input")
        self.search_input.returnPressed.connect(self._start_search)
        control_row.addWidget(self.search_input, 1)

        self.search_button = QPushButton("🔎 搜索")
        self.search_button.clicked.connect(self._start_search)
        self.search_button.setStyleSheet("""
            QPushButton {
                background-color: #89b4fa;
                color: #1e1e2e;
                border-radius: 6px;
                padding: 8px 14px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #b4befe; }
        """)
        control_row.addWidget(self.search_button)

        self.cancel_button = QPushButton("⏹ 取消")
        self.cancel_button.clicked.connect(self._cancel_search)
        self.cancel_button.setEnabled(False)
        self.cancel_button.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                color: #f38ba8;
                border-radius: 6px;
                padding: 8px 14px;
            }
            QPushButton:hover { background-color: #45475a; }
            QPushButton:disabled { color: #6c7086; }
        """)
        control_row.addWidget(self.cancel_button)

        layout.addLayout(control_row)

        # ===== 状态信息 =====
        self.status_label = QLabel("请选择要搜索的文件夹, 然后输入关键词。")
        self.status_label.setStyleSheet("color: #6c7086; font-size: 12px; padding: 4px;")
        layout.addWidget(self.status_label)

        # ===== 结果列表 =====
        self.result_list = QListWidget()
        self.result_list.setObjectName("file-list")
        self.result_list.setStyleSheet("""
            QListWidget {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 6px;
                color: #cdd6f4;
            }
            QListWidget::item {
                padding: 8px;
                border-radius: 4px;
            }
            QListWidget::item:hover {
                background-color: #313244;
            }
            QListWidget::item:selected {
                background-color: #45475a;
                color: #89b4fa;
            }
        """)
        self.result_list.itemDoubleClicked.connect(self._open_file)
        layout.addWidget(self.result_list, 1)

    def _select_directory(self):
        """选择搜索目录。"""
        dir_path = QFileDialog.getExistingDirectory(self, "选择搜索文件夹")
        if dir_path:
            self.search_dir = dir_path
            self.status_label.setText(f"搜索目录: {dir_path}")

    def _build_pattern(self, query: str) -> str:
        """无通配符时自动包裹为 *query* 做子串匹配，更符合普通用户直觉。"""
        if any(ch in query for ch in "*?["):
            return query
        return f"*{query}*"

    def _start_search(self):
        """开始搜索。"""
        if self.worker is not None and self.worker.isRunning():
            return

        query = self.search_input.text().strip()
        if not query:
            self.status_label.setText("请输入搜索关键词!")
            return

        if not self.search_dir or not os.path.isdir(self.search_dir):
            self.status_label.setText("请先选择搜索目录!")
            return

        # 清理旧结果
        self.result_list.clear()
        self.results = []
        self._pending = []

        self.status_label.setText(f"正在搜索: {self.search_dir} ...")
        self.search_button.setEnabled(False)
        self.cancel_button.setEnabled(True)

        pattern = self._build_pattern(query)
        self.worker = FileSearchWorker(self.search_dir, pattern)
        self.worker.found.connect(self._on_found)
        self.worker.finished.connect(self._on_search_finished)
        self.worker.start()
        self._flush_timer.start()

    def _cancel_search(self):
        """取消正在进行的搜索。"""
        if self.worker:
            self.worker.stop()
        self.status_label.setText("已取消搜索。")
        self.cancel_button.setEnabled(False)

    def _on_found(self, filepath: str):
        """发现文件时的回调 —— 只缓冲，不直接操作 UI(由定时器批量落地)。"""
        self.results.append(filepath)
        self._pending.append(filepath)

        if len(self.results) > self.MAX_RESULTS:
            if self.worker:
                self.worker.stop()

    def _flush_results(self):
        """把缓冲区的结果批量插入列表，减少逐条插入导致的 UI 卡顿。"""
        if not self._pending:
            return
        batch, self._pending = self._pending, []
        for path in batch:
            self.result_list.addItem(QListWidgetItem(f"📄 {path}"))
        self.status_label.setText(f"正在搜索... 已找到 {len(self.results)} 个")

    def _on_search_finished(self):
        """搜索完成的回调。"""
        self._flush_timer.stop()
        self._flush_results()  # 补一次 flush，避免尾部结果丢失
        self.search_button.setEnabled(True)
        self.cancel_button.setEnabled(False)

        count = len(self.results)
        if count == 0:
            self.status_label.setText("未找到匹配的文件。")
        elif count > self.MAX_RESULTS:
            self.status_label.setText(f"找到超过{self.MAX_RESULTS}个结果, 仅显示前{self.MAX_RESULTS}个。")
        else:
            self.status_label.setText(f"搜索完成! 共找到 {count} 个匹配文件。")

    def _open_file(self, item: QListWidgetItem):
        """双击打开文件。"""
        filepath = item.text().replace("📄 ", "", 1)
        try:
            os.startfile(filepath)
        except Exception as e:
            self.status_label.setText(f"无法打开文件: {e}")
