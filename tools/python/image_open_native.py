"""Bounded native-document checks shared by read-only API corpus campaigns."""

from __future__ import annotations

from collections import Counter
from collections.abc import Callable
from typing import Any
from urllib.parse import quote

Get = Callable[[str, dict[str, Any]], dict[str, Any]]


def native_items(get: Get, path: str, maximum_entries: int) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    cursor: str | None = None
    cursors: set[str] = set()
    while True:
        query: dict[str, Any] = {"limit": 200}
        if cursor is not None:
            query["cursor"] = cursor
        page = get(path, query)
        result.extend(page["items"])
        if len(result) > maximum_entries:
            raise RuntimeError(f"native {path} entry limit exceeded")
        cursor = page["nextCursor"]
        if cursor is None:
            return result
        if cursor in cursors or not page["items"]:
            raise RuntimeError(f"native {path} pagination did not progress")
        cursors.add(cursor)


def check_object_detail(get: Get, path: str, object_id: str, document_id: str, revision: int) -> None:
    detail = get(path + "/objects/" + quote(object_id, safe=""), {})
    if detail["formatId"] == "yamaha.a-series":
        identity = (detail["object"]["id"], detail["image"]["documentId"], detail["image"]["revision"])
    elif detail["formatId"] == "yamaha.tx16w":
        identity = (detail["objectId"], detail["documentId"], detail["revision"])
    else:
        raise RuntimeError("unsupported native detail schema")
    if identity != (object_id, document_id, revision):
        raise RuntimeError("native object detail identity/revision differs")


def inspect_native(
    get: Get, path: str, document: dict[str, Any], revision: int,
    expected: dict[str, Any], maximum_entries: int,
) -> dict[str, Any]:
    objects = native_items(get, path + "/objects", maximum_entries)
    ids = {item["id"] for item in objects}
    if len(ids) != len(objects) or len(ids) != document["objectCount"]:
        raise RuntimeError("native object identities/count differ")
    names = {(item["type"], item["name"]) for item in objects}
    if not {(item["type"], item["name"]) for item in expected.get("objects", [])} <= names:
        raise RuntimeError("expected companion objects are missing")
    inspected: set[str] = set()
    details = 0
    for item in objects:
        if expected.get("allObjectDetails") or item["type"] not in inspected:
            check_object_detail(get, path, item["id"], document["id"], revision)
            inspected.add(item["type"])
            details += 1
    relationships = native_items(get, path + "/relationships", maximum_entries)
    for edge in relationships:
        related = [edge["sourceObjectId"], *edge["candidateObjectIds"]]
        if edge["targetObjectId"] is not None:
            related.append(edge["targetObjectId"])
        if not set(related) <= ids:
            raise RuntimeError("relationship crosses document membership")
    if "completeness" in expected and document["completeness"] != expected["completeness"]:
        raise RuntimeError(f"source completeness differs: {document['completeness']}")
    requirements = document["companions"]["requirements"]
    states = dict(Counter(item["state"] for item in requirements))
    if "requirementStates" in expected and states != expected["requirementStates"]:
        raise RuntimeError(f"source requirement states differ: {states}")
    return {"objectDetails": details, "objectTypes": dict(Counter(item["type"] for item in objects)),
            "relationshipStates": dict(Counter(item["quality"] for item in relationships)),
            "sourceRequirements": requirements}
