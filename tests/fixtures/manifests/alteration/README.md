# Alteration manifest fixtures

These JSON files are language-neutral contracts used by native C++ transaction
tests. They contain synthetic sampler-facing names and relative paths only.
Tests provide generated source images and audio where an operation needs
semantic execution.

`all-operations.json` exercises the strict schema for every maintained operation
and backward-only operation references. It is a parser contract, not a request
to apply all rows to one source image.

`audio-import.json` is shared by axkdeck's audio request tests and native
transaction tests. It covers mono one-shot and stereo looping Samples, generated
16-bit/24-bit WAV inputs, new/existing volumes and optional Sample Bank grouping.
Native tests reopen the output and check Sample settings and object relationships.
