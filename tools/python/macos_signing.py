#!/usr/bin/env python3
"""Resolve the single Developer ID Application identity used by release CI."""

from __future__ import annotations

import sys
from dataclasses import dataclass
from re import compile as compile_pattern


@dataclass(frozen=True)
class DeveloperIdentity:
    fingerprint: str
    team_id: str


_APPLICATION_IDENTITY = compile_pattern(
    r"(?<![0-9A-Fa-f])"
    r"(?P<fingerprint>[0-9A-Fa-f]{40})\s+"
    r'"Developer ID Application: .+ \((?P<team_id>[A-Z0-9]{10})\)"'
)


def parse_application_identities(output: str) -> list[DeveloperIdentity]:
    identities: list[DeveloperIdentity] = []
    seen: set[DeveloperIdentity] = set()
    for line in output.splitlines():
        match = _APPLICATION_IDENTITY.search(line)
        if match is None:
            continue
        identity = DeveloperIdentity(
            fingerprint=match.group("fingerprint").upper(),
            team_id=match.group("team_id"),
        )
        if identity not in seen:
            identities.append(identity)
            seen.add(identity)
    return identities


def require_unique_application_identity(output: str) -> DeveloperIdentity:
    identities = parse_application_identities(output)
    if len(identities) != 1:
        raise ValueError(
            "expected exactly one Developer ID Application identity; "
            f"found {len(identities)}"
        )
    return identities[0]


def main() -> int:
    try:
        identity = require_unique_application_identity(sys.stdin.read())
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(f"{identity.fingerprint}|{identity.team_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
