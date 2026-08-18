#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import tempfile

import controlplane_source_sync


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="nb-controlplane-source-") as temporary:
        root = pathlib.Path(temporary)
        first = root / "src" / "first.c"
        second = root / "tools" / "second.py"
        first.parent.mkdir(parents=True)
        second.parent.mkdir(parents=True)
        first.write_text("first\n", encoding="ascii")
        second.write_text("second\n", encoding="ascii")
        manifest = controlplane_source_sync.source_manifest({
            "src/first.c": first,
            "tools/second.py": second,
        })
        assert list(manifest) == ["src/first.c", "tools/second.py"]
        assert all(len(digest) == 64 for digest in manifest.values())
        try:
            controlplane_source_sync.source_manifest({
                "src/first.c": first,
                "tools/missing.py": root / "missing.py",
            })
        except FileNotFoundError as error:
            assert "tools/missing.py" in str(error)
        else:
            raise AssertionError("missing source input was accepted")
    deployment_files = controlplane_source_sync.deployment_source_files()
    assert "tools/nb_p1_control.py" in deployment_files
    assert "tools/worker_snapshot.py" in deployment_files
    print("RESULT PASS")


if __name__ == "__main__":
    main()
