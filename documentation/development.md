# Development

## Layout

```
include/ghoti.io/font/      Public headers, one per module, by tier (design.md section 4.1)
src/core/                   Result strings, limits, the allocator
src/font.c                  The version
tests/unit/                 Unit tests (gtest)
tests/data/fonts/           The subset corpus, one directory per family with its licence (from phase 0)
tests/data/golden/          Coverage hashes for the cross-platform gate (from phase 1)
tests/fuzz/                 libFuzzer harnesses and seed corpus (from phase 0)
tools/fixtures/             make_fixtures.py, over fontTools in its container (from phase 0)
tools/oracle/               The differentials (from phase 0)
tools/check-stamps.py       The flag-stamp gate
```

## Adding a module

design.md section 4.1 names the modules and the tier each sits in. A module
arrives with:

1. Its public header under `include/ghoti.io/font/`, listed in its tier's
   `TIER<n>_FILES` in the Makefile, so that `check-layering` places it. Every
   header includes `macros.h` first.
2. Every read of file data through `GFNT_Reader` (design.md section 6);
   `check-reader` greps for anything else under `src/` and fails.
3. Every public name in `namespace.h`; `check-symbols` fails otherwise.
4. A `_dump` for every parsed table, so that `ttx_diff` can compare it.
5. Its fixtures, subset and renamed by `tools/fixtures/make_fixtures.py`, with
   the family's licence beside them (design.md section 14.5).
6. **A fuzz harness**, registered with
   `$(eval $(call fuzz-rule,fuzz_<name>,<name>))`, with its options byte
   driving `GFNT_Limits` - and the truncation sweep over each of its tables.

Point 6 is not optional. A parser over untrusted input without a fuzzer is a
parser nobody has actually tested.

## The gates

`make test` runs `TEST_GATES` - `check-symbols`, `check-layering`,
`check-aliasing`, `check-stamps` - before the tests. Each has been observed
to fail on a planted defect before it was trusted, and a gate that has never
failed is one whose sensitivity is unmeasured. Add a gate the same way: plant
the defect, watch it fail, then commit it green.

## Fuzzing

The library is rebuilt with `-fsanitize=fuzzer-no-link` rather than linking the
ordinary shared library, so libFuzzer sees the parser's branches. The first
byte of each input selects the limits, so the capped paths are reachable
rather than only the wide-open defaults.

```bash
make fuzz FUZZ_TIME=3600
```

## Memory

Every allocation goes through the `GFNT_Allocator` the caller supplied, which
is cutil's `GCU_Allocator`. `tests/unit/test_allocator.cpp` proves the default
is the suite's; as modules arrive, the counting allocator there proves each
returns everything, including on the error paths.
