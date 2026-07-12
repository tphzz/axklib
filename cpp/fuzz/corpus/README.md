# Fuzzer Seeds

These small inputs exercise initial parser branches during fixed-budget smoke
runs. A newly minimized crash input belongs in the matching target directory
with a regression test that states the expected rejection behavior.

The directories below are maintained seed sources, not writable fuzz working
directories. Copy the relevant `.seed` files to an ignored directory below
`build/fuzz-corpus/` and pass that copied directory as libFuzzer's first corpus
argument. Content-addressed discoveries written into this source tree are
ignored and must not be committed. Promote only a minimized, descriptively
named `.seed` file together with its regression test.

The canonical target, parser, envelope, bound, and invariant inventory is
`../harnesses.json`. Run the fixed local smoke tier with:

```bash
cmake --preset fuzz-local
cmake --build --preset fuzz-local
```

The target copies seeds to `build/native/fuzz-local/fuzz-corpus/`; libFuzzer
never writes discoveries into this source directory.
