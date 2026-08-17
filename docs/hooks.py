from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any


STANDALONE_OPENAPI_PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <title>axklib OpenAPI Reference</title>
  <style>
    html, body, #redoc-container {
      margin: 0;
      min-height: 100%;
      width: 100%;
    }
    body {
      background: #ffffff;
    }
  </style>
</head>
<body>
  <div id="redoc-container"></div>
  <script>
    window.process = {
      env: { NODE_ENV: "production" },
      cwd: () => "/"
    };
  </script>
  <script src="../assets/openapi/redoc.standalone.js"></script>
  <script>
    const openApiSpecification = __OPENAPI_SPECIFICATION__;
    Redoc.init(
      openApiSpecification,
      {
        hideHostname: true,
        nativeScrollbars: true,
        pathInMiddlePanel: true,
        theme: {
          colors: {
            primary: { main: "#4051b5" }
          },
          typography: {
            fontSize: "15px",
            lineHeight: "1.5em"
          }
        }
      },
      document.getElementById("redoc-container")
    );
  </script>
</body>
</html>
"""


def render_standalone_openapi_page(contract: dict[str, Any]) -> str:
    serialized_contract = json.dumps(
        contract,
        ensure_ascii=True,
        separators=(",", ":"),
    ).replace("<", "\\u003c")
    return STANDALONE_OPENAPI_PAGE.replace(
        "__OPENAPI_SPECIFICATION__",
        serialized_contract,
    )


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

    reference_directory = site_directory / "openapi"
    reference_directory.mkdir(parents=True, exist_ok=True)
    (reference_directory / "index.html").write_text(
        render_standalone_openapi_page(contract),
        encoding="utf-8",
        newline="\n",
    )


def on_post_build(config: Any, **_kwargs: Any) -> None:
    project_root = Path(__file__).resolve().parents[1]
    publish_openapi_assets(Path(config["site_dir"]), project_root)
