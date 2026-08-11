from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any


def publish_openapi_assets(site_directory: Path, project_root: Path) -> None:
    source = project_root / "apps/server/contracts/openapi-v1.json"
    renderer = project_root / "node_modules/redoc/bundles/redoc.standalone.js"
    if not source.is_file():
        raise FileNotFoundError(f"complete OpenAPI contract is missing: {source}")
    if not renderer.is_file():
        raise FileNotFoundError(f"pinned Redoc renderer is missing; run npm ci: {renderer}")

    contract = json.loads(source.read_text(encoding="utf-8"))
    if not str(contract.get("openapi", "")).startswith("3."):
        raise ValueError(f"OpenAPI contract does not declare a supported version: {source}")

    destination = site_directory / "assets/openapi"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination / source.name)
    shutil.copyfile(renderer, destination / renderer.name)


def on_post_build(config: Any, **_kwargs: Any) -> None:
    project_root = Path(__file__).resolve().parents[1]
    publish_openapi_assets(Path(config["site_dir"]), project_root)
