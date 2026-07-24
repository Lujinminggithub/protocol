"""
剧本->长视频流水线的数据模型与通用工具。
"""

from dataclasses import dataclass, field, asdict
from enum import Enum


class PipelineError(Exception):
    """流水线级错误，消息面向用户可读。"""


class Stage(Enum):
    BREAKDOWN = "breakdown"
    CHARACTERS = "characters"
    SCENES = "scenes"
    NARRATION = "narration"
    DIALOGUE = "dialogue"
    KEYFRAME = "keyframe"
    VIDEO = "video"
    MERGE = "merge"
    NORMALIZE = "normalize"
    ASSEMBLE = "assemble"
    DONE = "done"


# Agnes 接口支持到 441 帧；人物镜头主动限制为 193 帧，减少长镜头中的面容漂移。
MAX_NUM_FRAMES = 193

NARRATION_MODE_NONE = "no_narration"
NARRATION_MODE_SUBTITLES = "subtitles_only"
NARRATION_MODE_VOICE = "voice_subtitles"
NARRATION_MODES = {
    NARRATION_MODE_NONE,
    NARRATION_MODE_SUBTITLES,
    NARRATION_MODE_VOICE,
}


def duration_to_num_frames(seconds: float, frame_rate: int = 24) -> int:
    """把期望秒数换算成合法的 num_frames(满足 8n+1 且 <=193)。"""
    raw = round(seconds * frame_rate)
    n = max(1, round((raw - 1) / 8))
    frames = 8 * n + 1
    return min(frames, MAX_NUM_FRAMES)


@dataclass
class CharacterProfile:
    character_id: str
    name: str = ""
    role: str = ""
    species: str = ""
    appearance: str = ""
    outfit: str = ""
    personality: str = ""
    voice_hint: str = ""
    voice_id: str = ""
    reference_prompt: str = ""
    reference_url: str = ""
    reference_path: str = ""
    is_primary: bool = False

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "CharacterProfile":
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in (data or {}).items() if k in known})


@dataclass
class VisualAsset:
    asset_id: str
    kind: str = "object"  # object / setting
    name: str = ""
    description: str = ""
    scene_indexes: list[int] = field(default_factory=list)
    prompt: str = ""
    reference_url: str = ""
    reference_path: str = ""
    quality_validated: bool = False

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "VisualAsset":
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in (data or {}).items() if k in known})


@dataclass
class DialogueLine:
    character_id: str = ""
    character_name: str = ""
    text: str = ""
    tone: str = ""
    scene_purpose: str = ""

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "DialogueLine":
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in (data or {}).items() if k in known})


@dataclass
class Scene:
    index: int
    title: str = ""
    setting_description: str = ""
    scene_description: str = ""  # 视频生成 prompt(镜头运动/动作/氛围)
    keyframe_prompt: str = ""  # 关键帧参考图 prompt(含跨场景一致的角色/背景关键词)
    continuity_notes: str = ""
    visual_state: str = ""  # 本镜头人物/服装/道具状态，由制作规划阶段补全
    camera_plan: str = ""  # 景别、机位、镜头运动
    transition_plan: str = ""  # 与前后镜头的衔接策略
    narration: str = ""  # 旁白文本
    dialogue_lines: list[DialogueLine] = field(default_factory=list)
    required_character_ids: list[str] = field(default_factory=list)
    required_elements: list[str] = field(default_factory=list)  # 本场景必须明确出现的主体/元素
    required_asset_ids: list[str] = field(default_factory=list)
    setting_asset_id: str = ""
    forbidden_elements: list[str] = field(default_factory=list)  # 本场景不得出现的主体/元素
    duration_hint: float = 5.0  # 建议秒数
    num_frames: int = 121  # 映射后的合法帧数(运行期可按TTS真实时长校正)

    # 运行期产物路径(落盘后回填)
    keyframe_path: str = ""
    keyframe_url: str = ""  # Agnes CDN 公开URL，喂给视频生成的 extra_body.image
    narration_audio_path: str = ""
    narration_srt_path: str = ""
    raw_video_path: str = ""
    normalized_path: str = ""  # 统一转码后、可进 concat 的最终片段
    actual_duration: float = 0.0  # normalized 片段的 ffprobe 实测时长(拼字幕偏移用)

    status: str = "pending"  # pending/running/done/failed/skipped
    error: str = ""

    def to_dict(self) -> dict:
        data = asdict(self)
        data["dialogue_lines"] = [line.to_dict() for line in self.dialogue_lines]
        return data

    @classmethod
    def from_dict(cls, d: dict) -> "Scene":
        known = {f.name for f in cls.__dataclass_fields__.values()}
        data = {k: v for k, v in (d or {}).items() if k in known}
        data["dialogue_lines"] = [DialogueLine.from_dict(item) for item in data.get("dialogue_lines", [])]
        return cls(**data)


@dataclass
class PipelineConfig:
    script_text: str = ""
    scene_count_hint: int = 6  # 0 = 让模型自行决定
    voice: str = "zh-CN-XiaoxiaoNeural"
    width: int = 1152
    height: int = 768
    frame_rate: int = 24
    narration_mode: str = NARRATION_MODE_VOICE
    enable_narration: bool = True
    burn_subtitles: bool = True
    max_concurrent_scenes: int = 3
    enable_multimodal_planning: bool = True
    prompt_debug_mode: bool = False
    prompt_output_dir: str = ""
    production_bible: dict = field(default_factory=dict)
    job_id: str = ""
    work_dir: str = ""
    character_profiles: list[dict] = field(default_factory=list)
    visual_assets: list[dict] = field(default_factory=list)
    visual_asset_inventory_ready: bool = False
    character_voice_overrides: dict[str, str] = field(default_factory=dict)
    # 全片共用的角色参考图(锁定人物一致性): 只生成一次，所有场景视频都以它为起始帧
    character_reference_prompt: str = ""
    character_reference_url: str = ""
    character_reference_path: str = ""

    def __post_init__(self):
        if self.narration_mode not in NARRATION_MODES:
            self.narration_mode = NARRATION_MODE_VOICE if self.enable_narration else NARRATION_MODE_NONE
        self.enable_narration = self.narration_mode != NARRATION_MODE_NONE

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict) -> "PipelineConfig":
        d = dict(d or {})
        if "narration_mode" not in d:
            d["narration_mode"] = (
                NARRATION_MODE_VOICE if d.get("enable_narration", True) else NARRATION_MODE_NONE
            )
        known = {f.name for f in cls.__dataclass_fields__.values()}
        cfg = cls(**{k: v for k, v in d.items() if k in known})
        if cfg.narration_mode not in NARRATION_MODES:
            cfg.narration_mode = NARRATION_MODE_VOICE if cfg.enable_narration else NARRATION_MODE_NONE
        cfg.enable_narration = cfg.narration_mode != NARRATION_MODE_NONE
        return cfg
