"""Compare native and Python structured audio and SFZ products."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from axklib.audio import WavExportRequest, export_waveforms, iter_waveforms
from axklib.cli import _enrich_waveform_parameter_metadata, _wave_export_context
from axklib.containers import open as open_container
from axklib.sfz import SfzExportRequest, export_sfz


def _python_export(image: Path, output: Path) -> None:
    container = open_container(image)
    results = [SimpleNamespace(container=container)]
    placements, relationships = _wave_export_context(results)
    waveforms = _enrich_waveform_parameter_metadata(
        iter_waveforms(container), results, relationships
    )
    export_waveforms(
        WavExportRequest(
            output_dir=output,
            waveforms=waveforms,
            placements=placements,
            relationships=relationships,
        )
    )
    graphs = [json.loads(path.read_text(encoding="utf-8")) for path in output.rglob("volume.axklib.json")]
    export_sfz(SfzExportRequest(output_dir=output, volume_graphs=graphs))


def _source_key(value: str) -> str:
    return value.split("\u241f", 1)[-1]


def _normalize_graph(graph: dict[str, Any]) -> dict[str, Any]:
    objects = graph["objects"]
    id_to_key: dict[str, str] = {}
    for kind in ("smpl", "sbnk", "sbac", "prog"):
        for row in objects.get(kind, []):
            id_to_key[str(row.get("id", ""))] = str(row.get("object_key", ""))

    smpl = []
    for row in objects.get("smpl", []):
        origin = row.get("origin", {})
        smpl.append(
            {
                "object_key": row["object_key"],
                "display_name": row["display_name"],
                "wav_path": row["wav_path"],
                "audio": row["audio"],
                "playback": {
                    key: row["playback"].get(key)
                    for key in (
                        "root_key_midi",
                        "fine_tune_cents",
                        "loop_mode_raw",
                        "loop_mode_label",
                        "loop_start_frame",
                        "loop_length_frames",
                        "loop_end_frame_a4000_ui",
                    )
                },
                "origin": {
                    "container_kind": origin.get("container_kind"),
                    "partition_index": origin.get("partition_index"),
                    "quality": origin.get("quality"),
                    "alternating_byte_payload_detected": origin.get(
                        "alternating_byte_payload_detected"
                    ),
                },
            }
        )

    sbnk = []
    for row in objects.get("sbnk", []):
        members = []
        for member in row.get("physical_waveforms", []):
            members.append(
                {
                    "role": member["role"],
                    "waveform_key": id_to_key.get(str(member.get("smpl_id", "")), str(member.get("smpl_id", ""))),
                    "wav_path": member["wav_path"],
                    "quality": member.get("relationship_quality"),
                }
            )
        rendered = row.get("rendered_audio")
        parameters = row.get("parameters", {})
        contexts = parameters.get("decoded_current_sbnk_member_parameters", [])
        if isinstance(contexts, dict):
            parameter_rows = [contexts]
        else:
            parameter_rows = [
                context.get("member_parameters", {}) for context in contexts
            ]
        sbnk.append(
            {
                "object_key": row["object_key"],
                "display_name": row["display_name"],
                "members": members,
                "rendered_wav_path": rendered.get("wav_path") if rendered else None,
                "parameter_rows": parameter_rows,
                "resolved_key_range": parameters.get("resolved_key_range"),
            }
        )

    sbac = []
    for row in objects.get("sbac", []):
        sbac.append(
            {
                "object_key": row["object_key"],
                "display_name": row["display_name"],
                "members": sorted(
                    {
                        id_to_key.get(
                            str(item.get("sbnk_id", "")), str(item.get("sbnk_id", ""))
                        )
                        for item in row.get("members", [])
                    }
                ),
            }
        )

    relationships = []
    for row in graph.get("relationships", []):
        source = row.get("source_key") or _source_key(str(row.get("source_ref", "")))
        if "target_key" in row:
            targets = [row["target_key"]] if row["target_key"] else []
        else:
            targets = [_source_key(str(value)) for value in row.get("target_refs", [])]
        relationships.append(
            {
                "type": row["relationship_type"],
                "source": source,
                "targets": targets,
                "quality": row["quality"],
                "basis": row["basis"],
            }
        )
    return {
        "volume": {
            key: graph["volume"].get(key)
            for key in ("path", "partition_index", "partition_name", "name", "placement_quality")
        },
        "smpl": sorted(smpl, key=lambda row: row["object_key"]),
        "sbnk": sorted(sbnk, key=lambda row: row["object_key"]),
        "sbac": sorted(sbac, key=lambda row: row["object_key"]),
        "relationships": sorted(
            relationships,
            key=lambda row: (row["source"], row["type"], row["targets"]),
        ),
    }


def _products(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in root.rglob("*")
        if path.is_file() and path.name != "volume.axklib.json"
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    results: list[dict[str, object]] = []
    failed = False
    with tempfile.TemporaryDirectory(prefix="axklib-structured-parity-") as temporary:
        root = Path(temporary)
        for index, image in enumerate(args.images):
            python_root = root / f"python-{index}"
            cpp_root = root / f"cpp-{index}"
            _python_export(image, python_root)
            process = subprocess.run(
                [str(args.cpp_cli), "export", str(image), "--output-dir", str(cpp_root), "--sfz"],
                check=False,
                capture_output=True,
                text=True,
            )
            python_graphs = {
                path.parent.relative_to(python_root).as_posix(): _normalize_graph(
                    json.loads(path.read_text(encoding="utf-8"))
                )
                for path in python_root.rglob("volume.axklib.json")
            }
            cpp_graphs = {
                path.parent.relative_to(cpp_root).as_posix(): _normalize_graph(
                    json.loads(path.read_text(encoding="utf-8"))
                )
                for path in cpp_root.rglob("volume.axklib.json")
            }
            product_matches = _products(python_root) == _products(cpp_root)
            graph_matches = python_graphs == cpp_graphs
            matches = process.returncode == 0 and product_matches and graph_matches
            failed |= not matches
            results.append(
                {
                    "image": image.as_posix(),
                    "matches": matches,
                    "product_bytes_match": product_matches,
                    "normalized_graphs_match": graph_matches,
                    "python_graph": None if graph_matches else python_graphs,
                    "cpp_graph": None if graph_matches else cpp_graphs,
                    "cpp_stderr": process.stderr.strip(),
                }
            )
    report = {"schema_version": "1.0", "operation": "structured-audio-sfz", "results": results}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
