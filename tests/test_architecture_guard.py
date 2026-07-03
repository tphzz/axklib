from __future__ import annotations

from pathlib import Path

from axklib.dev.architecture_inventory import (
    KNOWN_INVENTORY_ONLY_VIOLATIONS,
    inventory_repository,
)

ROOT = Path(__file__).resolve().parents[1]


def test_new_library_code_does_not_import_public_tools() -> None:
    inventory = inventory_repository(ROOT)
    violations = []
    for record in inventory.imports:
        if record.disposition == "inventory-only-known-violation":
            continue
        if record.category == "axklib_package":
            violations.append(record)

    assert violations == []


def test_no_known_axklib_container_inversion_remains_after_plan_003() -> None:
    inventory = inventory_repository(ROOT)
    observed = {
        (record.path, record.imported)
        for record in inventory.imports
        if record.disposition == "inventory-only-known-violation"
    }

    assert KNOWN_INVENTORY_ONLY_VIOLATIONS == set()
    assert observed == set()


def test_sys_path_mutations_are_not_used_in_public_code_or_tests() -> None:
    inventory = inventory_repository(ROOT)
    observed = {(record.path, record.line) for record in inventory.sys_path_mutations}

    assert observed == set()
