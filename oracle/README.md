# Python conformance oracle

This directory contains the frozen Python implementation, its pinned project,
and language-neutral expectations used to compare native behavior. Production
targets and release packages do not import or install this package.

The source, tests, `pyproject.toml`, and `uv.lock` are under `oracle/python`.
From the repository root, create the pinned environment and run every oracle
gate with:

```bash
./oracle/run-python-oracle sync
./oracle/run-python-oracle test
```

`baseline.json` is an immutable snapshot. Regeneration requires explicit oracle
commit and test-count arguments so ordinary test runs cannot bless changed
behavior. Binary fixtures remain under `tests/fixtures`; the baseline stores
their relative paths and SHA-256 hashes.

Generate the snapshot into a temporary path with:

```bash
./oracle/run-python-oracle baseline \
  --oracle-commit 62d6ce2dfe7dd1c85ece5cdf459a25a0a8deb5d3 \
  --test-count 430 \
  --output /tmp/axklib-oracle-baseline.json
```
