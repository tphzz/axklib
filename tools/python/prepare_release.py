"""Prepare unpublished releases without creating or changing Git refs."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import uuid
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import release_metadata
import version_metadata

START = "<!-- axklib-release:start -->"
END = "<!-- axklib-release:end -->"
STATE = "<!-- axklib-release-state:"
STAGING = "axklib-incoming-"


class ReleaseError(RuntimeError):
    pass


def object_value(value: object) -> dict[str, Any]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise ReleaseError("GitHub returned an invalid JSON object")
    return value


def string_value(value: dict[str, Any], key: str) -> str:
    result = value.get(key)
    if not isinstance(result, str) or not result or "\r" in result or "\n" in result:
        raise ReleaseError(f"GitHub returned an invalid {key}")
    return result


def id_value(value: dict[str, Any]) -> int:
    result = value.get("id")
    if type(result) is not int or result <= 0:
        raise ReleaseError("GitHub returned an invalid object ID")
    return result


@dataclass(frozen=True)
class Release:
    id: int
    tag_name: str
    target_commitish: str
    draft: bool
    prerelease: bool
    body: str
    html_url: str

    @classmethod
    def parse(cls, raw: object, repository: str) -> Release:
        value = object_value(raw)
        if type(value.get("draft")) is not bool or type(value.get("prerelease")) is not bool:
            raise ReleaseError("GitHub returned invalid release flags")
        body = value.get("body")
        if body is not None and not isinstance(body, str):
            raise ReleaseError("GitHub returned an invalid release body")
        url = string_value(value, "html_url")
        if not url.startswith(f"https://github.com/{repository}/releases/"):
            raise ReleaseError("GitHub returned an unexpected release URL")
        return cls(
            id_value(value),
            string_value(value, "tag_name"),
            string_value(value, "target_commitish"),
            value["draft"],
            value["prerelease"],
            body or "",
            url,
        )


@dataclass(frozen=True)
class Asset:
    id: int
    name: str
    size: int
    state: str
    digest: str | None

    @classmethod
    def parse(cls, raw: object) -> Asset:
        value = object_value(raw)
        size = value.get("size")
        digest = value.get("digest")
        if type(size) is not int or size < 0:
            raise ReleaseError("GitHub returned an invalid asset size")
        if digest is not None and (
            not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
        ):
            raise ReleaseError("GitHub returned an invalid asset digest")
        return cls(
            id_value(value), string_value(value, "name"), size, string_value(value, "state"), digest
        )

    def matches(self, expected: release_metadata.ReleaseAsset) -> bool:
        return (
            self.state == "uploaded"
            and self.size == expected.size
            and self.digest == f"sha256:{expected.sha256}"
        )


@dataclass(frozen=True)
class ReleaseRequest:
    repository: str
    ref_type: str
    ref_name: str
    commit: str
    version: version_metadata.VersionMetadata
    assets_directory: Path
    notes: str
    run_id: str
    run_attempt: str
    artifacts_directory: Path | None = None


@dataclass(frozen=True)
class ReleaseResult:
    status: str
    release_id: int
    tag_name: str
    target_commit: str
    html_url: str


class GitHub:
    def __init__(self, repository: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ReleaseError("repository must be OWNER/NAME")
        if not os.environ.get("GH_TOKEN", "").strip():
            raise ReleaseError(
                "GH_TOKEN is required; configure secrets.GH_TOKEN on the release step"
            )
        self.repository = repository

    def request(
        self,
        method: str,
        path: str,
        *,
        data: dict[str, object] | None = None,
        file: Path | None = None,
        paginate: bool = False,
    ) -> object:
        endpoint = path if path.startswith("https://") else f"repos/{self.repository}/{path}"
        command = [
            "gh",
            "api",
            endpoint,
            "--hostname",
            "github.com",
            "--method",
            method,
            "-H",
            "Accept: application/vnd.github+json",
        ]
        if paginate:
            command.extend(["--paginate", "--slurp"])
        if data is not None:
            command.extend(["--input", "-"])
        if file is not None:
            command.extend(["--input", str(file), "-H", "Content-Type: application/octet-stream"])
        try:
            completed = subprocess.run(
                command,
                input=json.dumps(data) if data is not None else None,
                capture_output=True,
                text=True,
                check=False,
                timeout=600,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ReleaseError(f"GitHub {method} {path} failed: {type(error).__name__}") from error
        if completed.returncode:
            details = completed.stderr.strip().replace(os.environ["GH_TOKEN"], "<redacted>")
            raise ReleaseError(f"GitHub {method} {path} failed: {details}")
        if not completed.stdout.strip():
            if method == "DELETE":
                return None
            raise ReleaseError(f"GitHub {method} {path} returned an empty response")
        try:
            result: object = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise ReleaseError(f"GitHub {method} {path} returned malformed JSON") from error
        return result

    def all_objects(self, path: str) -> list[dict[str, Any]]:
        pages = self.request("GET", path + "?per_page=100", paginate=True)
        if (
            not isinstance(pages, list)
            or not pages
            or any(not isinstance(page, list) for page in pages)
        ):
            raise ReleaseError("GitHub returned invalid paginated results")
        return [object_value(value) for page in pages for value in page]

    def releases(self) -> list[Release]:
        return [Release.parse(value, self.repository) for value in self.all_objects("releases")]

    def release(self, release_id: int) -> Release:
        return Release.parse(self.request("GET", f"releases/{release_id}"), self.repository)

    def assets(self, release_id: int) -> dict[str, Asset]:
        assets = [Asset.parse(value) for value in self.all_objects(f"releases/{release_id}/assets")]
        if len({asset.name for asset in assets}) != len(assets):
            raise ReleaseError("GitHub returned multiple assets with the same filename")
        return {asset.name: asset for asset in assets}


def lookup(releases: list[Release], tag: str) -> Release | None:
    matches = [release for release in releases if release.tag_name == tag]
    if len(matches) > 1:
        raise ReleaseError(
            f"multiple releases match {tag!r}; manual reconciliation required: "
            + ", ".join(release.html_url for release in matches)
        )
    return matches[0] if matches else None


def verify_refs(client: GitHub, request: ReleaseRequest, tag: str) -> None:
    refs = client.all_objects("git/matching-refs/tags/")
    selected: dict[str, Any] | None = None
    for ref in refs:
        name = string_value(ref, "ref")
        if not name.startswith("refs/tags/"):
            raise ReleaseError("GitHub returned an unexpected Git ref")
        reference_object = object_value(ref.get("object"))
        if reference_object.get("type") not in {"commit", "tag"} or not re.fullmatch(
            r"[0-9a-f]{40}", string_value(reference_object, "sha")
        ):
            raise ReleaseError("GitHub returned an invalid Git ref object")
        if request.ref_type == "branch":
            target = f"refs/tags/{tag}"
            if name == target or name.startswith(target + "/") or target.startswith(name + "/"):
                raise ReleaseError(
                    f"snapshot Git ref {name!r} conflicts with {tag!r}; manual reconciliation required"
                )
        elif name == f"refs/tags/{tag}":
            selected = ref
    if request.ref_type == "tag":
        if selected is None:
            raise ReleaseError(
                f"selected release tag {tag!r} does not exist; no Git ref will be created"
            )
        target_object = object_value(selected.get("object"))
        visited: set[str] = set()
        while string_value(target_object, "type") == "tag":
            sha = string_value(target_object, "sha")
            if not re.fullmatch(r"[0-9a-f]{40}", sha) or sha in visited or len(visited) >= 32:
                raise ReleaseError("invalid annotated Git tag chain")
            visited.add(sha)
            target_object = object_value(
                object_value(client.request("GET", f"git/tags/{sha}")).get("object")
            )
        if target_object.get("type") != "commit" or target_object.get("sha") != request.commit:
            raise ReleaseError(f"release tag {tag!r} does not identify the full built commit")


def state_body(
    body: str,
    request: ReleaseRequest,
    assets: list[release_metadata.ReleaseAsset],
    operation: str,
    phase: str,
) -> str:
    if START in body or END in body:
        if body.count(START) != 1 or body.count(END) != 1 or body.index(START) > body.index(END):
            raise ReleaseError("malformed managed release notes; manual reconciliation required")
        before, remaining = body.split(START)
        _, after = remaining.split(END)
    else:
        before, after = body.rstrip() + ("\n\n" if body.strip() else ""), ""
    state = {
        "schema_version": 1,
        "ref_type": request.ref_type,
        "ref_name": request.ref_name,
        "commit": request.commit,
        "operation": operation,
        "run_id": request.run_id,
        "run_attempt": request.run_attempt,
        "phase": phase,
        "inventory": [asset.inventory_entry() for asset in assets],
    }
    encoded = json.dumps(state, sort_keys=True).replace("<", "\\u003c").replace(">", "\\u003e")
    return f"{before}{START}\nPreparation: **{phase.upper()}**\n\n{request.notes.rstrip()}\n\n{STATE}{encoded} -->\n{END}{after}"


def owned(release: Release, operation: str, request: ReleaseRequest) -> bool:
    match = re.search(re.escape(STATE) + r"(.*?) -->", release.body)
    if match is None:
        return False
    try:
        state = object_value(json.loads(match.group(1)))
    except (json.JSONDecodeError, ReleaseError):
        return False
    return (
        release.draft
        and release.target_commitish == request.commit
        and state.get("operation") == operation
        and state.get("commit") == request.commit
        and state.get("ref_name") == request.ref_name
    )


def require_draft(client: GitHub, release: Release) -> Release:
    current = client.release(release.id)
    if not current.draft or current.tag_name != release.tag_name:
        raise ReleaseError(
            f"release changed or was published during preparation; preserving {current.html_url}"
        )
    return current


def verify_release(
    release: Release,
    request: ReleaseRequest,
    target: release_metadata.DraftReleaseTarget,
    body: str,
) -> None:
    if (
        not release.draft
        or release.tag_name != target.tag_name
        or release.target_commitish != request.commit
        or release.prerelease != target.prerelease
        or release.body != body
    ):
        raise ReleaseError(f"GitHub did not retain the expected draft metadata: {release.html_url}")


def release_data(
    request: ReleaseRequest, target: release_metadata.DraftReleaseTarget, body: str
) -> dict[str, object]:
    return {
        "tag_name": target.tag_name,
        "name": target.title,
        "target_commitish": request.commit,
        "draft": True,
        "prerelease": target.prerelease,
        "body": body,
    }


def update_release(
    client: GitHub,
    release: Release,
    request: ReleaseRequest,
    target: release_metadata.DraftReleaseTarget,
    body: str,
) -> Release:
    require_draft(client, release)
    result = Release.parse(
        client.request("PATCH", f"releases/{release.id}", data=release_data(request, target, body)),
        client.repository,
    )
    if result.id != release.id:
        raise ReleaseError("GitHub returned a different release ID")
    verify_release(result, request, target, body)
    return result


def temporary_name(asset: release_metadata.ReleaseAsset) -> str:
    return f"{STAGING}{asset.sha256}-{asset.name}"


def verify_inventory(
    client: GitHub, release: Release, assets: list[release_metadata.ReleaseAsset]
) -> None:
    remote = client.assets(release.id)
    if set(remote) != {asset.name for asset in assets} or any(
        not remote[asset.name].matches(asset) for asset in assets
    ):
        raise ReleaseError(f"final release inventory verification failed: {release.html_url}")


def refresh_assets(
    client: GitHub, release: Release, assets: list[release_metadata.ReleaseAsset]
) -> None:
    remote = client.assets(release.id)
    for asset in assets:
        if asset.name in remote and remote[asset.name].matches(asset):
            continue
        name = temporary_name(asset)
        if name in remote and not remote[name].matches(asset):
            require_draft(client, release)
            client.request("DELETE", f"releases/assets/{remote[name].id}")
            del remote[name]
        if name not in remote:
            require_draft(client, release)
            if release_metadata.describe_asset(asset.path) != asset:
                raise ReleaseError(f"local release asset changed during preparation: {asset.name}")
            endpoint = f"https://uploads.github.com/repos/{client.repository}/releases/{release.id}/assets?{urlencode({'name': name})}"
            uploaded = Asset.parse(client.request("POST", endpoint, file=asset.path))
            if uploaded.name != name or not uploaded.matches(asset):
                raise ReleaseError(f"incoming asset verification failed: {asset.name}")
            remote[name] = uploaded

    # Re-read every page before any old final filename can be removed.
    remote = client.assets(release.id)
    for asset in assets:
        if not any(
            name in remote and remote[name].matches(asset)
            for name in (asset.name, temporary_name(asset))
        ):
            raise ReleaseError(f"incoming release inventory is incomplete: {asset.name}")

    # Promote the checksum last; all incoming bytes are already uploaded and verified.
    for asset in sorted(assets, key=lambda item: (item.name == "SHA256SUMS", item.name)):
        current = remote.get(asset.name)
        if current is not None and current.matches(asset):
            continue
        incoming = remote[temporary_name(asset)]
        if current is not None:
            require_draft(client, release)
            client.request("DELETE", f"releases/assets/{current.id}")
        require_draft(client, release)
        promoted = Asset.parse(
            client.request("PATCH", f"releases/assets/{incoming.id}", data={"name": asset.name})
        )
        if promoted.name != asset.name or not promoted.matches(asset):
            raise ReleaseError(f"asset promotion verification failed: {asset.name}")

    remote = client.assets(release.id)
    for asset in assets:
        if asset.name not in remote or not remote[asset.name].matches(asset):
            raise ReleaseError(f"replacement inventory is incomplete: {asset.name}")
    expected = {asset.name for asset in assets}
    for name, remote_asset in remote.items():
        if name not in expected:
            require_draft(client, release)
            client.request("DELETE", f"releases/assets/{remote_asset.id}")
    verify_inventory(client, release, assets)


def prepare_release(client: GitHub, request: ReleaseRequest) -> ReleaseResult:
    if client.repository != request.repository or not re.fullmatch(r"[0-9a-f]{40}", request.commit):
        raise ReleaseError("release preparation requires the repository and full built commit")
    if request.ref_type == "branch" and request.version.is_release:
        raise ReleaseError("branch preparation requires non-release version metadata")
    target = release_metadata.draft_release_target(
        request.ref_type, request.ref_name, request.version
    )
    releases = client.releases()
    existing = lookup(releases, target.tag_name)
    verify_refs(client, request, target.tag_name)
    if request.ref_type == "tag" and existing is not None:
        return ReleaseResult(
            "preserved", existing.id, existing.tag_name, request.commit, existing.html_url
        )
    if existing is not None and not existing.draft:
        raise ReleaseError(
            f"published snapshot requires manual reconciliation: {existing.html_url}"
        )
    if request.ref_type == "branch":
        legacy = lookup(releases, f"{request.ref_name}-preview")
        if legacy is not None:
            raise ReleaseError(
                f"legacy preview requires manual migration before preparing {target.tag_name!r}: {legacy.html_url}"
            )

    if request.artifacts_directory is not None:
        release_metadata.collect_release_artifacts(
            request.artifacts_directory, request.assets_directory
        )
    assets = release_metadata.prepare_release_assets(
        request.assets_directory,
        version=request.version,
        ref_type=request.ref_type,
        ref_name=request.ref_name,
        commit=request.commit,
    )
    operation = uuid.uuid4().hex
    body = state_body(
        existing.body if existing is not None else "", request, assets, operation, "incomplete"
    )
    release: Release | None = existing
    try:
        if release is None:
            release = Release.parse(
                client.request("POST", "releases", data=release_data(request, target, body)),
                client.repository,
            )
            verify_release(release, request, target, body)
        else:
            release = update_release(client, release, request, target, body)
        refresh_assets(client, release, assets)
        complete_body = state_body(release.body, request, assets, operation, "complete")
        release = update_release(client, release, request, target, complete_body)
        return ReleaseResult(
            "created" if existing is None else "refreshed",
            release.id,
            release.tag_name,
            request.commit,
            release.html_url,
        )
    except Exception as error:
        # An accepted POST can lose its response. Discover only this invocation's
        # draft; never retry creation blindly or clean up a pre-existing release.
        if release is None:
            try:
                candidate = lookup(client.releases(), target.tag_name)
                if candidate is not None and owned(candidate, operation, request):
                    release = candidate
            except ReleaseError as discovery_error:
                raise ReleaseError(
                    f"{error}\nCreation outcome unknown; manual recovery required for {target.tag_name!r}, operation {operation}: {discovery_error}"
                ) from error
        if release is not None:
            recovery = f"Draft {release.id}: {release.html_url}"
            if existing is None and request.ref_type == "tag":
                try:
                    current = require_draft(client, release)
                    if not owned(current, operation, request):
                        raise ReleaseError("draft ownership changed")
                    client.request("DELETE", f"releases/{release.id}")
                    recovery += "; newly created draft removed; Git tag retained"
                except ReleaseError as cleanup_error:
                    recovery += f"; manual recovery required: {cleanup_error}. Delete only this release ID after verifying it is still this invocation's draft; preserve any published release and the Git tag."
            else:
                recovery += "; if still a draft, retry the full build to repair it; published releases require manual reconciliation"
            raise ReleaseError(f"{error}\n{recovery}") from error
        raise ReleaseError(
            f"{error}\nCreation outcome unknown; manual recovery may be required for {target.tag_name!r}, operation {operation}; do not delete any Git ref."
        ) from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--ref-type", required=True, choices=("branch", "tag"))
    parser.add_argument("--ref-name", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version-metadata-file", required=True, type=Path)
    parser.add_argument("--artifacts-directory", required=True, type=Path)
    parser.add_argument("--notes-file", required=True, type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument("--github-output", type=Path, default=os.environ.get("GITHUB_OUTPUT"))
    parser.add_argument(
        "--github-step-summary", type=Path, default=os.environ.get("GITHUB_STEP_SUMMARY")
    )
    args = parser.parse_args()
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], check=True, capture_output=True, text=True
        ).stdout.strip()
        if head != args.commit:
            raise ReleaseError("checked-out commit differs from the full built commit")
        request = ReleaseRequest(
            args.repository,
            args.ref_type,
            args.ref_name,
            args.commit,
            version_metadata.read(args.version_metadata_file),
            args.artifacts_directory.parent / "release-assets",
            args.notes_file.read_text(encoding="utf-8"),
            args.run_id,
            args.run_attempt,
            args.artifacts_directory,
        )
        result = prepare_release(GitHub(args.repository), request)
    except (ReleaseError, ValueError, OSError, subprocess.SubprocessError) as error:
        message = f"Release preparation failed: {error}"
        print(message, file=sys.stderr)
        if args.github_step_summary:
            with args.github_step_summary.open("a", encoding="utf-8") as summary:
                summary.write(message + "\n")
        return 1
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8", newline="\n") as output:
            for key, value in asdict(result).items():
                output.write(f"{key}={value}\n")
    if args.github_step_summary:
        with args.github_step_summary.open("a", encoding="utf-8") as summary:
            summary.write(f"Release **{result.status}**: [{result.tag_name}]({result.html_url})\n")
    print(json.dumps(asdict(result), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
