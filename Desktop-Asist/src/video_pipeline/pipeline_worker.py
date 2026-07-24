"""
剧本->长视频 总编排器 (QThread)。

流程: 分镜拆解 -> 逐场景(关键帧图 -> 旁白TTS -> 按真实旁白时长校正视频帧数 ->
场景视频 -> 音视频合并 -> 统一转码) -> 拼接 + 全片字幕。

设计要点:
- 先TTS后视频: 先拿旁白真实时长反推 num_frames，让视频时长精确匹配旁白
- 场景级持续重试，直到成功或用户主动取消
- 中间产物落盘 + manifest 断点续跑
- 取消只在步骤边界检查(不强杀 ffmpeg)
"""

import json
import hashlib
import os
import re
import shutil
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import requests
from PyQt6.QtCore import QThread, pyqtSignal

from src.providers import registry
from src.providers.base import ProviderError
from src.media_worker import poll_video_result
from src.logger import get_logger
from src.video_pipeline import prompt_audit, script_breakdown, tts_engine, video_ops, workdir
from src.video_pipeline.base import (
    CharacterProfile, VisualAsset,
    DialogueLine,
    Scene, PipelineConfig, PipelineError, duration_to_num_frames,
    NARRATION_MODE_NONE, NARRATION_MODE_SUBTITLES, NARRATION_MODE_VOICE,
)
from src.video_pipeline.ffmpeg_runtime import ffmpeg_path, probe_duration
from src.video_pipeline.text_utils import clean_text, strict_chinese_caption

RETRY_BACKOFF = [0, 8, 15, 25, 40]  # 每次尝试前等待秒数
SCENE_FRAME_RETRY_FACTORS = [1.0, 1.0, 0.88, 0.76, 0.66]
MIN_RETRY_FRAMES = 96
TRIM_START_SECONDS = 0.8
DEFAULT_VIDEO_NEGATIVE_PROMPT = (
    "different person, identity change, face distortion, different hairstyle, "
    "different clothing, age change, gender change, duplicate person, extra protagonist, "
    "extra limbs, bad hands, extra hands, extra fingers, fused fingers, broken fingers, "
    "four arms, mutated hands, crossed anatomy, malformed palms, hand-object mismatch, "
    "deformed face, asymmetrical eyes, warped mouth, distorted nose, melted face, twisted face, "
    "double face, two mouths, broken jaw, static frame, frozen frame, "
    "cartoon, anime, chibi, illustration, comic, cel shading, 3d render, doll face, toy-like, "
    "text, english text, letters, gibberish text, mojibake, generated subtitles, watermark"
)

NONHUMAN_KEYWORDS = ("狗", "犬", "猫", "龙", "狐", "狼", "鸟", "马", "鹿", "兔", "熊", "虎", "狮", "鱼", "蛇", "龟", "凤凰", "麒麟")
HUMAN_FORBID_PROMPT = "human, person, man, woman, girl, boy, humanoid, human face"

FEMALE_VOICE_HINTS = ("女", "女生", "女人", "少女", "温柔", "活泼", "柔和")
MALE_VOICE_HINTS = ("男", "男生", "男人", "少年", "低沉", "稳重", "阳光")
HANDHELD_PROPS = ("手机", "包子", "豆浆", "玉玺", "诏书", "文件", "圣旨", "酒杯")


class ScriptToVideoWorker(QThread):
    stage_changed = pyqtSignal(str)  # 阶段级文案
    characters_ready = pyqtSignal(list)  # 角色提取完成，回传角色列表和建议音色
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
        self._voice_assignment_event = threading.Event()
        self._awaiting_voice_assignment = False
        self._manifest_lock = threading.RLock()
        self._logger = get_logger()

    def _save_manifest(self):
        with self._manifest_lock:
            workdir.save_manifest(self.pcfg, self.scenes)

    def cancel(self):
        self._cancel = True
        self._voice_assignment_event.set()

    def _cancelled(self) -> bool:
        return self._cancel

    def _wait_or_cancel(self, seconds: float) -> bool:
        deadline = time.monotonic() + max(0.0, seconds)
        while time.monotonic() < deadline:
            if self._cancelled():
                return False
            time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))
        return not self._cancelled()

    @staticmethod
    def _is_retryable_error(exc: Exception) -> bool:
        text = str(exc)
        status_match = re.search(r"HTTP\s+(4\d\d)", text, re.IGNORECASE)
        if status_match and status_match.group(1) != "429":
            return False
        return not any(marker in text for marker in ("API Key", "Token", "参数错误", "不支持的参数"))

    def apply_character_voice_overrides(self, overrides: dict[str, str]):
        self.pcfg.character_voice_overrides = {
            clean_text(k, max_len=40): clean_text(v, max_len=60)
            for k, v in (overrides or {}).items()
            if clean_text(k, max_len=40) and clean_text(v, max_len=60)
        }
        profiles = self._character_profiles()
        for profile in profiles:
            profile.voice_id = (
                self.pcfg.character_voice_overrides.get(profile.character_id)
                or self._voice_from_profile(profile)
            )
        self.pcfg.character_profiles = [profile.to_dict() for profile in profiles]
        self._voice_assignment_event.set()

    # ===== 主流程 =====

    def run(self):
        try:
            self._run_inner()
        except PipelineError as e:
            self.error_occurred.emit(str(e))
        except Exception as e:
            self._logger.exception("剧本成片流水线异常")
            self.error_occurred.emit(f"出错了: {e}")
        finally:
            prompt_audit.disable()

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
        if not self.pcfg.job_id:
            self.pcfg.job_id = time.strftime("%Y%m%d_%H%M%S")
        self.pcfg.work_dir = workdir.ensure_job_dir(self.pcfg.job_id)
        if self.pcfg.prompt_debug_mode:
            self.pcfg.prompt_output_dir = workdir.prompt_dir(self.pcfg.job_id)
            prompt_audit.configure(self.pcfg.prompt_output_dir)
            prompt_audit.record(
                "source_script",
                {"script_text": self.pcfg.script_text, "pipeline_config": self.pcfg.to_dict()},
                label="原始剧本与调试配置",
            )
        if not self.scenes:
            self.stage_changed.emit("正在提取视频结构...")
            char_ref, characters, self.scenes = script_breakdown.extract_story_data(
                self.cfg, self.pcfg.script_text,
                self.pcfg.scene_count_hint, self.pcfg.frame_rate,
                self.pcfg.narration_mode,
                on_stage=self.stage_changed.emit,
            )
            self.pcfg.character_reference_prompt = char_ref
            self.pcfg.character_profiles = [item.to_dict() for item in characters]
            self._save_manifest()

        self._wait_for_voice_assignment_if_needed()
        if self._cancelled():
            return

        self.job_id_ready.emit(self.pcfg.job_id)
        self.scenes_ready.emit([s.to_dict() for s in self.scenes])

        if self._cancelled():
            return

        # 为每个角色分别生成定妆图，后续按场景角色表选择对应参考图。
        self._ensure_character_references()

        if self._cancelled():
            return

        # 首次生成完整视觉资产库；后续关键帧只能基于这些固定图片做图生图。
        self._ensure_visual_asset_references()

        if self._cancelled():
            return

        self._enrich_production_plan()

        if self._cancelled():
            return

        # 关键帧按剧情顺序生成，后续镜头可以引用上一帧；视频任务仍并发执行。
        self._prepare_keyframes_in_order()

        if self._cancelled():
            return

        # 2) 以有限线程池并发处理分镜；最终拼接仍严格按 scene.index。
        self._process_scenes_concurrently()

        if self._cancelled():
            return

        if self.pcfg.prompt_debug_mode:
            output_dir = self._write_prompt_summary()
            self.stage_changed.emit("调试素材与 Prompt 已生成")
            self.finished_ok.emit(output_dir, "调试完成：未生成视频，全部 Prompt 已导出。")
            return

        # 3) 拼接 + 字幕
        self.stage_changed.emit("正在拼接最终视频...")
        output_path = self._assemble()

        summary = "视频生成完成！"
        self.stage_changed.emit("完成")
        self.finished_ok.emit(output_path, summary)

    def _process_scenes_concurrently(self):
        total = len(self.scenes)
        done_count = sum(1 for scene in self.scenes if scene.status == "done")
        for scene in self.scenes:
            if scene.status == "done":
                self.scene_status_changed.emit(scene.index, "done", "✅ 完成")
        self.overall_progress.emit(done_count, total)
        pending = [scene for scene in self.scenes if scene.status != "done"]
        max_workers = max(1, min(int(self.pcfg.max_concurrent_scenes or 1), 6, len(pending) or 1))
        self.stage_changed.emit(f"正在并发生成分镜（{max_workers} 路）...")
        with ThreadPoolExecutor(max_workers=max_workers, thread_name_prefix="scene") as executor:
            futures = {executor.submit(self._process_scene, scene): scene for scene in pending}
            for future in as_completed(futures):
                if self._cancelled():
                    break
                scene = futures[future]
                try:
                    future.result()
                except Exception as exc:
                    raise PipelineError(f"场景 {scene.index + 1} 处理失败：{exc}") from exc
                done_count += 1
                self.overall_progress.emit(done_count, total)
                self._save_manifest()

    def _wait_for_voice_assignment_if_needed(self):
        if not self._wants_voice():
            return
        profiles = self._character_profiles()
        if not profiles:
            return
        self._voice_assignment_event.clear()
        self._awaiting_voice_assignment = True
        self.characters_ready.emit(self._character_profiles_payload())
        self.stage_changed.emit("角色已提取，请确认每个角色的音色后继续生成...")
        while not self._voice_assignment_event.wait(timeout=0.2):
            if self._cancelled():
                self._awaiting_voice_assignment = False
                return
        self._awaiting_voice_assignment = False
        self._save_manifest()

    # ===== 角色参考图(全片共用) =====

    def _ensure_character_references(self):
        """为每个角色生成独立定妆图，并把外观、服装、音色一起固化到 manifest。"""
        profiles = self._character_profiles()
        if not profiles:
            self._logger.warning("没有角色档案，无法建立人物一致性锚点")
            return
        character_dir = os.path.join(workdir.ensure_job_dir(self.pcfg.job_id), "characters")
        os.makedirs(character_dir, exist_ok=True)
        pending = [
            profile for profile in profiles
            if not (profile.reference_url and os.path.exists(profile.reference_path))
        ]

        def generate_reference(profile: CharacterProfile):
            identity = "，".join(
                part for part in [profile.species, profile.appearance, profile.outfit] if part
            ) or profile.name or profile.character_id
            profile.reference_prompt = clean_text(
                f"写实影视角色定妆照。{profile.name}，{identity}。"
                "单人，胸像，正面自然表情，面部清晰，固定发型服装配饰，中性纯色背景，无文字。",
                max_len=220,
            )
            if not any(k in f"{profile.species}{profile.appearance}" for k in NONHUMAN_KEYWORDS):
                profile.reference_prompt += " 真实东方面孔，真实皮肤和布料，不要插画或卡通。"
            attempt = 0
            while not self._cancelled():
                try:
                    agnes = registry.build_provider("agnes", self.cfg)
                    self.stage_changed.emit(f"正在生成角色定妆图：{profile.name or profile.character_id}...")
                    img = agnes.generate_image(
                        profile.reference_prompt,
                        size="1K",
                        ratio="16:9",
                        prefer_url=True,
                    )
                    url = img.get("url")
                    if not url:
                        raise PipelineError("角色定妆图未返回URL")
                    safe_id = "".join(ch for ch in profile.character_id if ch.isalnum() or ch in "-_") or "character"
                    path = os.path.join(character_dir, f"{safe_id}.png")
                    self._download(url, path)
                    profile.reference_url = url
                    profile.reference_path = path
                    return
                except Exception as exc:
                    if not self._is_retryable_error(exc):
                        raise PipelineError(f"视觉资产 {item.name} 请求不可重试：{exc}") from exc
                    attempt += 1
                    wait = min(60, RETRY_BACKOFF[min(attempt, len(RETRY_BACKOFF) - 1)])
                    self._logger.warning(f"角色{profile.name}定妆图失败，第{attempt}次重试: {exc}")
                    self.stage_changed.emit(f"角色定妆图失败，{wait}s 后继续重试：{profile.name}")
                    if not self._wait_or_cancel(wait):
                        return

        workers = max(1, min(int(self.pcfg.max_concurrent_scenes or 1), 4, len(pending) or 1))
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="character") as executor:
            futures = [executor.submit(generate_reference, profile) for profile in pending]
            for future in as_completed(futures):
                future.result()
                if self._cancelled():
                    return

        self.pcfg.character_profiles = [item.to_dict() for item in profiles]
        for profile in profiles:
            if profile.is_primary or not self.pcfg.character_reference_path:
                self.pcfg.character_reference_prompt = profile.reference_prompt
                self.pcfg.character_reference_url = profile.reference_url
                self.pcfg.character_reference_path = profile.reference_path
        self._save_manifest()

    def _ensure_visual_asset_references(self):
        """为每个背景和关键道具生成独立文生图定妆，并固化到 manifest。"""
        assets = {item.asset_id: item for item in self._visual_assets()}
        character_names = {
            clean_text(value, max_len=40)
            for profile in self._character_profiles()
            for value in (profile.name, profile.species)
            if value
        }

        def make_id(kind: str, name: str) -> str:
            digest = hashlib.sha1(f"{kind}:{name}".encode("utf-8")).hexdigest()[:12]
            return f"{kind}_{digest}"

        if not self.pcfg.visual_asset_inventory_ready:
            self.stage_changed.emit("正在提取全剧人物、背景与道具资产清单...")
            try:
                inventory = script_breakdown.extract_visual_asset_inventory(
                    self.cfg,
                    self.pcfg.script_text,
                    self._character_profiles(),
                    self.scenes,
                )
            except Exception as exc:
                self._logger.warning("Agnes 视觉资产清单提取失败，使用本地清单兜底: %s", exc)
                inventory = []
            for raw in inventory:
                name = clean_text(raw.get("name", ""), max_len=40)
                kind = clean_text(raw.get("kind", "object"), max_len=16).lower()
                if not name or name in character_names:
                    continue
                visual_id = make_id(kind, name)
                indexes = [int(value) for value in raw.get("scene_indexes", [])]
                assets.setdefault(
                    visual_id,
                    VisualAsset(
                        asset_id=visual_id,
                        kind=kind,
                        name=name,
                        description=clean_text(raw.get("description", ""), max_len=140),
                        scene_indexes=indexes,
                    ),
                )
                for index in indexes:
                    scene = next((item for item in self.scenes if item.index == index), None)
                    if scene and visual_id not in scene.required_asset_ids:
                        scene.required_asset_ids.append(visual_id)
            self.pcfg.visual_asset_inventory_ready = True

        for scene in self.scenes:
            setting = clean_text(scene.setting_description or scene.title, max_len=120)
            time_match = re.search(
                r"时间[：:]\s*([^\s，,。；;]+)",
                clean_text(scene.continuity_notes, max_len=160),
            )
            time_state = f"时间：{time_match.group(1)}" if time_match else ""
            setting = clean_text("，".join(filter(None, [setting, time_state])), max_len=140)
            if setting:
                scene.setting_asset_id = make_id("setting", setting)
                assets.setdefault(
                    scene.setting_asset_id,
                    VisualAsset(asset_id=scene.setting_asset_id, kind="setting", name=setting),
                )
            scene.required_asset_ids = list(scene.required_asset_ids or [])
            for raw_name in scene.required_elements or []:
                name = clean_text(raw_name, max_len=40)
                if (
                    not name or name == "人类" or name.startswith("只用")
                    or name in character_names
                ):
                    continue
                kind = "group" if name in ("甲兵", "士兵") else "object"
                visual_id = make_id(kind, name)
                if visual_id not in scene.required_asset_ids:
                    scene.required_asset_ids.append(visual_id)
                assets.setdefault(
                    visual_id,
                    VisualAsset(asset_id=visual_id, kind=kind, name=name),
                )

        # 全剧资产总表独立于分镜切片，避免仅出现一次的道具在对白拆分时丢失。
        for name in script_breakdown.extract_script_visual_elements(self.pcfg.script_text):
            if name in character_names:
                continue
            kind = "group" if name in ("甲兵", "士兵") else "object"
            visual_id = make_id(kind, name)
            assets.setdefault(
                visual_id,
                VisualAsset(asset_id=visual_id, kind=kind, name=name),
            )

        self.pcfg.visual_assets = [item.to_dict() for item in assets.values()]
        asset_dir = os.path.join(workdir.ensure_job_dir(self.pcfg.job_id), "visual_assets")
        os.makedirs(asset_dir, exist_ok=True)
        pending = [
            item for item in assets.values()
            if not (
                item.reference_url and item.reference_path
                and os.path.exists(item.reference_path) and item.quality_validated
            )
        ]

        def generate_asset(item: VisualAsset):
            item.description = self._sanitize_asset_description(item.description)
            item.prompt = self._build_visual_asset_prompt(item)
            attempt = 0
            rejection = ""
            while not self._cancelled():
                try:
                    agnes = registry.build_provider("agnes", self.cfg)
                    label = "场景" if item.kind == "setting" else "物品"
                    self.stage_changed.emit(f"正在生成{label}定妆图：{item.name}...")
                    if (
                        item.reference_url and item.reference_path
                        and os.path.exists(item.reference_path) and not item.quality_validated
                    ):
                        valid, problem = self._validate_visual_asset(agnes, item, item.reference_path)
                        if valid:
                            item.quality_validated = True
                            return
                        rejection = problem
                        os.remove(item.reference_path)
                        item.reference_url = ""
                        item.reference_path = ""
                    request_prompt = item.prompt
                    if rejection:
                        request_prompt = clean_text(
                            f"{item.prompt} 上一次图片不合格：{rejection}。必须修正。",
                            max_len=260,
                        )
                    result = agnes.generate_image(
                        request_prompt,
                        size="1K",
                        ratio="16:9" if item.kind in ("setting", "group") else "4:3",
                        prefer_url=True,
                        timeout=360,
                    )
                    url = result.get("url")
                    if not url:
                        raise PipelineError("视觉资产定妆图未返回URL")
                    path = os.path.join(asset_dir, f"{item.asset_id}.png")
                    self._download(url, path)
                    valid, problem = self._validate_visual_asset(agnes, item, path)
                    if not valid:
                        rejection = problem
                        os.remove(path)
                        raise PipelineError(f"视觉资产质量验收失败：{problem}")
                    item.reference_url = url
                    item.reference_path = path
                    item.quality_validated = True
                    return
                except Exception as exc:
                    attempt += 1
                    wait = min(60, RETRY_BACKOFF[min(attempt, len(RETRY_BACKOFF) - 1)])
                    self._logger.warning(
                        "视觉资产%s生成失败，第%s次重试: %s", item.name, attempt, exc
                    )
                    detail = clean_text(str(exc), max_len=120)
                    self.stage_changed.emit(
                        f"视觉资产生成失败：{item.name}；{detail}；{wait}s 后继续重试"
                    )
                    if not self._wait_or_cancel(wait):
                        return

        workers = max(1, min(int(self.pcfg.max_concurrent_scenes or 1), 4, len(pending) or 1))
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="visual-asset") as executor:
            futures = [executor.submit(generate_asset, item) for item in pending]
            for future in as_completed(futures):
                future.result()
                if self._cancelled():
                    return

        self.pcfg.visual_assets = [item.to_dict() for item in assets.values()]
        self._save_manifest()

    @staticmethod
    def _sanitize_asset_description(description: str) -> str:
        value = clean_text(description, max_len=140)
        clauses = re.split(r"[，,。；;]", value)
        relation_words = ("手持", "持有", "随身携带", "歪戴", "戴在", "手中", "拿着", "使用")
        return clean_text(
            "，".join(clause for clause in clauses if not any(word in clause for word in relation_words)),
            max_len=100,
        )

    def _story_period_context(self) -> str:
        script = clean_text(self.pcfg.script_text, max_len=8000)
        if any(word in script for word in ("皇帝", "皇上", "丞相", "龙袍", "金銮殿", "养心殿")):
            return "中国明代历史实景，传统中式宫廷建筑与器物"
        return "符合原剧本时代与地域的真实环境"

    def _build_visual_asset_prompt(self, item: VisualAsset) -> str:
        description = f"，{item.description}" if item.description else ""
        period = self._story_period_context()
        if item.kind == "setting":
            return clean_text(
                f"真实电影摄影，{period}。{item.name}。无人空景广角定妆照，真实建筑材质和自然光照。"
                "画面中零人物、零人形、零雕塑主体；不是插画、卡通、动画或3D渲染；无招牌、无文字、无水印。",
                max_len=260,
            )
        if item.kind == "group":
            return clean_text(
                f"真实电影摄影，{period}。{item.name}{description}。固定服装、盔甲、兵器和人数范围，"
                "全身群像，中性无字背景；不是插画、卡通或3D渲染；无文字、无水印。",
                max_len=240,
            )
        subject = "写实生物" if item.kind == "creature" else "写实影视载具" if item.kind == "vehicle" else "写实影视道具"
        return clean_text(
            f"真实产品摄影，{period}。{subject}：{item.name}{description}。只出现一个完整主体，独自放在摄影台中央，"
            "固定材质、颜色和造型。绝对没有人物、手、手臂或身体部位。表面为空白，无汉字、字母、数字、符号、乱码、标签或水印。"
            "不是插画、卡通、动画或3D渲染。",
            max_len=280,
        )

    def _validate_visual_asset(self, agnes, item: VisualAsset, image_path: str) -> tuple[bool, str]:
        if not hasattr(agnes, "build_vision_content") or not hasattr(agnes, "chat"):
            return True, ""
        if item.kind == "setting":
            requirements = "必须是无人、无人物形象的真实摄影环境，建筑时代地域必须匹配描述"
        elif item.kind == "group":
            requirements = "必须是符合描述的真人群像，肢体结构正常、服装时代正确，无可读或乱码文字"
        elif item.kind == "creature":
            requirements = "必须是单一物种主体，无额外人物或错误生物，无可读或乱码文字"
        else:
            requirements = "必须无手、无手臂、无人物、无可读或乱码文字，并且主体数量与描述一致"
        content = agnes.build_vision_content(
            f"检查这张{item.kind}资产图是否合格。资产：{item.name}。要求：{requirements}；"
            "不得是卡通、插画、3D渲染。只输出JSON："
            '{"valid":true,"problems":[]}',
            [image_path],
        )
        result = agnes.chat(
            [
                {"role": "system", "content": "你是严格的影视资产质量检查员，只按可见画面判定。"},
                {"role": "user", "content": content},
            ],
            timeout=180,
        )
        payload = json.loads(script_breakdown._extract_json(result.content))
        problems = [clean_text(value, max_len=80) for value in payload.get("problems", []) if value]
        return bool(payload.get("valid", False)), "；".join(problems) or "不符合资产图硬约束"

    def _enrich_production_plan(self):
        if not self.pcfg.enable_multimodal_planning or self.pcfg.production_bible:
            return
        self.stage_changed.emit("正在使用 Agnes 多模态理解生成全片制作规划...")
        try:
            self.pcfg.production_bible = script_breakdown.enrich_production_plan(
                self.cfg,
                self.pcfg.script_text,
                self._character_profiles(),
                self.scenes,
            )
            self._save_manifest()
        except (PipelineError, ProviderError) as exc:
            self._logger.warning(f"多模态制作规划失败，继续使用确定性结构：{exc}")
            self.stage_changed.emit("多模态规划暂时不可用，已使用本地结构继续生成。")

    def _prepare_keyframes_in_order(self):
        """Generate image-conditioned keyframes in story order before video fan-out."""
        agnes = registry.build_provider("agnes", self.cfg)
        ordered = sorted(self.scenes, key=lambda item: item.index)
        for position, scene in enumerate(ordered, start=1):
            if self._cancelled():
                return
            if scene.keyframe_url and scene.keyframe_path and os.path.exists(scene.keyframe_path):
                continue
            attempt = 0
            while not self._cancelled():
                try:
                    self.stage_changed.emit(
                        f"正在按顺序生成图生图关键帧（{position}/{len(ordered)}）..."
                    )
                    self._ensure_scene_keyframe(
                        agnes,
                        scene,
                        workdir.scene_dir(self.pcfg.job_id, scene.index),
                    )
                    break
                except (PipelineError, ProviderError) as exc:
                    if not self._is_retryable_error(exc):
                        raise PipelineError(
                            f"场景 {scene.index + 1} 关键帧请求不可重试：{exc}"
                        ) from exc
                    attempt += 1
                    wait = min(60, RETRY_BACKOFF[min(attempt, len(RETRY_BACKOFF) - 1)])
                    self._logger.warning(
                        "场景%s关键帧图生图失败，第%s次重试: %s",
                        scene.index,
                        attempt,
                        exc,
                    )
                    self.scene_status_changed.emit(
                        scene.index,
                        "running",
                        f"⚠️ 关键帧失败：{clean_text(str(exc), max_len=100)}；{wait}s 后重试",
                    )
                    if not self._wait_or_cancel(wait):
                        return

    def _build_scene_keyframe_prompt(self, scene: Scene) -> str:
        shot = clean_text(scene.keyframe_prompt or scene.scene_description or "", max_len=180)
        if not shot:
            return ""
        parts = [shot]
        setting = clean_text(scene.setting_description, max_len=100)
        if setting:
            parts.append(f"地点：{setting}")
        char_map = self._character_map()
        character_parts = []
        for character_id in (scene.required_character_ids or [])[:3]:
            profile = char_map.get(character_id)
            if profile:
                character_parts.append(
                    clean_text(
                        f"{profile.name}：{profile.appearance}，穿{profile.outfit}",
                        max_len=120,
                    )
                )
        if character_parts:
            parts.append(f"人物：{'；'.join(character_parts)}")
        if scene.visual_state:
            parts.append(f"状态：{clean_text(scene.visual_state, max_len=140)}")
        if scene.camera_plan:
            parts.append(f"构图：{clean_text(scene.camera_plan, max_len=100)}")
        if scene.required_elements:
            parts.append(f"必须出现：{'、'.join(scene.required_elements[:4])}")
        if scene.forbidden_elements:
            parts.append(f"不要出现：{'、'.join(scene.forbidden_elements[:4])}")
        parts.append("写实电影画面，无文字水印，保持参考图人物身份、脸型、发型和服装")
        return "。".join(part for part in parts if part) + "。"

    def _build_scene_video_prompt(self, scene: Scene) -> str:
        setting = clean_text(scene.setting_description, max_len=160)
        chars = self._scene_character_context(scene)
        dialogue = self._scene_dialogue_context(scene)
        continuity = clean_text(scene.continuity_notes, max_len=160)
        extra = []
        story_bible = self._story_bible_context()
        previous = self._previous_scene_context(scene)
        if story_bible:
            extra.append(f"全片角色设定集：{story_bible}。")
        if previous:
            extra.append(f"承接上一镜头：{previous}。")
        if setting:
            extra.append(f"场景环境：{setting}。")
        if chars:
            extra.append(f"出镜角色：{chars}。")
        if continuity:
            extra.append(f"连续性要求：{continuity}。")
        if scene.visual_state:
            extra.append(f"本镜头状态锁定：{clean_text(scene.visual_state, max_len=260)}。")
        if scene.camera_plan:
            extra.append(f"摄影规划：{clean_text(scene.camera_plan, max_len=180)}。")
        if scene.transition_plan:
            extra.append(f"转场衔接：{clean_text(scene.transition_plan, max_len=180)}。")
        if dialogue:
            extra.append(f"本场对白上下文：{dialogue}。")
        extra.append(self._scene_constraint_text(scene, for_video=True))
        return f"{clean_text(scene.scene_description, max_len=260)}。{''.join(extra)}"

    def _story_bible_context(self) -> str:
        entries = []
        production_bible = self.pcfg.production_bible or {}
        global_style = clean_text(production_bible.get("global_style", ""), max_len=320)
        continuity_rules = production_bible.get("continuity_rules") or []
        if global_style:
            entries.append(f"全局影像风格[{global_style}]")
        if continuity_rules:
            entries.append(f"全局连续性规则[{'；'.join(continuity_rules[:6])}]")
        for profile in self._character_profiles()[:8]:
            entries.append(
                f"{profile.character_id}={profile.name}，{profile.species}，"
                f"固定面容外观[{profile.appearance}]，基础服装配饰[{profile.outfit}]，"
                f"固定音色[{profile.voice_id or self._voice_from_profile(profile)}]"
            )
        return clean_text("；".join(entries), max_len=1400)

    def _previous_scene_context(self, scene: Scene) -> str:
        previous = next((item for item in self.scenes if item.index == scene.index - 1), None)
        if not previous:
            return ""
        return clean_text(
            f"{previous.title}；地点[{previous.setting_description}]；"
            f"人物[{','.join(previous.required_character_ids)}]；状态[{previous.continuity_notes}]",
            max_len=360,
        )

    def _wants_narration_text(self) -> bool:
        return self.pcfg.narration_mode != NARRATION_MODE_NONE

    def _wants_voice(self) -> bool:
        return self.pcfg.narration_mode == NARRATION_MODE_VOICE

    def _wants_subtitles(self) -> bool:
        return self.pcfg.narration_mode in (NARRATION_MODE_SUBTITLES, NARRATION_MODE_VOICE)

    def _has_character_reference(self) -> bool:
        return bool(
            self.pcfg.character_reference_path
            and os.path.exists(self.pcfg.character_reference_path)
        )

    def _character_profiles(self) -> list[CharacterProfile]:
        return [CharacterProfile.from_dict(item) for item in (self.pcfg.character_profiles or [])]

    def _visual_assets(self) -> list[VisualAsset]:
        return [VisualAsset.from_dict(item) for item in (self.pcfg.visual_assets or [])]

    def _visual_asset_map(self) -> dict[str, VisualAsset]:
        return {item.asset_id: item for item in self._visual_assets()}

    def _character_map(self) -> dict[str, CharacterProfile]:
        return {item.character_id: item for item in self._character_profiles()}

    def _scene_reference_profiles(self, scene: Scene) -> list[CharacterProfile]:
        char_map = self._character_map()
        profiles = [
            char_map[char_id]
            for char_id in (scene.required_character_ids or [])
            if char_id in char_map and char_map[char_id].reference_path
            and os.path.exists(char_map[char_id].reference_path)
        ]
        if not profiles:
            primary = self._primary_character()
            if primary and primary.reference_path and os.path.exists(primary.reference_path):
                profiles = [primary]
        return profiles[:3]

    def _primary_character(self) -> CharacterProfile | None:
        profiles = self._character_profiles()
        for item in profiles:
            if item.is_primary:
                return item
        return profiles[0] if profiles else None

    def _character_profiles_payload(self) -> list[dict]:
        payload = []
        for profile in self._character_profiles():
            payload.append(
                {
                    **profile.to_dict(),
                    "suggested_voice": self._voice_from_profile(profile),
                    "selected_voice": (
                        self.pcfg.character_voice_overrides.get(profile.character_id)
                        or self._voice_from_profile(profile)
                    ),
                }
            )
        return payload

    def _character_is_nonhuman(self) -> bool:
        primary = self._primary_character()
        ref = clean_text((primary.species if primary else "") or self.pcfg.character_reference_prompt, max_len=260)
        return any(k in ref for k in NONHUMAN_KEYWORDS)

    def _scene_forbids_humans(self, scene: Scene) -> bool:
        return any("人类" in item or "人" == item for item in (scene.forbidden_elements or []))

    def _scene_requires_dragon(self, scene: Scene) -> bool:
        return any("龙" in item for item in (scene.required_elements or []))

    def _scene_has_multiple_required_subjects(self, scene: Scene) -> bool:
        return len(scene.required_elements or []) >= 2

    def _scene_character_context(self, scene: Scene) -> str:
        char_map = self._character_map()
        parts = []
        for char_id in scene.required_character_ids[:4]:
            profile = char_map.get(char_id)
            if not profile:
                continue
            voice = profile.voice_id or self._voice_from_profile(profile)
            parts.append(
                f"{profile.name or char_id}({profile.species})：固定面容/外观 {profile.appearance}；"
                f"固定服装/配饰 {profile.outfit or '沿用角色定妆图，除非本场剧本明确换装'}；"
                f"性格/状态 {profile.personality or profile.voice_hint}；固定音色 {voice}"
            )
        return "；".join(parts)

    def _scene_dialogue_context(self, scene: Scene) -> str:
        if not scene.dialogue_lines:
            return ""
        snippets = []
        for line in scene.dialogue_lines[:4]:
            speaker = line.character_name or line.character_id or "角色"
            tone = f" [{line.tone}]" if line.tone else ""
            snippets.append(f"{speaker}{tone}：{line.text}")
        return "；".join(snippets)

    def _scene_handheld_props(self, scene: Scene) -> list[str]:
        blob = " ".join(
            part for part in [
                scene.setting_description,
                scene.scene_description,
                scene.keyframe_prompt,
                " ".join(scene.required_elements or []),
            ] if part
        )
        props = []
        for item in HANDHELD_PROPS:
            if item in blob and item not in props:
                props.append(item)
        return props

    def _scene_needs_motion_boost(self, scene: Scene) -> bool:
        blob = " ".join(
            part for part in [
                scene.title,
                scene.scene_description,
                scene.keyframe_prompt,
                scene.narration,
            ] if part
        )
        return any(token in blob for token in ("尾声", "结尾", "定格", "看着", "坐着", "刷手机", "回头", "站着"))

    def _primary_character_in_scene(self, scene: Scene) -> bool:
        primary = self._primary_character()
        return bool(primary and primary.character_id in (scene.required_character_ids or []))

    def _promote_scene_keyframe_as_reference(self, scene: Scene):
        if self._has_character_reference():
            return
        if not scene.keyframe_path or not os.path.exists(scene.keyframe_path):
            return
        if not self._primary_character_in_scene(scene):
            return
        target = os.path.join(workdir.ensure_job_dir(self.pcfg.job_id), "character_ref.png")
        try:
            shutil.copyfile(scene.keyframe_path, target)
            self.pcfg.character_reference_path = target
            self.pcfg.character_reference_url = scene.keyframe_url or self.pcfg.character_reference_url
            self._save_manifest()
            self._logger.info("已使用首个成功场景关键帧回填全片角色参考图。")
        except OSError as exc:
            self._logger.warning(f"回填全片角色参考图失败: {exc}")

    def _scene_is_single_subject(self, scene: Scene) -> bool:
        return len(scene.required_character_ids or []) <= 1

    def _scene_constraint_text(self, scene: Scene, for_video: bool) -> str:
        parts = []
        if self._character_is_nonhuman():
            parts.append("主角必须保持原始物种和外观，不得变成人类、兽人或拟人化角色。")
        else:
            parts.append("主角必须保持与参考图一致的身份和外观，不要换脸或换人。")
            parts.append("整体风格必须是写实真人影视质感，东方面孔、真实皮肤、真实服装纹理，严禁卡通、二次元、插画、玩偶脸或Q版。")
        if self._scene_is_single_subject(scene):
            parts.append("本场以单个角色为核心，画面里只保留这一位主要人物，必须是正常一张脸、一个头、两只手、双臂比例正确。")
        if scene.required_elements:
            parts.append(f"本场景必须清晰出现：{'、'.join(scene.required_elements[:4])}。")
        if scene.forbidden_elements:
            parts.append(f"本场景不得出现：{'、'.join(scene.forbidden_elements[:4])}。")
        if self._scene_requires_dragon(scene):
            parts.append("龙必须作为清晰可见的实体出现在画面中，不要只用金光、光束、阴影或抽象特效代替。")
        if self._scene_forbids_humans(scene):
            parts.append("除非剧本明确提到，否则画面中不要出现任何人类。")
        props = self._scene_handheld_props(scene)
        if len(props) >= 2:
            parts.append(
                f"手持小道具包含：{'、'.join(props[:4])}。除一个核心道具外，其余道具放在桌案、手边或画面中，"
                "不要让角色同时完成拿手机、吃包子、端豆浆、换手等复杂手部动作。"
            )
            parts.append("若出现包子和豆浆，只允许一个动作道具进入嘴边，另一件道具保持静止放置，绝不能出现吃豆浆、喝包子等错误交互。")
        if for_video:
            parts.append("只做自然动作、表情和镜头运动，不要突然切换成别的物种或别的人物。")
            parts.append("画面内部禁止生成任何英文、字母、乱码、招牌文字或模型自带字幕；中文字幕由后期单独烧录。")
            if scene.dialogue_lines:
                parts.append(
                    "本模型没有音频驱动口型能力：对白期间采用侧脸、背面、过肩或反应镜头，"
                    "嘴部保持自然闭合或仅有极轻微动作，严禁清晰可见却与台词不同步的连续张嘴说话。"
                )
            if self._scene_needs_motion_boost(scene):
                parts.append("画面必须保持持续微动：人物轻微呼吸、眨眼、衣袖或发丝摆动，镜头缓慢推进或轻微移动，避免最后一段完全静止。")
        return "".join(parts)

    def _build_scene_negative_prompt(self, scene: Scene) -> str:
        parts = [DEFAULT_VIDEO_NEGATIVE_PROMPT]
        if self._scene_is_single_subject(scene):
            parts.append("extra person, crowd, duplicate body, duplicate face, extra arm, extra hand")
        if self._scene_forbids_humans(scene):
            parts.append(HUMAN_FORBID_PROMPT)
        if self._scene_requires_dragon(scene):
            parts.append("missing dragon, hidden dragon, only light beam, only glow")
        if len(self._scene_handheld_props(scene)) >= 2:
            parts.append(
                "wrong prop interaction, prop swap, eating wrong object, drinking wrong object, "
                "wrong hand object, hand-object mismatch, impossible hand pose"
            )
        return ", ".join(parts)

    def _build_video_request_payload(self, scene: Scene, attempt: int = 0) -> dict:
        ref_url = scene.keyframe_url or self.pcfg.character_reference_url
        return {
            "model": "agnes-video-v2.0",
            "prompt": self._build_scene_video_prompt(scene),
            "width": self.pcfg.width,
            "height": self.pcfg.height,
            "num_frames": scene.num_frames,
            "frame_rate": self.pcfg.frame_rate,
            "image": ref_url,
            "mode": "ti2vid" if ref_url else None,
            "seed": (
                sum(ord(ch) for ch in f"{self.pcfg.job_id}:{scene.index}") + attempt
            ) % 2147483647,
            "negative_prompt": self._build_scene_negative_prompt(scene),
        }

    def _build_scene_subtitle_lines(self, scene: Scene) -> list[str]:
        lines = []
        if scene.narration:
            narration = strict_chinese_caption(scene.narration)
            if narration:
                lines.append(narration)
        for item in scene.dialogue_lines:
            speaker = item.character_name or item.character_id or "角色"
            line = strict_chinese_caption(f"{speaker}：{item.text}", max_len=120)
            if line:
                lines.append(line)
        return lines

    def _voice_choice_pools(self):
        all_voices = [voice for voice, _ in tts_engine.VOICE_CHOICES]
        female = [voice for voice, label in tts_engine.VOICE_CHOICES if "女声" in label]
        male = [voice for voice, label in tts_engine.VOICE_CHOICES if "男声" in label]
        return all_voices, female or all_voices, male or all_voices

    def _voice_from_profile(self, profile: CharacterProfile | None) -> str:
        narrator_voice = self.pcfg.voice or tts_engine.DEFAULT_VOICE
        all_voices, female_voices, male_voices = self._voice_choice_pools()
        if not profile:
            return narrator_voice
        override = clean_text(
            (self.pcfg.character_voice_overrides or {}).get(profile.character_id, ""),
            max_len=60,
        )
        if override:
            return override
        hint_text = " ".join(
            part for part in [profile.role, profile.species, profile.personality, profile.voice_hint] if part
        )
        if any(token in hint_text for token in FEMALE_VOICE_HINTS):
            pool = female_voices
        elif any(token in hint_text for token in MALE_VOICE_HINTS):
            pool = male_voices
        else:
            pool = [voice for voice in all_voices if voice != narrator_voice] or all_voices
        stable_key = profile.character_id or profile.name or profile.role or "voice"
        return pool[sum(ord(ch) for ch in stable_key) % len(pool)]

    def _build_scene_voice_segments(self, scene: Scene) -> list[dict]:
        segments = []
        if scene.narration:
            segments.append(
                {
                    "kind": "narration",
                    "speech_text": scene.narration,
                    "subtitle_text": scene.narration,
                    "voice": self.pcfg.voice or tts_engine.DEFAULT_VOICE,
                }
            )
        char_map = self._character_map()
        for line in scene.dialogue_lines:
            profile = char_map.get(line.character_id)
            speaker = line.character_name or (profile.name if profile else "") or line.character_id or "角色"
            segments.append(
                {
                    "kind": "dialogue",
                    "speech_text": line.text,
                    "subtitle_text": clean_text(f"{speaker}：{line.text}", max_len=120),
                    "voice": self._voice_from_profile(profile),
                }
            )
        return segments

    def _build_scene_keyframe_request(self, scene: Scene, force_text_only: bool = False):
        # 兼容旧参数，但重试不再降级成文生图。
        reference_profiles = self._scene_reference_profiles(scene)
        asset_map = self._visual_asset_map()
        reference_paths = []
        setting_asset = asset_map.get(scene.setting_asset_id)
        if (
            setting_asset and setting_asset.reference_path
            and os.path.exists(setting_asset.reference_path)
        ):
            reference_paths.append(setting_asset.reference_url or setting_asset.reference_path)
        reference_paths.extend(
            profile.reference_url or profile.reference_path for profile in reference_profiles
        )
        for asset_id in scene.required_asset_ids:
            asset = asset_map.get(asset_id)
            if asset and asset.reference_path and os.path.exists(asset.reference_path):
                reference_paths.append(asset.reference_url or asset.reference_path)
        previous = next((item for item in self.scenes if item.index == scene.index - 1), None)
        same_setting = bool(
            previous
            and (
                (
                    previous.setting_asset_id and scene.setting_asset_id
                    and previous.setting_asset_id == scene.setting_asset_id
                )
                or (
                    not previous.setting_asset_id and not scene.setting_asset_id
                    and clean_text(previous.setting_description, max_len=100)
                    == clean_text(scene.setting_description, max_len=100)
                )
            )
        )
        if same_setting and previous.keyframe_path and os.path.exists(previous.keyframe_path):
            reference_paths.insert(0, previous.keyframe_url or previous.keyframe_path)
        reference_paths = list(dict.fromkeys(reference_paths))[:6]
        if not reference_paths:
            raise PipelineError("关键帧缺少已定妆的场景、人物或道具参考图，已停止，避免退化为文生图。")
        prompt = self._build_scene_keyframe_prompt(scene)
        return prompt, reference_paths

    def _ensure_scene_keyframe(self, agnes, scene: Scene, sdir: str, force_text_only: bool = False):
        if scene.keyframe_url and os.path.exists(scene.keyframe_path):
            return
        prompt, reference_paths = self._build_scene_keyframe_request(scene, force_text_only)
        if not prompt:
            return
        self.scene_status_changed.emit(scene.index, "running", "🔄 生成场景关键帧")
        img = agnes.generate_image(
            prompt,
            size="1K",
            ratio="16:9",
            image_paths=reference_paths,
            prefer_url=True,
            timeout=360,
        )
        url = img.get("url")
        if not url:
            raise PipelineError("场景关键帧未返回URL")
        keyframe_path = os.path.join(sdir, "scene_keyframe.png")
        self._download(url, keyframe_path)
        scene.keyframe_url = url
        scene.keyframe_path = keyframe_path
        self._promote_scene_keyframe_as_reference(scene)
        self._save_manifest()

    def _apply_retry_strategy(self, scene: Scene, attempt: int, base_num_frames: int):
        factor = SCENE_FRAME_RETRY_FACTORS[min(attempt, len(SCENE_FRAME_RETRY_FACTORS) - 1)]
        adapted_frames = max(MIN_RETRY_FRAMES, int(base_num_frames * factor))
        scene.num_frames = max(MIN_RETRY_FRAMES, min(base_num_frames, adapted_frames))
        scene.duration_hint = round(scene.num_frames / max(1, self.pcfg.frame_rate), 2)
        if attempt > 0:
            scene.raw_video_path = ""
            scene.normalized_path = ""
            scene.actual_duration = 0.0

    # ===== 单场景处理(持续重试，直到成功或用户取消) =====

    def _process_scene(self, scene: Scene):
        sdir = workdir.scene_dir(self.pcfg.job_id, scene.index)
        base_num_frames = max(scene.num_frames or 0, MIN_RETRY_FRAMES)

        attempt = 0
        while not self._cancelled():
            self._apply_retry_strategy(scene, attempt, base_num_frames)
            if attempt > 0:
                wait = min(60, RETRY_BACKOFF[min(attempt, len(RETRY_BACKOFF) - 1)])
                self.scene_status_changed.emit(
                    scene.index, "running", f"⚠️ 第{attempt}次失败，{wait}s后继续重试"
                )
                if not self._wait_or_cancel(wait):
                    return
            try:
                agnes = registry.build_provider("agnes", self.cfg)
                self._do_scene_steps(
                    agnes,
                    scene,
                    sdir,
                    attempt=attempt,
                    force_text_only_keyframe=False,
                )
                scene.status = "done"
                scene.error = ""
                self.scene_status_changed.emit(scene.index, "done", "✅ 完成")
                return
            except (PipelineError, ProviderError) as e:
                scene.error = str(e)
                self._logger.warning(f"场景{scene.index}处理失败(尝试{attempt+1}): {e}")
                if not self._is_retryable_error(e):
                    raise PipelineError(
                        f"场景 {scene.index + 1} 请求不可重试：{e}"
                    ) from e
            except Exception as e:
                scene.error = f"{e}"
                self._logger.exception(f"场景{scene.index}处理异常(尝试{attempt+1})")
            attempt += 1
            scene.status = "running"
            self._save_manifest()

    def _do_scene_steps(self, agnes, scene: Scene, sdir: str, attempt: int = 0,
                        force_text_only_keyframe: bool = False):
        rate = self.pcfg.frame_rate

        # a) 旁白 TTS(拿真实时长反推视频帧数)
        scene.narration = clean_text(scene.narration, max_len=500) if self._wants_narration_text() else ""
        voice_segments = self._build_scene_voice_segments(scene)
        if voice_segments and (not scene.narration_audio_path or not os.path.exists(scene.narration_audio_path)):
            if self._wants_voice():
                self.scene_status_changed.emit(scene.index, "running", "🔄 生成旁白与角色配音")
                mp3 = os.path.join(sdir, "narration.mp3")
                srt = os.path.join(sdir, "narration.srt")
                seg_dir = os.path.join(sdir, "tts_segments")
                tts_engine.synthesize_segments(voice_segments, self.pcfg.voice, mp3, srt, seg_dir)
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
        self._ensure_scene_keyframe(agnes, scene, sdir, force_text_only=force_text_only_keyframe)

        if self._cancelled():
            raise PipelineError("已取消")

        if self.pcfg.prompt_debug_mode:
            request_payload = self._build_video_request_payload(scene, attempt)
            prompt_audit.record(
                "video_prompt",
                request_payload,
                label=f"scene_{scene.index:02d}_debug_not_submitted",
            )
            self._write_scene_prompt_files(scene, request_payload)
            self._write_debug_subtitles(scene, sdir)
            self.scene_status_changed.emit(scene.index, "done", "✅ 素材与 Prompt 已导出")
            return

        # c) 场景视频: 优先用场景关键帧作 image-to-video 首帧，并在 prompt 中重复身份锁定。
        if not scene.raw_video_path or not os.path.exists(scene.raw_video_path):
            self.scene_status_changed.emit(scene.index, "running", "🔄 提交视频生成")
            request_payload = self._build_video_request_payload(scene, attempt)
            video_id = agnes.create_video_task(
                request_payload["prompt"],
                width=request_payload["width"], height=request_payload["height"],
                num_frames=request_payload["num_frames"], frame_rate=request_payload["frame_rate"],
                image_url=request_payload["image"],
                mode=request_payload["mode"],
                seed=request_payload["seed"],
                negative_prompt=request_payload["negative_prompt"],
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
        subtitle_lines = self._build_scene_subtitle_lines(scene)
        if (
            self._wants_subtitles()
            and subtitle_lines
            and (
                self.pcfg.narration_mode == NARRATION_MODE_SUBTITLES
                or not scene.narration_srt_path
                or not os.path.exists(scene.narration_srt_path)
            )
        ):
            srt = os.path.join(sdir, "narration.srt")
            video_ops.write_scripted_srt(subtitle_lines, scene.actual_duration or scene.duration_hint, srt)
            scene.narration_srt_path = srt

    def _write_debug_subtitles(self, scene: Scene, sdir: str):
        subtitle_lines = self._build_scene_subtitle_lines(scene)
        if not self._wants_subtitles() or not subtitle_lines:
            return
        if scene.narration_srt_path and os.path.exists(scene.narration_srt_path):
            return
        srt = os.path.join(sdir, "narration.srt")
        video_ops.write_scripted_srt(subtitle_lines, scene.duration_hint, srt)
        scene.narration_srt_path = srt

    def _write_scene_prompt_files(self, scene: Scene, video_payload: dict):
        output_dir = self.pcfg.prompt_output_dir or workdir.prompt_dir(self.pcfg.job_id)
        keyframe_prompt, reference_paths = self._build_scene_keyframe_request(scene)
        snapshot = {
            "scene": scene.to_dict(),
            "keyframe_request": {
                "model": "agnes-image-2.1-flash",
                "prompt": keyframe_prompt,
                "size": "1K",
                "ratio": "16:9",
                "reference_image_paths": reference_paths,
            },
            "video_request_not_submitted": video_payload,
        }
        stem = f"scene_{scene.index:02d}"
        with open(os.path.join(output_dir, f"{stem}_requests.json"), "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle, ensure_ascii=False, indent=2)
        readable = (
            f"分镜 {scene.index + 1}: {scene.title}\n\n"
            f"===== 关键帧 Prompt =====\n{keyframe_prompt}\n\n"
            f"===== 视频 Prompt（未提交）=====\n{video_payload['prompt']}\n\n"
            f"===== Negative Prompt =====\n{video_payload['negative_prompt']}\n\n"
            f"===== 视频参数 =====\n"
            f"model={video_payload['model']}\n"
            f"size={video_payload['width']}x{video_payload['height']}\n"
            f"num_frames={video_payload['num_frames']}\n"
            f"frame_rate={video_payload['frame_rate']}\n"
            f"mode={video_payload['mode']}\nseed={video_payload['seed']}\n"
            f"image={video_payload['image']}\n"
        )
        with open(os.path.join(output_dir, f"{stem}_prompts.txt"), "w", encoding="utf-8") as handle:
            handle.write(readable)
        self._logger.info("分镜 %s 视频 Prompt（调试未提交）:\n%s", scene.index + 1, video_payload["prompt"])

    def _write_prompt_summary(self) -> str:
        output_dir = self.pcfg.prompt_output_dir or workdir.prompt_dir(self.pcfg.job_id)
        profiles = self._character_profiles()
        with open(os.path.join(output_dir, "character_prompts.json"), "w", encoding="utf-8") as handle:
            json.dump([profile.to_dict() for profile in profiles], handle, ensure_ascii=False, indent=2)
        with open(os.path.join(output_dir, "production_bible.json"), "w", encoding="utf-8") as handle:
            json.dump(self.pcfg.production_bible, handle, ensure_ascii=False, indent=2)
        with open(os.path.join(output_dir, "scenes.json"), "w", encoding="utf-8") as handle:
            json.dump([scene.to_dict() for scene in self.scenes], handle, ensure_ascii=False, indent=2)

        combined = []
        for scene in sorted(self.scenes, key=lambda item: item.index):
            video_payload = self._build_video_request_payload(scene)
            combined.append(
                f"{'=' * 80}\n分镜 {scene.index + 1}: {scene.title}\n"
                f"{'=' * 80}\n{video_payload['prompt']}\n\n"
                f"Negative Prompt:\n{video_payload['negative_prompt']}\n\n"
            )
        with open(os.path.join(output_dir, "all_video_prompts.txt"), "w", encoding="utf-8") as handle:
            handle.write("".join(combined))
        with open(os.path.join(output_dir, "README.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "AISprite Prompt 调试输出\n\n"
                "本次任务没有调用视频生成接口。\n"
                "all_prompts.jsonl: 按真实调用顺序记录所有模型请求。\n"
                "all_video_prompts.txt: 汇总每个分镜原本将提交的视频 Prompt。\n"
                "scene_XX_prompts.txt: 单个分镜的关键帧、视频和反向 Prompt。\n"
                "scene_XX_requests.json: 单个分镜的完整请求参数。\n"
                "character_prompts.json: 角色定妆 Prompt 与角色上下文。\n"
                "production_bible.json: 多模态制作规划。\n"
            )
        self._save_manifest()
        return output_dir

    # ===== 拼接 =====

    def _assemble(self) -> str:
        job = self.pcfg.job_id
        jdir = workdir.job_dir(job)
        usable_scenes = [s for s in self.scenes if s.normalized_path and os.path.exists(s.normalized_path)]
        clips = [s.normalized_path for s in usable_scenes]
        if not clips:
            raise PipelineError("没有任何可用的场景片段，无法拼接。")

        filelist = os.path.join(jdir, "filelist.txt")
        concat_out = os.path.join(jdir, "concat.mp4")
        video_ops.concat_clips(
            clips, filelist, concat_out, frame_rate=self.pcfg.frame_rate
        )

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

        self._save_manifest()
        return output

    # ===== 工具 =====

    def _download(self, url: str, dest: str, timeout: int = 120):
        resp = requests.get(url, timeout=timeout, stream=True)
        resp.raise_for_status()
        with open(dest, "wb") as f:
            for chunk in resp.iter_content(chunk_size=65536):
                if chunk:
                    f.write(chunk)
