from __future__ import annotations

import os
import sys
import types

from axklib.docs_cli import main


class FakeMkdocsCli:
    def __init__(self) -> None:
        self.calls: list[dict[str, object]] = []

    def main(self, *, args: list[str], prog_name: str, standalone_mode: bool) -> None:
        self.calls.append(
            {"args": args, "prog_name": prog_name, "standalone_mode": standalone_mode}
        )


def test_docs_cli_sets_material_warning_env_and_delegates(monkeypatch) -> None:
    fake_cli = FakeMkdocsCli()
    fake_mkdocs = types.ModuleType("mkdocs")
    fake_mkdocs.__path__ = []
    fake_main = types.ModuleType("mkdocs.__main__")
    fake_main.cli = fake_cli

    monkeypatch.delenv("NO_MKDOCS_2_WARNING", raising=False)
    monkeypatch.setitem(sys.modules, "mkdocs", fake_mkdocs)
    monkeypatch.setitem(sys.modules, "mkdocs.__main__", fake_main)

    assert main(["build", "--strict"]) == 0
    assert fake_cli.calls == [
        {"args": ["build", "--strict"], "prog_name": "mkdocs", "standalone_mode": True}
    ]
    assert os.environ["NO_MKDOCS_2_WARNING"] == "true"

