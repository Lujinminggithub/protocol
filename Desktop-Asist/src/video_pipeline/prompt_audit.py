"""Thread-safe prompt/request audit output for script-video debugging."""

import json
import os
import threading
import time


_lock = threading.RLock()
_output_dir = ""
_sequence = 0


def configure(output_dir: str):
    global _output_dir, _sequence
    with _lock:
        _output_dir = output_dir or ""
        _sequence = 0
        if _output_dir:
            os.makedirs(_output_dir, exist_ok=True)
            with open(os.path.join(_output_dir, "all_prompts.jsonl"), "w", encoding="utf-8"):
                pass


def disable():
    global _output_dir
    with _lock:
        _output_dir = ""


def _sanitize(value):
    if isinstance(value, dict):
        return {key: _sanitize(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_sanitize(item) for item in value]
    if isinstance(value, str) and value.startswith("data:") and ";base64," in value:
        header = value.split(",", 1)[0]
        return f"<{header}, base64 omitted, {len(value)} chars>"
    return value


def record(kind: str, payload, label: str = ""):
    global _sequence
    with _lock:
        if not _output_dir:
            return
        _sequence += 1
        item = {
            "sequence": _sequence,
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "kind": kind,
            "label": label,
            "payload": _sanitize(payload),
        }
        stem = f"{_sequence:04d}_{kind}"
        with open(os.path.join(_output_dir, f"{stem}.json"), "w", encoding="utf-8") as handle:
            json.dump(item, handle, ensure_ascii=False, indent=2)
        with open(os.path.join(_output_dir, "all_prompts.jsonl"), "a", encoding="utf-8") as handle:
            handle.write(json.dumps(item, ensure_ascii=False) + "\n")
