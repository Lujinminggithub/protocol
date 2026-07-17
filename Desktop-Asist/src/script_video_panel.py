"""
剧本成片面板 - 输入剧本，自动拆分镜、逐场景生成、拼接成配音+字幕的长视频。
"""

import os
import subprocess

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLabel,
    QPushButton,
    QTextEdit,
    QComboBox,
    QCheckBox,
    QGroupBox,
    QProgressBar,
    QTreeWidget,
    QTreeWidgetItem,
    QScrollArea,
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont

from src.config import get_config
from src.video_pipeline.base import (
    PipelineConfig,
    NARRATION_MODE_NONE,
    NARRATION_MODE_SUBTITLES,
    NARRATION_MODE_VOICE,
)
from src.video_pipeline.pipeline_worker import ScriptToVideoWorker
from src.video_pipeline.tts_engine import VOICE_CHOICES

SCENE_COUNT_CHOICES = [
    ("自动(由AI决定)", 0),
    ("3个场景", 3),
    ("5个场景", 5),
    ("6个场景", 6),
    ("8个场景", 8),
    ("10个场景", 10),
]

NARRATION_MODE_CHOICES = [
    ("无旁白", NARRATION_MODE_NONE),
    ("仅字幕", NARRATION_MODE_SUBTITLES),
    ("旁白+字幕", NARRATION_MODE_VOICE),
]


class ScriptVideoPanel(QWidget):
    """剧本成片面板。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.worker = None
        self._output_path = None
        self._scene_items = {}  # index -> QTreeWidgetItem
        self._init_ui()

    def _init_ui(self):
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

        title = QLabel("🎬 剧本成片")
        title.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #cba6f7;")
        layout.addWidget(title)

        hint = QLabel(
            "输入一段剧本/故事，自动拆解为多个场景，逐场景生成画面+配音，"
            "最后拼接成带字幕的长视频。整个过程耗时较长(每个场景约1-5分钟)，请耐心等待。"
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #6c7086; font-size: 12px;")
        layout.addWidget(hint)

        # ===== 输入 =====
        input_group = QGroupBox("剧本内容")
        input_layout = QVBoxLayout(input_group)
        self.script_input = QTextEdit()
        self.script_input.setPlaceholderText(
            "在这里输入剧本或故事，例如：\n"
            "一只勇敢的小狐狸离开家去森林探险，途中遇到了会说话的猫头鹰，"
            "在猫头鹰的帮助下找到了传说中的月光泉..."
        )
        self.script_input.setMinimumHeight(140)
        self.script_input.setStyleSheet("""
            QTextEdit {
                background-color: #1e1e2e; border: 1px solid #313244;
                border-radius: 8px; padding: 10px; color: #cdd6f4; font-size: 13px;
            }
        """)
        input_layout.addWidget(self.script_input)
        layout.addWidget(input_group)

        # ===== 选项行 =====
        opts_group = QGroupBox("生成选项")
        opts_layout = QVBoxLayout(opts_group)

        cfg = get_config()
        vp = cfg.get_video_pipeline_settings()

        row1 = QHBoxLayout()
        row1.addWidget(QLabel("场景数量:"))
        self.scene_count_combo = QComboBox()
        for label, val in SCENE_COUNT_CHOICES:
            self.scene_count_combo.addItem(label, val)
        idx = self.scene_count_combo.findData(vp.get("default_scene_count", 6))
        if idx >= 0:
            self.scene_count_combo.setCurrentIndex(idx)
        row1.addWidget(self.scene_count_combo, 1)

        row1.addWidget(QLabel("配音音色:"))
        self.voice_combo = QComboBox()
        for value, label in VOICE_CHOICES:
            self.voice_combo.addItem(label, value)
        vidx = self.voice_combo.findData(vp.get("default_voice"))
        if vidx >= 0:
            self.voice_combo.setCurrentIndex(vidx)
        row1.addWidget(self.voice_combo, 1)
        opts_layout.addLayout(row1)

        row2 = QHBoxLayout()
        row2.addWidget(QLabel("旁白模式:"))
        self.narration_mode_combo = QComboBox()
        for label, value in NARRATION_MODE_CHOICES:
            self.narration_mode_combo.addItem(label, value)
        narration_mode = vp.get("narration_mode")
        if not narration_mode:
            narration_mode = NARRATION_MODE_VOICE if vp.get("enable_narration", True) else NARRATION_MODE_NONE
        midx = self.narration_mode_combo.findData(narration_mode)
        if midx >= 0:
            self.narration_mode_combo.setCurrentIndex(midx)
        self.narration_mode_combo.currentIndexChanged.connect(self._on_narration_mode_changed)
        row2.addWidget(self.narration_mode_combo, 1)
        opts_layout.addLayout(row2)

        self.burn_subs_cb = QCheckBox("烧录字幕(硬字幕，分享兼容性最好)")
        self.burn_subs_cb.setChecked(bool(vp.get("burn_subtitles", True)))
        self.burn_subs_cb.setStyleSheet("color: #cdd6f4;")
        opts_layout.addWidget(self.burn_subs_cb)
        self._on_narration_mode_changed()
        layout.addWidget(opts_group)

        # ===== 生成/取消 按钮 =====
        btn_row = QHBoxLayout()
        self.generate_btn = QPushButton("🎬 开始生成")
        self.generate_btn.setStyleSheet("""
            QPushButton {
                background-color: #a6e3a1; color: #1e1e2e; border-radius: 8px;
                padding: 10px 24px; font-weight: bold; font-size: 14px;
            }
            QPushButton:hover { background-color: #94e2d5; }
            QPushButton:disabled { background-color: #45475a; color: #6c7086; }
        """)
        self.generate_btn.clicked.connect(self._on_generate)
        btn_row.addWidget(self.generate_btn)

        self.cancel_btn = QPushButton("⏹ 取消")
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244; color: #f38ba8; border-radius: 8px;
                padding: 10px 18px;
            }
            QPushButton:hover { background-color: #45475a; }
            QPushButton:disabled { color: #6c7086; }
        """)
        self.cancel_btn.clicked.connect(self._on_cancel)
        btn_row.addWidget(self.cancel_btn)
        layout.addLayout(btn_row)

        # ===== 进度 =====
        self.stage_label = QLabel("")
        self.stage_label.setStyleSheet("color: #89b4fa; font-size: 13px; padding: 2px;")
        layout.addWidget(self.stage_label)

        self.progress_bar = QProgressBar()
        self.progress_bar.setMaximumHeight(16)
        self.progress_bar.setStyleSheet("""
            QProgressBar { background-color: #313244; border: none; border-radius: 8px;
                text-align: center; height: 16px; color: #cdd6f4; }
            QProgressBar::chunk { background-color: #a6e3a1; border-radius: 8px; }
        """)
        layout.addWidget(self.progress_bar)

        self.scene_list = QTreeWidget()
        self.scene_list.setHeaderLabels(["场景", "简述", "状态"])
        self.scene_list.setColumnWidth(0, 60)
        self.scene_list.setColumnWidth(1, 300)
        self.scene_list.setMinimumHeight(160)
        self.scene_list.setStyleSheet("""
            QTreeWidget { background-color: #181825; border: 1px solid #313244;
                border-radius: 8px; color: #cdd6f4; font-size: 12px; }
            QTreeWidget::item { padding: 4px; }
            QHeaderView::section { background-color: #313244; color: #cdd6f4;
                padding: 4px; border: none; }
        """)
        layout.addWidget(self.scene_list)

        # ===== 输出 =====
        output_group = QGroupBox("成片")
        output_layout = QVBoxLayout(output_group)
        self.output_path_label = QLabel("尚未生成")
        self.output_path_label.setWordWrap(True)
        self.output_path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.output_path_label.setStyleSheet("color: #a6adc8; font-size: 11px;")
        output_layout.addWidget(self.output_path_label)

        out_btn_row = QHBoxLayout()
        self.play_btn = QPushButton("▶ 用系统播放器打开")
        self.play_btn.clicked.connect(self._play_video)
        self.play_btn.setEnabled(False)
        out_btn_row.addWidget(self.play_btn)
        self.folder_btn = QPushButton("📂 在文件夹中打开")
        self.folder_btn.clicked.connect(self._open_folder)
        self.folder_btn.setEnabled(False)
        out_btn_row.addWidget(self.folder_btn)
        output_layout.addLayout(out_btn_row)
        layout.addWidget(output_group)

    # ===== 生成流程 =====

    def _on_narration_mode_changed(self):
        mode = self.narration_mode_combo.currentData()
        self.voice_combo.setEnabled(mode == NARRATION_MODE_VOICE)
        self.burn_subs_cb.setEnabled(mode != NARRATION_MODE_NONE)

    def _on_generate(self):
        script = self.script_input.toPlainText().strip()
        if not script:
            self.stage_label.setText("请先输入剧本内容！")
            return
        cfg = get_config()
        if not cfg.has_api_key("agnes"):
            self.stage_label.setText("请先在设置中配置 Agnes AI 的 API Token(图像/视频生成需要)！")
            return
        if not cfg.has_api_key():
            self.stage_label.setText(f"请先配置 {cfg.active_provider} 的 API Token(分镜拆解需要)！")
            return

        # 持久化用户偏好
        cfg.set_video_pipeline_settings(
            default_voice=self.voice_combo.currentData(),
            default_scene_count=self.scene_count_combo.currentData(),
            narration_mode=self.narration_mode_combo.currentData(),
            enable_narration=self.narration_mode_combo.currentData() != NARRATION_MODE_NONE,
            burn_subtitles=self.burn_subs_cb.isChecked(),
        )

        pcfg = PipelineConfig(
            script_text=script,
            scene_count_hint=self.scene_count_combo.currentData(),
            voice=self.voice_combo.currentData(),
            narration_mode=self.narration_mode_combo.currentData(),
            enable_narration=self.narration_mode_combo.currentData() != NARRATION_MODE_NONE,
            burn_subtitles=self.burn_subs_cb.isChecked(),
        )

        self.scene_list.clear()
        self._scene_items = {}
        self._output_path = None
        self.output_path_label.setText("生成中...")
        self.play_btn.setEnabled(False)
        self.folder_btn.setEnabled(False)
        self.progress_bar.setValue(0)
        self.generate_btn.setEnabled(False)
        self.cancel_btn.setEnabled(True)

        self.worker = ScriptToVideoWorker(cfg, pcfg)
        self.worker.stage_changed.connect(self.stage_label.setText)
        self.worker.scenes_ready.connect(self._populate_scenes)
        self.worker.scene_status_changed.connect(self._update_scene)
        self.worker.overall_progress.connect(self._update_progress)
        self.worker.finished_ok.connect(self._on_finished)
        self.worker.error_occurred.connect(self._on_error)
        self.worker.start()

    def _on_cancel(self):
        if self.worker is not None and self.worker.isRunning():
            self.worker.cancel()
            self.stage_label.setText("正在取消(当前步骤完成后停止)...")
        self.cancel_btn.setEnabled(False)

    def _populate_scenes(self, scene_dicts: list):
        self.scene_list.clear()
        self._scene_items = {}
        for s in scene_dicts:
            idx = s.get("index", 0)
            desc = (s.get("scene_description") or "")[:40]
            item = QTreeWidgetItem([f"{idx + 1}", desc, "⏳ 等待中"])
            self.scene_list.addTopLevelItem(item)
            self._scene_items[idx] = item
        self.progress_bar.setMaximum(max(1, len(scene_dicts)))

    def _update_scene(self, index: int, status: str, detail: str):
        item = self._scene_items.get(index)
        if item is not None:
            item.setText(2, detail)

    def _update_progress(self, done: int, total: int):
        self.progress_bar.setMaximum(max(1, total))
        self.progress_bar.setValue(done)

    def _on_finished(self, output_path: str, summary: str):
        self._output_path = output_path
        self.stage_label.setText(summary)
        self.output_path_label.setText(output_path)
        self.play_btn.setEnabled(True)
        self.folder_btn.setEnabled(True)
        self.generate_btn.setEnabled(True)
        self.cancel_btn.setEnabled(False)
        self.progress_bar.setValue(self.progress_bar.maximum())

    def _on_error(self, error: str):
        self.stage_label.setText(f"❌ {error}")
        self.output_path_label.setText("生成失败")
        self.generate_btn.setEnabled(True)
        self.cancel_btn.setEnabled(False)

    def _play_video(self):
        if self._output_path and os.path.exists(self._output_path):
            os.startfile(self._output_path)

    def _open_folder(self):
        if not (self._output_path and os.path.exists(self._output_path)):
            return
        path = os.path.normpath(self._output_path)
        # 用 Popen fire-and-forget，不等 explorer 的诡异退出码；重定向标准IO
        # (打包为无控制台程序时继承的句柄无效，不重定向会导致子进程创建异常，
        # 未捕获异常在 Qt 槽里会 abort 整个进程 —— 之前点击就是这样退出的)。
        # explorer 的 /select 参数必须与路径拼成一个参数。
        try:
            subprocess.Popen(
                ["explorer", f"/select,{path}"],
                creationflags=0x08000000,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            try:
                os.startfile(os.path.dirname(path))
            except Exception:
                self.stage_label.setText(f"无法打开文件夹，文件位置: {path}")
