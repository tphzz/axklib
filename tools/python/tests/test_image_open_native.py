from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from image_open_native import check_object_detail, inspect_native
from tx16w_corpus_smoke import load_sets, variants


def test_fetches_every_encoded_object_and_checks_document_relationship_membership() -> None:
    calls: list[str] = []
    objects = [{"id": "native:1/%+", "type": "wave", "name": "First"},
               {"id": "native:2", "type": "wave", "name": "Second"}]

    def get(path: str, query: dict[str, Any]) -> dict[str, Any]:
        calls.append(path)
        if path.endswith("/objects"):
            return {"items": objects[1:] if query.get("cursor") else objects[:1],
                    "nextCursor": None if query.get("cursor") else "page2"}
        if path.endswith("/relationships"):
            return {"items": [{"sourceObjectId": objects[0]["id"], "targetObjectId": None,
                               "candidateObjectIds": [objects[1]["id"]], "quality": "ambiguous"}],
                    "nextCursor": None}
        identifier = objects[0]["id"] if path.endswith("native%3A1%2F%25%2B") else objects[1]["id"]
        return {"formatId": "yamaha.tx16w", "objectId": identifier, "documentId": "document", "revision": 2}

    document = {"id": "document", "objectCount": 2, "companions": {"requirements": []}}
    result = inspect_native(get, "/documents/document", document, 2, {"allObjectDetails": True}, 10)
    assert result["objectDetails"] == 2
    assert result["relationshipStates"] == {"ambiguous": 1}
    assert "/documents/document/objects/native%3A1%2F%25%2B" in calls


def test_a_series_detail_uses_its_declared_typed_identity_fields() -> None:
    detail: dict[str, Any] = {"formatId": "yamaha.a-series", "object": {"id": "object"},
                              "image": {"documentId": "document", "revision": 3}}
    check_object_detail(lambda _path, _query: detail, "/documents/document", "object", "document", 3)
    detail["image"]["revision"] = 2
    with pytest.raises(RuntimeError, match="identity/revision"):
        check_object_detail(lambda _path, _query: detail, "/documents/document", "object", "document", 3)
    detail["formatId"] = "unsupported"
    with pytest.raises(RuntimeError, match="schema"):
        check_object_detail(lambda _path, _query: detail, "/documents/document", "object", "document", 3)


@pytest.mark.parametrize("problem", ["duplicate", "stale", "other-document", "other-object", "relationship", "cursor", "limit"])
def test_rejects_partial_stale_and_cross_document_results(problem: str) -> None:
    def get(path: str, query: dict[str, Any]) -> dict[str, Any]:
        if path.endswith("/objects"):
            item = {"id": "object", "type": "wave", "name": "Wave"}
            return {"items": [item, item] if problem == "duplicate" else [item],
                    "nextCursor": "repeated" if problem == "cursor" else None}
        if path.endswith("/relationships"):
            return {"items": [{"sourceObjectId": "object", "targetObjectId": "outside", "candidateObjectIds": []}],
                    "nextCursor": None}
        return {"formatId": "yamaha.tx16w", "objectId": "other" if problem == "other-object" else "object",
                "documentId": "other" if problem == "other-document" else "document",
                "revision": 1 if problem == "stale" else 2}

    document = {"id": "document", "objectCount": 1, "companions": {"requirements": []}}
    with pytest.raises(RuntimeError):
        inspect_native(get, "/documents/document", document, 2, {}, 0 if problem == "limit" else 10)


def test_corpus_variants_attach_setup_before_other_wave_only_members() -> None:
    assert variants(["one"]) == [("single", ["one"], False)]
    assert variants(["one", "two"])[-1] == ("last-first", ["two", "one"], True)
    assert variants(["one", "two", "three", "four"])[-1] == (
        "last-first", ["four", "one", "three", "two"], True,
    )


def test_corpus_manifest_requires_unique_contained_documented_members(tmp_path: Path) -> None:
    path = tmp_path / "manifest.json"
    case = {"id": "set", "sources": ["folder/one.img"], "catalog": "folder/catalog.txt",
            "expected": {"completeness": "COMPLETE", "requirementStates": {}}}
    path.write_text(json.dumps({"sets": [case]}))
    assert load_sets(path) == [case]
    for sources in [["../escape.img"], ["/absolute.img"], ["a/one.img", "b/two.img"], ["same.img", "same.img"]]:
        path.write_text(json.dumps({"sets": [{**case, "sources": sources}]}))
        with pytest.raises(ValueError):
            load_sets(path)
