"""
Structured story extraction for the script-to-video pipeline.

We intentionally split extraction into four passes:
1. character extraction
2. scene extraction
3. narration extraction
4. dialogue extraction

This gives the video generator a stable shared context instead of asking one
model call to do everything at once.
"""

import json
import re
import time
from collections import Counter

from src.providers import registry
from src.providers.base import ProviderError
from src.video_pipeline.base import (
    CharacterProfile,
    DialogueLine,
    Scene,
    PipelineError,
    duration_to_num_frames,
    NARRATION_MODE_NONE,
    NARRATION_MODE_SUBTITLES,
    NARRATION_MODE_VOICE,
)
from src.video_pipeline.text_utils import clean_text, unrepaired_mojibake_score

EXTRACTION_TIMEOUT = 180
EXTRACTION_MAX_ATTEMPTS = 3
EXTRACTION_RETRY_BACKOFF = [0, 3, 8]


CHARACTER_SYSTEM_PROMPT = """
你是视频前期策划中的“角色提取器”。

任务：
从剧本中提取角色与稳定身份信息，尤其要区分：
- 主角是谁
- 主角是不是人类、动物、神话生物或其他非人主体
- 哪些角色是后续场景中必须反复保持一致的

要求：
- 严禁把动物主角或神话生物主角改写成人类。
- 如果剧本主角是狗，就必须明确写出狗的品种、毛色、体型、项圈/服饰等稳定特征。
- 如果剧本有关键配角，如龙、怪物、导师、伙伴，也要提取出来。
- 所有拥有台词的具名说话人都必须进入 characters，绝不能遗漏，例如“青雀（语气）：台词”中的青雀。
- 即使剧本没有完整描述，也必须为每个具名角色补出具体且稳定的 appearance、outfit、personality 和 voice_hint，不得留空。
- outfit 必须单独记录角色的固定服装和配饰；场景明确换装时，以场景描述为准。
- voice_hint 必须描述年龄感、性别感、语速和情绪，供角色固定音色分配使用。
- 输出一段 character_reference，作为全片统一角色定妆图的描述；它必须只描述“主主角”这个单主体。

只输出 JSON，不要输出 JSON 之外的任何文字：
{
  "character_reference": "全片主主角的统一角色设定图描述，30-120字",
  "characters": [
    {
      "character_id": "dog_main",
      "name": "月饼",
      "role": "主角",
      "species": "边境牧羊犬",
      "appearance": "黑白毛色，竖耳，红色项圈",
      "outfit": "红色项圈，全片保持一致",
      "personality": "善良、执着、警觉",
      "voice_hint": "温和、坚定",
      "is_primary": true
    }
  ]
}
"""


SCENE_SYSTEM_PROMPT = """
你是视频前期策划中的“场景提取器”。

任务：
基于完整剧本和角色表，把故事拆成连续场景。每个场景都要明确：
- 发生在哪里
- 主要画面动作是什么
- 首帧静态构图是什么
- 场景连续性要点是什么
- 哪些角色必须出现
- 哪些元素必须出现
- 哪些元素不能出现

要求：
- 场景描述要服务视频生成，不要写抽象文学句子。
- required_character_ids 只填写角色表中的 character_id。
- required_elements 必须列出该镜头全部可见的关键道具、特殊物件和非角色生物，后续会逐项生成固定资产图。
- forbidden_elements 用于严格禁止的干扰元素，例如“人类”“现代汽车”。
- 如果某场景剧情重点是龙出现，required_elements 必须明确包含“金龙”或“龙”。
- 如果主角是动物且剧本没出现人类，forbidden_elements 应包含“人类”。
- keyframe_prompt 要描述“该场景定妆图”，scene_description 要描述“该场景动态视频”。

只输出 JSON：
{
  "scenes": [
    {
      "index": 0,
      "title": "山间奇遇",
      "setting_description": "清晨山谷与草丛小径",
      "scene_description": "动态视频描述",
      "keyframe_prompt": "首帧静态画面描述",
      "continuity_notes": "和上一场景/主角状态的衔接说明",
      "required_character_ids": ["dog_main"],
      "required_elements": ["边境牧羊犬"],
      "forbidden_elements": ["人类"],
      "duration_hint": 6
    }
  ]
}
"""


NARRATION_SYSTEM_PROMPT = """
你是视频前期策划中的“旁白提取器”。

任务：
只为每个场景提取适合配旁白或字幕的 narration。

要求：
- narration 可以为空。
- 如果该场景主要依赖角色对白或纯动作推进，不要强行补旁白。
- narration 要简洁，口语化，适合朗读或显示字幕。
- 每个场景的 narration 不超过 35 个汉字，避免旁白长于视频镜头。

只输出 JSON：
{
  "narrations": [
    {"scene_index": 0, "text": "该场景旁白，可为空字符串"}
  ]
}
"""


DIALOGUE_SYSTEM_PROMPT = """
你是视频前期策划中的“对白提取器”。

任务：
只提取场景中真正需要保留的人物语言/对白，并映射到对应角色。

要求：
- 每句对白都要对应 character_id。
- 没有对白的场景输出空列表。
- 如果剧本只有内心独白、画外音，不要误判成角色对白。
- tone 用简短词描述说话状态，例如“平静”“郑重”“惊讶”。
- scene_purpose 说明这句对白在场景中的作用，例如“提出问题”“回应承诺”。
- 单句对白不超过 35 个汉字；原文较长时拆成连续的多句，文字不得丢失或改写。

只输出 JSON：
{
  "dialogues": [
    {
      "scene_index": 0,
      "lines": [
        {
          "character_id": "dragon",
          "character_name": "金龙",
          "text": "孩子，你想要什么？",
          "tone": "慈祥",
          "scene_purpose": "提出问题"
        }
      ]
    }
  ]
}
"""

PRODUCTION_PLANNER_SYSTEM_PROMPT = """
你是影视生成流水线的多模态制作规划器。输入包含原始剧本、已确认角色表、已拆分镜头，
以及按角色表顺序附带的角色定妆图。你只能补充可执行的视觉信息，不能改写事实底座。

硬约束：
- 不得新增、删除、重命名角色，不得改变 character_id。
- 不得修改、翻译、删减或重排任何原始对白；输出中不需要复述对白。
- 不得改变剧情顺序、场景索引、人物物种、固定面容、基础服装和固定音色。
- 角色定妆图优先于文字推测；看不清时沿用角色表，不得自行换脸、换人或卡通化。
- 每个镜头只补充 visual_state、camera_plan、transition_plan、continuity_notes。
- visual_state 要明确本镜人物位置、服装、唯一核心手持道具和动作终态，避免多手、多道具错配。
- camera_plan 要规避复杂手部特写和无法匹配音频的正面连续口型。
- transition_plan 要说明如何承接前一镜终态，避免重复首帧、跳切和短暂黑帧。

只输出 JSON：
{
  "global_style": "全片统一的写实风格、时代、色彩和摄影规则",
  "continuity_rules": ["全片连续性规则"],
  "scenes": [
    {
      "scene_index": 0,
      "visual_state": "人物、服装、道具、位置及动作终态",
      "camera_plan": "景别、机位、运动及口型规避",
      "transition_plan": "从上一镜进入和向下一镜离开的方式",
      "continuity_notes": "本镜必须延续的状态"
    }
  ]
}
"""

ASSET_INVENTORY_SYSTEM_PROMPT = """
你是影视美术资产拆解器。必须从完整剧本和镜头表中列出后续画面会出现的全部非角色视觉资产。
角色由独立角色表管理，不要重复列入 assets。背景按镜头表管理；这里重点提取所有道具、载具、动物、
群体演员、武器、食物、文件和特殊视觉主体。相同资产只列一次，并标明出现的 scene_indexes。
不要把抽象概念、动作、对白、情绪或“人类”列为资产。只输出 JSON：
description 只描述资产自身可见的材质、颜色、形状和时代风格，不得写谁拿着、戴着、使用它或上面的文字内容。
{
  "assets": [
    {
      "name": "玉玺",
      "kind": "object",
      "description": "明代皇帝使用的青玉方形玉玺，固定材质颜色造型",
      "scene_indexes": [3, 4]
    }
  ]
}
kind 只能是 object、group、creature、vehicle。
"""


_HUMAN_KEYWORDS = ("人类", "人", "女人", "男人", "女孩", "男孩", "女子", "男子", "少女", "少年")
_NONHUMAN_KEYWORDS = (
    "狗", "犬", "猫", "龙", "狐", "狼", "鸟", "马", "鹿", "兔", "熊", "虎", "狮",
    "鱼", "鲸", "蛇", "龟", "鹤", "凤凰", "麒麟",
)
_DOG_SPECIES_KEYWORDS = ("边境牧羊犬", "牧羊犬", "小狗", "狗", "犬")
_DRAGON_SPECIES_KEYWORDS = ("金龙", "神龙", "龙")
_GENERIC_NAME_STOPWORDS = {
    "守望", "约定", "故事", "传奇", "主角", "角色", "场景", "清晨", "山谷", "生机",
    "碎石", "剧本", "内容", "画面", "视频", "旁白", "对白", "金色", "黑白",
}
_METADATA_SPEAKERS = {
    "剧本", "人物", "角色", "演员", "片名", "标题", "时间", "地点", "场景", "内容", "简介",
}
_SCENE_HEADER_RE = re.compile(
    r"^\s*[^\w\s]*\s*((?:第[一二三四五六七八九十百0-9]+[幕场章])|(?:尾声|尾 声|尾聲|终章|结尾|尾幕))(?:\s*[：:]\s*(.*))?\s*$"
)
_TIME_LINE_RE = re.compile(r"^\s*时间\s*[：:]\s*(.+?)\s*$")
_LOCATION_LINE_RE = re.compile(r"^\s*地点\s*[：:]\s*(.+?)\s*$")
_CHARACTER_LINE_RE = re.compile(r"^\s*([\u4e00-\u9fffA-Za-z0-9·]{1,16})\s*[：:]\s*(.+?)\s*$")
_DIALOGUE_LINE_RE = re.compile(
    r"^\s*([\u4e00-\u9fffA-Za-z0-9·]{1,16}(?:（[^）]{0,24}）|\([^)]{0,24}\))?)\s*[：:]\s*(.*?)\s*$"
)
_HANDHELD_PROP_KEYWORDS = ("手机", "包子", "豆浆", "玉玺", "诏书", "文件", "圣旨", "酒杯")
_RISKY_HAND_ACTIONS = ("拿", "握", "捏", "端", "举", "吃", "喝", "刷", "摸", "掏")
_TAIL_SCENE_TITLES = ("尾声", "尾 声", "尾聲", "终章", "结尾", "尾幕")


def _extract_json(raw_text: str) -> str:
    text = (raw_text or "").strip()
    m = re.search(r"```(?:json)?\s*([\s\S]*?)```", text)
    if m:
        text = m.group(1).strip()
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end > start:
        text = text[start:end + 1]
    return re.sub(r",\s*([}\]])", r"\1", text)


def _is_retryable_provider_error(message: str) -> bool:
    lowered = (message or "").lower()
    if any(token in lowered for token in ("401", "403", "api key", "模型不可用")):
        return False
    if any(token in lowered for token in ("超时", "timeout", "网络", "connection", "429", "500", "502", "503", "504")):
        return True
    return True


def _chat_json_messages(provider, messages: list, error_prefix: str) -> dict:
    last_error = None
    for attempt in range(EXTRACTION_MAX_ATTEMPTS):
        if attempt > 0:
            time.sleep(EXTRACTION_RETRY_BACKOFF[min(attempt, len(EXTRACTION_RETRY_BACKOFF) - 1)])
        try:
            result = provider.chat(messages, timeout=EXTRACTION_TIMEOUT)
            if not result.content:
                last_error = PipelineError(
                    f"{error_prefix}未返回内容。可能是模型响应超时、服务端暂时空响应，或当前供应商对该请求不稳定，请重试。"
                )
                continue
            try:
                return json.loads(_extract_json(result.content))
            except json.JSONDecodeError as exc:
                last_error = PipelineError(f"{error_prefix}解析失败：{exc}")
                continue
        except ProviderError as exc:
            last_error = exc
            if not _is_retryable_provider_error(str(exc)):
                break
            continue
    if isinstance(last_error, PipelineError):
        raise last_error
    if isinstance(last_error, ProviderError):
        raise PipelineError(f"{error_prefix}失败：{last_error}")
    raise PipelineError(f"{error_prefix}失败：未获得有效响应。")


def _chat_json(provider, system_prompt: str, user_prompt: str, error_prefix: str) -> dict:
    return _chat_json_messages(
        provider,
        [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        error_prefix,
    )


def _normalize_text(value: str, max_len: int) -> str:
    text = clean_text(value, max_len=max_len)
    if text and unrepaired_mojibake_score(text) >= 3:
        raise PipelineError("模型输出中存在疑似乱码，已停止生成。请重试。")
    return text


def _normalize_text_list(raw_value, max_len: int = 40) -> list[str]:
    if isinstance(raw_value, str):
        values = [raw_value]
    elif isinstance(raw_value, list):
        values = raw_value
    else:
        values = []
    result = []
    for item in values:
        text = clean_text(item, max_len=max_len)
        if text and text not in result:
            result.append(text)
    return result[:12]


def _first_matching_keyword(text: str, keywords: tuple[str, ...], default: str = "") -> str:
    for item in keywords:
        if item in text:
            return item
    return default


def _extract_between_pairs(text: str, pairs: list[tuple[str, str]], max_len: int) -> list[str]:
    result = []
    source = text or ""
    for start_token, end_token in pairs:
        search_pos = 0
        while True:
            start = source.find(start_token, search_pos)
            if start == -1:
                break
            end = source.find(end_token, start + len(start_token))
            if end == -1:
                break
            candidate = clean_text(source[start + len(start_token):end], max_len=max_len)
            if candidate and candidate not in result:
                result.append(candidate)
            search_pos = end + len(end_token)
    return result


def _extract_quoted_names(script_text: str) -> list[str]:
    result = []
    pairs = [
        ("“", "”"),
        ("\"", "\""),
        ("「", "」"),
        ("『", "』"),
        ("‘", "’"),
        ("'", "'"),
    ]
    for item in _extract_between_pairs(script_text or "", pairs, 40):
        if 0 < len(item) <= 12 and "：" not in item and "与" not in item and item not in result:
            result.append(item)
    return result


def _extract_title_names(script_text: str) -> list[str]:
    titles = _extract_between_pairs(script_text or "", [("《", "》")], 40)
    if not titles:
        return []
    title = clean_text(titles[0], max_len=40)
    if not title:
        return []
    parts = re.split(r"[与和、，,：:\s]+", title)
    result = []
    for item in parts:
        item = clean_text(item, max_len=20)
        if 1 < len(item) <= 8 and item not in ("守望", "约定", "传奇", "故事") and item not in result:
            result.append(item)
    return result


def _normalize_script_lines(script_text: str) -> list[str]:
    text = (script_text or "").replace("\r\n", "\n").replace("\r", "\n")
    return [line.strip() for line in text.split("\n")]


def _collapse_cjk_spaces(text: str, max_len: int = 120) -> str:
    value = clean_text(text, max_len=max_len)
    return re.sub(r"(?<=[\u4e00-\u9fff])\s+(?=[\u4e00-\u9fff])", "", value)


def _is_character_header(line: str) -> bool:
    return _collapse_cjk_spaces(line, max_len=40).startswith("人物")


def _normalize_scene_title(title: str) -> str:
    compact = _collapse_cjk_spaces(title, max_len=40)
    if compact in {item.replace(" ", "") for item in _TAIL_SCENE_TITLES}:
        return "尾声"
    return compact


def _is_narration_speaker(speaker: str) -> bool:
    return _collapse_cjk_spaces(speaker, max_len=20) in ("旁白", "画外音", "解说")


def _is_stage_direction(line: str) -> bool:
    value = clean_text(line, max_len=240)
    return (
        (value.startswith("（") and value.endswith("）"))
        or (value.startswith("(") and value.endswith(")"))
    )


def _extract_character_block_lines(script_text: str) -> list[str]:
    lines = _normalize_script_lines(script_text)
    start = None
    end = None
    for idx, line in enumerate(lines):
        if _is_character_header(line):
            start = idx + 1
            continue
        if start is not None and _SCENE_HEADER_RE.match(line):
            end = idx
            break
    if start is None:
        return []
    return [line for line in lines[start:end] if line]


def _guess_species_from_desc(desc: str, name: str) -> str:
    text = clean_text(f"{name} {desc}", max_len=240)
    if any(keyword in text for keyword in _DRAGON_SPECIES_KEYWORDS):
        return "龙"
    if any(keyword in text for keyword in _DOG_SPECIES_KEYWORDS):
        return "边境牧羊犬" if "边境牧羊犬" in text else "狗"
    return "人类"


def _guess_voice_hint_from_desc(desc: str) -> str:
    text = clean_text(desc, max_len=240)
    if any(token in text for token in ("胆小", "哆嗦", "尖细", "太监")):
        return "尖细、紧张"
    if any(token in text for token in ("权倾", "威严", "低沉", "冷笑")):
        return "低沉、威严"
    if any(token in text for token in ("学生", "手机", "游戏", "腹黑", "机智")):
        return "年轻、随意"
    return ""


def _guess_outfit_from_desc(desc: str) -> str:
    text = clean_text(desc, max_len=240)
    outfit_markers = (
        "卫衣", "龙袍", "皇冠", "官服", "朝服", "盔甲", "甲胄", "军装",
        "西装", "衬衫", "长裙", "短裙", "校服", "斗篷", "项圈", "帽子",
    )
    return "、".join(item for item in outfit_markers if item in text)


def _infer_character_outfit_from_script(script_text: str, profile: CharacterProfile) -> str:
    name = clean_text(profile.name, max_len=20)
    if not name:
        return ""
    text = clean_text(script_text, max_len=8000)
    markers = (
        "卫衣", "龙袍", "皇冠", "官服", "朝服", "盔甲", "甲胄", "军装",
        "西装", "衬衫", "长裙", "短裙", "校服", "斗篷", "项圈", "帽子",
    )
    for match in re.finditer(re.escape(name), text):
        segment = text[match.start(): match.start() + 80]
        segment = re.split(r"[。！？\n]", segment, maxsplit=1)[0]
        if not any(action in segment for action in ("穿着", "身穿", "换上", "一身", "戴着", "歪戴")):
            continue
        found = [marker for marker in markers if marker in segment]
        if found:
            return "、".join(found[:4])
    return ""


def _parse_characters_from_block(script_text: str) -> list[CharacterProfile]:
    lines = _extract_character_block_lines(script_text)
    if not lines:
        return []
    characters = []
    last_profile = None
    for idx, line in enumerate(lines):
        match = _CHARACTER_LINE_RE.match(line)
        if not match:
            if last_profile and line and not _is_stage_direction(line):
                extra = clean_text(line, max_len=120)
                if extra:
                    last_profile.appearance = clean_text(f"{last_profile.appearance}，{extra}", max_len=180)
                    last_profile.personality = clean_text(f"{last_profile.personality}，{extra}", max_len=100)
            continue
        name = clean_text(match.group(1), max_len=20)
        desc = clean_text(match.group(2), max_len=240)
        if not name or _is_character_header(name):
            continue
        species = _guess_species_from_desc(desc, name)
        role = "主角" if idx == 0 else ("关键配角" if "丞相" in name or "龙" in name else "配角")
        profile = CharacterProfile(
            character_id={
                "李铁柱": "li_tiezhu",
                "小顺子": "xiao_shunzi",
                "王丞相": "wang_chengxiang",
                "士兵": "shi_bing",
                "龙": "dragon",
                "金龙": "dragon",
            }.get(name, clean_text(name.lower(), max_len=20) or f"char_{idx:02d}"),
            name=name,
            role=role,
            species=species,
            appearance=desc,
            outfit=_guess_outfit_from_desc(desc),
            personality=desc,
            voice_hint=_guess_voice_hint_from_desc(desc),
            is_primary=(idx == 0),
        )
        characters.append(profile)
        last_profile = profile
    return characters


def _split_structured_scenes(script_text: str) -> list[tuple[str, list[str]]]:
    lines = _normalize_script_lines(script_text)
    scenes = []
    current_title = None
    current_lines: list[str] = []
    for line in lines:
        header = _SCENE_HEADER_RE.match(line)
        if header:
            if current_title is not None and current_lines:
                scenes.append((current_title, current_lines))
            suffix = clean_text(header.group(2) or "", max_len=40)
            current_title = _normalize_scene_title(suffix or header.group(1))
            current_lines = []
            continue
        if current_title is not None:
            current_lines.append(line)
    if current_title is not None and current_lines:
        scenes.append((current_title, current_lines))
    return scenes


def _sanitize_risky_hand_actions(text: str) -> str:
    value = clean_text(text, max_len=260)
    prop_count = sum(1 for item in _HANDHELD_PROP_KEYWORDS if item in value)
    action_count = sum(1 for item in _RISKY_HAND_ACTIONS if item in value)
    if prop_count >= 2 and action_count >= 2:
        return (
            f"{value}。注意：多件小道具放在桌案、手边或画面中，角色只做一个清晰动作，"
            "不要同时完成吃、喝、拿手机、换手等复杂手部操作。"
        )
    return value


def _extract_scene_characters(section_text: str, characters: list[CharacterProfile]) -> list[str]:
    required = []
    blob = _collapse_cjk_spaces(section_text, max_len=4000)
    for profile in characters:
        name = _collapse_cjk_spaces(profile.name, max_len=20)
        if name and name in blob and profile.character_id not in required:
            required.append(profile.character_id)
    if characters and characters[0].character_id not in required:
        required.insert(0, characters[0].character_id)
    return required[:6]


def _extract_scene_elements(section_text: str) -> list[str]:
    blob = _collapse_cjk_spaces(section_text, max_len=4000)
    result = []
    for item in (
        "手机", "爆米花", "床板", "包子", "豆浆", "皇冠", "龙袍", "龙床", "龙椅",
        "玉玺", "诏书", "文件", "废帝诏书", "长城", "黑洞", "甲兵", "士兵", "盔甲",
        "兵器", "宫灯", "金龙",
    ):
        if item in blob and item not in result:
            result.append(item)
    return result[:12]


def extract_script_visual_elements(script_text: str) -> list[str]:
    """Return the complete deterministic prop/creature inventory for initial asset locking."""
    return _extract_scene_elements(script_text)


def _extract_scene_forbidden_elements(section_text: str, characters: list[CharacterProfile]) -> list[str]:
    blob = _collapse_cjk_spaces(section_text, max_len=4000)
    forbidden = []
    if any(profile.species != "人类" for profile in characters if profile.is_primary) and "人类" not in blob:
        forbidden.append("人类")
    if _script_has_explicit_dragon(blob):
        forbidden.append("只用金光代替龙")
    return forbidden[:6]


def _build_local_scene_descriptions(title: str, lines: list[str]) -> tuple[str, str, str]:
    setting = ""
    actions = []
    continuity = []
    for line in lines:
        if not line:
            continue
        time_match = _TIME_LINE_RE.match(line)
        if time_match:
            continuity.append(f"时间：{clean_text(time_match.group(1), max_len=40)}")
            continue
        location_match = _LOCATION_LINE_RE.match(line)
        if location_match:
            setting = clean_text(location_match.group(1), max_len=120)
            continue
        if _is_stage_direction(line):
            actions.append(clean_text(line.strip("（）()"), max_len=220))
        elif not _DIALOGUE_LINE_RE.match(line) and not _is_narration_speaker(line.split("：", 1)[0].split(":", 1)[0]):
            actions.append(clean_text(line, max_len=220))
    action_text = _sanitize_risky_hand_actions(" ".join(actions[:3]))
    keyframe = _sanitize_risky_hand_actions(" ".join(actions[:2])) or action_text or title
    continuity_text = clean_text("；".join(continuity), max_len=160)
    return setting, action_text or title, continuity_text


def _estimate_spoken_seconds(text: str) -> float:
    compact = re.sub(r"\s+", "", clean_text(text, max_len=400))
    if not compact:
        return 0.0
    return max(1.2, len(compact) / 5.2)


def _split_spoken_text(text: str, max_chars: int = 35) -> list[str]:
    value = clean_text(text, max_len=500)
    if len(value) <= max_chars:
        return [value] if value else []
    clauses = re.findall(r"[^，。！？；,.!?;]+[，。！？；,.!?;]?", value)
    result = []
    current = ""
    for clause in clauses:
        clause = clause.strip()
        if not clause:
            continue
        if current and len(current + clause) > max_chars:
            result.append(current)
            current = ""
        while len(clause) > max_chars:
            if current:
                result.append(current)
                current = ""
            result.append(clause[:max_chars])
            clause = clause[max_chars:]
        current += clause
    if current:
        result.append(current)
    return result


def _split_local_scene_by_dialogue_budget(scene: Scene, frame_rate: int) -> list[Scene]:
    expanded_lines = []
    for line in scene.dialogue_lines:
        for text_part in _split_spoken_text(line.text):
            item = DialogueLine.from_dict(line.to_dict())
            item.text = text_part
            expanded_lines.append(item)
    scene.dialogue_lines = expanded_lines
    if not scene.dialogue_lines:
        scene.duration_hint = min(7.9, scene.duration_hint or 6.0)
        scene.num_frames = duration_to_num_frames(scene.duration_hint, frame_rate)
        return [scene]

    chunks: list[list[DialogueLine]] = []
    current_chunk: list[DialogueLine] = []
    current_seconds = _estimate_spoken_seconds(scene.narration)

    for line in scene.dialogue_lines:
        line_seconds = _estimate_spoken_seconds(f"{line.character_name}：{line.text}")
        exceeds_budget = current_chunk and (current_seconds + line_seconds > 7.9 or len(current_chunk) >= 2)
        if exceeds_budget:
            chunks.append(current_chunk)
            current_chunk = []
            current_seconds = 0.0
        current_chunk.append(line)
        current_seconds += line_seconds

    if current_chunk:
        chunks.append(current_chunk)

    if len(chunks) <= 1:
        estimated = _estimate_spoken_seconds(scene.narration) + sum(
            _estimate_spoken_seconds(f"{line.character_name}：{line.text}") for line in scene.dialogue_lines
        )
        scene.duration_hint = max(4.0, min(7.9, estimated + 1.2))
        scene.num_frames = duration_to_num_frames(scene.duration_hint, frame_rate)
        return [scene]

    split_scenes: list[Scene] = []
    base_setting = clean_text(scene.setting_description, max_len=160)
    base_required_elements = list(scene.required_elements or [])
    base_forbidden_elements = list(scene.forbidden_elements or [])
    base_required_ids = list(scene.required_character_ids or [])
    dialogue_character_ids = list(dict.fromkeys(
        item.character_id for item in scene.dialogue_lines if item.character_id
    ))
    narration_assigned = False

    for chunk_index, chunk in enumerate(chunks, start=1):
        chunk_required_ids = []
        for item in chunk:
            if item.character_id and item.character_id not in chunk_required_ids:
                chunk_required_ids.append(item.character_id)
        if len(chunk_required_ids) == 1:
            counterpart = next(
                (item for item in dialogue_character_ids if item not in chunk_required_ids),
                "",
            )
            if counterpart:
                chunk_required_ids.append(counterpart)
        focus_lines = " ".join(
            clean_text(f"{item.character_name}：{item.text}", max_len=60) for item in chunk[:3]
        )
        estimated = sum(_estimate_spoken_seconds(f"{item.character_name}：{item.text}") for item in chunk)
        narration_text = scene.narration if scene.narration and not narration_assigned else ""
        if narration_text:
            estimated += _estimate_spoken_seconds(narration_text)
            narration_assigned = True
        title = scene.title if chunk_index == 1 else f"{scene.title} 第{chunk_index}段"
        speaker_names = "与".join(dict.fromkeys(
            item.character_name for item in chunk if item.character_name
        ))
        chunk_setting = base_setting
        if base_setting in _TAIL_SCENE_TITLES or scene.title in _TAIL_SCENE_TITLES:
            if narration_text and "长城" in narration_text:
                chunk_setting = "中国明代长城脚下，夕阳"
            elif any("宫殿" in item.tone for item in chunk) or any(
                item.character_name == "李铁柱" for item in chunk
            ):
                chunk_setting = "中国明代皇宫内殿"
        visual_focus = (
            f"{speaker_names}在{chunk_setting}对话，聚焦当前说话者和对方自然反应"
            if speaker_names else f"{base_setting}中的连续剧情镜头"
        )
        scene_description = clean_text(visual_focus, max_len=180)
        element_blob = f"{focus_lines} {visual_focus}"
        chunk_required_elements = [
            item for item in base_required_elements
            if item != "人类" and item in element_blob
        ]
        if chunk_index == 1:
            for item in base_required_elements:
                if item != "人类" and item in (scene.keyframe_prompt or scene.scene_description):
                    if item not in chunk_required_elements:
                        chunk_required_elements.append(item)
        continuity_notes = clean_text(
            f"{scene.continuity_notes} 同一幕内连续镜头，人物服装、道具、场景保持一致。",
            max_len=160,
        )
        split_scenes.append(
            Scene(
                index=scene.index + chunk_index - 1,
                title=title,
                setting_description=chunk_setting,
                scene_description=scene_description,
                keyframe_prompt=visual_focus,
                continuity_notes=continuity_notes,
                narration=narration_text,
                dialogue_lines=chunk,
                required_character_ids=_normalize_text_list(chunk_required_ids, max_len=40),
                required_elements=_normalize_text_list(chunk_required_elements[:3], max_len=40),
                forbidden_elements=_normalize_text_list(base_forbidden_elements, max_len=40),
                duration_hint=max(4.0, min(7.9, estimated + 1.2)),
                num_frames=duration_to_num_frames(max(4.0, min(7.9, estimated + 1.2)), frame_rate),
            )
        )
    return split_scenes


def _extract_cjk_terms(script_text: str, min_len: int = 2, max_len: int = 4) -> list[str]:
    pattern = rf"[\u4e00-\u9fff]{{{min_len},{max_len}}}"
    return [item for item in re.findall(pattern, script_text or "") if item]


def _frequent_name_candidates(script_text: str) -> list[str]:
    counts = Counter(_extract_cjk_terms(script_text))
    result = []
    for item, count in counts.most_common():
        if count < 2:
            continue
        if item in _GENERIC_NAME_STOPWORDS:
            continue
        if any(keyword == item for keyword in _DOG_SPECIES_KEYWORDS + _DRAGON_SPECIES_KEYWORDS):
            continue
        if item not in result:
            result.append(item)
    return result


def _infer_primary_name(script_text: str) -> str:
    text = script_text or ""
    quoted = _extract_quoted_names(text)
    for species in _DOG_SPECIES_KEYWORDS:
        pos = text.find(species)
        if pos != -1:
            for candidate in quoted:
                cpos = text.find(candidate, pos)
                if cpos != -1 and cpos - pos <= 24:
                    return candidate
    names = quoted + _extract_title_names(text)
    for candidate in names:
        if candidate not in ("守望", "约定", "传奇", "故事", "金龙"):
            return candidate
    for candidate in _frequent_name_candidates(text):
        if candidate not in ("金龙", "神龙", "边境牧羊犬"):
            return candidate
    return "主角"


def _build_character_reference_from_profile(profile: CharacterProfile) -> str:
    parts = []
    if profile.species:
        parts.append(profile.species)
    if profile.appearance:
        parts.append(profile.appearance)
    if profile.outfit:
        parts.append(f"固定服装{profile.outfit}")
    if profile.personality:
        parts.append(f"气质{profile.personality}")
    base = "，".join(parts) or (profile.name or "主角")
    return clean_text(f"{base}，单主体角色设定图，纯净背景，主体清晰完整。", max_len=220)


def _fallback_characters_from_script(script_text: str) -> tuple[str, list[CharacterProfile]]:
    text = clean_text(script_text, max_len=4000)
    quoted_names = _extract_quoted_names(text)
    characters = []

    primary_name = quoted_names[0] if quoted_names else "主角"
    primary_species = _first_matching_keyword(text, _DOG_SPECIES_KEYWORDS, "")
    if not primary_species and any(keyword in text for keyword in _NONHUMAN_KEYWORDS):
        primary_species = _first_matching_keyword(text, _NONHUMAN_KEYWORDS, "非人主角")
    primary_role = "主角"
    primary_appearance_parts = []
    for marker in ("黑白", "红色项圈", "竖耳", "金色", "白色", "黑色"):
        if marker in text and marker not in primary_appearance_parts:
            primary_appearance_parts.append(marker)
    if primary_species:
        primary_appearance_parts.insert(0, primary_species)
    primary_personality_parts = []
    for marker in ("善良", "执着", "警觉", "勇敢", "温和", "坚定"):
        if marker in text and marker not in primary_personality_parts:
            primary_personality_parts.append(marker)
    primary = CharacterProfile(
        character_id="char_main",
        name=primary_name,
        role=primary_role,
        species=primary_species or "主角",
        appearance="，".join(primary_appearance_parts[:4]),
        personality="，".join(primary_personality_parts[:4]),
        voice_hint="温和、坚定" if any(k in text for k in _DOG_SPECIES_KEYWORDS) else "",
        is_primary=True,
    )
    characters.append(primary)

    dragon_species = _first_matching_keyword(text, _DRAGON_SPECIES_KEYWORDS, "") if _script_has_explicit_dragon(text) else ""
    if dragon_species:
        dragon_name = "金龙" if "金龙" in text else dragon_species
        if dragon_name not in {c.name for c in characters}:
            characters.append(
                CharacterProfile(
                    character_id="char_dragon",
                    name=dragon_name,
                    role="关键配角",
                    species=dragon_species,
                    appearance="金色鳞片，巨型神话生物",
                    personality="庄重、神秘",
                    voice_hint="低沉、威严",
                    is_primary=False,
                )
            )

    return _build_character_reference_from_profile(primary), characters


def _script_implied_characters(script_text: str) -> list[CharacterProfile]:
    text = clean_text(script_text, max_len=4000)
    characters = []

    primary_name = _infer_primary_name(text)
    primary_species = _first_matching_keyword(text, _DOG_SPECIES_KEYWORDS, "") or _first_matching_keyword(
        text, _NONHUMAN_KEYWORDS, "主角"
    )
    primary = CharacterProfile(
        character_id="dog_main" if any(k in text for k in _DOG_SPECIES_KEYWORDS) else "char_main",
        name=primary_name,
        role="主角",
        species=primary_species,
        appearance="黑白相间，红色项圈" if "黑白" in text or "红色项圈" in text else "",
        personality="温和、坚定" if any(k in text for k in _DOG_SPECIES_KEYWORDS) else "",
        voice_hint="温和、坚定" if any(k in text for k in _DOG_SPECIES_KEYWORDS) else "",
        is_primary=True,
    )
    characters.append(primary)

    if _script_has_explicit_dragon(text):
        characters.append(
            CharacterProfile(
                character_id="dragon",
                name="金龙" if "金龙" in text else "龙",
                role="关键配角",
                species=_first_matching_keyword(text, _DRAGON_SPECIES_KEYWORDS, "龙"),
                appearance="金色鳞片，神话生物",
                personality="庄重、神秘",
                voice_hint="低沉、威严",
                is_primary=False,
            )
        )
    return characters


def _augment_characters_from_script(script_text: str, characters: list[CharacterProfile]) -> list[CharacterProfile]:
    text = clean_text(script_text, max_len=4000)
    quoted_names = _extract_quoted_names(text)
    existing_ids = {item.character_id for item in characters}
    existing_names = {item.name for item in characters if item.name}
    implied = _script_implied_characters(script_text)

    if characters:
        primary = _primary_character(characters) or characters[0]
        if not primary.name or primary.name in ("未知", "主角", "狗_main"):
            if quoted_names:
                primary.name = quoted_names[0]
        if not primary.species or primary.species in ("主角", "未知"):
            species = _first_matching_keyword(text, _DOG_SPECIES_KEYWORDS, "")
            if species:
                primary.species = species
        if not primary.appearance:
            appearance_parts = []
            for marker in ("黑白", "红色项圈", "竖耳", "长毛", "金色鳞片"):
                if marker in text and marker not in appearance_parts:
                    appearance_parts.append(marker)
            primary.appearance = "，".join(appearance_parts[:4])
        if not primary.voice_hint and any(k in text for k in _DOG_SPECIES_KEYWORDS):
            primary.voice_hint = "温和、坚定"

    for profile in characters:
        if not profile.outfit:
            profile.outfit = _infer_character_outfit_from_script(script_text, profile)

    for inferred in implied:
        if inferred.is_primary:
            continue
        if inferred.name in existing_names:
            continue
        char_id = inferred.character_id
        if char_id in existing_ids:
            char_id = f"{char_id}_{len(existing_ids)}"
        inferred.character_id = char_id
        characters.append(inferred)
        existing_ids.add(char_id)
        if inferred.name:
            existing_names.add(inferred.name)
    return characters


def _scene_text_blob(scene: Scene) -> str:
    return clean_text(
        " ".join(
            part for part in [
                scene.title,
                scene.setting_description,
                scene.scene_description,
                scene.keyframe_prompt,
                scene.continuity_notes,
                scene.narration,
            ] if part
        ),
        max_len=2000,
    )


def _character_keywords(profile: CharacterProfile) -> list[str]:
    keywords = []
    for value in [profile.name, profile.species, profile.role]:
        text = clean_text(value, max_len=40)
        if text and text not in keywords:
            keywords.append(text)
    if profile.character_id == "dragon" and "龙" not in keywords:
        keywords.append("龙")
    return keywords


def _augment_scenes_with_required_characters(script_text: str, characters: list[CharacterProfile], scenes: list[Scene]) -> list[Scene]:
    if not scenes:
        return scenes

    primary = _primary_character(characters)
    dragon = next((item for item in characters if "龙" in (item.name or "") or "龙" in (item.species or "")), None)

    script_units = _story_units(script_text)
    scene_unit_map = {}
    for idx, scene in enumerate(scenes):
        start = min(len(script_units), idx)
        stop = min(len(script_units), idx + 2)
        scene_unit_map[scene.index] = " ".join(script_units[start:stop]) if script_units else ""

    dragon_mentions_script = bool(
        dragon is not None and (
            any("龙" in unit for unit in script_units)
            or "金龙" in script_text
            or "神龙" in script_text
        )
    )
    dragon_scene_assigned = False

    for idx, scene in enumerate(scenes):
        blob = f"{_scene_text_blob(scene)} {scene_unit_map.get(scene.index, '')}"
        if primary and primary.character_id not in scene.required_character_ids:
            scene.required_character_ids.append(primary.character_id)
        if (
            primary and primary.species
            and any(k in primary.species for k in _NONHUMAN_KEYWORDS)
            and primary.species not in scene.required_elements
        ):
            scene.required_elements.append(primary.species)
        if primary and any(k in (primary.species or "") for k in _NONHUMAN_KEYWORDS) and "人类" not in scene.forbidden_elements:
            scene.forbidden_elements.append("人类")

        if dragon and not dragon_scene_assigned:
            if any(keyword and keyword in blob for keyword in _character_keywords(dragon)):
                dragon_scene_assigned = True
                if dragon.character_id not in scene.required_character_ids:
                    scene.required_character_ids.append(dragon.character_id)
                if dragon.name and dragon.name not in scene.required_elements:
                    scene.required_elements.append(dragon.name)

    if dragon and dragon_mentions_script:
        dragon_scene_indexes = {
            scene.index
            for scene in scenes
            if dragon.character_id in scene.required_character_ids
            or any("龙" in item for item in scene.required_elements)
        }
        if not dragon_scene_indexes:
            target_index = max(len(scenes) // 2, len(scenes) - 2 if len(scenes) > 1 else 0)
            target_scene = scenes[target_index]
            if dragon.character_id not in target_scene.required_character_ids:
                target_scene.required_character_ids.append(dragon.character_id)
            if dragon.name and dragon.name not in target_scene.required_elements:
                target_scene.required_elements.append(dragon.name)
            dragon_scene_indexes.add(target_scene.index)

        for scene in scenes:
            if scene.index in dragon_scene_indexes and "只用金光代替龙" not in scene.forbidden_elements:
                scene.forbidden_elements.append("只用金光代替龙")

    for scene in scenes:
        dedup_ids = []
        for item in scene.required_character_ids:
            if item and item not in dedup_ids:
                dedup_ids.append(item)
        scene.required_character_ids = dedup_ids[:6]
        scene.required_elements = _normalize_text_list(scene.required_elements, max_len=40)
        scene.forbidden_elements = _normalize_text_list(scene.forbidden_elements, max_len=40)

    return scenes


def _story_units(script_text: str) -> list[str]:
    text = clean_text(script_text, max_len=4000)
    units = []
    for chunk in re.split(r"[\n\r]+|(?<=[。！？!?])", text):
        chunk = clean_text(chunk, max_len=200)
        if len(chunk) >= 6:
            units.append(chunk)
    if not units and text:
        compact = clean_text(text, max_len=800)
        if compact:
            units = [compact[i:i + 120] for i in range(0, len(compact), 120) if compact[i:i + 120].strip()]
    return units


def _clean_speaker_name(text: str) -> str:
    value = clean_text(text, max_len=40)
    value = re.sub(r"(（[^）]{0,24}）|\([^)]{0,24}\))", "", value).strip()
    return clean_text(value, max_len=20)


def _clean_dialogue_text(text: str) -> str:
    value = clean_text(text, max_len=160)
    value = value.strip("“”\"'「」『』：: ")
    return clean_text(value, max_len=120)


def _script_scene_sections(script_text: str) -> list[str]:
    structured = _split_structured_scenes(script_text)
    if structured:
        return [clean_text("\n".join([title, *lines]), max_len=4000) for title, lines in structured]
    text = script_text or ""
    pattern = re.compile(r"((?:第[一二三四五六七八九十百0-9]+[幕场章])|(?:尾声|尾 声|尾聲|终章|结尾|尾幕))(?:\s*[：:]\s*)?")
    parts = pattern.split(text)
    if len(parts) <= 1:
        cleaned = clean_text(text, max_len=4000)
        return [cleaned] if cleaned else []

    sections = []
    current_title = ""
    for part in parts:
        cleaned = clean_text(part, max_len=4000)
        if not cleaned:
            continue
        if pattern.fullmatch(cleaned):
            current_title = cleaned
            continue
        section = clean_text(f"{current_title}\n{cleaned}", max_len=4000)
        if section:
            sections.append(section)
            current_title = ""
    return sections


def _parse_dialogues_from_section(section_text: str) -> list[tuple[str, str]]:
    results = []
    pending_speaker = ""
    for raw_line in section_text.splitlines():
        line = clean_text(raw_line, max_len=240)
        if not line:
            continue
        if _TIME_LINE_RE.match(line) or _LOCATION_LINE_RE.match(line) or _SCENE_HEADER_RE.match(line) or _is_character_header(line):
            pending_speaker = ""
            continue
        match = _DIALOGUE_LINE_RE.match(line)
        if not match:
            if pending_speaker and not _is_stage_direction(line):
                dialogue = _clean_dialogue_text(line)
                if dialogue:
                    results.append((pending_speaker, dialogue))
                    pending_speaker = ""
            continue
        speaker = _clean_speaker_name(match.group(1))
        dialogue = _clean_dialogue_text(match.group(2))
        if speaker in _METADATA_SPEAKERS:
            pending_speaker = ""
            continue
        if speaker and dialogue:
            results.append((speaker, dialogue))
            pending_speaker = ""
        elif speaker:
            pending_speaker = speaker
    return results


def _match_character_id_by_name(speaker: str, characters: list[CharacterProfile]) -> str:
    speaker_clean = _clean_speaker_name(speaker)
    if not speaker_clean:
        return ""
    for profile in characters:
        if speaker_clean == clean_text(profile.name, max_len=20):
            return profile.character_id
    for profile in characters:
        name = clean_text(profile.name, max_len=20)
        if name and (speaker_clean in name or name in speaker_clean):
            return profile.character_id
    return clean_text(f"dialogue_{speaker_clean}", max_len=40)


def _resolve_character_by_speaker(speaker: str, characters: list[CharacterProfile]) -> CharacterProfile | None:
    speaker_clean = _clean_speaker_name(speaker)
    if not speaker_clean:
        return None
    for profile in characters:
        name = clean_text(profile.name, max_len=20)
        if speaker_clean == name:
            return profile
    for profile in characters:
        name = clean_text(profile.name, max_len=20)
        if name and (speaker_clean in name or name in speaker_clean):
            return profile
    return None


def _augment_dialogue_speakers_from_script(
    script_text: str, characters: list[CharacterProfile]
) -> list[CharacterProfile]:
    """Every named speaker is a real character and must receive a stable reference profile."""
    existing_names = {clean_text(item.name, max_len=20) for item in characters if item.name}
    existing_ids = {item.character_id for item in characters}
    for speaker, _ in _parse_dialogues_from_section(script_text):
        name = _clean_speaker_name(speaker)
        if not name or name in _METADATA_SPEAKERS or _is_narration_speaker(name) or name in existing_names:
            continue
        base_id = clean_text(f"dialogue_{name}", max_len=40)
        character_id = base_id
        suffix = 2
        while character_id in existing_ids:
            character_id = clean_text(f"{base_id}_{suffix}", max_len=40)
            suffix += 1
        characters.append(
            CharacterProfile(
                character_id=character_id,
                name=name,
                role="对白角色",
                species="人类",
                appearance="",
                outfit=_infer_character_outfit_from_script(script_text, CharacterProfile(character_id, name=name)),
                personality="",
                voice_hint="",
                is_primary=False,
            )
        )
        existing_names.add(name)
        existing_ids.add(character_id)
    return characters


def _fallback_scenes_from_script(script_text: str, characters: list[CharacterProfile], scene_count_hint: int, frame_rate: int) -> list[Scene]:
    units = _story_units(script_text)
    if not units:
        text = clean_text(script_text, max_len=800)
        if not text:
            raise PipelineError("场景提取失败：剧本文本无法切分为有效场景。")
        units = [text]

    target_count = scene_count_hint if scene_count_hint and scene_count_hint > 0 else min(6, max(3, len(units)))
    chunk_size = max(1, (len(units) + target_count - 1) // target_count)
    primary = _primary_character(characters)
    primary_id = primary.character_id if primary else ""
    dragon = next((item for item in characters if "龙" in (item.species or item.name)), None)
    scenes = []

    for idx in range(0, len(units), chunk_size):
        chunk_units = units[idx: idx + chunk_size]
        combined = clean_text(" ".join(chunk_units), max_len=260)
        required_character_ids = [primary_id] if primary_id else []
        required_elements = []
        forbidden_elements = []
        if primary and primary.species and any(k in primary.species for k in _NONHUMAN_KEYWORDS):
            required_elements.append(primary.species)
            forbidden_elements.append("人类")
        if dragon and any(keyword in combined for keyword in _DRAGON_SPECIES_KEYWORDS):
            required_character_ids.append(dragon.character_id)
            required_elements.append(dragon.name or dragon.species)
        scene = Scene(
            index=len(scenes),
            title=f"场景 {len(scenes) + 1}",
            setting_description=combined[:80],
            scene_description=combined,
            keyframe_prompt=combined,
            continuity_notes="按原剧本顺序延续上一场景的角色状态和环境变化。",
            required_character_ids=[item for item in required_character_ids if item],
            required_elements=required_elements,
            forbidden_elements=forbidden_elements,
            duration_hint=max(3.0, min(8.0, len(combined) / 14)),
            num_frames=duration_to_num_frames(max(3.0, min(8.0, len(combined) / 14)), frame_rate),
        )
        scenes.append(scene)

    return scenes[:max(1, target_count)]


def _fallback_dialogues_from_script(script_text: str, characters: list[CharacterProfile], scenes: list[Scene]) -> dict[int, list[DialogueLine]]:
    sections = _script_scene_sections(script_text)
    if not sections or not scenes:
        return {}

    dialogue_map: dict[int, list[DialogueLine]] = {}
    scene_count = len(scenes)
    section_count = len(sections)

    for section_index, section in enumerate(sections):
        target_scene_index = min(scene_count - 1, int(section_index * scene_count / max(1, section_count)))
        lines = []
        for speaker, text in _parse_dialogues_from_section(section):
            if _is_narration_speaker(speaker):
                continue
            profile = _resolve_character_by_speaker(speaker, characters)
            if profile is None:
                continue
            lines.append(
                DialogueLine(
                    character_id=profile.character_id,
                    character_name=profile.name or speaker,
                    text=text,
                    tone="",
                    scene_purpose="角色对白",
                )
            )
        if lines:
            dialogue_map.setdefault(target_scene_index, []).extend(lines)
    return dialogue_map


def _fallback_narration_from_script(script_text: str, scenes: list[Scene]) -> dict[int, str]:
    sections = _script_scene_sections(script_text)
    if not sections or not scenes:
        return {}

    narration_map = {}
    scene_count = len(scenes)
    section_count = len(sections)
    for section_index, section in enumerate(sections):
        target_scene_index = min(scene_count - 1, int(section_index * scene_count / max(1, section_count)))
        narration_lines = []
        for speaker, text in _parse_dialogues_from_section(section):
            if _is_narration_speaker(speaker):
                narration_lines.append(text)
        if narration_lines:
            narration_map[target_scene_index] = clean_text(" ".join(narration_lines), max_len=260)
    return narration_map


def _parse_structured_script_locally(script_text: str, frame_rate: int, narration_mode: str) -> tuple[str, list[CharacterProfile], list[Scene]] | None:
    structured_scenes = _split_structured_scenes(script_text)
    if not structured_scenes:
        return None
    characters = _parse_characters_from_block(script_text)
    if not characters:
        _, characters = _fallback_characters_from_script(script_text)
    characters = _augment_characters_from_script(script_text, characters)
    characters = _augment_dialogue_speakers_from_script(script_text, characters)

    scenes: list[Scene] = []
    primary = _primary_character(characters)
    for idx, (title, lines) in enumerate(structured_scenes):
        section_text = "\n".join(lines)
        setting_description, scene_description, continuity_notes = _build_local_scene_descriptions(title, lines)
        required_character_ids = _extract_scene_characters(section_text, characters)
        required_elements = _extract_scene_elements(section_text)
        forbidden_elements = _extract_scene_forbidden_elements(section_text, characters)

        narration_lines = []
        dialogue_lines: list[DialogueLine] = []
        for speaker, text in _parse_dialogues_from_section(section_text):
            if _is_narration_speaker(speaker):
                narration_lines.append(text)
                continue
            profile = _resolve_character_by_speaker(speaker, characters)
            if profile is None:
                continue
            if profile.character_id not in required_character_ids:
                required_character_ids.append(profile.character_id)
            dialogue_lines.append(
                DialogueLine(
                    character_id=profile.character_id,
                    character_name=profile.name or speaker,
                    text=text,
                    tone="",
                    scene_purpose="角色对白",
                )
            )

        if (
            primary and primary.species
            and any(k in primary.species for k in _NONHUMAN_KEYWORDS)
            and primary.species not in required_elements
        ):
            required_elements.insert(0, primary.species)
        if not setting_description:
            setting_description = clean_text(title, max_len=120)
        if not scene_description:
            scene_description = clean_text(section_text, max_len=260)
        duration_hint = max(4.0, min(7.9, len(clean_text(section_text, max_len=4000)) / 24))
        if narration_mode == NARRATION_MODE_NONE:
            narration = ""
        else:
            narration = clean_text(" ".join(narration_lines), max_len=260)

        scene = Scene(
            index=idx,
            title=clean_text(title, max_len=60) or f"场景 {idx + 1}",
            setting_description=setting_description,
            scene_description=scene_description,
            keyframe_prompt=scene_description,
            continuity_notes=continuity_notes,
            narration=narration,
            dialogue_lines=dialogue_lines,
            required_character_ids=_normalize_text_list(required_character_ids, max_len=40),
            required_elements=_normalize_text_list(required_elements, max_len=40),
            forbidden_elements=_normalize_text_list(forbidden_elements, max_len=40),
            duration_hint=duration_hint,
            num_frames=duration_to_num_frames(duration_hint, frame_rate),
        )
        scenes.extend(_split_local_scene_by_dialogue_budget(scene, frame_rate))

    for idx, scene in enumerate(scenes):
        scene.index = idx
    scenes = _augment_scenes_with_required_characters(script_text, characters, scenes)
    character_reference = _build_character_reference_from_profile(primary or characters[0])
    return character_reference, characters, scenes


def _extract_characters(provider, script_text: str) -> tuple[str, list[CharacterProfile]]:
    payload = _chat_json(
        provider,
        CHARACTER_SYSTEM_PROMPT,
        f"剧本内容：\n{script_text.strip()}",
        "角色提取",
    )
    character_reference = _normalize_text(payload.get("character_reference", ""), 220)
    characters = []
    for idx, raw in enumerate(payload.get("characters") or []):
        if not isinstance(raw, dict):
            continue
        character_id = clean_text(raw.get("character_id", ""), max_len=40) or f"char_{idx:02d}"
        profile = CharacterProfile(
            character_id=character_id,
            name=_normalize_text(raw.get("name", ""), 40),
            role=_normalize_text(raw.get("role", ""), 40),
            species=_normalize_text(raw.get("species", ""), 40),
            appearance=_normalize_text(raw.get("appearance", ""), 180),
            outfit=_normalize_text(raw.get("outfit", ""), 120),
            personality=_normalize_text(raw.get("personality", ""), 100),
            voice_hint=_normalize_text(raw.get("voice_hint", ""), 60),
            is_primary=bool(raw.get("is_primary", False)),
        )
        characters.append(profile)
    if not characters:
        return _fallback_characters_from_script(script_text)
    characters = _augment_characters_from_script(script_text, characters)
    if not any(profile.is_primary for profile in characters):
        characters[0].is_primary = True
    if not character_reference:
        character_reference = _build_character_reference_from_profile(_primary_character(characters))
    return character_reference, characters


def _primary_character(characters: list[CharacterProfile]) -> CharacterProfile | None:
    for item in characters:
        if item.is_primary:
            return item
    return characters[0] if characters else None


def _scene_user_prompt(script_text: str, characters: list[CharacterProfile], scene_count_hint: int) -> str:
    char_lines = []
    for profile in characters:
        char_lines.append(
            f"- {profile.character_id}: {profile.name} / {profile.role} / {profile.species} / "
            f"外观 {profile.appearance} / 固定服装 {profile.outfit} / "
            f"性格 {profile.personality} / 音色 {profile.voice_hint}"
        )
    count_hint = (
        f"请拆成大约 {scene_count_hint} 个场景。"
        if scene_count_hint and scene_count_hint > 0
        else "请根据内容长度和节奏自动决定合理场景数，建议 8-20 个；长对白必须拆成多个短镜头。"
    )
    return (
        f"{count_hint}\n\n"
        "角色表：\n"
        + "\n".join(char_lines)
        + "\n\n剧本内容：\n"
        + script_text.strip()
    )


def _extract_scenes(provider, script_text: str, characters: list[CharacterProfile], scene_count_hint: int, frame_rate: int) -> list[Scene]:
    payload = _chat_json(
        provider,
        SCENE_SYSTEM_PROMPT,
        _scene_user_prompt(script_text, characters, scene_count_hint),
        "场景提取",
    )
    primary = _primary_character(characters)
    scenes = []
    for idx, raw in enumerate(payload.get("scenes") or []):
        if not isinstance(raw, dict):
            continue
        try:
            duration_hint = float(raw.get("duration_hint", 5) or 5)
        except (TypeError, ValueError):
            duration_hint = 5.0
        duration_hint = max(2.0, min(duration_hint, 7.9))
        required_character_ids = _normalize_text_list(raw.get("required_character_ids"), 40)
        required_elements = _normalize_text_list(raw.get("required_elements"), 40)
        forbidden_elements = _normalize_text_list(raw.get("forbidden_elements"), 40)
        if primary and any(key in (primary.species or primary.name or "") for key in _NONHUMAN_KEYWORDS):
            if "人类" not in forbidden_elements and not any(item in raw.get("scene_description", "") for item in _HUMAN_KEYWORDS):
                forbidden_elements.append("人类")
        scene = Scene(
            index=len(scenes),
            title=_normalize_text(raw.get("title", ""), 60) or f"场景 {idx + 1}",
            setting_description=_normalize_text(raw.get("setting_description", ""), 160),
            scene_description=_normalize_text(raw.get("scene_description", ""), 260),
            keyframe_prompt=_normalize_text(raw.get("keyframe_prompt", ""), 260),
            continuity_notes=_normalize_text(raw.get("continuity_notes", ""), 160),
            required_character_ids=required_character_ids,
            required_elements=required_elements,
            forbidden_elements=forbidden_elements,
            duration_hint=duration_hint,
            num_frames=duration_to_num_frames(duration_hint, frame_rate),
        )
        scenes.append(scene)
    if not scenes:
        return _fallback_scenes_from_script(script_text, characters, scene_count_hint, frame_rate)
    return scenes


def _extract_narration(provider, script_text: str, scenes: list[Scene], narration_mode: str):
    if narration_mode == NARRATION_MODE_NONE:
        return {}
    scene_outline = []
    for scene in scenes:
        scene_outline.append(
            {
                "scene_index": scene.index,
                "title": scene.title,
                "setting_description": scene.setting_description,
                "scene_description": scene.scene_description,
            }
        )
    mode_hint = (
        "只需要字幕文本，不需要配音。"
        if narration_mode == NARRATION_MODE_SUBTITLES
        else "需要旁白和字幕。"
    )
    payload = _chat_json(
        provider,
        NARRATION_SYSTEM_PROMPT,
        f"{mode_hint}\n\n场景列表：\n{json.dumps(scene_outline, ensure_ascii=False, indent=2)}\n\n剧本内容：\n{script_text.strip()}",
        "旁白提取",
    )
    narrations = {}
    for raw in payload.get("narrations") or []:
        if not isinstance(raw, dict):
            continue
        try:
            scene_index = int(raw.get("scene_index"))
        except (TypeError, ValueError):
            continue
        narrations[scene_index] = _normalize_text(raw.get("text", ""), 260)
    return narrations


def _extract_dialogues(provider, script_text: str, characters: list[CharacterProfile], scenes: list[Scene]) -> dict[int, list[DialogueLine]]:
    char_index = {profile.character_id: profile for profile in characters}
    scene_outline = []
    for scene in scenes:
        scene_outline.append(
            {
                "scene_index": scene.index,
                "title": scene.title,
                "setting_description": scene.setting_description,
                "scene_description": scene.scene_description,
                "required_character_ids": scene.required_character_ids,
            }
        )
    payload = _chat_json(
        provider,
        DIALOGUE_SYSTEM_PROMPT,
        "角色表：\n"
        + json.dumps([item.to_dict() for item in characters], ensure_ascii=False, indent=2)
        + "\n\n场景列表：\n"
        + json.dumps(scene_outline, ensure_ascii=False, indent=2)
        + "\n\n剧本内容：\n"
        + script_text.strip(),
        "对白提取",
    )
    dialogue_map: dict[int, list[DialogueLine]] = {}
    for raw in payload.get("dialogues") or []:
        if not isinstance(raw, dict):
            continue
        try:
            scene_index = int(raw.get("scene_index"))
        except (TypeError, ValueError):
            continue
        lines = []
        for item in raw.get("lines") or []:
            if not isinstance(item, dict):
                continue
            character_id = clean_text(item.get("character_id", ""), max_len=40)
            profile = char_index.get(character_id)
            lines.append(
                DialogueLine(
                    character_id=character_id,
                    character_name=_normalize_text(
                        item.get("character_name", "") or (profile.name if profile else ""),
                        40,
                    ),
                    text=_normalize_text(item.get("text", ""), 120),
                    tone=_normalize_text(item.get("tone", ""), 40),
                    scene_purpose=_normalize_text(item.get("scene_purpose", ""), 60),
                )
            )
        dialogue_map[scene_index] = [line for line in lines if line.text]
    return dialogue_map


def extract_visual_asset_inventory(
    cfg,
    script_text: str,
    characters: list[CharacterProfile],
    scenes: list[Scene],
) -> list[dict]:
    """Use Agnes to map every non-character visual asset to its scenes."""
    provider = registry.build_provider("agnes", cfg)
    provider.max_tokens = max(int(getattr(provider, "max_tokens", 2048) or 2048), 4096)
    scene_outline = [
        {
            "scene_index": scene.index,
            "title": scene.title,
            "setting": scene.setting_description,
            "action": scene.scene_description,
            "required_elements": scene.required_elements,
            "dialogue": [line.text for line in scene.dialogue_lines],
        }
        for scene in scenes
    ]
    payload = _chat_json(
        provider,
        ASSET_INVENTORY_SYSTEM_PROMPT,
        "角色名称（不要重复为资产）：\n"
        + json.dumps([item.name for item in characters], ensure_ascii=False)
        + "\n\n镜头表：\n"
        + json.dumps(scene_outline, ensure_ascii=False, indent=2)
        + "\n\n完整剧本：\n"
        + script_text.strip(),
        "视觉资产清单提取",
    )
    result = []
    for raw in payload.get("assets") or []:
        if not isinstance(raw, dict):
            continue
        name = _normalize_text(raw.get("name", ""), 40)
        kind = clean_text(raw.get("kind", "object"), max_len=16).lower()
        if not name or kind not in ("object", "group", "creature", "vehicle"):
            continue
        indexes = []
        for value in raw.get("scene_indexes") or []:
            try:
                index = int(value)
            except (TypeError, ValueError):
                continue
            if 0 <= index < len(scenes) and index not in indexes:
                indexes.append(index)
        result.append(
            {
                "name": name,
                "kind": kind,
                "description": _normalize_text(raw.get("description", ""), 140),
                "scene_indexes": indexes,
            }
        )
    return result


def enrich_production_plan(
    cfg,
    script_text: str,
    characters: list[CharacterProfile],
    scenes: list[Scene],
) -> dict:
    """Use Agnes multimodal understanding to enrich, never replace, extracted facts."""
    provider = registry.build_provider("agnes", cfg)
    provider.max_tokens = max(int(getattr(provider, "max_tokens", 2048) or 2048), 8192)
    character_payload = []
    image_paths = []
    for profile in characters:
        reference_order = None
        if profile.reference_path and len(image_paths) < 8:
            reference_order = len(image_paths)
            image_paths.append(profile.reference_path)
        character_payload.append(
            {
                "character_id": profile.character_id,
                "name": profile.name,
                "species": profile.species,
                "appearance": profile.appearance,
                "outfit": profile.outfit,
                "personality": profile.personality,
                "voice_id": profile.voice_id,
                "reference_image_order": reference_order,
            }
        )

    scene_payload = []
    for scene in scenes:
        scene_payload.append(
            {
                "scene_index": scene.index,
                "title": scene.title,
                "setting_description": scene.setting_description,
                "scene_description": scene.scene_description,
                "keyframe_prompt": scene.keyframe_prompt,
                "continuity_notes": scene.continuity_notes,
                "required_character_ids": scene.required_character_ids,
                "required_elements": scene.required_elements,
                "forbidden_elements": scene.forbidden_elements,
                "dialogues": [line.to_dict() for line in scene.dialogue_lines],
            }
        )
    request_text = (
        "请基于以下不可变事实生成制作规划。附件图片按角色表中的 reference_image_order 对应。\n\n"
        f"角色表：\n{json.dumps(character_payload, ensure_ascii=False, indent=2)}\n\n"
        f"镜头表：\n{json.dumps(scene_payload, ensure_ascii=False, indent=2)}\n\n"
        f"原始剧本：\n{script_text.strip()}"
    )
    content = provider.build_vision_content(request_text, image_paths)
    payload = _chat_json_messages(
        provider,
        [
            {"role": "system", "content": PRODUCTION_PLANNER_SYSTEM_PROMPT},
            {"role": "user", "content": content},
        ],
        "多模态制作规划",
    )

    scene_by_index = {scene.index: scene for scene in scenes}
    for raw in payload.get("scenes") or []:
        if not isinstance(raw, dict):
            continue
        try:
            scene = scene_by_index.get(int(raw.get("scene_index")))
        except (TypeError, ValueError):
            scene = None
        if scene is None:
            continue
        scene.visual_state = _normalize_text(raw.get("visual_state", ""), 260)
        scene.camera_plan = _normalize_text(raw.get("camera_plan", ""), 180)
        scene.transition_plan = _normalize_text(raw.get("transition_plan", ""), 180)
        enriched_continuity = _normalize_text(raw.get("continuity_notes", ""), 180)
        if enriched_continuity:
            scene.continuity_notes = clean_text(
                "；".join(filter(None, [scene.continuity_notes, enriched_continuity])),
                max_len=280,
            )
    return {
        "global_style": _normalize_text(payload.get("global_style", ""), 320),
        "continuity_rules": _normalize_text_list(payload.get("continuity_rules"), 160),
        "planner_model": getattr(provider, "model", ""),
        "character_ids": [profile.character_id for profile in characters],
    }


def extract_story_data(cfg, script_text: str, scene_count_hint: int = 6, frame_rate: int = 24,
                       narration_mode: str = NARRATION_MODE_VOICE, on_stage=None):
    """Extract characters, scenes, narration, and dialogue in separate passes."""
    if not script_text or not script_text.strip():
        raise PipelineError("剧本内容为空。")

    local_structured = _parse_structured_script_locally(script_text, frame_rate, narration_mode)
    if local_structured is not None:
        character_reference, characters, scenes = local_structured
        if on_stage:
            on_stage("检测到结构化中文剧本，正在使用本地规则提取角色、场景和对白...")
        if any(not item.appearance or not item.voice_hint for item in characters):
            try:
                provider = registry.build_provider(cfg.active_provider, cfg)
                if on_stage:
                    on_stage("正在补全缺失的角色外观、服装和声音上下文...")
                _, enriched = _extract_characters(provider, script_text)
                enriched_by_name = {clean_text(item.name, max_len=20): item for item in enriched}
                for profile in characters:
                    source = enriched_by_name.get(clean_text(profile.name, max_len=20))
                    if not source:
                        continue
                    for field_name in ("role", "species", "appearance", "outfit", "personality", "voice_hint"):
                        if not getattr(profile, field_name):
                            setattr(profile, field_name, getattr(source, field_name))
                primary = _primary_character(characters)
                if primary:
                    character_reference = _build_character_reference_from_profile(primary)
            except PipelineError:
                pass
        return character_reference, characters, scenes

    provider = registry.build_provider(cfg.active_provider, cfg)

    if on_stage:
        on_stage("正在提取角色设定...")
    character_reference, characters = _extract_characters(provider, script_text)
    characters = _augment_dialogue_speakers_from_script(script_text, characters)

    if on_stage:
        on_stage("正在提取场景结构...")
    try:
        scenes = _extract_scenes(provider, script_text, characters, scene_count_hint, frame_rate)
    except PipelineError:
        if on_stage:
            on_stage("场景提取不稳定，正在使用脚本内容自动兜底生成场景...")
        scenes = _fallback_scenes_from_script(script_text, characters, scene_count_hint, frame_rate)
    scenes = _augment_scenes_with_required_characters(script_text, characters, scenes)

    if on_stage:
        on_stage("正在提取旁白内容...")
    try:
        narration_map = _extract_narration(provider, script_text, scenes, narration_mode)
    except PipelineError:
        narration_map = _fallback_narration_from_script(script_text, scenes)

    if on_stage:
        on_stage("正在提取角色对白...")
    try:
        dialogue_map = _extract_dialogues(provider, script_text, characters, scenes)
    except PipelineError:
        dialogue_map = {}

    fallback_dialogue_map = _fallback_dialogues_from_script(script_text, characters, scenes)

    for scene in scenes:
        scene.narration = narration_map.get(scene.index, "")
        scene.dialogue_lines = dialogue_map.get(scene.index, []) or fallback_dialogue_map.get(scene.index, [])

    expanded_scenes = []
    for scene in scenes:
        expanded_scenes.extend(_split_local_scene_by_dialogue_budget(scene, frame_rate))
    for index, scene in enumerate(expanded_scenes):
        scene.index = index

    return character_reference, characters, expanded_scenes
def _script_has_explicit_dragon(script_text: str) -> bool:
    text = clean_text(script_text, max_len=8000)
    for decorative_term in ("龙袍", "龙床", "龙椅", "龙纹", "龙颜", "龙体", "龙凤"):
        text = text.replace(decorative_term, "")
    return bool(
        re.search(r"(?:金龙|神龙|巨龙|苍龙|青龙|白龙|黑龙|龙\s*[：:（(]|龙(?:出现|飞翔|盘旋|咆哮|开口|说话|守护))", text)
    )
