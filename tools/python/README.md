# Python Development Tools

This project contains repository and release utilities that are independent of
the axklib implementation. It is not an axklib language binding and is not
required to configure, build, install, or use the native SDK or CLI.

Run its quality gates from the repository root:

```bash
uv --project tools/python run ruff check tools/python
uv --project tools/python run mypy tools/python
uv --project tools/python run pytest tools/python/tests
```

The header generators consume versioned JSON tables under `library/data` and
write deterministic C++ headers. Package inspection, SBOM generation, boundary
checking, and benchmark comparison are used by release workflows.
