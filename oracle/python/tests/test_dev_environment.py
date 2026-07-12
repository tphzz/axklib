from __future__ import annotations

import tomllib
from pathlib import Path


def test_pyproject_declares_static_tools_and_package_discovery() -> None:
    config = tomllib.loads(Path("oracle/python/pyproject.toml").read_text(encoding="utf-8"))

    assert config["project"]["name"] == "axklib-python-oracle"
    assert "scripts" not in config["project"]
    assert config["project"]["requires-python"] == ">=3.13"
    assert config["tool"]["ruff"]["target-version"] == "py313"
    assert config["tool"]["mypy"]["python_version"] == "3.13"
    dev_deps = config["dependency-groups"]["dev"]
    assert any(dep.startswith("ruff") for dep in dev_deps)
    assert any(dep.startswith("mypy") for dep in dev_deps)
    assert config["build-system"]["build-backend"] == "setuptools.build_meta"
    assert config["tool"]["setuptools"]["packages"]["find"]["include"] == ["axklib*"]


def test_pytest_config_uses_repo_package_path_without_tool_bootstrap() -> None:
    config = tomllib.loads(Path("oracle/python/pyproject.toml").read_text(encoding="utf-8"))
    pytest_config = config["tool"]["pytest"]["ini_options"]

    assert pytest_config["testpaths"] == ["tests"]
    assert pytest_config["pythonpath"] == [".", "../.."]
    conftest = Path("oracle/python/tests/conftest.py").read_text(encoding="utf-8")
    assert "sys.path" + ".insert" not in conftest
    assert "sys.path" + ".append" not in conftest
