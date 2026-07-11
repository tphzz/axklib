from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", ".venv", "__pycache__", "build", ".mypy_cache", ".pytest_cache", ".ruff_cache"}
SKIP_SUFFIXES = {".hds", ".bin", ".png", ".jpg", ".jpeg", ".gif", ".pyc", ".lock"}


def _term(*parts: str) -> str:
    return "".join(parts)


BLOCKED = {
    _term("g", "hidra"),
    _term("rev", "erse", "-", "engineer"),
    _term("rev", "erse", " ", "engineer"),
    _term("de", "compil"),
    _term("dis", "assembl"),
    _term("firm", "ware"),
    _term("pro", "be"),
    _term("ctrl", "r"),
    _term("panel", "-", "derived"),
    _term("sampler", "-", "confirmed"),
    _term("evi", "dence"),
    _term("proven", "ance"),
    _term("pro", "of"),
    _term("confi", "dence"),
    _term("hypo", "thesis"),
    _term("source", "-", "materials"),
    _term("docs", "/", "archive"),
    _term("docs", "\\", "archive"),
    _term("res", "earch", "/", "lab"),
    _term("res", "earch", "\\", "lab"),
    _term("dis", "ky"),
    _term("010", " editor"),
}

PATH_MARKERS = {
    _term("drop", "box"),
    _term("one", "drive"),
    _term("hardware", " sampler"),
    _term("sampler", "-", "fs"),
    _term("c", ":", "\\", "develop"),
    _term("/", "users", "/"),
    _term("/", "home", "/"),
    _term("/", "mnt", "/"),
    _term("/", "volumes", "/"),
}

PATH_PATTERNS = {
    "windows-drive-path": re.compile(_term(r"\b[A-Za-z]", ":", r"[\\/]", r"[^\s`'\"<>]+")),
    "unc-path": re.compile(_term(r"\\\\", r"[A-Za-z0-9._$-]+", r"[\\/]", r"[^\s`'\"<>]+")),
}

LEGACY_PATTERNS = {
    "old-project-package-name": re.compile(_term(r"\b", "ax", "k", "-new", r"\b"), re.IGNORECASE),
    "old-docs-title": re.compile(_term(r"\b", "ax", "k", r"\s+documentation\b"), re.IGNORECASE),
    "old-library-title": re.compile(_term(r"\b", "ax", "k", r"\s+library\b"), re.IGNORECASE),
    "old-project-intro": re.compile(_term(r"\b", "ax", "k", r"\s+is\b"), re.IGNORECASE),
    "old-cli-command": re.compile(_term(r"\buv\s+run\s+", "ax", "k", r"\b"), re.IGNORECASE),
    "old-python-import": re.compile(_term(r"\b(import|from)\s+", "ax", "k", r"\b"), re.IGNORECASE),
    "old-mkdocstrings-target": re.compile(_term(r":::\s+", "ax", "k", r"\b"), re.IGNORECASE),
    "old-graph-filename": re.compile(_term(r"\bvolume\.", "ax", "k", r"\.json\b"), re.IGNORECASE),
}

DOC_INSTRUCTION_PATTERNS = {
    "instruction-for-automation": re.compile(
        _term(r"\b", "ag", "ent", r" instructions?\b"), re.IGNORECASE
    ),
    "note-to-self": re.compile(r"\bnote\s+to\s+self\b", re.IGNORECASE),
    "task-marker": re.compile(_term(r"\b", "TO", "DO", r"\b")),
    "do-not-expand": re.compile(r"\bdo\s+not\s+expand\b", re.IGNORECASE),
    "must-not-grow": re.compile(r"\bmust\s+not\s+grow\b", re.IGNORECASE),
    "should-stay": re.compile(r"\bshould\s+stay\b", re.IGNORECASE),
    "keep-it-below": re.compile(r"\bkeep\s+it\s+below\b", re.IGNORECASE),
    "useful-for-analysis-work": re.compile(
        _term(r"\buseful\s+for\s+", "res", "earch", r"\b"), re.IGNORECASE
    ),
    "write-side-policy": re.compile(r"\bwrite-side\s+policy\b", re.IGNORECASE),
}


def _public_text_files() -> list[Path]:
    files: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.suffix.lower() in SKIP_SUFFIXES:
            continue
        files.append(path)
    return files


def _public_doc_files() -> list[Path]:
    return [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md"))]


def test_public_tree_uses_neutral_boundary_language() -> None:
    hits: list[str] = []
    for path in _public_text_files():
        text = path.read_text(encoding="utf-8", errors="ignore").lower()
        for blocked in BLOCKED:
            if blocked in text:
                hits.append(f"{path.relative_to(ROOT)}: {blocked}")
    assert hits == []


def test_public_tree_does_not_leak_internal_paths() -> None:
    hits: list[str] = []
    for path in _public_text_files():
        raw_text = path.read_text(encoding="utf-8", errors="ignore")
        lower_text = raw_text.lower()
        for marker in PATH_MARKERS:
            if marker in lower_text:
                hits.append(f"{path.relative_to(ROOT)}: {marker}")
        for name, pattern in PATH_PATTERNS.items():
            if pattern.search(raw_text):
                hits.append(f"{path.relative_to(ROOT)}: {name}")
    assert hits == []


def test_public_tree_does_not_use_legacy_project_branding() -> None:
    hits: list[str] = []
    for path in _public_text_files():
        raw_text = path.read_text(encoding="utf-8", errors="ignore")
        for name, pattern in LEGACY_PATTERNS.items():
            if pattern.search(raw_text):
                hits.append(f"{path.relative_to(ROOT)}: {name}")
    assert hits == []


def test_public_docs_do_not_contain_agent_or_note_to_self_phrasing() -> None:
    hits: list[str] = []
    for path in _public_doc_files():
        raw_text = path.read_text(encoding="utf-8", errors="ignore")
        for name, pattern in DOC_INSTRUCTION_PATTERNS.items():
            if pattern.search(raw_text):
                hits.append(f"{path.relative_to(ROOT)}: {name}")
    assert hits == []
