# Native Workflow Benchmark

`axk_workflow_benchmark` records release-build timings for opening, inventory,
relationships, preview generation, exact export, fresh writing, and alteration.
The report also includes processed bytes, throughput, object count, and peak
resident memory.

The checked-in Linux x64 baseline uses the small sampler-authored test image.
`tools/python/check_benchmark.py` warns above 15 percent and fails above 30 percent.
It also enforces the profile's absolute peak-resident-memory budget. A separate
CTest inventory pass covers the sparse 2 GiB, eight-partition boundary under a
256 MiB process budget.
Platform release jobs may maintain separate baselines because compiler, storage,
and operating-system timing differ.
