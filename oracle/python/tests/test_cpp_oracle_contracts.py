from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
from typing import cast

import pytest
from oracle.python_object_semantic import semantic_value as object_semantic_value
from oracle.python_sfs_semantic import semantic_value
from oracle.run_external_corpus import main as external_corpus_main

ROOT = Path(__file__).resolve().parents[3]
CONTRACTS_PATH = ROOT / "oracle" / "contracts.json"
TOP_LEVEL_KEYS = {"schema_version", "oracle_commit", "contracts"}
CONTRACT_KEYS = {
    "id",
    "input_path",
    "input_sha256",
    "operation",
    "expected_semantic_sha256",
    "verification_class",
    "regeneration_command",
}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _semantic_bytes(path: Path) -> bytes:
    return (
        json.dumps(semantic_value(path), sort_keys=True, separators=(",", ":")) + "\n"
    ).encode()


def _contract_semantic_bytes(operation: str, path: Path) -> bytes:
    value = object_semantic_value(path) if operation == "current-object-read" else semantic_value(path)
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _validate(value: object) -> list[dict[str, str]]:
    if not isinstance(value, dict) or set(value) != TOP_LEVEL_KEYS:
        raise ValueError("oracle contract manifest has unknown or missing top-level fields")
    if value["schema_version"] != "1.0":
        raise ValueError("unsupported oracle contract schema")
    contracts = value["contracts"]
    if not isinstance(contracts, list):
        raise ValueError("oracle contracts must be a list")
    for contract in contracts:
        if not isinstance(contract, dict) or set(contract) != CONTRACT_KEYS:
            raise ValueError("oracle contract has unknown or missing fields")
        if not all(isinstance(item, str) for item in contract.values()):
            raise ValueError("oracle contract fields must be strings")
    return cast(list[dict[str, str]], contracts)


def test_cpp_oracle_contracts_are_strict_unique_and_current() -> None:
    value = json.loads(CONTRACTS_PATH.read_text(encoding="utf-8"))
    contracts = _validate(value)
    assert [item["id"] for item in contracts] == sorted(item["id"] for item in contracts)
    assert len({item["id"] for item in contracts}) == len(contracts)
    for contract in contracts:
        path = Path(str(contract["input_path"]))
        assert not path.is_absolute()
        resolved = ROOT / path
        assert _sha256(resolved.read_bytes()) == contract["input_sha256"]
        assert _sha256(_contract_semantic_bytes(contract["operation"], resolved)) == contract[
            "expected_semantic_sha256"
        ]


def test_cpp_oracle_contract_validation_rejects_unknown_fields_and_tampering() -> None:
    value = json.loads(CONTRACTS_PATH.read_text(encoding="utf-8"))
    unknown = copy.deepcopy(value)
    unknown["contracts"][0]["unexpected"] = True
    with pytest.raises(ValueError, match="unknown or missing"):
        _validate(unknown)

    contracts = _validate(value)
    tampered = bytearray(_semantic_bytes(ROOT / str(contracts[0]["input_path"])))
    tampered[-2] ^= 1
    assert _sha256(bytes(tampered)) != contracts[0]["expected_semantic_sha256"]


def test_external_corpus_is_optional_by_default_and_strict_when_requested(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    missing = tmp_path / "missing-corpus"
    cli = tmp_path / "unused-cli"
    monkeypatch.setattr(
        "sys.argv", ["run_external_corpus.py", "--root", str(missing), "--cpp-cli", str(cli)]
    )
    assert external_corpus_main() == 0
    monkeypatch.setattr(
        "sys.argv",
        [
            "run_external_corpus.py",
            "--root",
            str(missing),
            "--cpp-cli",
            str(cli),
            "--strict",
        ],
    )
    assert external_corpus_main() == 2
