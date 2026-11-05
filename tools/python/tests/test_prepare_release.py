from __future__ import annotations

import copy
import hashlib
import json
import subprocess
from dataclasses import asdict, replace
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlsplit

import pytest
from test_release_assets import COMMIT, IDENTITY, branch_version, write_assets

import prepare_release
import version_metadata


class FakeGitHub:
    def __init__(self) -> None:
        self.releases: list[dict[str, Any]] = []
        self.assets: dict[int, list[dict[str, Any]]] = {}
        self.refs: list[dict[str, Any]] = []
        self.tags: dict[str, dict[str, Any]] = {}
        self.writes: list[tuple[str, str]] = []
        self.calls: list[list[str]] = []
        self.next_id = 100
        self.interrupt_after: int | None = None
        self.fail_upload = False
        self.fail_cleanup = False
        self.fail_listing = False
        self.lost_create_response: str | None = None
        self.corrupt_upload = False
        self.publish_after_upload = False
        self.leave_starter = False

    def release(self, tag: str, *, draft: bool = True) -> dict[str, Any]:
        self.next_id += 1
        item: dict[str, Any] = {
            "id": self.next_id,
            "tag_name": tag,
            "name": tag,
            "target_commitish": COMMIT,
            "draft": draft,
            "prerelease": True,
            "body": "Human release notes",
            "html_url": f"https://github.com/tphzz/axklib/releases/tag/untagged-{self.next_id}",
        }
        self.releases.append(item)
        self.assets[item["id"]] = []
        return item

    def tag(self, name: str, *, commit: str = COMMIT, annotated: bool = False) -> None:
        sha = "a" * 40 if annotated else commit
        self.refs.append(
            {
                "ref": f"refs/tags/{name}",
                "object": {"type": "tag" if annotated else "commit", "sha": sha},
            }
        )
        if annotated:
            self.tags[sha] = {"object": {"type": "commit", "sha": commit}}

    def add_asset(self, release_id: int, name: str, contents: bytes) -> dict[str, Any]:
        self.next_id += 1
        item = {
            "id": self.next_id,
            "name": name,
            "size": len(contents),
            "state": "uploaded",
            "digest": "sha256:" + hashlib.sha256(contents).hexdigest(),
        }
        self.assets[release_id].append(item)
        return item

    def run(self, command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        self.calls.append(command)
        method = command[command.index("--method") + 1]
        url = urlsplit(command[2])
        path = url.path.removeprefix("repos/tphzz/axklib/").removeprefix("/repos/tphzz/axklib/")
        query = parse_qs(url.query)
        payload: dict[str, Any] = (
            json.loads(kwargs.get("input") or "{}")
            if "--input" in command and command[command.index("--input") + 1] == "-"
            else {}
        )
        if method == "GET":
            if path == "releases":
                if self.fail_listing:
                    return subprocess.CompletedProcess(command, 1, "[[]]", "HTTP 403 on later page")
                # Always split pages so tests exercise flattening and draft lookup.
                result: Any = [[item] for item in self.releases] or [[]]
            elif path == "git/matching-refs/tags/":
                result = [self.refs]
            elif path.startswith("git/tags/"):
                result = self.tags[path.rsplit("/", 1)[1]]
            elif path.endswith("/assets"):
                items = self.assets[int(path.split("/")[1])]
                result = [items[index : index + 2] for index in range(0, len(items), 2)] or [[]]
            else:
                result = next(
                    item for item in self.releases if item["id"] == int(path.split("/")[1])
                )
            if path == "releases" or path.endswith("/assets") or path == "git/matching-refs/tags/":
                assert "--paginate" in command and "--slurp" in command
            return subprocess.CompletedProcess(command, 0, json.dumps(result), "")

        assert not path.startswith("git/"), "release preparation must never mutate Git refs"
        if payload is not None and "draft" in payload:
            assert payload["draft"] is True, "automatic publication is forbidden"
        if method == "POST" and path.endswith("/assets"):
            if self.fail_upload:
                if self.leave_starter:
                    item = self.add_asset(int(path.split("/")[1]), query["name"][0], b"")
                    item.update(state="starter", digest=None)
                return subprocess.CompletedProcess(command, 1, "", "HTTP 502 upload interrupted")
            release_id = int(path.split("/")[1])
            name = query["name"][0]
            assert not any(item["name"] == name for item in self.assets[release_id])
            contents = Path(command[command.index("--input") + 1]).read_bytes()
            result = self.add_asset(release_id, name, contents)
            if self.corrupt_upload:
                result["digest"] = "sha256:" + "0" * 64
            if self.publish_after_upload:
                next(item for item in self.releases if item["id"] == release_id)["draft"] = False
        elif method == "POST" and path == "releases":
            result = self.release(payload["tag_name"])
            result.update(payload)
        elif method == "PATCH" and path.startswith("releases/assets/"):
            asset_id = int(path.rsplit("/", 1)[1])
            result = next(
                item for items in self.assets.values() for item in items if item["id"] == asset_id
            )
            release_assets = next(items for items in self.assets.values() if result in items)
            assert not any(
                item["id"] != asset_id and item["name"] == payload["name"]
                for item in release_assets
            )
            result.update(payload)
        elif method == "PATCH":
            result = next(item for item in self.releases if item["id"] == int(path.split("/")[1]))
            result.update(payload)
        elif path.startswith("releases/assets/"):
            asset_id = int(path.rsplit("/", 1)[1])
            for items in self.assets.values():
                items[:] = [item for item in items if item["id"] != asset_id]
            result = None
        else:
            if self.fail_cleanup:
                return subprocess.CompletedProcess(command, 1, "", "HTTP 503 cleanup interrupted")
            release_id = int(path.split("/")[1])
            self.releases[:] = [item for item in self.releases if item["id"] != release_id]
            self.assets.pop(release_id)
            result = None
        self.writes.append((method, path))
        if len(self.writes) == self.interrupt_after:
            raise KeyboardInterrupt("process interrupted after server accepted mutation")
        if method == "POST" and path == "releases" and self.lost_create_response:
            if self.lost_create_response == "timeout":
                raise subprocess.TimeoutExpired(command, 600)
            if self.lost_create_response == "discovery-failure":
                self.fail_listing = True
            return subprocess.CompletedProcess(command, 0, "not JSON", "")
        return subprocess.CompletedProcess(
            command, 0, json.dumps(result) if result is not None else "", ""
        )


@pytest.fixture
def github(monkeypatch: pytest.MonkeyPatch) -> FakeGitHub:
    fake = FakeGitHub()
    monkeypatch.setenv("GH_TOKEN", "fake-token-never-send-to-github")
    monkeypatch.setattr(prepare_release.subprocess, "run", fake.run)
    return fake


def request(
    tmp_path: Path, *, ref_type: str = "branch", name: str = "v0.6.0"
) -> prepare_release.ReleaseRequest:
    version = branch_version()
    identity = IDENTITY
    if ref_type == "tag":
        version = version_metadata.VersionMetadata(1, "0.6.0", "0.6.0", 0, 6, 0, name, True, False)
        identity = "0.6.0"
    write_assets(tmp_path, identity)
    return prepare_release.ReleaseRequest(
        "tphzz/axklib", ref_type, name, COMMIT, version, tmp_path, "Generated notes", "234", "1"
    )


@pytest.mark.parametrize("draft", [True, False])
def test_tag_rerun_preserves_everything_including_notes_and_extra_assets(
    tmp_path: Path, github: FakeGitHub, draft: bool
) -> None:
    req = request(tmp_path, ref_type="tag")
    github.tag(req.ref_name, annotated=True)
    github.release("another-release", draft=False)
    existing = github.release(req.ref_name, draft=draft)
    existing["target_commitish"] = "main"
    github.add_asset(existing["id"], "manual-attachment.txt", b"preserve")
    before = copy.deepcopy((github.releases, github.assets, github.refs))
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert result.status == "preserved"
    assert result.html_url == existing["html_url"]
    assert (github.releases, github.assets, github.refs) == before
    assert github.writes == []


def test_new_tag_draft_uses_exact_tag_and_full_commit(tmp_path: Path, github: FakeGitHub) -> None:
    req = request(tmp_path, ref_type="tag", name="0.6.0")
    github.tag(req.ref_name, annotated=True)
    refs = copy.deepcopy(github.refs)
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    release = github.releases[0]
    assert result.status == "created"
    assert release["tag_name"] == "0.6.0" and release["target_commitish"] == COMMIT
    assert release["draft"] and not release["prerelease"]
    assert len(github.assets[release["id"]]) == 13
    assert github.refs == refs
    assert "COMPLETE" in release["body"] and "INCOMPLETE" not in release["body"]


def test_branch_refresh_preserves_id_and_uploads_before_deleting(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path)
    existing = github.release("snapshot/v0.6.0")
    github.add_asset(existing["id"], "old-package.zip", b"old")
    github.add_asset(existing["id"], "SHA256SUMS", b"old checksums")
    unrelated = github.release("snapshot/main")
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert result.status == "refreshed" and result.release_id == existing["id"]
    assert "Human release notes" in existing["body"]
    assert unrelated["body"] == "Human release notes"
    writes = github.writes
    upload_positions = [
        index
        for index, (method, path) in enumerate(writes)
        if method == "POST" and path.endswith("/assets")
    ]
    delete_positions = [index for index, (method, _) in enumerate(writes) if method == "DELETE"]
    assert max(upload_positions) < min(delete_positions)
    assert {item["name"] for item in github.assets[existing["id"]]} == {
        path.name for path in tmp_path.iterdir()
    }


def test_same_commit_rerun_replaces_changed_bytes_without_filename_collisions(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path)
    client = prepare_release.GitHub(req.repository)
    first = prepare_release.prepare_release(client, req)
    package = next(tmp_path.glob("*-windows-x64.exe"))
    package.write_bytes(package.read_bytes() + b"changed rebuild")
    second = prepare_release.prepare_release(client, req)
    third = prepare_release.prepare_release(client, req)
    assert first.release_id == second.release_id == third.release_id
    assets = github.assets[first.release_id]
    assert len(assets) == 13
    assert (
        next(item for item in assets if item["name"] == package.name)["digest"]
        == "sha256:" + hashlib.sha256(package.read_bytes()).hexdigest()
    )


@pytest.mark.parametrize("conflict", ["published", "tag", "ancestor-tag", "legacy", "duplicate"])
def test_branch_conflicts_require_reconciliation_without_writes(
    tmp_path: Path, github: FakeGitHub, conflict: str
) -> None:
    req = request(tmp_path)
    if conflict == "published":
        github.release("snapshot/v0.6.0", draft=False)
    elif conflict == "tag":
        github.tag("snapshot/v0.6.0")
    elif conflict == "ancestor-tag":
        github.tag("snapshot")
    elif conflict == "legacy":
        github.release("v0.6.0-preview")
    else:
        github.release("snapshot/v0.6.0")
        github.release("snapshot/v0.6.0")
    with pytest.raises(prepare_release.ReleaseError, match="reconcil|migration|multiple"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert github.writes == []


def test_listing_failure_cannot_become_absent_release(tmp_path: Path, github: FakeGitHub) -> None:
    req = request(tmp_path)
    github.fail_listing = True
    with pytest.raises(prepare_release.ReleaseError, match="403"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert github.writes == []


def test_invalid_inventory_cannot_even_mark_existing_draft_incomplete(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path)
    existing = github.release("snapshot/v0.6.0")
    next(tmp_path.glob("*.dmg")).write_bytes(b"garbage")
    with pytest.raises(ValueError, match="invalid desktop"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert existing["body"] == "Human release notes" and github.writes == []


@pytest.mark.parametrize("cleanup_fails", [False, True])
def test_failed_initial_tag_cleans_only_its_new_draft_or_reports_manual_recovery(
    tmp_path: Path, github: FakeGitHub, cleanup_fails: bool
) -> None:
    req = request(tmp_path, ref_type="tag")
    github.tag(req.ref_name)
    other = github.release("unrelated")
    github.fail_upload = True
    github.fail_cleanup = cleanup_fails
    with pytest.raises(
        prepare_release.ReleaseError, match="manual recovery" if cleanup_fails else "502"
    ):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert other in github.releases
    assert len(github.refs) == 1
    assert len(github.releases) == (2 if cleanup_fails else 1)


def test_branch_recovers_from_process_death_after_every_server_mutation(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch
) -> None:
    req = request(tmp_path)
    baseline = FakeGitHub()
    monkeypatch.setattr(prepare_release.subprocess, "run", baseline.run)
    baseline.release("snapshot/v0.6.0")
    baseline.add_asset(baseline.releases[0]["id"], "SHA256SUMS", b"old")
    prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    mutations = len(baseline.writes)
    for interruption in range(1, mutations + 1):
        fake = FakeGitHub()
        monkeypatch.setattr(prepare_release.subprocess, "run", fake.run)
        existing = fake.release("snapshot/v0.6.0")
        fake.add_asset(existing["id"], "SHA256SUMS", b"old")
        fake.interrupt_after = interruption
        with pytest.raises(KeyboardInterrupt):
            prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
        if interruption < mutations:
            assert "INCOMPLETE" in existing["body"]
        fake.interrupt_after = None
        result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
        assert result.release_id == existing["id"]
        assert len(fake.assets[result.release_id]) == 13
        assert "INCOMPLETE" not in existing["body"]


@pytest.mark.parametrize("ref_type", ["branch", "tag"])
@pytest.mark.parametrize("failure", ["timeout", "malformed"])
def test_accepted_creation_with_lost_response_is_owned_and_recoverable(
    tmp_path: Path, github: FakeGitHub, ref_type: str, failure: str
) -> None:
    req = request(tmp_path, ref_type=ref_type)
    if ref_type == "tag":
        github.tag(req.ref_name)
    other = github.release("unrelated", draft=False)
    refs = copy.deepcopy(github.refs)
    github.lost_create_response = failure
    with pytest.raises(prepare_release.ReleaseError):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert other in github.releases and github.refs == refs
    assert sum(method == "POST" and path == "releases" for method, path in github.writes) == 1
    github.lost_create_response = None
    if ref_type == "tag":
        assert github.releases == [other]
    else:
        incomplete = next(item for item in github.releases if item is not other)
        assert "INCOMPLETE" in incomplete["body"]
        result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
        assert result.release_id == incomplete["id"]
        assert len(github.assets[result.release_id]) == 13


def test_unknown_creation_outcome_is_not_retried_or_blindly_deleted(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path, ref_type="tag")
    github.tag(req.ref_name)
    github.lost_create_response = "discovery-failure"
    with pytest.raises(prepare_release.ReleaseError, match="outcome unknown; manual recovery"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert len(github.releases) == 1 and len(github.refs) == 1
    assert github.writes == [("POST", "releases")]


@pytest.mark.parametrize("failure", ["starter", "digest"])
def test_bad_incoming_upload_keeps_old_assets_and_retry_repairs_it(
    tmp_path: Path, github: FakeGitHub, failure: str
) -> None:
    req = request(tmp_path)
    existing = github.release("snapshot/v0.6.0")
    old = github.add_asset(existing["id"], "old.zip", b"previous build")
    github.fail_upload = failure == "starter"
    github.leave_starter = failure == "starter"
    github.corrupt_upload = failure == "digest"
    with pytest.raises(prepare_release.ReleaseError):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert old in github.assets[existing["id"]]
    assert not any(method == "DELETE" for method, _ in github.writes)
    assert "INCOMPLETE" in existing["body"]
    github.fail_upload = github.corrupt_upload = False
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert len(github.assets[result.release_id]) == 13
    assert "INCOMPLETE" not in existing["body"]


def test_publication_during_upload_stops_all_further_writes(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path)
    existing = github.release("snapshot/v0.6.0")
    old = github.add_asset(existing["id"], "old.zip", b"previous build")
    github.publish_after_upload = True
    with pytest.raises(prepare_release.ReleaseError, match="published"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert not existing["draft"]
    assert old in github.assets[existing["id"]]
    assert len(github.writes) == 2  # incomplete marker, then the accepted upload


def test_interrupted_tag_cleanup_is_preserved_on_an_ordinary_rerun(
    tmp_path: Path, github: FakeGitHub
) -> None:
    req = request(tmp_path, ref_type="tag")
    github.tag(req.ref_name)
    github.fail_upload = github.fail_cleanup = True
    with pytest.raises(prepare_release.ReleaseError, match="manual recovery"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    before = copy.deepcopy((github.releases, github.assets, github.writes))
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert result.status == "preserved"
    assert (github.releases, github.assets, github.writes) == before


def test_snapshot_uses_exact_branch_name_and_never_mutates_other_branches(
    tmp_path: Path, github: FakeGitHub
) -> None:
    ordinary = version_metadata.VersionMetadata(1, "0.0.0", "0.0.0", 0, 0, 0, "", False, False)
    write_assets(tmp_path, "feature-audio-8c783a7")
    req = prepare_release.ReleaseRequest(
        "tphzz/axklib", "branch", "feature/audio", COMMIT, ordinary, tmp_path, "Notes", "234", "1"
    )
    other = github.release("snapshot/feature-audio")
    tag = github.release("v0.5.1", draft=False)
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert result.tag_name == "snapshot/feature/audio"
    assert other["body"] == tag["body"] == "Human release notes"


def test_missing_token_is_rejected_without_fallback(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("GH_TOKEN")
    monkeypatch.setenv("GITHUB_TOKEN", "must-not-be-used")
    with pytest.raises(prepare_release.ReleaseError, match="GH_TOKEN is required"):
        prepare_release.GitHub("tphzz/axklib")
    assert not github.calls


def test_two_branches_with_identical_sanitized_filenames_remain_isolated(
    tmp_path: Path, github: FakeGitHub
) -> None:
    ordinary = version_metadata.VersionMetadata(1, "0.0.0", "0.0.0", 0, 0, 0, "", False, False)
    write_assets(tmp_path, "feature-audio-8c783a7")
    req = prepare_release.ReleaseRequest(
        "tphzz/axklib", "branch", "feature/audio", COMMIT, ordinary, tmp_path, "Notes", "234", "1"
    )
    client = prepare_release.GitHub(req.repository)
    first = prepare_release.prepare_release(client, req)
    first_state = copy.deepcopy((github.releases, github.assets))
    second = prepare_release.prepare_release(client, replace(req, ref_name="feature-audio"))
    assert first.release_id != second.release_id
    assert github.releases[0] == first_state[0][0]
    assert github.assets[first.release_id] == first_state[1][first.release_id]
    assert set(item["name"] for item in github.assets[first.release_id]) == set(
        item["name"] for item in github.assets[second.release_id]
    )


@pytest.mark.parametrize("problem", ["missing", "digest", "extra"])
def test_final_inventory_failure_leaves_draft_incomplete_until_retry(
    tmp_path: Path, github: FakeGitHub, monkeypatch: pytest.MonkeyPatch, problem: str
) -> None:
    req = request(tmp_path)
    original = prepare_release.verify_inventory
    corrupt = True

    def verify(
        client: prepare_release.GitHub,
        release: prepare_release.Release,
        assets: list[prepare_release.release_metadata.ReleaseAsset],
    ) -> None:
        nonlocal corrupt
        if corrupt:
            corrupt = False
            if problem == "missing":
                github.assets[release.id].pop()
            elif problem == "digest":
                github.assets[release.id][0]["digest"] = "sha256:" + "0" * 64
            else:
                github.add_asset(release.id, "unexpected.txt", b"unexpected")
        original(client, release, assets)

    monkeypatch.setattr(prepare_release, "verify_inventory", verify)
    with pytest.raises(prepare_release.ReleaseError, match="final release inventory"):
        prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert "INCOMPLETE" in github.releases[0]["body"]
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert len(github.assets[result.release_id]) == 13
    assert "INCOMPLETE" not in github.releases[0]["body"]


def test_semver_prerelease_and_build_metadata_survive_upload_names(
    tmp_path: Path, github: FakeGitHub
) -> None:
    version = version_metadata.VersionMetadata(
        1, "0.6.0-rc.1+build.7", "0.6.0", 0, 6, 0, "v0.6.0-rc.1+build.7", True, True
    )
    write_assets(tmp_path, version.semantic_version)
    github.tag(version.release_tag)
    req = prepare_release.ReleaseRequest(
        "tphzz/axklib", "tag", version.release_tag, COMMIT, version, tmp_path, "Notes", "234", "1"
    )
    result = prepare_release.prepare_release(prepare_release.GitHub(req.repository), req)
    assert result.tag_name == version.release_tag
    assert github.releases[0]["prerelease"]
    assert any("+build.7" in asset["name"] for asset in github.assets[result.release_id])


def test_cli_collects_separate_artifacts_and_reports_actual_url(
    tmp_path: Path,
    github: FakeGitHub,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    downloads = tmp_path / "downloads"
    write_assets(downloads / "release-test")
    version_file = tmp_path / "version.json"
    version_file.write_text(json.dumps(asdict(branch_version())))
    notes_file = tmp_path / "notes.md"
    notes_file.write_text("Generated notes")
    output = tmp_path / "outputs"
    summary = tmp_path / "summary.md"

    def run(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        if command == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(command, 0, COMMIT + "\n", "")
        return github.run(command, **kwargs)

    monkeypatch.setattr(prepare_release.subprocess, "run", run)
    monkeypatch.setattr(
        "sys.argv",
        [
            "prepare_release.py",
            "--repository",
            "tphzz/axklib",
            "--ref-type",
            "branch",
            "--ref-name",
            "v0.6.0",
            "--commit",
            COMMIT,
            "--version-metadata-file",
            str(version_file),
            "--artifacts-directory",
            str(downloads),
            "--notes-file",
            str(notes_file),
            "--run-id",
            "234",
            "--run-attempt",
            "1",
            "--github-output",
            str(output),
            "--github-step-summary",
            str(summary),
        ],
    )
    assert prepare_release.main() == 0
    result = json.loads(capsys.readouterr().out)
    assert result["status"] == "created"
    assert result["html_url"] == github.releases[0]["html_url"]
    assert f"html_url={result['html_url']}" in output.read_text()
    assert result["html_url"] in summary.read_text()
    assert len(list((tmp_path / "release-assets").iterdir())) == 13
