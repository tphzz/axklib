# Native Conformance Contracts

These versioned JSON files are language-neutral inputs for native regression
tests. They record public fixture identities, supported command/profile
inventories, and expected semantic hashes. They deliberately do not identify or
depend on the implementation used to establish an expectation.

Fixture paths are relative to the repository root. Every contract must be
consumed by a maintained native test. Review the decoded semantic change before
updating an expected hash.
