from __future__ import annotations

import os
import sys
from collections.abc import Sequence


def main(argv: Sequence[str] | None = None) -> int:
    """Run MkDocs with axklib's documentation defaults."""
    os.environ.setdefault("NO_MKDOCS_2_WARNING", "true")

    try:
        from mkdocs.__main__ import cli as mkdocs_cli
    except ModuleNotFoundError as exc:
        if exc.name == "mkdocs":
            print(
                "mkdocs is not installed. Run this command with the docs dependency group, "
                "for example: uv run --group docs axklib-docs build --strict",
                file=sys.stderr,
            )
            return 2
        raise

    args = list(sys.argv[1:] if argv is None else argv)
    try:
        mkdocs_cli.main(args=args, prog_name="mkdocs", standalone_mode=True)
    except SystemExit as exc:
        return int(exc.code or 0)
    return 0
