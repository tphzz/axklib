from __future__ import annotations

from typing import Any

import pytest

from image_open_smoke import check_image, load_manifest, result_code, source_signature


class FakeClient:
    def __init__(self, *, broken_job: bool = False, device: str | None = "a-series") -> None:
        self.calls: list[tuple[str, str]] = []
        self.broken_job = broken_job
        self.device = device

    def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
        self.calls.append((method, path))
        summary = {
            "imageId": "image-1",
            "revision": 1,
            "format": "fat12",
            "objectCount": 1,
            "validation": {"valid": True, "errorCount": 0},
        }
        if path == "/images" and method == "POST":
            assert body == {
                "source": {"kind": "FILE", "file": {"rootId": "corpus", "relativePath": "disk.img"}}
            }
            return 202, {"data": {"jobId": "job-1"}}
        if path == "/jobs/job-1":
            if self.broken_job:
                return 500, {"error": "Job result violated its declared schema"}
            return 200, {"data": {"state": "COMPLETED", "result": summary}}
        if path == "/images/image-1":
            return 200, {"data": summary}
        if "/validation/issues?" in path:
            return 200, {"data": {"items": [{"code": "TEST_INVALID"}], "nextCursor": None}}
        if "/filesystem?" in path:
            parent = "parentId=root" in path
            offset = "offset=1" in path
            entry = {
                "id": "f2" if offset else "f1",
                "parentId": "root",
                "kind": "FILE",
                "name": "second" if offset else "first",
            }
            return 200, {
                "data": {
                    "available": True,
                    "filesystemName": "FAT12",
                    "deviceView": self.device,
                    "revision": 1,
                    "totalCount": 2 if parent else 1,
                    "items": [entry]
                    if parent
                    else [{"id": "root", "parentId": None, "kind": "ROOT", "name": "Disk"}],
                }
            }
        if "/content?" in path:
            parent = "parentId=volume" in path
            return 200, {
                "data": {
                    "items": [{"id": "object", "childCount": 0}]
                    if parent
                    else [{"id": "volume", "childCount": 1}],
                    "totalCount": 1,
                    "nextCursor": None,
                }
            }
        raise AssertionError((method, path))


def profile(device: str | None = "a-series") -> dict[str, Any]:
    return {"format": "fat12", "filesystemName": "FAT12", "deviceView": device, "minimumFiles": 2}


def test_reads_completed_job_summary_all_files_pages_and_device_children() -> None:
    client = FakeClient()
    report = check_image(client, "disk.img", profile())
    assert report["files"] == 2
    assert report["deviceNodes"] == 2
    assert any("offset=1" in path for _, path in client.calls)
    assert client.calls[-1] == ("DELETE", "/images/image-1")


def test_detects_original_completed_job_serialization_failure() -> None:
    with pytest.raises(RuntimeError, match="HTTP 500"):
        check_image(FakeClient(broken_job=True), "disk.img", profile())


def test_files_only_image_does_not_require_a_sampler_object_model() -> None:
    client = FakeClient(device=None)
    assert check_image(client, "disk.img", profile(None))["deviceNodes"] == 0
    assert not any("/content" in path for _, path in client.calls)


def test_wrong_detection_is_a_failure_and_still_closes_session() -> None:
    client = FakeClient()
    with pytest.raises(RuntimeError, match="format"):
        check_image(client, "disk.img", {**profile(), "format": "sfs"})
    assert client.calls[-1] == ("DELETE", "/images/image-1")


def test_traversal_limit_is_not_a_successful_partial_scan() -> None:
    client = FakeClient()
    with pytest.raises(RuntimeError, match="limit"):
        check_image(client, "disk.img", profile(), maximum_entries=1)
    assert client.calls[-1] == ("DELETE", "/images/image-1")


def test_missing_cases_never_silently_pass() -> None:
    assert result_code([{"status": "passed"}]) == 0
    assert result_code([{"status": "passed"}, {"status": "missing"}]) == 2
    assert result_code([{"status": "failed"}, {"status": "missing"}]) == 1


def test_image_validation_errors_are_not_a_successful_open() -> None:
    class InvalidClient(FakeClient):
        def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
            status, document = super().request(method, path, body)
            if path == "/jobs/job-1":
                document["data"]["result"]["validation"] = {"valid": False, "errorCount": 1}
            elif path == "/images/image-1":
                document["data"]["validation"] = {"valid": False, "errorCount": 1}
            return status, document

    with pytest.raises(RuntimeError, match="validation"):
        check_image(InvalidClient(), "disk.img", profile())
    report = check_image(InvalidClient(), "disk.img", profile(), reject_invalid=False)
    assert report["files"] == 2
    assert report["validation"]["valid"] is False
    assert report["validationIssues"] == [{"code": "TEST_INVALID"}]


def test_warning_only_image_reports_issues_and_requires_requested_write_capabilities() -> None:
    class WarningClient(FakeClient):
        def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
            status, document = super().request(method, path, body)
            if path == "/jobs/job-1":
                document["data"]["result"]["validation"]["warningCount"] = 1
            elif path == "/images/image-1":
                document["data"]["validation"]["warningCount"] = 1
            elif "/filesystem?" in path:
                document["data"]["rootCapabilities"] = [
                    {"createDirectory": True, "putFile": True, "deleteEntry": True}
                ]
            return status, document

    expected = {**profile(), "warningCodes": ["TEST_INVALID"], "writable": True}
    report = check_image(WarningClient(), "disk.img", expected)
    assert report["validationIssues"] == [{"code": "TEST_INVALID"}]
    with pytest.raises(RuntimeError, match="warnings"):
        check_image(FakeClient(), "disk.img", expected)
    with pytest.raises(RuntimeError, match="writable"):
        check_image(FakeClient(), "disk.img", {**profile(), "writable": True})


def test_manifest_rejects_empty_matrix_and_duplicate_case_ids(tmp_path: Any) -> None:
    import json

    path = tmp_path / "matrix.json"
    path.write_text(json.dumps({"cases": []}))
    with pytest.raises(ValueError):
        load_manifest(path)
    path.write_text(json.dumps({"cases": [{"id": "same"}, {"id": "same"}]}))
    with pytest.raises(ValueError):
        load_manifest(path)


def test_extended_manifest_requires_explicit_larger_bound(tmp_path: Any) -> None:
    import json

    path = tmp_path / "extended.json"
    cases = [
        {"id": str(index), "root": "corpus", "path": "disk.img", "expected": profile()}
        for index in range(17)
    ]
    path.write_text(json.dumps({"cases": cases}))
    with pytest.raises(ValueError, match="16"):
        load_manifest(path)
    assert len(load_manifest(path, maximum_cases=256)) == 17


def test_request_bound_is_configurable_but_never_truncates() -> None:
    with pytest.raises(RuntimeError, match="limit"):
        check_image(FakeClient(), "disk.img", profile(), maximum_requests=1)


def test_directory_companions_require_each_completed_session_state() -> None:
    class DirectoryClient(FakeClient):
        revision = 1

        def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
            if path == "/images" and method == "POST":
                assert body["source"] == {
                    "kind": "AXK_OBJECT_DIRECTORY",
                    "directory": {"rootId": "corpus", "relativePath": "disk1"},
                }
                return 202, {"data": {"jobId": "job-1"}}
            if path.endswith("/companions"):
                assert body["expectedRevision"] == self.revision
                assert (
                    body["selection"]["sources"][0]["directory"]["relativePath"]
                    == f"disk{self.revision + 1}"
                )
                self.revision += 1
                path = "/images/image-1"
            if path == "/images/image-1?":
                path = "/images/image-1"
            status, document = super().request(method, path, body)
            if path in {"/jobs/job-1", "/images/image-1"}:
                summary = document["data"]["result"] if path == "/jobs/job-1" else document["data"]
                summary.update(
                    format="axk-object-directory",
                    revision=self.revision,
                    floppySet={
                        "status": "COMPLETE" if self.revision == 3 else "INCOMPLETE",
                        "nextRequiredIndex": None if self.revision == 3 else self.revision + 1,
                    },
                )
            if "/filesystem?" in path:
                document["data"].update(
                    available=False,
                    filesystemName="",
                    items=[],
                    totalCount=0,
                    revision=self.revision,
                )
            return status, document

    client = DirectoryClient()
    report = check_image(
        client,
        "disk1",
        {
            **profile(),
            "format": "axk-object-directory",
            "filesystemName": "",
            "filesystemAvailable": False,
            "minimumFiles": 0,
        },
        source_kind="AXK_OBJECT_DIRECTORY",
        companion_steps=[
            {"path": "disk2", "beforeNextIndex": 2, "nextRequiredIndex": 3, "status": "INCOMPLETE"},
            {
                "path": "disk3",
                "beforeNextIndex": 3,
                "nextRequiredIndex": None,
                "status": "COMPLETE",
            },
        ],
    )
    assert len(report["companionSteps"]) == 2
    assert report["floppySet"]["status"] == "COMPLETE"
    assert report["deviceNodes"] == 2


def test_directory_signatures_cover_file_contents_and_reject_links(tmp_path: Any) -> None:
    folder = tmp_path / "disk1"
    folder.mkdir()
    member = folder / "YAMAHA.SYM"
    member.write_bytes(b"catalog")
    before = source_signature(folder)
    member.write_bytes(b"changed")
    assert source_signature(folder) != before
    (folder / "link").symlink_to(member)
    with pytest.raises(RuntimeError, match="symlink"):
        source_signature(folder)
