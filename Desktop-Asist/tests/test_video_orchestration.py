import json
import os
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

from src.providers.agnes import AgnesProvider
from src.video_pipeline import prompt_audit, script_breakdown, workdir
from src.video_pipeline.base import (
    CharacterProfile,
    DialogueLine,
    NARRATION_MODE_NONE,
    PipelineConfig,
    Scene,
    VisualAsset,
)
from src.video_pipeline.pipeline_worker import ScriptToVideoWorker


class _Result:
    def __init__(self, content):
        self.content = content


class _PlannerProvider:
    model = "agnes-2.0-flash"

    def build_vision_content(self, text, image_paths):
        return [{"type": "text", "text": text}]

    def chat(self, messages, timeout=180):
        return _Result(
            json.dumps(
                {
                    "global_style": "写实古装电影",
                    "continuity_rules": ["角色面容和服装保持一致"],
                    "scenes": [
                        {
                            "scene_index": 0,
                            "visual_state": "李铁柱右手只拿手机，桌上放包子和豆浆",
                            "camera_plan": "中景侧面，避免手部和正面口型特写",
                            "transition_plan": "承接上一镜动作终态",
                            "continuity_notes": "保持明黄色龙袍",
                        },
                        {"scene_index": 99, "visual_state": "不得新增镜头"},
                    ],
                },
                ensure_ascii=False,
            )
        )


class VideoOrchestrationTests(unittest.TestCase):
    def test_multimodal_plan_only_enriches_existing_scene(self):
        characters = [
            CharacterProfile(character_id="li", name="李铁柱", outfit="明黄色龙袍")
        ]
        original_line = DialogueLine(character_id="li", character_name="李铁柱", text="我手机呢！")
        scenes = [
            Scene(
                index=0,
                title="穿越",
                required_character_ids=["li"],
                dialogue_lines=[original_line],
            )
        ]
        cfg = object()
        with patch.object(script_breakdown.registry, "build_provider", return_value=_PlannerProvider()):
            bible = script_breakdown.enrich_production_plan(cfg, "李铁柱：我手机呢！", characters, scenes)

        self.assertEqual([item.character_id for item in characters], ["li"])
        self.assertEqual(scenes[0].dialogue_lines[0].text, "我手机呢！")
        self.assertIn("右手只拿手机", scenes[0].visual_state)
        self.assertEqual(bible["planner_model"], "agnes-2.0-flash")

    def test_new_fields_round_trip_through_manifest(self):
        config = PipelineConfig(
            job_id="test_job",
            max_concurrent_scenes=4,
            production_bible={"global_style": "写实"},
        )
        scene = Scene(index=0, visual_state="固定服装", camera_plan="中景")
        with tempfile.TemporaryDirectory() as root:
            with patch.object(workdir, "VIDEO_JOBS_DIR", root):
                workdir.save_manifest(config, [scene])
                loaded_config, loaded_scenes = workdir.load_manifest("test_job")

        self.assertEqual(loaded_config.max_concurrent_scenes, 4)
        self.assertEqual(loaded_config.production_bible["global_style"], "写实")
        self.assertEqual(loaded_scenes[0].visual_state, "固定服装")

    def test_scene_scheduler_respects_parallel_limit(self):
        worker = ScriptToVideoWorker(object(), PipelineConfig(max_concurrent_scenes=3))
        worker.scenes = [Scene(index=index) for index in range(8)]
        state = {"active": 0, "peak": 0}
        lock = threading.Lock()

        def fake_process(scene):
            with lock:
                state["active"] += 1
                state["peak"] = max(state["peak"], state["active"])
            time.sleep(0.03)
            scene.status = "done"
            with lock:
                state["active"] -= 1

        worker._process_scene = fake_process
        worker._save_manifest = lambda: None
        worker._process_scenes_concurrently()

        self.assertEqual(state["peak"], 3)
        self.assertTrue(all(scene.status == "done" for scene in worker.scenes))

    def test_debug_mode_exports_video_prompt_without_submitting_video(self):
        class NoVideoProvider:
            def create_video_task(self, *args, **kwargs):
                raise AssertionError("video endpoint must not be called in debug mode")

        with tempfile.TemporaryDirectory() as root:
            reference_path = os.path.join(root, "character.png")
            with open(reference_path, "wb") as handle:
                handle.write(b"reference")
            config = PipelineConfig(
                job_id="debug_job",
                prompt_debug_mode=True,
                prompt_output_dir=root,
                narration_mode=NARRATION_MODE_NONE,
                character_profiles=[
                    CharacterProfile(
                        character_id="main",
                        name="主角",
                        appearance="黑发青年",
                        outfit="蓝色外套",
                        reference_path=reference_path,
                    ).to_dict()
                ],
            )
            worker = ScriptToVideoWorker(object(), config)
            scene = Scene(
                index=0,
                title="测试镜头",
                scene_description="人物缓慢向前走",
                required_character_ids=["main"],
            )
            worker.scenes = [scene]
            worker._ensure_scene_keyframe = lambda *args, **kwargs: None
            worker._do_scene_steps(NoVideoProvider(), scene, root)

            request_path = os.path.join(root, "scene_00_requests.json")
            text_path = os.path.join(root, "scene_00_prompts.txt")
            self.assertTrue(os.path.isfile(request_path))
            self.assertTrue(os.path.isfile(text_path))
            with open(request_path, "r", encoding="utf-8") as handle:
                request = json.load(handle)
            self.assertEqual(request["video_request_not_submitted"]["model"], "agnes-video-v2.0")
            self.assertIn("人物缓慢向前走", request["video_request_not_submitted"]["prompt"])

    def test_keyframes_keep_image_references_and_use_previous_frame_in_same_setting(self):
        with tempfile.TemporaryDirectory() as root:
            character_path = os.path.join(root, "character.png")
            previous_path = os.path.join(root, "previous.png")
            for path in (character_path, previous_path):
                with open(path, "wb") as handle:
                    handle.write(b"image")
            profile = CharacterProfile(
                character_id="main",
                name="李铁柱",
                appearance="短黑发青年",
                outfit="明黄色龙袍",
                reference_path=character_path,
            )
            worker = ScriptToVideoWorker(
                object(),
                PipelineConfig(character_profiles=[profile.to_dict()]),
            )
            previous = Scene(index=0, setting_description="养心殿", keyframe_path=previous_path)
            current = Scene(
                index=1,
                setting_description="养心殿",
                keyframe_prompt="李铁柱从床边站起",
                required_character_ids=["main"],
            )
            worker.scenes = [previous, current]

            prompt, references = worker._build_scene_keyframe_request(current, force_text_only=True)

            self.assertEqual(references[0], previous_path)
            self.assertIn(character_path, references)
            self.assertLess(len(prompt), 700)
            self.assertNotIn("固定音色", prompt)

    def test_prompt_audit_omits_base64_data(self):
        with tempfile.TemporaryDirectory() as root:
            prompt_audit.configure(root)
            prompt_audit.record("image_prompt", {"image": "data:image/png;base64," + "A" * 100})
            prompt_audit.disable()
            with open(os.path.join(root, "0001_image_prompt.json"), "r", encoding="utf-8") as handle:
                data = json.load(handle)
            self.assertIn("base64 omitted", data["payload"]["image"])
            self.assertNotIn("A" * 50, data["payload"]["image"])

    def test_script_metadata_is_not_treated_as_character_and_decorative_dragon_is_ignored(self):
        script = (
            "剧本：《这个皇帝不太冷》\n"
            "人物：\n李铁柱：现代男大学生。\n"
            "第一幕：穿越\n时间：夜晚\n地点：养心殿\n"
            "李铁柱（惊讶）：我的手机呢？\n"
            "他穿着龙袍，坐在龙床旁的龙椅上。"
        )
        characters = script_breakdown._augment_dialogue_speakers_from_script(script, [])
        self.assertEqual([item.name for item in characters], ["李铁柱"])
        self.assertNotIn(
            "只用金光代替龙",
            script_breakdown._extract_scene_forbidden_elements(script, characters),
        )
        self.assertIn(
            "只用金光代替龙",
            script_breakdown._extract_scene_forbidden_elements("一条金龙飞翔出现", characters),
        )

    def test_visual_asset_library_is_text_to_image_then_keyframe_uses_fixed_assets(self):
        class FakeImageProvider:
            def __init__(self):
                self.calls = []

            def generate_image(self, prompt, **kwargs):
                self.calls.append((prompt, kwargs))
                return {"url": f"https://example.invalid/{len(self.calls)}.png"}

        provider = FakeImageProvider()
        with tempfile.TemporaryDirectory() as root:
            character_path = os.path.join(root, "character.png")
            with open(character_path, "wb") as handle:
                handle.write(b"character")
            config = PipelineConfig(
                job_id="asset_job",
                script_text="李铁柱拿起量子罗盘。",
                character_profiles=[
                    CharacterProfile(
                        character_id="li",
                        name="李铁柱",
                        reference_path=character_path,
                    ).to_dict()
                ],
            )
            worker = ScriptToVideoWorker(object(), config)
            worker.scenes = [
                Scene(
                    index=0,
                    title="早朝",
                    setting_description="清晨的金銮殿",
                    keyframe_prompt="李铁柱坐在龙椅上",
                    required_character_ids=["li"],
                    required_elements=["龙椅", "玉玺"],
                )
            ]

            def fake_download(url, path, timeout=120):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as handle:
                    handle.write(b"asset")

            worker._download = fake_download
            inventory = [{
                "name": "量子罗盘",
                "kind": "object",
                "description": "银色圆盘与蓝色刻度",
                "scene_indexes": [0],
            }]
            with patch.object(workdir, "VIDEO_JOBS_DIR", root), patch(
                "src.video_pipeline.pipeline_worker.registry.build_provider", return_value=provider
            ), patch.object(
                script_breakdown, "extract_visual_asset_inventory", return_value=inventory
            ):
                worker._ensure_visual_asset_references()

            self.assertEqual(len(provider.calls), 4)
            self.assertTrue(all("image_paths" not in kwargs for _, kwargs in provider.calls))
            self.assertEqual(len(config.visual_assets), 4)
            prompt, references = worker._build_scene_keyframe_request(worker.scenes[0])
            self.assertIn(character_path, references)
            self.assertEqual(len(references), 5)
            self.assertIn("李铁柱坐在龙椅上", prompt)

    def test_asset_prompt_removes_holder_actions_and_forbids_hands_and_text(self):
        worker = ScriptToVideoWorker(object(), PipelineConfig(script_text="明代皇帝在金銮殿早朝"))
        description = worker._sanitize_asset_description("李铁柱手持的饮品容器及液体，白色瓷杯")
        asset = VisualAsset(
            asset_id="soy",
            kind="object",
            name="豆浆",
            description=description,
        )
        prompt = worker._build_visual_asset_prompt(asset)

        self.assertNotIn("手持", description)
        self.assertIn("白色瓷杯", description)
        self.assertIn("绝对没有人物、手、手臂", prompt)
        self.assertIn("乱码", prompt)
        self.assertIn("中国明代", prompt)

    def test_tail_scene_uses_distinct_chinese_settings(self):
        scene = Scene(
            index=0,
            title="尾声",
            setting_description="尾声",
            narration="三个月后，王丞相在长城脚下搬砖。",
            dialogue_lines=[
                DialogueLine(character_id="wang", character_name="王丞相", text="这皇帝的手机究竟是从什么地方弄来的？"),
                DialogueLine(character_id="li", character_name="李铁柱", text="热搜说穿越者如何搞定反派，朕这集早就看过了。"),
            ],
            required_character_ids=["wang", "li"],
        )
        scenes = script_breakdown._split_local_scene_by_dialogue_budget(scene, 24)

        self.assertEqual(len(scenes), 2)
        self.assertIn("明代长城脚下", scenes[0].setting_description)
        self.assertIn("明代皇宫内殿", scenes[1].setting_description)

    def test_agnes_image_request_keeps_public_reference_urls(self):
        provider = AgnesProvider(
            base_url="https://example.invalid/v1",
            api_key="test",
            model="agnes-2.0-flash",
        )
        with tempfile.TemporaryDirectory() as root:
            local_path = os.path.join(root, "local.png")
            with open(local_path, "wb") as handle:
                handle.write(b"image")
            response = Mock()
            response.raise_for_status.return_value = None
            response.json.return_value = {"data": [{"url": "https://example.invalid/out.png"}]}
            with patch("src.providers.agnes.requests.post", return_value=response) as post:
                provider.generate_image(
                    "test",
                    image_paths=["https://cdn.example/ref.png", local_path],
                    prefer_url=True,
                )

        images = post.call_args.kwargs["json"]["extra_body"]["image"]
        self.assertEqual(images[0], "https://cdn.example/ref.png")
        self.assertTrue(images[1].startswith("data:image/png;base64,"))
        self.assertEqual(post.call_args.kwargs["timeout"], 360)

    def test_permanent_http_errors_do_not_retry(self):
        worker = ScriptToVideoWorker(object(), PipelineConfig())
        self.assertFalse(worker._is_retryable_error(Exception("API Key 无效(HTTP 401)")))
        self.assertFalse(worker._is_retryable_error(Exception("API 请求失败 (HTTP 400)")))
        self.assertTrue(worker._is_retryable_error(Exception("图像生成超时")))
        self.assertTrue(worker._is_retryable_error(Exception("HTTP 429 限流")))


if __name__ == "__main__":
    unittest.main()
