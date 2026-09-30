from __future__ import annotations

import pathlib
import subprocess
import tempfile

import nb_release


def run(root: pathlib.Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True, text=True)
    return result.stdout.strip()


with tempfile.TemporaryDirectory(prefix="nb-release-git-") as directory:
    root = pathlib.Path(directory)
    run(root, "init")
    (root / "CMakeLists.txt").write_text("project(nb)\n", encoding="utf-8")
    run(root, "add", ".")
    run(root, "-c", "user.email=test@example.com", "-c", "user.name=test", "commit", "-m", "initial")
    commit = run(root, "rev-parse", "HEAD")
    metadata = nb_release.git_metadata(root, commit)
    assert metadata["commit"] == commit
    assert len(metadata["tree"]) == 40

    (root / "untracked.txt").write_text("must fail\n", encoding="utf-8")
    metadata = nb_release.git_metadata(root, require_clean=False)
    assert metadata["dirty"] is True
    try:
        nb_release.git_metadata(root, require_clean=True)
    except ValueError as error:
        assert "clean" in str(error)
    else:
        raise AssertionError("dirty Git worktree was accepted")

print("nb_release git validation passed")
