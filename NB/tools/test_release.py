#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import tempfile

import nb_release


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="nb-release-") as temporary:
        root = pathlib.Path(temporary)
        binary = root / "build" / "nb_node"
        source = root / "src" / "node.c"
        topology = root / "tools" / "hosts.json"
        profile = root / "tools" / "profile.json"
        runtime = root / "tools" / "runtime.json"
        for path, data in (
            (binary, b"ELF-test-binary"),
            (source, b"int main(void){return 0;}\n"),
            (topology, b"{}\n"),
            (profile, b"{}\n"),
            (runtime, b'{"log_dir":"/var/log/nb"}\n'),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

        manifest = nb_release.create_manifest(
            root, binary, "linux-x86_64", {"src/node.c": source}, topology, profile,
            {"tools/runtime.json": runtime},
        )
        snapshot = nb_release.snapshot_inputs(root, {"src/node.c": source})
        assert snapshot == manifest["inputs"]
        manifest_path = root / "build" / "release-manifest.json"
        nb_release.write_manifest(manifest_path, manifest)
        loaded = nb_release.load_and_validate_manifest(manifest_path, root, binary)
        assert loaded["release_id"] == nb_release.sha256_file(binary)[:16]
        assert loaded["deployment_id"].startswith(loaded["release_id"] + "-")
        assert loaded["node_version"] == {"product": "V200R001C02", "semantic": "2.1.2"}

        # Manifests from before the input split may contain deployment Python;
        # changing that orchestration code must not invalidate the Node binary.
        legacy_runtime = root / "tools" / "deploy_shard_runtime.py"
        legacy_runtime.write_bytes(b"old orchestration\n")
        legacy = dict(manifest)
        legacy["inputs"] = manifest["inputs"] + [nb_release._file_record(root, legacy_runtime)]
        legacy["source_digest"] = nb_release._source_digest(legacy["inputs"])
        legacy_path = root / "build" / "legacy-release-manifest.json"
        nb_release.write_manifest(legacy_path, legacy)
        legacy_runtime.write_bytes(b"new orchestration\n")
        assert nb_release.load_and_validate_manifest(legacy_path, root, binary)["release_id"] == loaded["release_id"]

        with tempfile.TemporaryDirectory(prefix="nb-line-state-") as external:
            external_root = pathlib.Path(external)
            external_topology = external_root / "bootstrap-hosts.json"
            external_profile = external_root / "bootstrap-profile.json"
            external_topology.write_bytes(topology.read_bytes())
            external_profile.write_bytes(profile.read_bytes())
            external_manifest = nb_release.create_manifest(
                root, binary, "linux-x86_64", {"src/node.c": source},
                external_topology, external_profile,
                {"tools/runtime.json": runtime},
            )
            external_manifest_path = root / "build" / "external-release-manifest.json"
            nb_release.write_manifest(external_manifest_path, external_manifest)
            validated = nb_release.load_and_validate_manifest(
                external_manifest_path, root, binary,
                external_topology, external_profile,
            )
            assert validated["deployment_id"] == external_manifest["deployment_id"]
            external_topology.write_bytes(b'{"changed":true}\n')
            try:
                nb_release.load_and_validate_manifest(
                    external_manifest_path, root, binary,
                    external_topology, external_profile,
                )
            except ValueError as error:
                assert "topology" in str(error)
            else:
                raise AssertionError("external topology change was accepted")

        alternate_topology=root/"tools"/"hosts-alt.json";alternate_topology.write_bytes(b'{"line":"alternate"}\n')
        alternate=nb_release.create_manifest(root,binary,"linux-x86_64",{"src/node.c":source},alternate_topology,profile)
        assert alternate["release_id"]==loaded["release_id"] and alternate["deployment_id"]!=loaded["deployment_id"]

        runtime.write_bytes(b'{"log_dir":"/var/log/nb-next"}\n')
        runtime_changed=nb_release.create_manifest(
            root,binary,"linux-x86_64",{"src/node.c":source},topology,profile,
            {"tools/runtime.json":runtime},
        )
        assert runtime_changed["release_id"]==loaded["release_id"]
        assert runtime_changed["deployment_id"]!=loaded["deployment_id"]
        runtime.write_bytes(b'{"log_dir":"/var/log/nb"}\n')

        source.write_bytes(b"changed\n")
        try:
            nb_release.load_and_validate_manifest(manifest_path, root, binary)
        except ValueError as error:
            assert "重新构建" in str(error)
        else:
            raise AssertionError("被修改的构建输入未被拒绝")

        source.write_bytes(b"int main(void){return 0;}\n")
        topology.write_bytes(b'{"changed":true}\n')
        try:
            nb_release.load_and_validate_manifest(manifest_path, root, binary)
        except ValueError as error:
            assert "topology" in str(error)
        else:
            raise AssertionError("被修改的拓扑未被拒绝")

        removals = nb_release.select_release_removals(
            [("aaaaaaaaaaaaaaaa", 4), ("bbbbbbbbbbbbbbbb", 3),
             ("legacy-cccccccccccccccc", 2), ("not-a-release", 1)],
            {"legacy-cccccccccccccccc"}, 2,
        )
        assert removals == []
        removals = nb_release.select_release_removals(
            [("aaaaaaaaaaaaaaaa", 4), ("bbbbbbbbbbbbbbbb", 3),
             ("cccccccccccccccc", 2), ("dddddddddddddddd", 1)],
            {"dddddddddddddddd"}, 2,
        )
        assert removals == ["cccccccccccccccc"]
    print("RESULT PASS")


if __name__ == "__main__":
    main()
