"""
样式表 - 深色主题
"""

STYLE_SHEET = """
/* ===== 全局 ===== */
QWidget {
    background-color: #1e1e2e;
    color: #cdd6f4;
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 13px;
}

/* ===== 主窗口 ===== */
MainWindow {
    background-color: #1e1e2e;
}

/* ===== 侧边栏 ===== */
#sidebar {
    background-color: #181825;
    border-right: 1px solid #313244;
}

#sidebar QPushButton {
    background-color: transparent;
    border: none;
    border-radius: 8px;
    padding: 10px 14px;
    text-align: left;
    font-size: 13px;
    color: #a6adc8;
}

#sidebar QPushButton:hover {
    background-color: #313244;
    color: #cdd6f4;
}

#sidebar QPushButton:checked {
    background-color: #45475a;
    color: #cba6f7;
    font-weight: bold;
}

/* ===== 标题栏 ===== */
#titlebar {
    background-color: #181825;
    border-bottom: 1px solid #313244;
}

#titlebar QLabel {
    font-size: 15px;
    font-weight: bold;
    color: #cba6f7;
    padding: 12px;
}

#titlebar QPushButton {
    background-color: transparent;
    border: none;
    border-radius: 6px;
    padding: 6px 10px;
    color: #6c7086;
}

#titlebar QPushButton:hover {
    background-color: #313244;
    color: #cdd6f4;
}

/* ===== 聊天区域(AI助手) ===== */
#chat-area {
    background-color: #1e1e2e;
    border: none;
}

#chat-area QScrollBar:vertical {
    background-color: #1e1e2e;
    width: 6px;
    border: none;
}

#chat-area QScrollBar::handle:vertical {
    background-color: #45475a;
    border-radius: 3px;
    min-height: 20px;
}

#chat-area QScrollBar::add-line, #chat-area QScrollBar::sub-line {
    border: none;
    background: none;
}

.message-label {
    background-color: #313244;
    border-radius: 10px;
    padding: 10px 14px;
    margin: 4px 8px;
}

.user-message {
    background-color: #45475a;
    border: 1px solid #585b70;
}

.system-message {
    background-color: #181825;
    color: #6c7086;
    font-style: italic;
}

/* ===== 输入框 ===== */
#input-bar {
    background-color: #181825;
    border-top: 1px solid #313244;
}

#input-bar QLineEdit {
    background-color: #1e1e2e;
    border: 1px solid #313244;
    border-radius: 8px;
    padding: 10px 14px;
    color: #cdd6f4;
    font-size: 13px;
}

#input-bar QLineEdit:focus {
    border: 1px solid #cba6f7;
}

#input-bar QLineEdit::placeholder {
    color: #6c7086;
}

#input-bar QPushButton {
    background-color: #cba6f7;
    color: #1e1e2e;
    border: none;
    border-radius: 8px;
    padding: 10px 18px;
    font-weight: bold;
    font-size: 13px;
}

#input-bar QPushButton:hover {
    background-color: #b4befe;
}

/* ===== 日历 ===== */
QCalendarWidget {
    background-color: #1e1e2e;
    border: none;
}

QCalendarWidget QHeaderView#header {
    color: #cba6f7;
}

QCalendarWidget QAbstractItemView:enabled {
    selection-background-color: #cba6f7;
    selection-color: #1e1e2e;
    background-color: #1e1e2e;
    color: #cdd6f4;
    gridline-color: #313244;
}

QCalendarWidget QAbstractItemView:disabled {
    color: #45475a;
}

/* ===== 文件搜索 ===== */
#search-input {
    background-color: #1e1e2e;
    border: 1px solid #313244;
    border-radius: 8px;
    padding: 10px 14px;
    color: #cdd6f4;
    font-size: 13px;
}

#search-input:focus {
    border: 1px solid #89b4fa;
}

#file-list QScrollBar:vertical {
    background-color: #1e1e2e;
    width: 6px;
    border: none;
}

#file-list QScrollBar::handle:vertical {
    background-color: #45475a;
    border-radius: 3px;
    min-height: 20px;
}

#file-list QListWidget {
    background-color: #1e1e2e;
    border: none;
    border-radius: 6px;
    padding: 4px;
    color: #cdd6f4;
}

#file-list QListWidget::item {
    padding: 8px;
    border-radius: 4px;
}

#file-list QListWidget::item:hover {
    background-color: #313244;
}

#file-list QListWidget::item:selected {
    background-color: #45475a;
    color: #cba6f7;
}

/* ===== AIGC & Hermes ===== */
#text-output {
    background-color: #1e1e2e;
    border: 1px solid #313244;
    border-radius: 8px;
    padding: 12px;
    color: #cdd6f4;
    font-family: "Consolas", "Courier New", monospace;
    font-size: 12px;
}

/* ===== 通用按钮 ===== */
QPushButton {
    background-color: #45475a;
    color: #cdd6f4;
    border: none;
    border-radius: 8px;
    padding: 8px 16px;
    font-size: 13px;
}

QPushButton:hover {
    background-color: #585b70;
}

QPushButton:pressed {
    background-color: #cba6f7;
    color: #1e1e2e;
}

/* ===== 分组框 ===== */
QGroupBox {
    background-color: transparent;
    border: 1px solid #313244;
    border-radius: 8px;
    margin-top: 10px;
    padding-top: 16px;
    font-weight: bold;
    color: #cba6f7;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
}

/* ===== 标签页 ===== */
QTabWidget::pane {
    border: 1px solid #313244;
    border-radius: 8px;
    background-color: #1e1e2e;
}

QTabBar::tab {
    background-color: #181825;
    color: #a6adc8;
    padding: 8px 16px;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}

QTabBar::tab:selected {
    background-color: #1e1e2e;
    color: #cba6f7;
    font-weight: bold;
}

/* ===== 工具提示 ===== */
QToolTip {
    background-color: #313244;
    color: #cdd6f4;
    border: 1px solid #45475a;
    border-radius: 6px;
    padding: 6px;
}

/* ===== 分割器 ===== */
QSplitter::handle {
    background-color: #313244;
    border-radius: 2px;
}
"""
