"""Bounded native-document checks shared by read-only API corpus campaigns."""

from __future__ import annotations

import math
from collections import Counter
from collections.abc import Callable
from typing import Any
from urllib.parse import quote

Get = Callable[[str, dict[str, Any]], dict[str, Any]]
RunJob = Callable[[str, dict[str, Any]], dict[str, Any]]


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
    expected: dict[str, Any], maximum_entries: int, image_id: str | None = None,
    run_job: RunJob | None = None,
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
    previews = 0
    for item in objects:
        if expected.get("allObjectDetails") or item["type"] not in inspected:
            check_object_detail(get, path, item["id"], document["id"], revision)
            inspected.add(item["type"])
            details += 1
        if expected.get("wavePreviews") and item["type"] == "wave":
            if run_job is None:
                raise RuntimeError("wave previews require completed job retrieval")
            check_wave_preview(run_job, item["id"], image_id, document["id"], revision)
            previews += 1
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
    return {"objectDetails": details, "wavePreviews": previews,
            "objectTypes": dict(Counter(item["type"] for item in objects)),
            "relationshipStates": dict(Counter(item["quality"] for item in relationships)),
            "sourceRequirements": requirements}


def check_wave_preview(
    run_job: RunJob, object_id: str, image_id: str | None, document_id: str, revision: int,
) -> None:
    preview = run_job("/image-waveform-previews", {
        "imageId": image_id, "documentId": document_id, "objectId": object_id,
        "expectedRevision": revision, "bins": 64, "readIntent": "SELECTED",
    })
    if image_id is None or (preview["imageId"], preview["documentId"], preview["revision"], preview["objectId"]) != (
        image_id, document_id, revision, object_id,
    ):
        raise RuntimeError("wave preview identity/revision differs")
    if not 1 <= len(preview["lanes"]) <= 2:
        raise RuntimeError("wave preview lane count differs")
    for lane in preview["lanes"]:
        frames = lane["storedFrameCount"]
        if (frames < 1 or lane["sampleRate"] <= 0 or lane["sampleWidthBytes"] not in (1, 2)
                or not 1 <= lane["nativeBitDepth"] <= lane["sampleWidthBytes"] * 8
                or len(lane["bins"]) != min(64, frames)
                or lane["playbackStartFrame"] + lane["playbackLengthFrames"] > frames
                or lane["loopStartFrame"] + lane["loopLengthFrames"] > frames):
            raise RuntimeError("wave preview dimensions differ")
        if lane["sampleRateApproximate"] and not preview["warnings"]:
            raise RuntimeError("approximate sample rate has no warning")
        maximum = (1 << (lane["sampleWidthBytes"] * 8 - 1)) - 1
        if any(not math.isfinite(value) or not -maximum - 1 <= value <= maximum
               for item in lane["bins"] for value in (item["minimum"], item["maximum"])):
            raise RuntimeError("wave preview PCM envelope is invalid")
        if any(item["minimum"] > item["maximum"] for item in lane["bins"]):
            raise RuntimeError("wave preview envelope is inverted")
