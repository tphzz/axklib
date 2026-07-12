# Python compatibility oracle

This package is the frozen Python implementation used to compare native
behavior during development. It is not a supported library, command-line tool,
or release artifact. Production packages are built only from the repository's
CMake project.

From the repository root, run the pinned suite with:

```bash
./oracle/run-python-oracle test
```
