"""
日历面板 - 日程管理

日程按日期持久化到本地 JSON，选中日期时会过滤展示当天日程，
支持编辑(删除后重建)/删除，以及到期提醒(定时器 + 转发给托盘通知)。
"""

from datetime import datetime

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLabel,
    QPushButton,
    QCalendarWidget,
    QListWidget,
    QListWidgetItem,
    QLineEdit,
    QTimeEdit,
    QCheckBox,
    QMessageBox,
    QDialog,
    QDialogButtonBox,
)
from PyQt6.QtCore import Qt, QDate, QTime, QTimer, pyqtSignal
from PyQt6.QtGui import QFont, QTextCharFormat, QColor

from src import calendar_store


class CalendarPanel(QWidget):
    """日历面板, 支持日程查看、增删与到期提醒。"""

    # 日程到期提醒 -> (标题, 内容)，由 main.py 转发给托盘通知
    reminder_triggered = pyqtSignal(str, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.events = calendar_store.load()
        self._init_ui()
        self._mark_dates_with_events()
        self._refresh_event_list()

        # 到期提醒定时器(每30秒检查一次今天的日程)
        self.reminder_timer = QTimer(self)
        self.reminder_timer.setInterval(30_000)
        self.reminder_timer.timeout.connect(self._check_reminders)
        self.reminder_timer.start()

    def reload_from_disk(self):
        """重新从磁盘加载日程数据并刷新界面。

        供 AI 助手通过 Function Calling 增/改日程后调用，让已打开的日历面板
        感知到外部写入的变化(否则用户会看到"AI说加成功了，但日历没变化")。
        """
        self.events = calendar_store.load()
        self._mark_dates_with_events()
        self._refresh_event_list()

    def _init_ui(self):
        """初始化UI。"""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(12)

        # ===== 标题行 =====
        title_row = QHBoxLayout()
        title = QLabel("📅 日历")
        title.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #cba6f7;")
        title_row.addWidget(title)
        title_row.addStretch()
        layout.addLayout(title_row)

        # ===== 日期导航 =====
        nav_row = QHBoxLayout()
        self.prev_month_btn = QPushButton("◀ 上月")
        self.prev_month_btn.clicked.connect(self._prev_month)
        self.prev_month_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                border-radius: 6px;
                padding: 6px 12px;
            }
            QPushButton:hover { background-color: #45475a; }
        """)

        self.date_label = QLabel()
        self.date_label.setFont(QFont("Microsoft YaHei UI", 13, QFont.Weight.Bold))
        self.date_label.setStyleSheet("color: #cdd6f4; padding: 6px;")

        self.next_month_btn = QPushButton("下月 ▶")
        self.next_month_btn.clicked.connect(self._next_month)
        self.next_month_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                border-radius: 6px;
                padding: 6px 12px;
            }
            QPushButton:hover { background-color: #45475a; }
        """)

        nav_row.addWidget(self.prev_month_btn)
        nav_row.addWidget(self.date_label, 1)
        nav_row.addWidget(self.next_month_btn)
        layout.addLayout(nav_row)

        # ===== 日历控件 =====
        self.calendar = QCalendarWidget(self)
        self.calendar.setGridVisible(True)
        self.calendar.setNavigationBarVisible(True)
        self.calendar.setSelectedDate(QDate.currentDate())
        self.calendar.currentPageChanged.connect(self._update_date_label)
        self.calendar.clicked.connect(self._on_date_selected)
        self.calendar.setStyleSheet("""
            QCalendarWidget QAbstractItemView {
                selection-background-color: #cba6f7;
                selection-color: #1e1e2e;
                background-color: #1e1e2e;
                color: #cdd6f4;
                gridline-color: #313244;
            }
            QCalendarWidget QMenuBar {
                background-color: #181825;
                color: #cdd6f4;
            }
            QCalendarWidget QLabel {
                color: #cba6f7;
            }
        """)
        layout.addWidget(self.calendar, 1)

        # 初始化日期标签（必须在 calendar 创建之后）
        self._update_date_label()

        # ===== 分隔线 =====
        separator = QLabel()
        separator.setStyleSheet("border-top: 1px solid #313244; margin: 8px 0;")
        layout.addWidget(separator)

        # ===== 日程区域 =====
        event_header = QHBoxLayout()
        self.event_title = QLabel("📌 当日日程")
        self.event_title.setFont(QFont("Microsoft YaHei UI", 13, QFont.Weight.Bold))
        self.event_title.setStyleSheet("color: #89b4fa;")
        event_header.addWidget(self.event_title)
        event_header.addStretch()

        self.delete_event_btn = QPushButton("🗑 删除选中")
        self.delete_event_btn.clicked.connect(self._delete_selected_event)
        self.delete_event_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                color: #a6adc8;
                border-radius: 6px;
                padding: 6px 14px;
            }
            QPushButton:hover { background-color: #45475a; color: #f38ba8; }
        """)
        event_header.addWidget(self.delete_event_btn)

        self.add_event_btn = QPushButton("+ 添加日程")
        self.add_event_btn.clicked.connect(self._add_event)
        self.add_event_btn.setStyleSheet("""
            QPushButton {
                background-color: #89b4fa;
                color: #1e1e2e;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #b4befe; }
        """)
        event_header.addWidget(self.add_event_btn)
        layout.addLayout(event_header)

        # 日程列表
        self.event_list = QListWidget()
        self.event_list.setObjectName("file-list")
        self.event_list.setStyleSheet("""
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
                color: #cba6f7;
            }
        """)
        layout.addWidget(self.event_list)

    # ===== 日期/数据辅助 =====

    def _date_key(self, qdate: QDate = None) -> str:
        return (qdate or self.calendar.selectedDate()).toString("yyyy-MM-dd")

    def _update_date_label(self):
        """更新日期显示标签。"""
        date = self.calendar.selectedDate()
        if date:
            self.date_label.setText(f"{date.year()}年{date.month()}月{date.day()}日")
        else:
            now = datetime.now()
            self.date_label.setText(f"{now.year}年{now.month}月{now.day}日")

    def _prev_month(self):
        self.calendar.showPreviousMonth()
        self._update_date_label()

    def _next_month(self):
        self.calendar.showNextMonth()
        self._update_date_label()

    def _on_date_selected(self, date: QDate):
        """日期选中事件 —— 按选中日期过滤展示日程。"""
        self._update_date_label()
        self._refresh_event_list()

    def _refresh_event_list(self):
        """按当前选中日期刷新日程列表。"""
        self.event_list.clear()
        key = self._date_key()
        date = self.calendar.selectedDate()
        self.event_title.setText(f"📌 {date.month()}月{date.day()}日 日程")
        for entry in self.events.get(key, []):
            reminder_mark = "🔔 " if entry.get("reminder") else ""
            time_str = entry.get("time", "")
            item = QListWidgetItem(f"{reminder_mark}[{time_str}] {entry['text']}")
            item.setData(Qt.ItemDataRole.UserRole, entry["id"])
            self.event_list.addItem(item)

    def _mark_dates_with_events(self):
        """在日历上标粗有日程的日期(先重置上一次标记过的日期，避免删空后残留高亮)。"""
        default_fmt = QTextCharFormat()
        for date in getattr(self, "_marked_dates", []):
            self.calendar.setDateTextFormat(date, default_fmt)

        fmt = QTextCharFormat()
        fmt.setFontWeight(QFont.Weight.Bold)
        fmt.setForeground(QColor("#f9e2af"))
        marked = []
        for key, entries in self.events.items():
            if not entries:
                continue
            parts = key.split("-")
            if len(parts) != 3:
                continue
            try:
                y, m, d = map(int, parts)
                date = QDate(y, m, d)
                self.calendar.setDateTextFormat(date, fmt)
                marked.append(date)
            except ValueError:
                continue
        self._marked_dates = marked

    # ===== 增删 =====

    def _add_event(self):
        """添加日程(带时间与是否提醒)。"""
        dialog = QDialog(self)
        dialog.setWindowTitle("添加日程")
        dialog.setStyleSheet("""
            QDialog { background-color: #1e1e2e; }
            QLabel, QLineEdit { color: #cdd6f4; }
            QDialogButtonBox { border-top: 1px solid #313244; }
        """)
        dialog.setMinimumSize(360, 220)

        layout = QVBoxLayout(dialog)

        label = QLabel("日程内容:")
        layout.addWidget(label)

        input_field = QLineEdit()
        input_field.setPlaceholderText("输入日程内容...")
        layout.addWidget(input_field)

        time_row = QHBoxLayout()
        time_row.addWidget(QLabel("提醒时间:"))
        time_edit = QTimeEdit()
        time_edit.setDisplayFormat("HH:mm")
        time_edit.setTime(QTime.currentTime())
        time_row.addWidget(time_edit, 1)
        layout.addLayout(time_row)

        reminder_cb = QCheckBox("到时间时弹出提醒")
        reminder_cb.setChecked(True)
        layout.addWidget(reminder_cb)

        btns = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        btns.accepted.connect(dialog.accept)
        btns.rejected.connect(dialog.reject)
        layout.addWidget(btns)

        if dialog.exec() == QDialog.DialogCode.Accepted:
            text = input_field.text().strip()
            if not text:
                return
            entry = {
                "id": calendar_store.new_id(),
                "text": text,
                "time": time_edit.time().toString("HH:mm"),
                "reminder": reminder_cb.isChecked(),
                "reminded": False,
            }
            key = self._date_key()
            self.events.setdefault(key, []).append(entry)
            calendar_store.save(self.events)
            self._refresh_event_list()
            self._mark_dates_with_events()

    def _delete_selected_event(self):
        """删除选中的日程。"""
        item = self.event_list.currentItem()
        if not item:
            QMessageBox.information(self, "提示", "请先选中一条日程。")
            return
        eid = item.data(Qt.ItemDataRole.UserRole)
        key = self._date_key()
        self.events[key] = [e for e in self.events.get(key, []) if e["id"] != eid]
        if not self.events[key]:
            del self.events[key]
        calendar_store.save(self.events)
        self._refresh_event_list()
        self._mark_dates_with_events()

    # ===== 到期提醒 =====

    def _check_reminders(self):
        """检查今天的日程是否到达提醒时间。"""
        key = self._date_key(QDate.currentDate())
        now_str = QTime.currentTime().toString("HH:mm")
        changed = False
        for entry in self.events.get(key, []):
            if (
                entry.get("reminder")
                and not entry.get("reminded")
                and entry.get("time", "") <= now_str
            ):
                self.reminder_triggered.emit("日程提醒", entry["text"])
                entry["reminded"] = True
                changed = True
        if changed:
            calendar_store.save(self.events)
            if key == self._date_key():
                self._refresh_event_list()
