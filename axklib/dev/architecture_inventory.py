"""Architecture import inventory helpers.

These helpers are intentionally independent from the axklib domain model. They are
used to keep migration debt visible while public tooling stays behind the
maintained `axklib` package.
"""

from __future__ import annotations

import ast
import csv
import json
from collections.abc import Iterable
from dataclasses import asdict, dataclass
from pathlib import Path

TOP_LEVEL_COMMAND_MODULES = {
    "axklib_hds_dump",
    "combine_sfs_stereo_waves",
    "export_sbnk_exact_audio",
    "extract_sfs_waves",
    "report_floppy_objects",
    "report_sbnk_links",
    "report_sfs_allocation",
    "report_sfs_inventory",
    "report_sfs_volume_validation",
    "scan_sfs_objects",
}

KNOWN_INVENTORY_ONLY_VIOLATIONS: set[tuple[str, str]] = set()

KNOWN_VIOLATION_REMOVAL_TARGET = "plan 011 - Test Import Boundary Cleanup"


@dataclass(frozen=True)
class ImportRecord:
    path: str
    line: int
    category: str
    imported: str
    import_kind: str
    disposition: str
    removal_target: str


@dataclass(frozen=True)
class SysPathMutationRecord:
    path: str
    line: int
    expression: str


@dataclass(frozen=True)
class ArchitectureInventory:
    imports: list[ImportRecord]
    sys_path_mutations: list[SysPathMutationRecord]


def iter_python_files(root: Path) -> Iterable[Path]:
    scanned_roots = [root / "axklib", root / "tests"]
    ignored_parts = {".git", ".pytest_cache", ".uv-cache", ".venv", "build"}
    for scan_root in scanned_roots:
        if not scan_root.exists():
            continue
        for path in scan_root.rglob("*.py"):
            if any(part in ignored_parts for part in path.parts):
                continue
            yield path


def module_roots_from_import(node: ast.AST) -> list[tuple[str, str]]:
    if isinstance(node, ast.Import):
        return [(alias.name.split(".", 1)[0], "import") for alias in node.names]
    if isinstance(node, ast.ImportFrom):
        if node.module is None:
            return []
        return [(node.module.split(".", 1)[0], "from")]
    return []


def is_sys_path_mutation(node: ast.AST) -> bool:
    if not isinstance(node, ast.Call):
        return False
    func = node.func
    if not isinstance(func, ast.Attribute):
        return False
    if func.attr not in {"insert", "append", "extend"}:
        return False
    value = func.value
    return (
        isinstance(value, ast.Attribute)
        and value.attr == "path"
        and isinstance(value.value, ast.Name)
        and value.value.id == "sys"
    )


def normalized_repo_path(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def categorize_path(repo_path: str) -> str:
    if repo_path.startswith("tools/"):
        return "removed_tool_path"
    if repo_path.startswith("tests/"):
        return "test"
    if repo_path.startswith("axklib/"):
        return "axklib_package"
    return "other"


def inventory_repository(root: Path) -> ArchitectureInventory:
    import_records: list[ImportRecord] = []
    sys_path_records: list[SysPathMutationRecord] = []

    for path in sorted(iter_python_files(root), key=lambda item: str(item).lower()):
        repo_path = normalized_repo_path(root, path)
        source = path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=repo_path)
        category = categorize_path(repo_path)

        for node in ast.walk(tree):
            for imported, import_kind in module_roots_from_import(node):
                if imported not in TOP_LEVEL_COMMAND_MODULES and imported != "tools":
                    continue
                key = (repo_path, imported)
                if key in KNOWN_INVENTORY_ONLY_VIOLATIONS:
                    disposition = "inventory-only-known-violation"
                    removal_target = KNOWN_VIOLATION_REMOVAL_TARGET
                else:
                    disposition = "migration-debt"
                    removal_target = ""
                import_records.append(
                    ImportRecord(
                        path=repo_path,
                        line=getattr(node, "lineno", 0),
                        category=category,
                        imported=imported,
                        import_kind=import_kind,
                        disposition=disposition,
                        removal_target=removal_target,
                    )
                )

            if is_sys_path_mutation(node):
                sys_path_records.append(
                    SysPathMutationRecord(
                        path=repo_path,
                        line=getattr(node, "lineno", 0),
                        expression=ast.unparse(node),
                    )
                )

    return ArchitectureInventory(
        imports=import_records,
        sys_path_mutations=sys_path_records,
    )


def write_inventory(inventory: ArchitectureInventory, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path = output_dir / "architecture_import_inventory.json"
    csv_path = output_dir / "architecture_import_inventory.csv"
    sys_path_csv = output_dir / "architecture_sys_path_mutations.csv"

    json_path.write_text(
        json.dumps(asdict(inventory), indent=2, sort_keys=True),
        encoding="utf-8",
    )

    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "path",
                "line",
                "category",
                "imported",
                "import_kind",
                "disposition",
                "removal_target",
            ],
        )
        writer.writeheader()
        for import_record in inventory.imports:
            writer.writerow(asdict(import_record))

    with sys_path_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["path", "line", "expression"])
        writer.writeheader()
        for sys_path_record in inventory.sys_path_mutations:
            writer.writerow(asdict(sys_path_record))
