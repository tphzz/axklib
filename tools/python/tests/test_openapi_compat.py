from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from openapi_compat import compatibility_issues, main


def contract() -> dict[str, object]:
    return {
        "openapi": "3.1.0",
        "paths": {
            "/items": {
                "post": {
                    "operationId": "items.create",
                    "security": [{"bearerAuth": []}],
                    "parameters": [
                        {
                            "name": "limit",
                            "in": "query",
                            "schema": {"type": "integer", "minimum": 1, "maximum": 100},
                        }
                    ],
                    "requestBody": {
                        "required": True,
                        "content": {
                            "application/json": {
                                "schema": {"$ref": "#/components/schemas/ItemRequest"}
                            }
                        },
                    },
                    "responses": {
                        "200": {
                            "content": {
                                "application/json": {
                                    "schema": {"$ref": "#/components/schemas/Item"}
                                }
                            }
                        },
                        "400": {"content": {"application/json": {"schema": {"type": "object"}}}},
                    },
                }
            }
        },
        "components": {
            "schemas": {
                "ItemRequest": {
                    "type": "object",
                    "additionalProperties": False,
                    "required": ["kind"],
                    "properties": {
                        "kind": {"enum": ["ONE", "TWO"]},
                        "name": {"type": "string", "minLength": 1, "maxLength": 32},
                    },
                },
                "Item": {
                    "type": "object",
                    "required": ["id", "kind"],
                    "properties": {"id": {"type": "string"}, "kind": {"enum": ["ONE", "TWO"]}},
                },
            }
        },
    }


def test_additive_routes_schemas_and_optional_fields_are_compatible() -> None:
    baseline = contract()
    candidate = copy.deepcopy(baseline)
    schemas = candidate["components"]["schemas"]  # type: ignore[index]
    schemas["ItemRequest"]["properties"]["description"] = {"type": "string"}
    schemas["NewResult"] = {"type": "object"}
    candidate["paths"]["/new"] = {"get": {"operationId": "new.get", "responses": {"204": {}}}}  # type: ignore[index]
    assert compatibility_issues(baseline, candidate) == []


def test_removals_required_changes_enum_removals_and_tightened_bounds_are_breaking() -> None:
    baseline = contract()
    candidate = copy.deepcopy(baseline)
    operation = candidate["paths"]["/items"]["post"]  # type: ignore[index]
    operation["responses"].pop("400")
    operation["parameters"][0]["schema"]["maximum"] = 10
    request = candidate["components"]["schemas"]["ItemRequest"]  # type: ignore[index]
    request["required"].append("name")
    request["properties"]["kind"]["enum"].remove("TWO")
    request["properties"]["name"]["minLength"] = 2
    request["properties"]["kind"]["pattern"] = "^[A-Z]+$"
    issues = compatibility_issues(baseline, candidate)
    assert any("response removed" in issue for issue in issues)
    assert any("maximum tightened" in issue for issue in issues)
    assert any("required fields changed" in issue for issue in issues)
    assert any("enum values removed" in issue for issue in issues)
    assert any("minLength tightened" in issue for issue in issues)
    assert any("pattern constraint added" in issue for issue in issues)


def test_removed_operation_media_type_and_security_change_are_breaking() -> None:
    baseline = contract()
    candidate = copy.deepcopy(baseline)
    operation = candidate["paths"]["/items"]["post"]  # type: ignore[index]
    operation["security"] = []
    operation["requestBody"]["content"] = {"application/cbor": {"schema": {"type": "object"}}}
    issues = compatibility_issues(baseline, candidate)
    assert any("security contract changed" in issue for issue in issues)
    assert any("media type removed" in issue for issue in issues)


def test_inherited_security_and_referenced_scheme_changes_are_breaking() -> None:
    baseline = contract()
    operation = baseline["paths"]["/items"]["post"]  # type: ignore[index]
    operation.pop("security")
    baseline["security"] = [{"bearerAuth": []}]
    baseline["components"]["securitySchemes"] = {  # type: ignore[index]
        "bearerAuth": {"type": "http", "scheme": "bearer"}
    }

    without_security = copy.deepcopy(baseline)
    without_security.pop("security")
    assert any(
        "security contract changed" in issue
        for issue in compatibility_issues(baseline, without_security)
    )

    changed_scheme = copy.deepcopy(baseline)
    changed_scheme["components"]["securitySchemes"]["bearerAuth"]["scheme"] = "basic"  # type: ignore[index]
    assert any(
        "security scheme changed" in issue
        for issue in compatibility_issues(baseline, changed_scheme)
    )

    malformed_schemes = copy.deepcopy(baseline)
    malformed_schemes["components"]["securitySchemes"] = []  # type: ignore[index]
    assert any(
        "security schemes changed shape" in issue
        for issue in compatibility_issues(baseline, malformed_schemes)
    )


@pytest.mark.parametrize("location", ["path", "query", "header", "cookie"])
def test_candidate_only_required_parameters_are_breaking(location: str) -> None:
    baseline = contract()
    candidate = copy.deepcopy(baseline)
    operation = candidate["paths"]["/items"]["post"]  # type: ignore[index]
    operation["parameters"].append(
        {
            "name": "newInput",
            "in": location,
            "required": True,
            "schema": {"type": "string"},
        }
    )
    issues = compatibility_issues(baseline, candidate)
    assert any("required parameter added" in issue for issue in issues)


def test_candidate_only_required_referenced_body_is_breaking() -> None:
    baseline = contract()
    baseline["paths"]["/items"]["post"].pop("requestBody")  # type: ignore[index]
    candidate = copy.deepcopy(baseline)
    candidate["components"]["requestBodies"] = {  # type: ignore[index]
        "RequiredBody": {
            "required": True,
            "content": {"application/json": {"schema": {"type": "object"}}},
        }
    }
    candidate["paths"]["/items"]["post"]["requestBody"] = {  # type: ignore[index]
        "$ref": "#/components/requestBodies/RequiredBody"
    }
    issues = compatibility_issues(baseline, candidate)
    assert any("required request body added" in issue for issue in issues)


def test_candidate_only_optional_inputs_remain_compatible() -> None:
    baseline = contract()
    baseline["paths"]["/items"]["post"].pop("requestBody")  # type: ignore[index]
    candidate = copy.deepcopy(baseline)
    operation = candidate["paths"]["/items"]["post"]  # type: ignore[index]
    operation["parameters"].append(
        {"name": "hint", "in": "query", "required": False, "schema": {"type": "string"}}
    )
    operation["requestBody"] = {
        "required": False,
        "content": {"application/json": {"schema": {"type": "object"}}},
    }
    assert compatibility_issues(baseline, candidate) == []


def test_cli_snapshot_and_check_have_stable_exit_codes(tmp_path: Path) -> None:
    source = tmp_path / "source.json"
    baseline = tmp_path / "baseline.json"
    candidate = tmp_path / "candidate.json"
    source.write_text(json.dumps(contract()), encoding="utf-8")
    assert main(["snapshot", "--source", str(source), "--output", str(baseline)]) == 0
    candidate.write_text(baseline.read_text(encoding="utf-8"), encoding="utf-8")
    assert main(["check", "--baseline", str(baseline), "--candidate", str(candidate)]) == 0
    value = json.loads(candidate.read_text(encoding="utf-8"))
    value["paths"].pop("/items")
    candidate.write_text(json.dumps(value), encoding="utf-8")
    assert main(["check", "--baseline", str(baseline), "--candidate", str(candidate)]) == 1
