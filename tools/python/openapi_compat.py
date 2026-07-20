"""Check an OpenAPI candidate against the supported v1 compatibility baseline."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

JsonObject = dict[str, Any]
HTTP_METHODS = {"delete", "get", "head", "options", "patch", "post", "put", "trace"}
LOWER_BOUNDS = ("minimum", "exclusiveMinimum", "minLength", "minItems", "minProperties")
UPPER_BOUNDS = ("maximum", "exclusiveMaximum", "maxLength", "maxItems", "maxProperties")


def _load(path: Path) -> JsonObject:
    value: object = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} does not contain a JSON object")
    return value


def _local_reference(document: JsonObject, value: object) -> object:
    current = value
    seen: set[str] = set()
    while isinstance(current, dict) and isinstance(current.get("$ref"), str):
        reference = current["$ref"]
        if not reference.startswith("#/") or reference in seen:
            return current
        seen.add(reference)
        target: object = document
        for token in reference[2:].split("/"):
            token = token.replace("~1", "/").replace("~0", "~")
            if not isinstance(target, dict) or token not in target:
                return current
            target = target[token]
        current = target
    return current


def _types(value: object) -> set[str]:
    if isinstance(value, str):
        return {value}
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return set(value)
    return set()


def _number(value: object) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        return None
    return value


def _compare_schema(
    baseline_document: JsonObject,
    candidate_document: JsonObject,
    baseline_value: object,
    candidate_value: object,
    path: str,
    issues: list[str],
) -> None:
    baseline = _local_reference(baseline_document, baseline_value)
    candidate = _local_reference(candidate_document, candidate_value)
    if not isinstance(baseline, dict) or not isinstance(candidate, dict):
        if baseline != candidate:
            issues.append(f"{path}: schema value changed")
        return

    baseline_types = _types(baseline.get("type"))
    candidate_types = _types(candidate.get("type"))
    if not baseline_types and candidate_types:
        issues.append(f"{path}: type constraint added ({sorted(candidate_types)})")
    elif baseline_types and not baseline_types.issubset(candidate_types):
        issues.append(
            f"{path}: accepted types removed ({sorted(baseline_types - candidate_types)})"
        )

    if "const" not in baseline and "const" in candidate:
        issues.append(f"{path}: const constraint added")
    elif "const" in baseline and baseline.get("const") != candidate.get("const"):
        issues.append(f"{path}: const value changed")
    if not isinstance(baseline.get("enum"), list) and isinstance(candidate.get("enum"), list):
        issues.append(f"{path}: enum constraint added")
    elif isinstance(baseline.get("enum"), list):
        candidate_enum = candidate.get("enum")
        if not isinstance(candidate_enum, list):
            issues.append(f"{path}: enum constraint removed or changed shape")
        else:
            removed = [item for item in baseline["enum"] if item not in candidate_enum]
            if removed:
                issues.append(f"{path}: enum values removed ({removed})")

    baseline_required = (
        set(baseline.get("required", [])) if isinstance(baseline.get("required"), list) else set()
    )
    candidate_required = (
        set(candidate.get("required", [])) if isinstance(candidate.get("required"), list) else set()
    )
    if baseline_required != candidate_required:
        issues.append(
            f"{path}: required fields changed "
            f"(removed={sorted(baseline_required - candidate_required)}, "
            f"added={sorted(candidate_required - baseline_required)})"
        )

    baseline_properties = baseline.get("properties")
    candidate_properties = candidate.get("properties")
    if isinstance(baseline_properties, dict):
        if not isinstance(candidate_properties, dict):
            issues.append(f"{path}: object properties removed")
        else:
            for name, schema in baseline_properties.items():
                if name not in candidate_properties:
                    issues.append(f"{path}/properties/{name}: property removed")
                else:
                    _compare_schema(
                        baseline_document,
                        candidate_document,
                        schema,
                        candidate_properties[name],
                        f"{path}/properties/{name}",
                        issues,
                    )

    if (
        baseline.get("additionalProperties", True) is not False
        and candidate.get("additionalProperties", True) is False
    ):
        issues.append(f"{path}: additional properties are no longer accepted")

    for name in LOWER_BOUNDS:
        old = _number(baseline.get(name))
        new = _number(candidate.get(name))
        if old is None and new is not None:
            issues.append(f"{path}: {name} constraint added ({new})")
        elif old is not None and new is not None and new > old:
            issues.append(f"{path}: {name} tightened from {old} to {new}")
    for name in UPPER_BOUNDS:
        old = _number(baseline.get(name))
        new = _number(candidate.get(name))
        if old is None and new is not None:
            issues.append(f"{path}: {name} constraint added ({new})")
        elif old is not None and new is not None and new < old:
            issues.append(f"{path}: {name} tightened from {old} to {new}")
    for name in ("format", "pattern"):
        if name not in baseline and name in candidate:
            issues.append(f"{path}: {name} constraint added")
        elif name in baseline and baseline.get(name) != candidate.get(name):
            issues.append(f"{path}: {name} changed")

    if "items" in baseline:
        if "items" not in candidate:
            issues.append(f"{path}: array item schema removed")
        else:
            _compare_schema(
                baseline_document,
                candidate_document,
                baseline["items"],
                candidate["items"],
                f"{path}/items",
                issues,
            )

    for keyword in ("allOf", "anyOf", "oneOf"):
        old_variants = baseline.get(keyword)
        new_variants = candidate.get(keyword)
        if not isinstance(old_variants, list):
            continue
        if not isinstance(new_variants, list) or len(new_variants) < len(old_variants):
            issues.append(f"{path}: {keyword} alternatives removed")
            continue
        for index, old_variant in enumerate(old_variants):
            _compare_schema(
                baseline_document,
                candidate_document,
                old_variant,
                new_variants[index],
                f"{path}/{keyword}/{index}",
                issues,
            )


def _parameters(
    document: JsonObject, path_item: JsonObject, operation: JsonObject
) -> dict[tuple[str, str], JsonObject]:
    result: dict[tuple[str, str], JsonObject] = {}
    for source in (path_item.get("parameters", []), operation.get("parameters", [])):
        if not isinstance(source, list):
            continue
        for raw in source:
            value = _local_reference(document, raw)
            if (
                not isinstance(value, dict)
                or not isinstance(value.get("name"), str)
                or not isinstance(value.get("in"), str)
            ):
                continue
            result[(value["name"], value["in"])] = value
    return result


def _effective_security(document: JsonObject, operation: JsonObject) -> object:
    return operation["security"] if "security" in operation else document.get("security")


def _canonical_security(
    value: object,
) -> tuple[tuple[tuple[str, tuple[str, ...]], ...], ...] | None:
    if value is None:
        return None
    if not isinstance(value, list):
        return ()
    requirements: list[tuple[tuple[str, tuple[str, ...]], ...]] = []
    for raw_requirement in value:
        if not isinstance(raw_requirement, dict):
            return ()
        requirement: list[tuple[str, tuple[str, ...]]] = []
        for scheme, raw_scopes in raw_requirement.items():
            if (
                not isinstance(scheme, str)
                or not isinstance(raw_scopes, list)
                or not all(isinstance(scope, str) for scope in raw_scopes)
            ):
                return ()
            requirement.append((scheme, tuple(sorted(raw_scopes))))
        requirements.append(tuple(sorted(requirement)))
    return tuple(sorted(requirements))


def _compare_content(
    baseline_document: JsonObject,
    candidate_document: JsonObject,
    baseline: object,
    candidate: object,
    path: str,
    issues: list[str],
) -> None:
    if not isinstance(baseline, dict):
        return
    if not isinstance(candidate, dict):
        issues.append(f"{path}: media contract removed")
        return
    for media_type, media in baseline.items():
        if media_type not in candidate:
            issues.append(f"{path}/{media_type}: media type removed")
            continue
        if isinstance(media, dict) and "schema" in media:
            candidate_media = candidate[media_type]
            if not isinstance(candidate_media, dict) or "schema" not in candidate_media:
                issues.append(f"{path}/{media_type}: schema removed")
            else:
                _compare_schema(
                    baseline_document,
                    candidate_document,
                    media["schema"],
                    candidate_media["schema"],
                    f"{path}/{media_type}/schema",
                    issues,
                )


def compatibility_issues(baseline: JsonObject, candidate: JsonObject) -> list[str]:
    """Return deterministic descriptions of breaking v1 contract changes."""
    issues: list[str] = []
    baseline_paths = baseline.get("paths")
    candidate_paths = candidate.get("paths")
    if not isinstance(baseline_paths, dict) or not isinstance(candidate_paths, dict):
        return ["$: paths must be objects"]

    old_security_schemes = baseline.get("components", {}).get("securitySchemes", {})
    new_security_schemes = candidate.get("components", {}).get("securitySchemes", {})
    if isinstance(old_security_schemes, dict) and not isinstance(new_security_schemes, dict):
        issues.append("components/securitySchemes: security schemes changed shape")
    elif isinstance(old_security_schemes, dict) and isinstance(new_security_schemes, dict):
        for name, old_scheme in old_security_schemes.items():
            if name not in new_security_schemes:
                issues.append(f"components/securitySchemes/{name}: security scheme removed")
            elif old_scheme != new_security_schemes[name]:
                issues.append(f"components/securitySchemes/{name}: security scheme changed")

    for path, old_path_item in baseline_paths.items():
        if path not in candidate_paths:
            issues.append(f"paths/{path}: path removed")
            continue
        new_path_item = candidate_paths[path]
        if not isinstance(old_path_item, dict) or not isinstance(new_path_item, dict):
            issues.append(f"paths/{path}: path item changed shape")
            continue
        for method, old_operation in old_path_item.items():
            if method not in HTTP_METHODS or not isinstance(old_operation, dict):
                continue
            new_operation = new_path_item.get(method)
            location = f"paths/{path}/{method}"
            if not isinstance(new_operation, dict):
                issues.append(f"{location}: operation removed")
                continue
            if old_operation.get("operationId") != new_operation.get("operationId"):
                issues.append(f"{location}: operationId changed")
            if _canonical_security(
                _effective_security(baseline, old_operation)
            ) != _canonical_security(_effective_security(candidate, new_operation)):
                issues.append(f"{location}: security contract changed")

            old_parameters = _parameters(baseline, old_path_item, old_operation)
            new_parameters = _parameters(candidate, new_path_item, new_operation)
            for key, old_parameter in old_parameters.items():
                if key not in new_parameters:
                    issues.append(f"{location}/parameters/{key[1]}/{key[0]}: parameter removed")
                    continue
                new_parameter = new_parameters[key]
                if not old_parameter.get("required", False) and new_parameter.get(
                    "required", False
                ):
                    issues.append(
                        f"{location}/parameters/{key[1]}/{key[0]}: parameter became required"
                    )
                if "schema" in old_parameter and "schema" in new_parameter:
                    _compare_schema(
                        baseline,
                        candidate,
                        old_parameter["schema"],
                        new_parameter["schema"],
                        f"{location}/parameters/{key[1]}/{key[0]}/schema",
                        issues,
                    )
            for key, new_parameter in new_parameters.items():
                if key not in old_parameters and new_parameter.get("required", False):
                    issues.append(
                        f"{location}/parameters/{key[1]}/{key[0]}: required parameter added"
                    )

            old_body = _local_reference(baseline, old_operation.get("requestBody"))
            new_body = _local_reference(candidate, new_operation.get("requestBody"))
            if isinstance(old_body, dict):
                if not isinstance(new_body, dict):
                    issues.append(f"{location}/requestBody: request body removed")
                else:
                    if not old_body.get("required", False) and new_body.get("required", False):
                        issues.append(f"{location}/requestBody: request body became required")
                    _compare_content(
                        baseline,
                        candidate,
                        old_body.get("content"),
                        new_body.get("content"),
                        f"{location}/requestBody/content",
                        issues,
                    )
            elif isinstance(new_body, dict) and new_body.get("required", False):
                issues.append(f"{location}/requestBody: required request body added")

            old_responses = old_operation.get("responses")
            new_responses = new_operation.get("responses")
            if not isinstance(old_responses, dict) or not isinstance(new_responses, dict):
                issues.append(f"{location}/responses: responses changed shape")
                continue
            for status, old_response in old_responses.items():
                if status not in new_responses:
                    issues.append(f"{location}/responses/{status}: response removed")
                    continue
                old_resolved = _local_reference(baseline, old_response)
                new_resolved = _local_reference(candidate, new_responses[status])
                if isinstance(old_resolved, dict) and isinstance(new_resolved, dict):
                    _compare_content(
                        baseline,
                        candidate,
                        old_resolved.get("content"),
                        new_resolved.get("content"),
                        f"{location}/responses/{status}/content",
                        issues,
                    )

    old_schemas = baseline.get("components", {}).get("schemas", {})
    new_schemas = candidate.get("components", {}).get("schemas", {})
    if isinstance(old_schemas, dict) and isinstance(new_schemas, dict):
        for name, old_schema in old_schemas.items():
            if name not in new_schemas:
                issues.append(f"components/schemas/{name}: schema removed")
            else:
                _compare_schema(
                    baseline,
                    candidate,
                    old_schema,
                    new_schemas[name],
                    f"components/schemas/{name}",
                    issues,
                )
    return sorted(set(issues))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    check = subparsers.add_parser("check", help="reject breaking changes from a baseline")
    check.add_argument("--baseline", type=Path, required=True)
    check.add_argument("--candidate", type=Path, required=True)
    snapshot = subparsers.add_parser("snapshot", help="write a canonical reviewed baseline")
    snapshot.add_argument("--source", type=Path, required=True)
    snapshot.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "snapshot":
            source = _load(args.source)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(source, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            return 0
        issues = compatibility_issues(_load(args.baseline), _load(args.candidate))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"OpenAPI compatibility check failed: {error}", file=sys.stderr)
        return 2
    if issues:
        print("Breaking OpenAPI changes detected:", file=sys.stderr)
        for issue in issues:
            print(f"- {issue}", file=sys.stderr)
        return 1
    print("OpenAPI compatibility check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
