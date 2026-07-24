#!/usr/bin/env python3
"""Shared structural limits for first-party NB source files."""
from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAX_SOURCE_LINES = 1000
SOURCE_SUFFIXES = {".c", ".h", ".inc", ".py", ".sh", ".cmake", ".yml", ".yaml"}
SOURCE_ROOTS = (ROOT / "src", ROOT / "tools", ROOT / "scripts")


def first_party_sources() -> list[pathlib.Path]:
    files = [
        path
        for source_root in SOURCE_ROOTS
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    ]
    files.append(ROOT / "CMakeLists.txt")
    return sorted(set(files))


def line_count(path: pathlib.Path) -> int:
    with path.open("r", encoding="utf-8") as stream:
        return sum(1 for _ in stream)


def enforce_source_line_limit() -> None:
    oversized = [(path, line_count(path)) for path in first_party_sources()]
    oversized = [(path, count) for path, count in oversized if count > MAX_SOURCE_LINES]
    if oversized:
        detail = ", ".join(f"{path.relative_to(ROOT)}={count}" for path, count in oversized)
        raise RuntimeError(f"first-party source exceeds {MAX_SOURCE_LINES} lines: {detail}")


def node_source_text() -> str:
    paths = [ROOT / "src" / "nb_node.c", *sorted((ROOT / "src").glob("nb_node_*.inc"))]
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


def main() -> None:
    enforce_source_line_limit()
    print(f"SOURCE LINE GATE PASS (max={MAX_SOURCE_LINES})")


if __name__ == "__main__":
    main()
