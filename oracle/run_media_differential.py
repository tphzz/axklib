"""Compare native FAT12, ISO9660, and standalone reads with the Python oracle."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from io import BytesIO
from pathlib import Path
from typing import Any

from axklib.containers import load_objects
from axklib.containers import open as open_container
from axklib.content_tree import build_content_tree_for_container
from axklib.relationships import build_relationship_graph

SECTOR_SIZE = 512
TOTAL_SECTORS = 2880
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
ROOT_OFFSET = (1 + 2 * SECTORS_PER_FAT) * SECTOR_SIZE
DATA_OFFSET = ROOT_OFFSET + ((ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE


def _le16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "little")


def _le32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "little")


def _set_fat12(buf: bytearray, cluster: int, value: int) -> None:
    for fat_index in range(2):
        fat_offset = (1 + fat_index * SECTORS_PER_FAT) * SECTOR_SIZE
        byte_index = fat_offset + cluster + cluster // 2
        pair = buf[byte_index] | (buf[byte_index + 1] << 8)
        if cluster & 1:
            pair = (pair & 0x000F) | ((value & 0x0FFF) << 4)
        else:
            pair = (pair & 0xF000) | (value & 0x0FFF)
        buf[byte_index] = pair & 0xFF
        buf[byte_index + 1] = pair >> 8


def _fat_image(payloads: list[bytes]) -> bytes:
    buf = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    buf[0:3] = b"\xeb\x3c\x90"
    buf[3:11] = b"YAMAHA  "
    _le16(buf, 0x0B, SECTOR_SIZE)
    buf[0x0D] = 1
    _le16(buf, 0x0E, 1)
    buf[0x10] = 2
    _le16(buf, 0x11, ROOT_ENTRIES)
    _le16(buf, 0x13, TOTAL_SECTORS)
    buf[0x15] = 0xF0
    _le16(buf, 0x16, SECTORS_PER_FAT)
    _le16(buf, 0x18, 18)
    _le16(buf, 0x1A, 2)
    buf[0x36:0x3E] = b"FAT12   "
    buf[510:512] = b"\x55\xaa"
    for fat_index in range(2):
        fat_offset = (1 + fat_index * SECTORS_PER_FAT) * SECTOR_SIZE
        buf[fat_offset : fat_offset + 3] = b"\xf0\xff\xff"

    cluster = 2
    for index, payload in enumerate(payloads):
        count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        first = cluster
        for relative in range(count):
            current = first + relative
            _set_fat12(buf, current, 0xFFF if relative + 1 == count else current + 1)
        root = ROOT_OFFSET + index * 32
        buf[root : root + 8] = f"OBJ{index:05d}".encode("ascii")
        buf[root + 8 : root + 11] = b"A3K"
        buf[root + 0x0B] = 0x20
        _le16(buf, root + 0x1A, first)
        _le32(buf, root + 0x1C, len(payload))
        start = DATA_OFFSET + (first - 2) * SECTOR_SIZE
        buf[start : start + len(payload)] = payload
        cluster += count
    return bytes(buf)


def _write_iso(path: Path, payloads: list[bytes]) -> None:
    import pycdlib

    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, vol_ident="AXKORACLE")
    iso.add_directory(iso_path="/GROUP")
    iso.add_directory(iso_path="/GROUP/F001")
    for index, payload in enumerate(payloads):
        iso.add_fp(
            BytesIO(payload),
            len(payload),
            iso_path=f"/GROUP/F001/O{index:05d}.A3K;1",
        )
    iso.write(str(path))
    iso.close()


def _cpp(cpp_cli: Path, command: str, image: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="axklib-media-oracle-") as output:
        arguments = (
            [str(cpp_cli), "info", str(image), "--format", "json"]
            if command == "tree"
            else [str(cpp_cli), command, str(image), "--output-dir", output]
        )
        process = subprocess.run(arguments, check=False, capture_output=True, text=True)
        if process.returncode != 0:
            raise RuntimeError(
                f"native {command} failed for {image} with {process.returncode}: "
                f"{process.stderr.strip()}"
            )
        if command == "tree":
            return dict(json.loads(process.stdout)["trees"][0])
        rows = json.loads((Path(output) / f"{command}.json").read_text())
        return {command: rows}


def _cpp_semantic(cpp_cli: Path, image: Path) -> dict[str, Any] | None:
    executable = cpp_cli.with_name("axk_media_semantic_dump")
    if not executable.is_file():
        return None
    process = subprocess.run([str(executable), str(image)], check=False, capture_output=True, text=True)
    if process.returncode != 0:
        raise RuntimeError(
            f"native media summary failed for {image} with {process.returncode}: "
            f"{process.stderr.strip()}"
        )
    return dict(json.loads(process.stdout))


def _python_objects(path: Path, kind: str) -> list[Any]:
    return load_objects(path, kind)


def _object_rows_python(items: list[Any]) -> list[tuple[str, str, int, int]]:
    return sorted((item.type, item.name, item.payload_offset or 0, item.payload_size) for item in items)


def _object_rows_cpp(value: dict[str, Any]) -> list[tuple[str, str, int, int]]:
    return sorted(
        (
            str(row["object_type"]),
            str(row["object_name"]),
            int(row["payload_offset"]),
            int(row["payload_size"]),
        )
        for row in value["objects"]
    )


def _relationship_rows_python(items: list[Any]) -> list[tuple[str, str, str, str, str]]:
    graph = build_relationship_graph(items)
    identity = {item.object_key: (item.type, item.name) for item in items}
    rows = []
    for row in graph.relationships:
        target = identity.get(row.target_key, ("", ""))
        rows.append(
            (
                f"{identity[row.source_key][0]}:{identity[row.source_key][1]}",
                f"{target[0]}:{target[1]}",
                row.relationship_type,
                row.quality,
                row.basis,
            )
        )
    return sorted(rows)


def _relationship_rows_cpp(objects: dict[str, Any], relationships: dict[str, Any]) -> list[tuple[str, str, str, str, str]]:
    identity = {
        str(row["object_key"]): (str(row["object_type"]), str(row["object_name"]))
        for row in objects["objects"]
    }
    rows = []
    for row in relationships["relationships"]:
        source = identity[str(row["source_key"])]
        target = identity.get(str(row["target_key"] or ""), ("", ""))
        rows.append(
            (
                f"{source[0]}:{source[1]}",
                f"{target[0]}:{target[1]}",
                str(row["relationship_type"]),
                str(row["quality"]),
                str(row["basis"]),
            )
        )
    return sorted(rows)


def _tree_rows_python(path: Path) -> list[tuple[str, str, str]]:
    tree = build_content_tree_for_container(open_container(path))
    rows: list[tuple[str, str, str]] = []

    def visit(node: Any) -> None:
        rows.append((node.node_type, node.display_name, node.object_type))
        for child in node.children:
            visit(child)

    for root in tree.roots:
        visit(root)
    return sorted(rows)


def _tree_rows_cpp(value: dict[str, Any]) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []

    def visit(node: dict[str, Any]) -> None:
        rows.append(
            (str(node["node_type"]), str(node["display_name"]), str(node["object_type"]))
        )
        for child in node["children"]:
            visit(dict(child))

    for root in value["roots"]:
        visit(dict(root))
    return sorted(rows)


def _case(cpp_cli: Path, image: Path, kind: str) -> dict[str, Any]:
    python_items = _python_objects(image, kind)
    python_object_rows = _object_rows_python(python_items)
    python_relationship_rows = _relationship_rows_python(python_items)
    python_tree_rows = _tree_rows_python(image)
    semantic = _cpp_semantic(cpp_cli, image)
    if semantic is None:
        cpp_objects = _cpp(cpp_cli, "objects", image)
        cpp_relationships = _cpp(cpp_cli, "relationships", image)
        cpp_tree = _cpp(cpp_cli, "tree", image)
        cpp_object_rows = _object_rows_cpp(cpp_objects)
        cpp_relationship_rows = _relationship_rows_cpp(cpp_objects, cpp_relationships)
        cpp_tree_rows = _tree_rows_cpp(cpp_tree)
    else:
        cpp_object_rows = sorted(tuple(row) for row in semantic["objects"])
        cpp_relationship_rows = sorted(tuple(row) for row in semantic["relationships"])
        cpp_tree_rows = sorted(tuple(row) for row in semantic["tree"])
    return {
        "container": kind,
        "object_count": len(python_items),
        "relationship_count": len(python_relationship_rows),
        "objects_match": python_object_rows == cpp_object_rows,
        "relationships_match": python_relationship_rows == cpp_relationship_rows,
        "tree_match": python_tree_rows == cpp_tree_rows,
        "python_relationships": python_relationship_rows,
        "cpp_relationships": cpp_relationship_rows,
        "python_tree_node_count": len(python_tree_rows),
        "cpp_tree_node_count": len(cpp_tree_rows),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    source = load_objects(args.fixture, "sfs")
    payloads = [item.payload for item in source]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fat_path = root / "oracle.ima"
        iso_path = root / "oracle.iso"
        standalone_path = root / "oracle.a3k"
        fat_path.write_bytes(_fat_image(payloads))
        _write_iso(iso_path, payloads)
        standalone_path.write_bytes(payloads[0])
        results = [
            _case(args.cpp_cli, fat_path, "fat12_floppy"),
            _case(args.cpp_cli, iso_path, "iso"),
            _case(args.cpp_cli, standalone_path, "standalone_object"),
        ]

    success = all(
        row["objects_match"] and row["relationships_match"] and row["tree_match"]
        for row in results
    )
    report = {
        "schema_version": "1.0",
        "operation": "fat-iso-standalone-read",
        "fixture": args.fixture.as_posix(),
        "success": success,
        "results": results,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
