from __future__ import annotations

import copy
import json
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import Any

import pytest
from test_prepare_release import FakeGitHub, request

import prepare_release
from release_github import GitHub, ReleaseError


@pytest.fixture
def github(monkeypatch: pytest.MonkeyPatch) -> FakeGitHub:
    fake = FakeGitHub()
    monkeypatch.setenv("GH_TOKEN", "fake-token-never-send-to-github")
    monkeypatch.setattr(subprocess, "run", fake.run)
    return fake


def test_branch_replaces_old_draft_only_after_new_draft_is_complete(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    previous["target_commitish"] = "a" * 40
    github.add_asset(previous["id"], "old.zip", b"previous build")
    unrelated = github.release("snapshot/main")
    unrelated["name"] = None
    before = copy.deepcopy((previous, github.assets[previous["id"]], unrelated, github.refs))
    run = github.run
    deleted = False

    def observe(command: list[str], **kwargs: Any) -> Any:
        nonlocal deleted
        if command[command.index("--method") + 1] == "DELETE" and command[2].endswith(
            f"/releases/{previous['id']}"
        ):
            assert previous == before[0]
            assert github.assets[previous["id"]] == before[1]
            candidates = [item for item in github.releases if item not in (previous, unrelated)]
            assert len(candidates) == 1
            assert "Preparation: **COMPLETE**" in candidates[0]["body"]
            assert len(github.assets[candidates[0]["id"]]) == 13
            deleted = True
        return run(command, **kwargs)

    monkeypatch.setattr(subprocess, "run", observe)
    result = prepare_release.prepare_release(GitHub(req.repository), req)
    assert deleted and result.status == "replaced"
    assert result.release_id != previous["id"]
    assert previous not in github.releases and previous["id"] not in github.assets
    replacement = next(item for item in github.releases if item["id"] == result.release_id)
    assert replacement["tag_name"] == "snapshot/v0.6.0"
    assert "Human release notes" in replacement["body"]
    assert unrelated == before[2] and github.refs == before[3]


def test_failed_new_branch_draft_leaves_previous_release_and_assets_unchanged(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    github.add_asset(previous["id"], "old.zip", b"previous build")
    before = copy.deepcopy((previous, github.assets[previous["id"]]))
    github.fail_upload = True
    with pytest.raises(ReleaseError, match="502"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert (previous, github.assets[previous["id"]]) == before
    assert not any(method == "DELETE" for method, _ in github.writes)


@pytest.mark.parametrize("draft", [True, False])
def test_snapshot_promoted_to_real_version_tag_is_preserved_on_later_branch_build(
    tmp_path: Path, github: FakeGitHub, draft: bool
) -> None:
    req = request(tmp_path)
    client = GitHub(req.repository)
    first = prepare_release.prepare_release(client, req)
    tagged = next(item for item in github.releases if item["id"] == first.release_id)
    tagged.update(tag_name="v0.6.0", name="Version 0.6.0", draft=draft)
    github.tag("v0.6.0")
    previous = github.release("snapshot/v0.6.0")
    before = copy.deepcopy((tagged, github.assets[tagged["id"]], github.refs))
    result = prepare_release.prepare_release(client, req)
    assert result.status == "replaced" and result.release_id not in {previous["id"], tagged["id"]}
    assert (tagged, github.assets[tagged["id"]], github.refs) == before
    assert previous not in github.releases


@pytest.mark.parametrize(
    "problem",
    [
        "published",
        "renamed",
        "moved",
        "duplicate",
        "title",
        "prerelease",
        "notes",
        "marker",
        "json",
        "owner",
        "schema",
    ],
)
def test_ambiguous_or_changed_incoming_draft_requires_recovery_without_writes(
    tmp_path: Path, github: FakeGitHub, problem: str
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    github.fail_upload = True
    with pytest.raises(ReleaseError, match="502"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    candidate = next(item for item in github.releases if item is not previous)
    if problem == "published":
        candidate["draft"] = False
    elif problem == "renamed":
        candidate["tag_name"] += "-edited"
    elif problem == "moved":
        candidate["tag_name"] = "operator-retained-draft"
    elif problem == "title":
        candidate["name"] = "Manually edited incoming draft"
    elif problem == "prerelease":
        candidate["prerelease"] = False
    elif problem == "notes":
        candidate["body"] += "\nManually added notes"
    elif problem == "marker":
        candidate["body"] = candidate["body"].replace(prepare_release.STATE, "<!-- removed:")
    elif problem == "json":
        candidate["body"] = candidate["body"].replace('"schema_version": 1', '"schema_version":')
    elif problem == "owner":
        candidate["body"] = candidate["body"].replace(
            '"ref_type": "branch"', '"ref_type": "unknown"'
        )
    elif problem == "schema":
        candidate["body"] = candidate["body"].replace(
            '"schema_version": 1', '"schema_version": true'
        )
    else:
        operation = "f" * 32
        other = github.release(prepare_release.INCOMING_RELEASE + operation)
        other["body"] = prepare_release.state_body("", req, [], operation, "incomplete")
    before = copy.deepcopy((github.releases, github.assets, github.refs, github.writes))
    github.fail_upload = False
    with pytest.raises(ReleaseError, match="manual recovery"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert (github.releases, github.assets, github.refs, github.writes) == before


def test_snapshot_ref_created_during_upload_prevents_old_draft_deletion(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    before = copy.deepcopy(previous)
    run = github.run

    def add_ref(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        result = run(command, **kwargs)
        if (
            not github.refs
            and command[command.index("--method") + 1] == "POST"
            and "/assets?" in command[2]
        ):
            github.tag("snapshot/v0.6.0")
        return result

    monkeypatch.setattr(subprocess, "run", add_ref)
    with pytest.raises(ReleaseError, match="snapshot Git ref"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert previous == before and previous in github.releases
    assert len(github.refs) == 1
    assert not any(method == "DELETE" for method, _ in github.writes)


@pytest.mark.parametrize("failure", ["delete", "promote"])
def test_failed_cutover_retains_complete_candidate_and_retry_finishes_replacement(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch, failure: str
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    github.add_asset(previous["id"], "old.zip", b"previous build")
    before = copy.deepcopy((previous, github.assets[previous["id"]]))
    run = github.run

    def interrupt(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        method = command[command.index("--method") + 1]
        data = json.loads(kwargs.get("input") or "{}")
        if (
            failure == "delete"
            and method == "DELETE"
            and command[2].endswith(f"/releases/{previous['id']}")
        ) or (
            failure == "promote" and method == "PATCH" and data.get("tag_name") == "snapshot/v0.6.0"
        ):
            return subprocess.CompletedProcess(command, 1, "", "HTTP 503 cutover interrupted")
        return run(command, **kwargs)

    monkeypatch.setattr(subprocess, "run", interrupt)
    with pytest.raises(ReleaseError, match="503"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    candidate = next(item for item in github.releases if item is not previous)
    assert candidate["tag_name"].startswith("axklib-incoming/")
    assert "Preparation: **COMPLETE**" in candidate["body"]
    assert len(github.assets[candidate["id"]]) == 13
    if failure == "delete":
        assert (previous, github.assets[previous["id"]]) == before
    else:
        assert previous not in github.releases
    monkeypatch.setattr(subprocess, "run", run)
    result = prepare_release.prepare_release(GitHub(req.repository), replace(req, run_attempt="2"))
    assert result.release_id == candidate["id"]
    assert len(github.releases) == 1 and candidate["tag_name"] == "snapshot/v0.6.0"


def test_publishing_previous_snapshot_during_upload_prevents_its_replacement(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    github.add_asset(previous["id"], "old.zip", b"previous build")
    assets = copy.deepcopy(github.assets[previous["id"]])
    run = github.run

    def publish(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        result = run(command, **kwargs)
        if command[command.index("--method") + 1] == "POST" and "/assets?" in command[2]:
            previous["draft"] = False
        return result

    monkeypatch.setattr(subprocess, "run", publish)
    with pytest.raises(ReleaseError, match="changed"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert previous in github.releases and not previous["draft"]
    assert github.assets[previous["id"]] == assets
    assert not any(method == "DELETE" for method, _ in github.writes)


@pytest.mark.parametrize("field", ["name", "prerelease", "body", "target_commitish"])
def test_candidate_metadata_edits_during_upload_stop_further_writes(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch, field: str
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    before = copy.deepcopy(previous)
    run = github.run
    candidate: dict[str, Any] = {}
    edited_value: object = None

    def edit(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        nonlocal candidate, edited_value
        result = run(command, **kwargs)
        if command[command.index("--method") + 1] == "POST" and "/assets?" in command[2]:
            candidate = next(item for item in github.releases if item is not previous)
            edited_value = {
                "name": "Manually edited candidate",
                "prerelease": False,
                "body": candidate["body"] + "\nManual note",
                "target_commitish": "a" * 40,
            }[field]
            candidate[field] = edited_value
        return result

    monkeypatch.setattr(subprocess, "run", edit)
    with pytest.raises(ReleaseError, match="changed"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert previous == before and previous in github.releases
    assert candidate[field] == edited_value
    assert len(github.writes) == 2


@pytest.mark.parametrize("tagged_release", ["candidate", "previous", "previous_early"])
def test_git_tag_created_during_preparation_stops_mutations_to_tagged_release(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch, tagged_release: str
) -> None:
    req = request(tmp_path)
    previous = github.release("snapshot/v0.6.0")
    before = copy.deepcopy(previous)
    run = github.run
    candidate: dict[str, Any] = {}
    writes_at_tagging: int | None = None

    def tag(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        nonlocal candidate, writes_at_tagging
        result = run(command, **kwargs)
        method = command[command.index("--method") + 1]
        if (
            writes_at_tagging is None
            and tagged_release in {"candidate", "previous_early"}
            and method == "POST"
            and "/assets?" in command[2]
        ):
            candidate = next(item for item in github.releases if item is not previous)
            github.tag(
                candidate["tag_name"] if tagged_release == "candidate" else previous["tag_name"]
            )
            writes_at_tagging = len(github.writes)
        elif (
            writes_at_tagging is None
            and tagged_release == "previous"
            and method == "GET"
            and command[2].endswith(f"/releases/{previous['id']}")
        ):
            candidate = next(item for item in github.releases if item is not previous)
            github.tag(previous["tag_name"])
            writes_at_tagging = len(github.writes)
        return result

    monkeypatch.setattr(subprocess, "run", tag)
    with pytest.raises(ReleaseError, match="snapshot Git ref"):
        prepare_release.prepare_release(GitHub(req.repository), req)
    assert writes_at_tagging is not None and len(github.writes) == writes_at_tagging
    assert previous == before and previous in github.releases
    assert candidate in github.releases and candidate["tag_name"].startswith(
        prepare_release.INCOMING_RELEASE
    )
    assert len(github.refs) == 1
