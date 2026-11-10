"""Typed GitHub release API; never manages Git refs or publication."""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import release_metadata


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
    name: str | None
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
        name = value.get("name")
        if name is not None and not isinstance(name, str):
            raise ReleaseError("GitHub returned an invalid release name")
        if body is not None and not isinstance(body, str):
            raise ReleaseError("GitHub returned an invalid release body")
        url = string_value(value, "html_url")
        if not url.startswith(f"https://github.com/{repository}/releases/"):
            raise ReleaseError("GitHub returned an unexpected release URL")
        return cls(
            id_value(value),
            string_value(value, "tag_name"),
            name,
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
