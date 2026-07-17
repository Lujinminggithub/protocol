"""
剧本->长视频 总编排器 (QThread)。

流程: 分镜拆解 -> 逐场景(关键帧图 -> 旁白TTS -> 按真实旁白时长校正视频帧数 ->
场景视频 -> 音视频合并 -> 统一转码) -> 拼接 + 全片字幕。

设计要点:
- 先TTS后视频: 先拿旁白真实时长反推 num_frames，让视频时长精确匹配旁白
- 场景级重试(3次)+失败跳过(用关键帧定格/黑屏静音占位)，不让整体推倒重来
- 中间产物落盘 + manifest 断点续跑
- 取消只在步骤边界检查(不强杀 ffmpeg)
"""

import os
import time

import requests
from PyQt6.QtCore import QThread, pyqtSignal

from src.providers import registry
from src.providers.base import ProviderError
from src.media_worker import poll_video_result
from src.logger import get_logger
from src.video_pipeline import script_breakdown, tts_engine, video_ops, workdir
from src.video_pipeline.base import (
    Scene, PipelineConfig, PipelineError, duration_to_num_frames,
    NARRATION_MODE_NONE, NARRATION_MODE_SUBTITLES, NARRATION_MODE_VOICE,
)
from src.video_pipeline.ffmpeg_runtime import ffmpeg_path, probe_duration
from src.video_pipeline.text_utils import clean_text

SCENE_MAX_ATTEMPTS = 3
RETRY_BACKOFF = [0, 10, 30]  # 每次尝试前等待秒数
TRIM_START_SECONDS = 0.8
DEFAULT_VIDEO_NEGATIVE_PROMPT = (
    "different person, identity change, face distortion, different hairstyle, "
    "different clothing, age change, gender change, duplicate person, extra protagonist, "
    "extra limbs, bad hands, text, watermark"
)

NONHUMAN_KEYWORDS = ("狗", "犬", "猫", "龙", "狐", "狼", "鸟", "马", "鹿", "兔", "熊", "虎", "狮", "鱼", "蛇", "龟", "凤凰", "麒麟")
HUMAN_FORBID_PROMPT = "human, person, man, woman, girl, boy, humanoid, human face"
class ScriptToVideoWorker(QThread):
    stage_changed = pyqtSignal(str)  # 阶段级文案
    scene_status_changed = pyqtSignal(int, str, str)  # index, status, detail
    scenes_ready = pyqtSignal(list)  # 拆解完成，回传 Scene 列表(供UI铺列表)
    overall_progress = pyqtSignal(int, int)  # 完成场景数, 总场景数
    job_id_ready = pyqtSignal(str)
    finished_ok = pyqtSignal(str, str)  # 输出文件路径, 汇总提示(可能含跳过场景警告)
    error_occurred = pyqtSignal(str)

    def __init__(self, cfg, pipeline_config: PipelineConfig, resume_job_id: str = None):
        super().__init__()
        self.cfg = cfg
        self.pcfg = pipeline_config
        self.resume_job_id = resume_job_id
        self._cancel = False
        self.scenes = []
        self._logger = get_logger()

    def cancel(self):
        self._cancel = True

    def _cancelled(self) -> bool:
        return self._cancel

    # ===== 主流程 =====

    def run(self):
        try:
            self._run_inner()
        except PipelineError as e:
            self.error_occurred.emit(str(e))
        except Exception as e:
            self._logger.exception("剧本成片流水线异常")
            self.error_occurred.emit(f"出错了: {e}")

    def _run_inner(self):
        # ffmpeg 前置检查(缺失属环境性问题，直接整体失败)
        if not os.path.exists(ffmpeg_path()):
            raise PipelineError("找不到内置 ffmpeg，无法合成视频(打包资源可能损坏)。")

        # 1) 分镜拆解 或 从 manifest 续跑
        if self.resume_job_id:
            saved_cfg, saved_scenes = workdir.load_manifest(self.resume_job_id)
            if saved_cfg and saved_scenes:
                self.pcfg = saved_cfg
                self.scenes = saved_scenes
        if not self.scenes:
            self.stage_changed.emit("正在拆解剧本分镜...")
            char_ref, self.scenes = script_breakdown.breakdown_script(
                self.cfg, self.pcfg.script_text,
                self.pcfg.scene_count_hint, self.pcfg.frame_rate,
                self.pcfg.narration_mode,
            )
            self.pcfg.character_reference_prompt = char_ref
            if not self.pcfg.job_id:
                self.pcfg.job_id = time.strftime("%Y%m%d_%H%M%S")
            self.pcfg.work_dir = workdir.ensure_job_dir(self.pcfg.job_id)
            workdir.save_manifest(self.pcfg, self.scenes)

        self.job_id_ready.emit(self.pcfg.job_id)
        self.scenes_ready.emit([s.to_dict() for s in self.scenes])

        if self._cancelled():
            return

        # 生成全片共用的角色参考图(锁定人物一致性): 只生成一次，所有场景视频以它为起始帧
        self._ensure_character_reference()

        if self._cancelled():
            return

        # 2) 逐场景处理
        total = len(self.scenes)
        done_count = 0
        for scene in self.scenes:
            if self._cancelled():
                return
            if scene.status == "done":
                done_count += 1
                self.overall_progress.emit(done_count, total)
                self.scene_status_changed.emit(scene.index, "done", "✅ 完成")
                continue
            self._process_scene(scene)
            done_count += 1
            self.overall_progress.emit(done_count, total)
            workdir.save_manifest(self.pcfg, self.scenes)

        if self._cancelled():
            return

        # 3) 拼接 + 字幕
        self.stage_changed.emit("正在拼接最终视频...")
        output_path = self._assemble()

        skipped = [s.index + 1 for s in self.scenes if s.status == "skipped"]
        summary = "视频生成完成！"
        if skipped:
            summary += f" 注意：场景 {skipped} 生成失败已用占位画面代替，可稍后单独重做。"
        self.stage_changed.emit("完成")
        self.finished_ok.emit(output_path, summary)

    # ===== 角色参考图(全片共用) =====

    def _ensure_character_reference(self):
        """生成一张全片共用的角色定妆图，作为后续场景关键帧的统一人物来源。"""
        if self.pcfg.character_reference_url and os.path.exists(self.pcfg.character_reference_path):
            return
        prompt = clean_text(self.pcfg.character_reference_prompt, max_len=220)
        if not prompt:
            # 没有角色参考描述(退化情况)，跳过，场景视频将退回纯文生视频
            self._logger.warning("无角色参考描述，跳过角色锚点(人物一致性无法保证)")
            return
        prompt = (
            f"{prompt}。单主体角色设定图，主体可能是人类、动物或神话生物；主体清晰完整，物种明确且稳定，"
            "尽量全身或大半身，纯净简洁背景，无文字，无水印，无其他主体，不要拟人化。"
        )
        self.stage_changed.emit("正在生成角色定妆图(锁定人物形象)...")
        try:
            agnes = registry.build_provider("agnes", self.cfg)
            img = agnes.generate_image(
                prompt, size=f"{self.pcfg.width}x{self.pcfg.height}", prefer_url=True
            )
            url = img.get("url")
            if not url:
                raise PipelineError("角色定妆图未返回URL")
            self.pcfg.character_reference_url = url
            path = os.path.join(workdir.ensure_job_dir(self.pcfg.job_id), "character_ref.png")
            self._download(url, path)
            self.pcfg.character_reference_path = path
            workdir.save_manifest(self.pcfg, self.scenes)
        except Exception as e:
            self._logger.warning(f"角色定妆图生成失败，人物一致性将无法保证: {e}")

    def _build_scene_keyframe_prompt(self, scene: Scene) -> str:
        prompt = clean_text(scene.keyframe_prompt or scene.scene_description or "", max_len=260)
        if not prompt:
            return ""
        return f"{prompt}。{self._scene_constraint_text(scene, for_video=False)}"

    def _build_scene_video_prompt(self, scene: Scene) -> str:
        return f"{clean_text(scene.scene_description, max_len=260)}。{self._scene_constraint_text(scene, for_video=True)}"

    def _wants_narration_text(self) -> bool:
        return self.pcfg.narration_mode != NARRATION_MODE_NONE

    def _wants_voice(self) -> bool:
        return self.pcfg.narration_mode == NARRATION_MODE_VOICE

    def _wants_subtitles(self) -> bool:
        return self.pcfg.narration_mode in (NARRATION_MODE_SUBTITLES, NARRATION_MODE_VOICE)

    def _character_is_nonhuman(self) -> bool:
        ref = clean_text(self.pcfg.character_reference_prompt, max_len=260)
        return any(k in ref for k in NONHUMAN_KEYWORDS)

    def _scene_forbids_humans(self, scene: Scene) -> bool:
        return any("人类" in item or "人" == item for item in (scene.forbidden_elements or []))

    def _scene_requires_dragon(self, scene: Scene) -> bool:
        return any("龙" in item for item in (scene.required_elements or []))

    def _scene_has_multiple_required_subjects(self, scene: Scene) -> bool:
        return len(scene.required_elements or []) >= 2

    def _scene_constraint_text(self, scene: Scene, for_video: bool) -> str:
        parts = []
        if self._character_is_nonhuman():
            parts.append("主角必须保持原始物种和外观，不得变成人类、兽人或拟人化角色。")
        else:
            parts.append("主角必须保持与参考图一致的身份和外观，不要换脸或换人。")
        if scene.required_elements:
            parts.append(f"本场景必须清晰出现：{'、'.join(scene.required_elements[:4])}。")
        if scene.forbidden_elements:
            parts.append(f"本场景不得出现：{'、'.join(scene.forbidden_elements[:4])}。")
        if self._scene_requires_dragon(scene):
            parts.append("龙必须作为清晰可见的实体出现在画面中，不要只用金光、光束、阴影或抽象特效代替。")
        if self._scene_forbids_humans(scene):
            parts.append("除非剧本明确提到，否则画面中不要出现任何人类。")
        if for_video:
            parts.append("只做自然动作、表情和镜头运动，不要突然切换成别的物种或别的人物。")
        return "".join(parts)

    def _build_scene_negative_prompt(self, scene: Scene) -> str:
        parts = [DEFAULT_VIDEO_NEGATIVE_PROMPT]
        if self._scene_forbids_humans(scene):
            parts.append(HUMAN_FORBID_PROMPT)
        if self._scene_requires_dragon(scene):
            parts.append("missing dragon, hidden dragon, only light beam, only glow")
        return ", ".join(parts)

    def _ensure_scene_keyframe(self, agnes, scene: Scene, sdir: str):
        if scene.keyframe_url and os.path.exists(scene.keyframe_path):
            return
        ref_path = self.pcfg.character_reference_path
        prompt = self._build_scene_keyframe_prompt(scene)
        if not prompt:
            return
        self.scene_status_changed.emit(scene.index, "running", "🔄 生成场景关键帧")
        use_text_only = self._scene_has_multiple_required_subjects(scene)
        image_path = None
        if use_text_only:
            prompt = f"{clean_text(self.pcfg.character_reference_prompt, max_len=220)}。{prompt}"
        elif ref_path and os.path.exists(ref_path):
            image_path = ref_path
        else:
            use_text_only = True
            prompt = f"{clean_text(self.pcfg.character_reference_prompt, max_len=220)}。{prompt}"
        img = agnes.generate_image(
            prompt,
            size=f"{self.pcfg.width}x{self.pcfg.height}",
            image_path=image_path,
            prefer_url=True,
        )
        url = img.get("url")
        if not url:
            raise PipelineError("场景关键帧未返回URL")
        keyframe_path = os.path.join(sdir, "scene_keyframe.png")
        self._download(url, keyframe_path)
        scene.keyframe_url = url
        scene.keyframe_path = keyframe_path
        workdir.save_manifest(self.pcfg, self.scenes)

    # ===== 单场景处理(含重试+跳过) =====

    def _process_scene(self, scene: Scene):
        agnes = registry.build_provider("agnes", self.cfg)
        sdir = workdir.scene_dir(self.pcfg.job_id, scene.index)

        for attempt in range(SCENE_MAX_ATTEMPTS):
            if self._cancelled():
                return
            if attempt > 0:
                wait = RETRY_BACKOFF[min(attempt, len(RETRY_BACKOFF) - 1)]
                self.scene_status_changed.emit(
                    scene.index, "failed", f"⚠️ 失败，{wait}s后重试({attempt}/{SCENE_MAX_ATTEMPTS-1})"
                )
                time.sleep(wait)
            try:
                self._do_scene_steps(agnes, scene, sdir)
                scene.status = "done"
                scene.error = ""
                self.scene_status_changed.emit(scene.index, "done", "✅ 完成")
                return
            except (PipelineError, ProviderError) as e:
                scene.error = str(e)
                self._logger.warning(f"场景{scene.index}处理失败(尝试{attempt+1}): {e}")
            except Exception as e:
                scene.error = f"{e}"
                self._logger.exception(f"场景{scene.index}处理异常(尝试{attempt+1})")

        # 3次仍失败 -> 跳过，用占位片段
        self._make_placeholder(scene, sdir)
        scene.status = "skipped"
        self.scene_status_changed.emit(scene.index, "skipped", "⏭ 已跳过(占位)")

    def _do_scene_steps(self, agnes, scene: Scene, sdir: str):
        rate = self.pcfg.frame_rate

        # a) 旁白 TTS(拿真实时长反推视频帧数)
        scene.narration = clean_text(scene.narration, max_len=500) if self._wants_narration_text() else ""
        if scene.narration and (not scene.narration_audio_path or not os.path.exists(scene.narration_audio_path)):
            if self._wants_voice():
                self.scene_status_changed.emit(scene.index, "running", "🔄 生成旁白配音")
                mp3 = os.path.join(sdir, "narration.mp3")
                srt = os.path.join(sdir, "narration.srt")
                tts_engine.synthesize(scene.narration, self.pcfg.voice, mp3, srt)
                scene.narration_audio_path = mp3
                scene.narration_srt_path = srt

        # 按旁白真实时长校正 num_frames(旁白+0.8s收尾余量)
        if self._wants_voice() and scene.narration_audio_path and os.path.exists(scene.narration_audio_path):
            real = probe_duration(scene.narration_audio_path)
            if real > 0:
                scene.num_frames = duration_to_num_frames(real + 0.8, rate)
                scene.duration_hint = round(scene.num_frames / rate, 2)

        if self._cancelled():
            raise PipelineError("已取消")

        # b) 场景关键帧: 先把角色定妆图迁移到当前场景，避免视频直接从证件照硬变形。
        self._ensure_scene_keyframe(agnes, scene, sdir)

        if self._cancelled():
            raise PipelineError("已取消")

        # c) 场景视频: 优先用场景关键帧作 image-to-video 首帧，并在 prompt 中重复身份锁定。
        if not scene.raw_video_path or not os.path.exists(scene.raw_video_path):
            self.scene_status_changed.emit(scene.index, "running", "🔄 提交视频生成")
            ref_url = scene.keyframe_url or self.pcfg.character_reference_url
            video_id = agnes.create_video_task(
                self._build_scene_video_prompt(scene),
                width=self.pcfg.width, height=self.pcfg.height,
                num_frames=scene.num_frames, frame_rate=rate,
                image_url=ref_url,
                negative_prompt=self._build_scene_negative_prompt(scene),
            )
            video_url = poll_video_result(
                agnes, video_id,
                on_progress=lambda msg: self.scene_status_changed.emit(scene.index, "running", f"🔄 {msg}"),
                should_cancel=self._cancelled,
            )
            if video_url is None:
                raise PipelineError("已取消")
            raw = os.path.join(sdir, "raw.mp4")
            self._download(video_url, raw)
            # 裁掉开头的起始定妆照定格段(切场景时不再"闪一下证件照")。
            # 视频足够长才裁，避免极短片段被裁没。
            raw_dur = probe_duration(raw)
            if raw_dur > TRIM_START_SECONDS + 1.0:
                trimmed = os.path.join(sdir, "raw_trimmed.mp4")
                try:
                    video_ops.trim_clip_start(raw, trimmed, TRIM_START_SECONDS)
                    scene.raw_video_path = trimmed
                except PipelineError:
                    scene.raw_video_path = raw  # 裁剪失败退回用原片，不影响主流程
            else:
                scene.raw_video_path = raw

        if self._cancelled():
            raise PipelineError("已取消")

        # d) 音视频合并 + 统一转码
        self.scene_status_changed.emit(scene.index, "running", "🔄 合成片段")
        if self._wants_voice() and scene.narration_audio_path and os.path.exists(scene.narration_audio_path):
            with_audio = os.path.join(sdir, "with_audio.mp4")
            video_ops.merge_audio_video(scene.raw_video_path, scene.narration_audio_path, with_audio)
            src_for_norm = with_audio
        else:
            src_for_norm = scene.raw_video_path

        normalized = os.path.join(sdir, "normalized.mp4")
        video_ops.normalize_clip(src_for_norm, normalized, self.pcfg.width, self.pcfg.height, rate)
        scene.normalized_path = normalized
        scene.actual_duration = probe_duration(normalized)
        if (
            self.pcfg.narration_mode == NARRATION_MODE_SUBTITLES
            and scene.narration
            and (not scene.narration_srt_path or not os.path.exists(scene.narration_srt_path))
        ):
            srt = os.path.join(sdir, "narration.srt")
            video_ops.write_plain_srt(scene.narration, scene.actual_duration or scene.duration_hint, srt)
            scene.narration_srt_path = srt

    def _make_placeholder(self, scene: Scene, sdir: str):
        """场景彻底失败时的占位片段(角色定妆图定格或黑屏 + 旁白或静音)。"""
        try:
            normalized = os.path.join(sdir, "normalized.mp4")
            seconds = scene.duration_hint if scene.duration_hint else 5.0
            ref_path = scene.keyframe_path or self.pcfg.character_reference_path
            video_ops.make_placeholder_clip(
                normalized, self.pcfg.width, self.pcfg.height, self.pcfg.frame_rate,
                seconds,
                audio_path=scene.narration_audio_path
                if self._wants_voice() and os.path.exists(scene.narration_audio_path or "")
                else None,
                image_path=ref_path if os.path.exists(ref_path or "") else None,
            )
            scene.normalized_path = normalized
            scene.actual_duration = probe_duration(normalized)
        except Exception as e:
            self._logger.exception(f"场景{scene.index}占位片段生成也失败: {e}")

    # ===== 拼接 =====

    def _assemble(self) -> str:
        job = self.pcfg.job_id
        jdir = workdir.job_dir(job)
        usable_scenes = [s for s in self.scenes if s.normalized_path and os.path.exists(s.normalized_path)]
        if any(s.status == "done" for s in usable_scenes):
            while usable_scenes and usable_scenes[0].status == "skipped":
                usable_scenes.pop(0)
        clips = [s.normalized_path for s in usable_scenes]
        if not clips:
            raise PipelineError("没有任何可用的场景片段，无法拼接。")

        filelist = os.path.join(jdir, "filelist.txt")
        concat_out = os.path.join(jdir, "concat.mp4")
        video_ops.concat_clips(clips, filelist, concat_out)

        # 字幕(全片SRT时间戳按各场景实测时长偏移)
        srt_paths = [s.narration_srt_path for s in usable_scenes]
        durations = [s.actual_duration for s in usable_scenes]
        has_subs = self._wants_subtitles() and any(p and os.path.exists(p) for p in srt_paths)

        output = os.path.join(jdir, "output.mp4")
        if has_subs:
            full_srt = os.path.join(jdir, "full.srt")
            video_ops.merge_srt(srt_paths, durations, full_srt)
            if self.pcfg.burn_subtitles:
                video_ops.burn_subtitles(concat_out, full_srt, output)
            else:
                video_ops.mux_soft_subtitles(concat_out, full_srt, output)
        else:
            os.replace(concat_out, output)

        workdir.save_manifest(self.pcfg, self.scenes)
        return output

    # ===== 工具 =====

    def _download(self, url: str, dest: str, timeout: int = 120):
        resp = requests.get(url, timeout=timeout, stream=True)
        resp.raise_for_status()
        with open(dest, "wb") as f:
            for chunk in resp.iter_content(chunk_size=65536):
                if chunk:
                    f.write(chunk)
