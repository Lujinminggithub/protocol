"""
分镜拆解 - 把剧本文本拆成结构化的多场景列表。

复用当前配置的对话 Provider(active_provider)，要求模型输出严格 JSON，
带跨场景一致性的视觉锚点关键词。JSON 解析做多层容错。
"""

import json
import re

from src.providers import registry
from src.video_pipeline.base import (
    Scene, PipelineError, duration_to_num_frames,
    NARRATION_MODE_NONE, NARRATION_MODE_SUBTITLES, NARRATION_MODE_VOICE,
)
from src.video_pipeline.text_utils import clean_text, unrepaired_mojibake_score

SYSTEM_PROMPT = (
    "你是专业的分镜师和视频脚本编导。将用户提供的剧本/故事文本拆解为若干个连续场景。\n"
    "\n"
    "【角色一致性 - 极其重要】\n"
    "先确定贯穿全片的主角。主角可能是人类、动物、神话生物或其他非人主体，严禁擅自把非人主角改成人类。"
    "写一段 character_reference：一张主角\"定妆图/角色设定图\"式的画面描述(纯净背景、主体清晰、物种明确、"
    "外观特征固定)。这张图会作为所有场景视频的统一锚点，所以特征描述要具体、固定。每个场景还要写 keyframe_prompt："
    "描述该场景首帧静态画面，必须明确沿用同一主角的物种、脸部特征、毛色/皮肤/鳞片/服装等稳定信息，不要把狗变成人、"
    "不要把龙变成光效、不要把非人主体拟人化。\n"
    "\n"
    "【主体约束 - 极其重要】\n"
    "每个场景必须输出 required_elements 和 forbidden_elements：\n"
    "- required_elements: 该场景必须清晰出现的主体/关键元素，1-4项，例如[\"边境牧羊犬\", \"金龙\"]；\n"
    "- forbidden_elements: 该场景不得出现的主体/干扰元素，0-4项，例如[\"人类\"]；\n"
    "- 如果剧本没有提到人类，尤其是主角本身是动物时，forbidden_elements 应明确包含\"人类\"；\n"
    "- 如果剧本提到龙/怪物/关键配角登场，required_elements 必须明确写出它，不能只用\"金光\"\"阴影\"\"气息\"代替；\n"
    "- 如果某角色/生物是本场戏剧情重点，它必须在画面里清晰可见，而不是被裁掉或藏在背景里。\n"
    "\n"
    "【动作可拍性 - 重要】\n"
    "AI视频模型不擅长表现\"手与小物体的精细交互\"，比如从口袋掏手机/钱包、拿起或放下"
    "杯子、开门关门、递东西等，这类动作会出现物体凭空浮现、穿模等诡异效果。因此"
    "scene_description 和 keyframe_prompt 里都要避开这类精细道具交互，改用模型擅长的表现方式：\n"
    "- 需要用到道具时，让道具一开始就已经在手里/在画面中(如\"手里拿着手机通话\"而不是"
    "\"从口袋掏出手机\")；\n"
    "- 优先描述大幅度肢体动作、表情、镜头运动、环境氛围(走动、转身、微笑、皱眉、"
    "镜头推拉环绕等)，这些模型表现自然；\n"
    "- 复杂的物品操作用旁白交代，画面只呈现结果状态。\n"
    "- 如果原剧本是角色对白、任务对话或无需解说的动作片段，不要强行添加旁白，narration 可以为空字符串。\n"
    "\n"
    "请严格按以下 JSON 格式输出，不要输出任何 JSON 之外的文字，不要用```代码块包裹：\n"
    "{\n"
    '  "character_reference": "主角角色设定图描述，物种明确、外观稳定、纯净背景、无其他主体，30-100字",\n'
    '  "scenes": [\n'
    "    {\n"
    '      "keyframe_prompt": "该场景首帧静态画面描述，沿用同一主角的固定外貌/发型/服装，写清场景、构图、姿态，30-90字",\n'
    '      "scene_description": "该场景视频的动态描述(同一主角在什么场景做什么动作/镜头运动/氛围)，20-60字",\n'
    '      "narration": "该场景的旁白解说文本，可为空；不要把角色对白硬改成解说旁白，0-100字",\n'
    '      "required_elements": ["本场景必须清晰出现的主体或关键元素"],\n'
    '      "forbidden_elements": ["本场景不得出现的主体或干扰元素"],\n'
    '      "duration_hint": 5\n'
    "    }\n"
    "  ]\n"
    "}\n"
    "\n"
    "duration_hint 单位秒(建议3-15)。有旁白时，按朗读时长再加1-2秒余量；无旁白时，按画面动作需要估算。"
)


def _extract_json(raw_text: str) -> str:
    text = (raw_text or "").strip()
    # 1) 剥离 ```json ... ``` 或 ``` ... ``` 代码块围栏
    m = re.search(r"```(?:json)?\s*([\s\S]*?)```", text)
    if m:
        text = m.group(1).strip()
    # 2) 截取第一个 { 到最后一个 } 之间(去掉前后解释文字)
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end > start:
        text = text[start:end + 1]
    # 3) 去掉尾随逗号
    text = re.sub(r",\s*([}\]])", r"\1", text)
    return text


_HUMAN_KEYWORDS = ("人类", "人", "女人", "男人", "女孩", "男孩", "女子", "男子", "少女", "少年")
_DOG_KEYWORDS = ("边境牧羊犬", "牧羊犬", "狗", "犬")
_DRAGON_KEYWORDS = ("金龙", "龙", "神龙")
_NONHUMAN_KEYWORDS = (
    "狗", "犬", "猫", "龙", "狐", "狼", "鸟", "马", "鹿", "兔", "熊", "虎", "狮",
    "鱼", "鲸", "蛇", "龟", "鹤", "凤凰", "麒麟",
)


def _normalize_elements(raw_value) -> list[str]:
    if isinstance(raw_value, str):
        values = [raw_value]
    elif isinstance(raw_value, list):
        values = raw_value
    else:
        values = []
    result = []
    for item in values:
        cleaned = clean_text(item, max_len=40)
        if cleaned and cleaned not in result:
            result.append(cleaned)
    return result[:4]


def _infer_required_elements(char_ref: str, scene_desc: str, keyframe_prompt: str) -> list[str]:
    text = f"{char_ref} {scene_desc} {keyframe_prompt}"
    required = []
    if any(k in text for k in _DOG_KEYWORDS):
        required.append("边境牧羊犬" if "边境牧羊犬" in text or "牧羊犬" in text else "狗")
    if any(k in scene_desc or k in keyframe_prompt for k in _DRAGON_KEYWORDS):
        required.append("金龙" if "金龙" in scene_desc or "金龙" in keyframe_prompt else "龙")
    return required[:4]


def _infer_forbidden_elements(char_ref: str, scene_desc: str, keyframe_prompt: str, required: list[str]) -> list[str]:
    text = f"{scene_desc} {keyframe_prompt}"
    forbidden = []
    nonhuman_protagonist = any(k in char_ref for k in _NONHUMAN_KEYWORDS) and not any(k in char_ref for k in _HUMAN_KEYWORDS)
    mentions_human = any(k in text for k in _HUMAN_KEYWORDS)
    if nonhuman_protagonist and not mentions_human:
        forbidden.append("人类")
    if any(el in ("金龙", "龙") for el in required):
        forbidden.append("只用金光代替龙")
    return forbidden[:4]


def parse_breakdown(raw_text: str, frame_rate: int = 24, narration_mode: str = NARRATION_MODE_VOICE):
    """解析模型返回的分镜 JSON，返回 (character_reference_prompt, [Scene])。失败抛 PipelineError。"""
    text = _extract_json(raw_text)
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        raise PipelineError(f"分镜拆解结果解析失败，模型输出格式异常：{e}")

    if not isinstance(data, dict):
        raise PipelineError("分镜拆解结果格式异常。")
    char_ref = clean_text(data.get("character_reference", ""), max_len=180)
    if unrepaired_mojibake_score(char_ref) >= 3:
        raise PipelineError("分镜结果中的角色提示词疑似乱码，已停止生成以避免产出异常画面。请重试或检查模型输出编码。")
    scenes_raw = data.get("scenes")
    if not isinstance(scenes_raw, list) or not scenes_raw:
        raise PipelineError("分镜拆解结果里没有找到有效的 scenes 列表。")

    scenes = []
    for i, s in enumerate(scenes_raw):
        if not isinstance(s, dict):
            continue
        scene_desc = clean_text(s.get("scene_description", ""), max_len=220)
        narration = clean_text(s.get("narration", ""), max_len=260) if narration_mode != NARRATION_MODE_NONE else ""
        if unrepaired_mojibake_score(scene_desc) >= 3:
            raise PipelineError(f"第 {len(scenes) + 1} 个场景提示词疑似乱码，已停止生成。请重试或检查模型输出编码。")
        if narration and unrepaired_mojibake_score(narration) >= 3:
            narration = ""
        try:
            dur = float(s.get("duration_hint", 5) or 5)
        except (TypeError, ValueError):
            dur = 5.0
        dur = max(2.0, min(dur, 18.0))  # 单场景不超过模型上限约18秒
        keyframe_prompt = (
            clean_text(s.get("keyframe_prompt", ""), max_len=260)
            or clean_text(s.get("scene_keyframe_prompt", ""), max_len=260)
            or scene_desc
        )
        if unrepaired_mojibake_score(keyframe_prompt) >= 3:
            raise PipelineError(f"第 {len(scenes) + 1} 个场景关键帧提示词疑似乱码，已停止生成。请重试或检查模型输出编码。")
        required_elements = _normalize_elements(s.get("required_elements"))
        if not required_elements:
            required_elements = _infer_required_elements(char_ref, scene_desc, keyframe_prompt)
        forbidden_elements = _normalize_elements(s.get("forbidden_elements"))
        if not forbidden_elements:
            forbidden_elements = _infer_forbidden_elements(char_ref, scene_desc, keyframe_prompt, required_elements)
        sc = Scene(
            index=len(scenes),
            scene_description=scene_desc,
            keyframe_prompt=keyframe_prompt,
            narration=narration,
            required_elements=required_elements,
            forbidden_elements=forbidden_elements,
            duration_hint=dur,
            num_frames=duration_to_num_frames(dur, frame_rate),
        )
        scenes.append(sc)

    if not scenes:
        raise PipelineError("分镜拆解结果为空。")
    return char_ref, scenes


def breakdown_script(cfg, script_text: str, scene_count_hint: int = 6,
                     frame_rate: int = 24, narration_mode: str = NARRATION_MODE_VOICE):
    """调用对话 Provider 把剧本拆成 (角色参考图prompt, 场景列表)。"""
    if not script_text or not script_text.strip():
        raise PipelineError("剧本内容为空。")

    provider = registry.build_provider(cfg.active_provider, cfg)

    count_hint = (
        f"请拆解为大约 {scene_count_hint} 个场景。"
        if scene_count_hint and scene_count_hint > 0
        else "请根据内容长度和情节自行决定合理的场景数量(建议3-10个)。"
    )
    if narration_mode == NARRATION_MODE_NONE:
        narration_instruction = "不需要旁白和字幕：每个场景的 narration 必须输出空字符串，画面通过动作、环境和角色对话感表达。"
    elif narration_mode == NARRATION_MODE_SUBTITLES:
        narration_instruction = "只需要字幕文本，不需要配音：为每个场景生成适合直接显示为字幕的简洁 narration，可为空。"
    else:
        narration_instruction = "需要旁白+字幕：为每个场景生成自然的 narration，可为空，不要把角色对白强行改成旁白。"
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": f"{count_hint}\n{narration_instruction}\n\n剧本内容：\n{script_text.strip()}"},
    ]
    result = provider.chat(messages, timeout=120)
    if not result.content:
        raise PipelineError("分镜拆解未返回内容(模型可能超时或额度不足)。")
    return parse_breakdown(result.content, frame_rate, narration_mode)
