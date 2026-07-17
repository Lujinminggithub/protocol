"""
工作目录管理 - 每个成片任务一个目录，manifest.json 记录进度支持断点续跑。

~/.aisprite/video_jobs/<job_id>/
    manifest.json
    scene_00/ scene_01/ ...
    filelist.txt  full.srt  output.mp4
"""

import json
import os

from src.paths import USER_DATA_DIR
from src.video_pipeline.base import Scene, PipelineConfig

VIDEO_JOBS_DIR = os.path.join(USER_DATA_DIR, "video_jobs")


def job_dir(job_id: str) -> str:
    return os.path.join(VIDEO_JOBS_DIR, job_id)


def scene_dir(job_id: str, index: int) -> str:
    d = os.path.join(job_dir(job_id), f"scene_{index:02d}")
    os.makedirs(d, exist_ok=True)
    return d


def ensure_job_dir(job_id: str) -> str:
    d = job_dir(job_id)
    os.makedirs(d, exist_ok=True)
    return d


def manifest_path(job_id: str) -> str:
    return os.path.join(job_dir(job_id), "manifest.json")


def save_manifest(config: PipelineConfig, scenes: list):
    """把当前流水线状态原子写入 manifest.json。"""
    ensure_job_dir(config.job_id)
    data = {
        "config": config.to_dict(),
        "scenes": [s.to_dict() for s in scenes],
    }
    path = manifest_path(config.job_id)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def load_manifest(job_id: str):
    """读取 manifest，返回 (PipelineConfig, [Scene])；不存在返回 (None, None)。"""
    path = manifest_path(job_id)
    if not os.path.exists(path):
        return None, None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        return None, None
    config = PipelineConfig.from_dict(data.get("config", {}))
    scenes = [Scene.from_dict(s) for s in data.get("scenes", [])]
    return config, scenes
