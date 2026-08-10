from __future__ import annotations

import pytest

from macos_signing import DeveloperIdentity, require_unique_application_identity


def test_requires_exactly_one_developer_id_application_identity() -> None:
    output = (
        '  1) 0123456789abcdef0123456789abcdef01234567 '
        '"Developer ID Application: Axk Tools (ABCDE12345)"\n'
        '  2) fedcba9876543210fedcba9876543210fedcba98 '
        '"Developer ID Installer: Axk Tools (ABCDE12345)"\n'
    )

    assert require_unique_application_identity(output) == DeveloperIdentity(
        fingerprint="0123456789ABCDEF0123456789ABCDEF01234567",
        team_id="ABCDE12345",
    )


@pytest.mark.parametrize(
    "output",
    [
        "0 valid identities found\n",
        (
            '  1) 0123456789abcdef0123456789abcdef01234567 '
            '"Developer ID Application: Axk One (ABCDE12345)"\n'
            '  2) fedcba9876543210fedcba9876543210fedcba98 '
            '"Developer ID Application: Axk Two (ABCDE12345)"\n'
        ),
    ],
)
def test_rejects_missing_or_ambiguous_application_identity(output: str) -> None:
    with pytest.raises(ValueError, match="exactly one Developer ID Application"):
        require_unique_application_identity(output)
