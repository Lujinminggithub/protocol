"""
Hermes面板 - 系统工具与信息

系统监控支持定时自动刷新；清理临时文件改为异步扫描 + 按分组勾选确认，
不再是"一键删除整个目录"的黑盒操作。
"""

import os
import platform
from datetime import datetime

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLabel,
    QPushButton,
    QGroupBox,
    QTextEdit,
    QProgressBar,
    QFrame,
    QCheckBox,
    QMessageBox,
    QTreeWidget,
    QTreeWidgetItem,
    QScrollArea,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
from PyQt6.QtGui import QFont

from src.logger import get_logger


class SystemInfoWorker(QThread):
    """系统信息查询线程。"""

    info_updated = pyqtSignal(dict)

    def run(self):
        """收集系统信息(纯逻辑抽取到 src/tools/system_info.py，与工具调用共用)。"""
        from src.tools.system_info import collect_system_info

        info = collect_system_info()
        self.info_updated.emit(info)


class ScanWorker(QThread):
    """异步扫描可清理的临时文件，按来源分组回传(不阻塞 UI 线程)。"""

    group_scanned = pyqtSignal(dict)  # {"label", "path", "files", "size", "count"}
    scan_finished = pyqtSignal()

    def __init__(self, targets: list):
        """targets: [(label, path), ...]"""
        super().__init__()
        self.targets = targets
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def run(self):
        for label, path in self.targets:
            if self._cancel or not path or not os.path.isdir(path):
                continue
            files = []
            size = 0
            try:
                for root, dirs, fnames in os.walk(path, topdown=False):
                    if self._cancel:
                        break
                    for f in fnames:
                        fp = os.path.join(root, f)
                        try:
                            size += os.path.getsize(fp)
                            files.append(fp)
                        except OSError:
                            pass
            except (PermissionError, OSError):
                pass
            self.group_scanned.emit(
                {"label": label, "path": path, "files": files, "size": size, "count": len(files)}
            )
        self.scan_finished.emit()


class CleanWorker(QThread):
    """清理工作线程 —— 直接消费扫描阶段得到的文件清单，不重复 os.walk。"""

    log = pyqtSignal(str)
    finished = pyqtSignal(int, int)  # files_deleted, total_freed_bytes

    def __init__(self, files: list):
        super().__init__()
        self.files = files
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def run(self):
        count = 0
        freed = 0
        for fp in self.files:
            if self._cancel:
                break
            try:
                size = os.path.getsize(fp)
                os.remove(fp)
                count += 1
                freed += size
            except (PermissionError, OSError):
                self.log.emit(f"跳过(占用/无权限): {fp}")
        self.finished.emit(count, freed)


class HermesPanel(QWidget):
    """Hermes 系统工具面板。"""

    AUTO_REFRESH_INTERVAL_MS = 5000

    def __init__(self, parent=None):
        super().__init__(parent)
        self.sys_worker = None
        self.scan_worker = None
        self.clean_worker = None
        self._scan_groups = []  # 扫描到的分组数据，供清理阶段直接消费
        self._init_ui()

        self.refresh_timer = QTimer(self)
        self.refresh_timer.setInterval(self.AUTO_REFRESH_INTERVAL_MS)
        self.refresh_timer.timeout.connect(self._refresh_system_info)

        # 初始日志
        self._log("Hermes 面板已初始化")
        self._log(f"时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

        # 首次刷新
        self._refresh_system_info()

    def _init_ui(self):
        """初始化UI。"""
        # 外层套滚动区域，系统信息/清理/日志内容较多时可滚动查看，不被窗口高度裁掉
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet("border: none; background: transparent;")
        outer.addWidget(scroll)
        content = QWidget()
        scroll.setWidget(content)

        layout = QVBoxLayout(content)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(12)

        # ===== 标题 =====
        title = QLabel("Hermes 系统工具")
        title.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #cba6f7;")
        layout.addWidget(title)

        # ===== 系统信息卡片 =====
        info_frame = QFrame()
        info_frame.setObjectName("info-card")
        info_layout = QVBoxLayout(info_frame)
        info_layout.setSpacing(10)

        hostname_label = QLabel("主机名")
        hostname_label.setStyleSheet("color: #89b4fa; font-weight: bold; font-size: 13px;")
        self.hostname_value = QLabel(platform.node())
        self.hostname_value.setStyleSheet("color: #cdd6f4; font-size: 13px;")
        self.hostname_value.setWordWrap(True)
        info_layout.addWidget(hostname_label)
        info_layout.addWidget(self.hostname_value)

        os_label = QLabel("操作系统")
        os_label.setStyleSheet("color: #89b4fa; font-weight: bold; font-size: 13px;")
        self.os_value = QLabel(platform.platform())
        self.os_value.setStyleSheet("color: #cdd6f4; font-size: 13px;")
        self.os_value.setWordWrap(True)
        info_layout.addWidget(os_label)
        info_layout.addWidget(self.os_value)

        py_label = QLabel("Python版本")
        py_label.setStyleSheet("color: #89b4fa; font-weight: bold; font-size: 13px;")
        self.python_value = QLabel(platform.python_version())
        self.python_value.setStyleSheet("color: #cdd6f4; font-size: 13px;")
        info_layout.addWidget(py_label)
        info_layout.addWidget(self.python_value)

        layout.addWidget(info_frame)

        # ===== 资源监控 =====
        resource_group = QGroupBox("系统资源监控")
        resource_layout = QVBoxLayout(resource_group)

        cpu_label = QLabel("CPU 使用率")
        cpu_label.setStyleSheet("color: #a6adc8; font-size: 12px;")
        self.cpu_bar = QProgressBar()
        self.cpu_bar.setMaximumHeight(16)
        self.cpu_bar.setStyleSheet(self._progress_style("#f38ba8"))
        resource_layout.addWidget(cpu_label)
        resource_layout.addWidget(self.cpu_bar)

        mem_label = QLabel("内存使用率")
        mem_label.setStyleSheet("color: #a6adc8; font-size: 12px;")
        self.mem_bar = QProgressBar()
        self.mem_bar.setMaximumHeight(16)
        self.mem_bar.setStyleSheet(self._progress_style("#89b4fa"))
        resource_layout.addWidget(mem_label)
        resource_layout.addWidget(self.mem_bar)

        self.mem_detail = QLabel("")
        self.mem_detail.setStyleSheet("color: #6c7086; font-size: 11px;")
        resource_layout.addWidget(self.mem_detail)

        disk_label = QLabel("磁盘使用率")
        disk_label.setStyleSheet("color: #a6adc8; font-size: 12px;")
        self.disk_bar = QProgressBar()
        self.disk_bar.setMaximumHeight(16)
        self.disk_bar.setStyleSheet(self._progress_style("#a6e3a1"))
        resource_layout.addWidget(disk_label)
        resource_layout.addWidget(self.disk_bar)

        self.disk_detail = QLabel("")
        self.disk_detail.setStyleSheet("color: #6c7086; font-size: 11px;")
        resource_layout.addWidget(self.disk_detail)

        refresh_row = QHBoxLayout()
        refresh_btn = QPushButton("刷新数据")
        refresh_btn.clicked.connect(self._refresh_system_info)
        refresh_btn.setStyleSheet(self._btn_style("#313244", "#a6adc8", "#45475a"))
        refresh_row.addWidget(refresh_btn)

        self.auto_refresh_cb = QCheckBox(f"自动刷新(每{self.AUTO_REFRESH_INTERVAL_MS // 1000}秒)")
        self.auto_refresh_cb.setStyleSheet("color: #cdd6f4;")
        self.auto_refresh_cb.stateChanged.connect(self._toggle_auto_refresh)
        refresh_row.addWidget(self.auto_refresh_cb)
        resource_layout.addLayout(refresh_row)

        layout.addWidget(resource_group)

        # ===== 清理临时文件 =====
        clean_group = QGroupBox("清理临时文件")
        clean_layout = QVBoxLayout(clean_group)

        clean_mode_row = QHBoxLayout()
        clean_label = QLabel("清理模式:")
        clean_label.setStyleSheet("color: #a6adc8;")
        clean_mode_row.addWidget(clean_label)

        self.safe_radio = QCheckBox("安全模式 (仅系统临时文件)")
        self.safe_radio.setChecked(True)
        self.safe_radio.setStyleSheet("color: #cdd6f4;")
        clean_mode_row.addWidget(self.safe_radio)

        self.aggressive_radio = QCheckBox("激进模式 (含浏览器缓存)")
        self.aggressive_radio.setStyleSheet("color: #cdd6f4;")
        clean_mode_row.addWidget(self.aggressive_radio)
        clean_layout.addLayout(clean_mode_row)

        self.scan_btn = QPushButton("🔍 扫描可清理文件")
        self.scan_btn.clicked.connect(self._scan_temp)
        self.scan_btn.setStyleSheet(self._btn_style("#f38ba8", "#1e1e2e", "#eba0ac"))
        clean_layout.addWidget(self.scan_btn)

        # 扫描结果 —— 按来源分组，可勾选/取消勾选决定是否清理该分组
        self.clean_tree = QTreeWidget()
        self.clean_tree.setHeaderHidden(True)
        self.clean_tree.setMaximumHeight(160)
        self.clean_tree.setStyleSheet("""
            QTreeWidget {
                background-color: #181825;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 4px;
                color: #cdd6f4;
                font-size: 11px;
            }
            QTreeWidget::item { padding: 4px; }
        """)
        clean_layout.addWidget(self.clean_tree)

        self.clean_btn = QPushButton("🗑️ 执行清理(仅删除已勾选分组)")
        self.clean_btn.clicked.connect(self._execute_clean)
        self.clean_btn.setEnabled(False)
        self.clean_btn.setStyleSheet(self._btn_style("#a6e3a1", "#1e1e2e", "#94e2d5"))
        clean_layout.addWidget(self.clean_btn)

        layout.addWidget(clean_group)

        # ===== 快捷操作 =====
        action_group = QGroupBox("快捷操作")
        action_layout = QHBoxLayout(action_group)

        self.open_env_btn = QPushButton("环境变量")
        self.open_env_btn.clicked.connect(self._show_env)
        self.open_env_btn.setStyleSheet(self._btn_style("#89b4fa", "#1e1e2e", "#b4befe"))
        action_layout.addWidget(self.open_env_btn)

        self.open_logs_btn = QPushButton("日志查看")
        self.open_logs_btn.clicked.connect(self._show_logs)
        self.open_logs_btn.setStyleSheet(self._btn_style("#f9e2af", "#1e1e2e", "#fab387"))
        action_layout.addWidget(self.open_logs_btn)

        layout.addWidget(action_group)

        # ===== 日志输出 =====
        log_group = QGroupBox("运行日志")
        log_layout = QVBoxLayout(log_group)

        self.log_output = QTextEdit()
        self.log_output.setReadOnly(True)
        self.log_output.setMaximumHeight(200)
        self.log_output.setStyleSheet("""
            QTextEdit {
                background-color: #181825;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 8px;
                color: #a6adc8;
                font-family: Consolas, Courier New, monospace;
                font-size: 11px;
            }
        """)
        log_layout.addWidget(self.log_output)

        layout.addWidget(log_group)
        layout.addStretch()

    def _btn_style(self, bg: str, fg: str, hover_bg: str) -> str:
        return f"""
            QPushButton {{
                background-color: {bg};
                color: {fg};
                border-radius: 6px;
                padding: 8px 14px;
                font-weight: bold;
            }}
            QPushButton:hover {{ background-color: {hover_bg}; }}
        """

    def _progress_style(self, color: str) -> str:
        return f"""
            QProgressBar {{
                background-color: #313244;
                border: none;
                border-radius: 8px;
                text-align: center;
                height: 16px;
            }}
            QProgressBar::chunk {{
                background-color: {color};
                border-radius: 8px;
            }}
        """

    # ===== 系统监控 =====

    def _toggle_auto_refresh(self, state):
        if state == Qt.CheckState.Checked.value:
            self.refresh_timer.start()
        else:
            self.refresh_timer.stop()

    def _refresh_system_info(self):
        if self.sys_worker is not None and self.sys_worker.isRunning():
            return  # 上一次查询还没返回时不重复启动，避免堆积
        self._log("正在获取系统信息...")
        self.sys_worker = SystemInfoWorker()
        self.sys_worker.info_updated.connect(self._on_system_info)
        self.sys_worker.start()

    def _on_system_info(self, info: dict):
        cpu = info.get("cpu_percent", 0)
        mem = info.get("memory_percent", 0)
        disk = info.get("disk_percent", 0)

        self.cpu_bar.setValue(int(cpu))
        self.mem_bar.setValue(int(mem))
        self.disk_bar.setValue(int(disk))

        self.hostname_value.setText(info.get("hostname", "N/A"))

        mem_total = info.get("memory_total_gb", 0)
        mem_used = info.get("memory_used_gb", 0)
        if mem_total > 0:
            self.mem_detail.setText(f"已使用 {mem_used:.1f} GB / 总计 {mem_total:.1f} GB")

        disk_total = info.get("disk_total_gb", 0)
        disk_used = info.get("disk_used_gb", 0)
        if disk_total > 0:
            self.disk_detail.setText(f"已使用 {disk_used:.1f} GB / 总计 {disk_total:.1f} GB")

        self._log(f"CPU: {cpu}% | 内存: {mem}% | 磁盘: {disk}%")

    # ===== 清理：异步扫描 =====

    def _build_scan_targets(self) -> list:
        """收集需要扫描的 (label, path) 列表。

        %TEMP% 和 %TMP% 在绝大多数 Windows 系统上指向同一目录，按标准化路径
        去重，避免同一批文件被扫描/清理两次(以及清理时对已删除文件的误报日志)。
        """
        candidates = [
            ("用户临时目录 (TEMP)", os.environ.get("TEMP", "")),
            ("临时目录 (TMP)", os.environ.get("TMP", "")),
        ]
        if self.aggressive_radio.isChecked():
            user_profile = os.environ.get("USERPROFILE", "")
            candidates += [
                ("Chrome 缓存", os.path.join(user_profile, "AppData", "Local", "Google", "Chrome", "User Data", "Default", "Cache")),
                ("Edge 缓存 (INetCache)", os.path.join(user_profile, "AppData", "Local", "Microsoft", "Windows", "INetCache")),
                ("Discord 缓存", os.path.join(user_profile, "AppData", "Local", "Discord", "Cache")),
                ("Telegram 缓存", os.path.join(user_profile, "AppData", "Local", "Telegram Desktop", "tdata")),
                ("腾讯Files", os.path.join(user_profile, "AppData", "Roaming", "腾讯", "Files")),
            ]

        targets = []
        seen_paths = set()
        for label, path in candidates:
            if not path:
                continue
            norm = os.path.normcase(os.path.abspath(path))
            if norm in seen_paths:
                continue
            seen_paths.add(norm)
            targets.append((label, path))
        return targets

    def _scan_temp(self):
        """异步扫描可清理的临时文件(不阻塞UI)，按来源分组展示可勾选清单。"""
        self.clean_tree.clear()
        self._scan_groups = []
        self.clean_btn.setEnabled(False)
        self.scan_btn.setEnabled(False)
        self._log("正在扫描临时文件...")

        targets = self._build_scan_targets()
        self.scan_worker = ScanWorker(targets)
        self.scan_worker.group_scanned.connect(self._on_group_scanned)
        self.scan_worker.scan_finished.connect(self._on_scan_finished)
        self.scan_worker.start()

    def _on_group_scanned(self, group: dict):
        idx = len(self._scan_groups)
        self._scan_groups.append(group)
        item = QTreeWidgetItem(
            [f"{group['label']} — {group['count']} 个文件, {self._format_size(group['size'])}"]
        )
        item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
        item.setCheckState(0, Qt.CheckState.Checked)  # 默认全选，用户可取消勾选整个分组
        item.setData(0, Qt.ItemDataRole.UserRole, idx)
        item.setToolTip(0, group["path"])
        self.clean_tree.addTopLevelItem(item)

    def _on_scan_finished(self):
        self.scan_btn.setEnabled(True)
        self.clean_btn.setEnabled(bool(self._scan_groups))
        self._log(f"扫描完成，共 {len(self._scan_groups)} 个分组")

    # ===== 清理：执行(直接消费扫描结果，不重复 os.walk) =====

    def _execute_clean(self):
        selected_files = []
        for i in range(self.clean_tree.topLevelItemCount()):
            item = self.clean_tree.topLevelItem(i)
            if item.checkState(0) == Qt.CheckState.Checked:
                idx = item.data(0, Qt.ItemDataRole.UserRole)
                selected_files.extend(self._scan_groups[idx]["files"])

        if not selected_files:
            QMessageBox.information(self, "提示", "请先勾选要清理的分组。")
            return

        reply = QMessageBox.question(
            self,
            "确认清理",
            f"确定要删除已勾选分组中的 {len(selected_files)} 个文件吗？\n此操作不可撤销。",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply != QMessageBox.StandardButton.Yes:
            return

        self._log("开始清理临时文件...")
        self.scan_btn.setEnabled(False)
        self.clean_btn.setEnabled(False)

        self.clean_worker = CleanWorker(selected_files)
        self.clean_worker.log.connect(self._log)
        self.clean_worker.finished.connect(self._on_clean_finished)
        self.clean_worker.start()

    def _on_clean_finished(self, files_deleted: int, freed_bytes: int):
        self.scan_btn.setEnabled(True)
        self._log(f"清理完成: 删除 {files_deleted} 个文件, 释放 {self._format_size(freed_bytes)}")
        self.clean_tree.clear()
        self._scan_groups = []
        self.clean_btn.setEnabled(False)
        self._refresh_system_info()

    def _show_env(self):
        """显示环境变量。"""
        env_keys = ["PYTHONPATH", "PATH", "HOME", "USERPROFILE", "APPDATA"]
        lines = []
        for key in env_keys:
            val = os.environ.get(key, "(未设置)")
            if len(val) > 80:
                val = val[:77] + "..."
            lines.append(f"{key}={val}")
        self._log("\n".join(lines))

    def _show_logs(self):
        """显示应用日志文件最后若干行(真实读取日志文件，不再是空壳)。"""
        from src.paths import LOG_FILE

        self._log("===== 日志文件内容(最后50行) =====")
        if not os.path.exists(LOG_FILE):
            self._log("(暂无日志文件)")
        else:
            try:
                with open(LOG_FILE, "r", encoding="utf-8") as f:
                    lines = f.readlines()[-50:]
                for line in lines:
                    self.log_output.append(line.rstrip("\n"))
            except OSError as e:
                self._log(f"读取日志失败: {e}")
        self._log("--------------------")

    def _log(self, message: str):
        """添加日志条目(界面 + 落盘)。"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log_output.append(f"[{timestamp}] {message}")
        get_logger().info(message)

    @staticmethod
    def _format_size(size_bytes: int) -> str:
        """格式化文件大小。"""
        if size_bytes == 0:
            return "0 B"
        units = ["B", "KB", "MB", "GB", "TB"]
        i = 0
        size = float(size_bytes)
        while size >= 1024 and i < len(units) - 1:
            size /= 1024
            i += 1
        return f"{size:.1f} {units[i]}"
