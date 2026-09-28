# Development

## Layout

```
include/ghoti.io/font/      Public headers, one per module (design.md section 4.1)
src/core/                   Result strings, limits, the allocator, fixed point, diagnostics
src/blob/                   GFNT_Blob: bytes, length, ownership
src/reader/                 The checked reader - every read of font bytes goes through it
src/sfnt/                   The offset table, the directory, collections, GFNT_Face
src/tables/                 head, hhea, OS/2, post and its glyph names, the metrics accessors
src/tables/post_names.h     Generated: the 258 standard Macintosh glyph names
src/cmap/                   Codepoint to glyph
src/name/                   Name records, decoded to UTF-8
src/name/mac_encodings.h    Generated: the single-byte Macintosh encodings and their selector
src/glyph/                  The glyph kinds and the strike policy
src/glyf/                   glyf and loca: a glyph's points, and composites (tier 1)
src/outline/                GFNT_Outline: storage, transforms, bounds, the path walk (tier 1)
src/raster/                 The scan converter and GFNT_Coverage (tier 1)
src/core/fixed.h            The rounding rules, shared: design.md section 5.2 states one
src/font.c                  The version
tests/unit/                 Unit tests (gtest)
tests/sfnt_builder.h        Builds sfnt bytes for the tests; see its header for what it is not
tests/data/fonts/           Synthetic fixtures built by tools/fixtures/, this library's own; no third-party font is committed
tests/data/vectors/         The generated vectors as text, read by testVectors
tests/data/golden/          Coverage hashes, for testGolden and check-golden
tests/fuzz/                 libFuzzer harnesses, and seeds under corpus/<name>/*.seed
tools/fuzz-seeds.py         Writes those seeds; they are generated, not hand-written
tools/fixtures/             make_fixtures.py and check_fixtures.py, over fontTools in its container
tools/vectors/              make_vectors.py and check_vectors.py, likewise
tools/golden/               make_golden.py, check_golden.py, and the cross build's cutil shim
tools/oracle/               The differentials, and the pinned fontTools image they run in
tools/check-reader.py       The reader gate
tools/check-stamps.py       The flag-stamp gate
```

## Adding a module

design.md section 4.1 names the modules. A module arrives with:

1. Its public header under `include/ghoti.io/font/`, listed in the Makefile
   so that `check-layering` places it. Every
   header includes `macros.h` first.
2. Every read of file data through `GFNT_Reader` (design.md section 6);
   `check-reader` greps for anything else under `src/` and fails.
3. Every public name in `namespace.h`; `check-symbols` fails otherwise.
4. A `_dump` for every parsed table, so that `ttx_diff` can compare it.
5. Its fixtures, built by `tools/fixtures/make_fixtures.py` in the `fonttools`
   image from outlines drawn here, and committed; real fonts are reached only
   through the oracle image (design.md section 14.5).
6. **A fuzz harness**, registered with
   `$(eval $(call fuzz-rule,fuzz_<name>,<name>))`, with its options byte
   driving `GFNT_Limits` - and the truncation sweep over each of its tables.

Point 6 is not optional. A parser over untrusted input without a fuzzer is a
parser nobody has actually tested.

## The gates

`make test` runs `TEST_GATES` - `check-symbols`, `check-layering`,
`check-aliasing`, `check-stamps`, `check-reader` - before the tests. Each has
been observed to fail on a planted defect before it was trusted, and a gate that
has never failed is one whose sensitivity is unmeasured. Add a gate the same way:
plant the defect, watch it fail, then commit it green.

`check-reader` goes further and carries its control inside itself: every pattern
is run against a planted violation in the same invocation, so a pattern that has
rotted into matching nothing fails the build rather than reporting a clean tree.
A gate that can only be trusted because someone once watched it fail is a gate
whose sensitivity is a year out of date.

## Fuzzing

The library is rebuilt with `-fsanitize=fuzzer-no-link` rather than linking the
ordinary shared library, so libFuzzer sees the parser's branches. The first
byte of each input selects the limits, so the capped paths are reachable
rather than only the wide-open defaults.

Six harnesses exist, and each takes its input through the door that harness's
bugs come in.

- `fuzz_sfnt` takes a whole font and walks everything a face can answer.
- `fuzz_cmap` takes a `cmap` table and wraps a valid font around it with
  `tests/sfnt_builder.h`, so the budget goes on the subtable formats rather than
  on rediscovering the directory.
- `fuzz_glyf` takes a `loca` **and** a `glyf`, split at a point the fuzzer
  chooses. The interesting inputs are in the relationship between the two - an
  entry running backwards, an entry past the end, a description cut inside its
  flag stream - and a harness given one of them would have to rediscover the
  other.
- `fuzz_raster` takes no font at all: the rasteriser has no bytes of its own, so
  the input is read as a list of points and tags. Reaching it through a `glyf`
  would mean every shape had to be a font first, and the shapes that break a
  scan converter are degenerate rather than malformed.
- `fuzz_cff` takes the whole `CFF ` table. A CFF is a nest of offsets that point
  at each other - the Top DICT at the charset and the `CharStrings` INDEX, the
  Private DICT at its local subroutines *relative to itself*, every INDEX's
  offsets one-based from the byte before its data - and a fuzzer given one
  structure cannot write the offset that reaches another.
- `fuzz_charstring` takes no font at all, for `fuzz_raster`'s reason: what breaks
  an interpreter is a *program*. The input is split into a charstring and
  subroutines at positions the fuzzer picks, and **both languages run over every
  input**, because `255` introduces a 16.16 in one and an integer in the other -
  the same bytes are two programs.

Three pieces of undefined behaviour came out of the last two on their first runs,
none of them reachable from a font: a left shift of a negative value in nine
places, and signed overflow in `div`, in `sqrt` and in a DICT real's decimal
scale. A fourth finding was in the build rather than the library - the fuzz object
tree had no depfiles, so a header change left one binary linked from two layouts
of one struct, and the crash report named a source line in working code.

Seeds are generated by `tools/fuzz-seeds.py` into
`tests/fuzz/corpus/<name>/*.seed`; libFuzzer's own findings land beside them and
are gitignored. **Whether a seed corpus reaches the parser is measured, not
assumed**: the seeds take `fuzz_glyf` from 519 edges to 1047 and `fuzz_raster`
from 461 to 1043, against a single junk input as the baseline.

```bash
make fuzz FUZZ_TIME=3600
```

A minute each is a smoke test. A real campaign belongs in its own prefix with
an `OWNER` file: a soak that outlives a shared
rebuild must not be invalidated by one.

## The oracles

`make test` needs no container, and that is deliberate: a contributor without a
container engine must still be able to run the suite and see that they did not
run the differentials. The oracle targets are therefore separate.

```bash
make oracle-build          # build the fontTools image from its pinned recipe
make oracle-version        # prove the reference is reachable and matches its pin
make check-oracle          # both differentials, quiet
make check-oracle-cmap-exhaustive   # all 1,114,112 codepoints per font, ~20 min
```

Three things to know before changing anything under `tools/oracle/`:

1. **The image is built here, so the run-time version check is the only
   guarantee.** `oracle_env.check_pin()` therefore runs in both modes, unlike the
   `unicode` copy this came from. See its docstring.
2. **The corpus lives in the image.** `corpus.py` copies it into
   `build/oracle/corpus` in one tar so both sides read the same bytes; no
   third-party font is ever committed here (design.md section 14.6).
   `make oracle-corpus-clean` removes the copy - it is cache, not content.
3. **A differential reports its denominators and its coverage**, and fails when
   they collapse: nothing compared, a corpus that mostly skipped, or too few
   fields for the number of fonts are each an error rather than a clean report.
   `ttx_diff` also prints which table versions it met, because a four-font sample
   of this corpus once passed a planted defect that only affects `OS/2` version 1.

`GHOTI_ORACLE_REQUIRED=1` turns an unreachable reference from a loud SKIPPED into
a failure, which is what a CI with the images wants.

## The fixtures

`tests/data/fonts/` holds thirty-one synthetic fonts and a `MANIFEST` saying what
each one exercises. They are **generated, not written**: `make fixtures` runs
`tools/fixtures/make_fixtures.py` in the pinned `fonttools` image and installs
what it produces, and `make check-fixtures` regenerates and fails on a byte
difference.

```bash
make fixtures-list         # what each fixture is for
make check-fixtures        # the committed bytes equal a fresh generation
make fixtures              # regenerate and install (a deliberate act)
```

Five things to know before touching them:

1. **Editing a fixture by hand is not a thing you can do.** The gate compares
   bytes; a hand edit fails it. Change `make_fixtures.py`, run `make fixtures`,
   and let the commit say why the bytes moved.
2. **Determinism is the contract, and it is easy to break.** Anything that
   varies with the clock, the locale or the interpreter has to be pinned in the
   generator - see its docstring, which names the two that bit: `FontBuilder`
   writes the current time into `head` regardless of the defaults table, and a
   `TTFont` re-opened from a file has `recalcTimestamp` *on*, so the one fixture
   that is read back before it is written was different on every run while every
   other one was stable.
3. **`head.created` must be at or above 0x7C259DC0.** fontTools treats anything
   below that as a misencoded unix timestamp and silently adds the constant, so
   a fixture dated 1904 reads back as 1970 and `ttx_diff` scores `created` as a
   disagreement on every fixture - the reference being helpful, the library
   being right, and the gate red for neither's reason.
4. **They are in `ttx_diff`'s population by default**, and `--fonts N` thins
   the real corpus without thinning them. That is the point: the corpus holds
   OS/2 versions 1, 3 and 4 only, so versions 0, 2 and 5 exist nowhere else, and
   a thinned run that dropped the fixtures would be a smoke test that stopped
   covering the arm a defect is in - which has happened here once already.
5. **Seven of them are not what fontTools would write.** The `cff-*.otf` fixtures
   carry CFF tables assembled byte by byte by the generator and wrapped in an
   sfnt fontTools builds, because a `T2CharStringPen` writes moves, lines and
   curves and nothing else - no flex, no `hintmask`, no accented character, no
   arithmetic, no subroutine, and no CID-keyed font at all. A survey of the
   image's 35 CFF fonts found the same gaps from the other end. Everything a pen
   can write still comes from the reference; the hand-built half is named in the
   MANIFEST, as `outline-broken-loca.ttf`'s patched entry already was.

`make test` does not need a container to cover them: `testFixtures` loads every
committed fixture and asserts what it contains, so a corrupted or missing
fixture fails the suite anywhere. What needs the image is only the claim that
the bytes can be *regenerated*, which is what `check-fixtures` is.

That suite is also the one place the unit tests are not self-referential. Every
other expectation under `tests/` is one this library wrote for itself -
`tests/sfnt_builder.h` says so in its own header - and these are bytes fontTools
wrote.

## The generated vectors

Three tables are generated from the pinned image and committed: the 258-entry
standard Macintosh glyph order that `post` format 1.0 *is* and format 2.0 indexes
into, the eight single-byte Macintosh `name` encodings, and the CFF tables - the
391 standard strings a charset names its glyphs by, the Standard Encoding a `seac`
names its two glyphs by, and the two predefined charsets. None is written by hand
- design.md section 14's rule is that a vector comes from an oracle.

```bash
make gen-vectors           # regenerate and install (read the diff)
make check-vectors         # the committed tables are what the generator emits
```

Three gates cover them, and knowing which finds what is the point:

| gate | finds | needs |
| --- | --- | --- |
| `check-vectors` | a hand-edited table, or a generator changed without regenerating | the image |
| `testVectors` | the same, on a fresh clone | nothing |
| `check-oracle-ttx` | a table **wrong about reality**, over 346 faces | the image |
| `check-oracle-cff` | the CFF tables wrong about reality: every glyph's name through the standard strings, every accented character through the Standard Encoding | the image |

The CFF tables are the one place `testVectors` reads a generated header directly
rather than going through the public API, and the reason is worth stating: there
is no call that walks 391 strings or 256 encoding codes, and a fixture naming
every one of them would be a font built for one test. Their *use* is covered from
the public API in `test_cff.cpp` instead.

## The golden renderings

`tests/data/golden/coverage.txt` holds 3,825 renderings: every glyph of every
fixture that carries outlines of its own, at six pixel sizes and five origins,
with the coverage's shape, its total and its hash. `make golden` rewrites it and
needs no container.

Phase 2 put the charstrings in it, which is where the rasteriser meets a **cubic**
producer: `glyf` sends quadratics, a charstring sends cubics, and the flattener's
other arm is now under the gate that reproduces every rendering on three
big-endian targets. Two things fell out of adding them. `cff.otf` left the
generator's "renders nothing" list - its charstrings are the same five outlines
`basic.ttf` holds as `glyf`, so the subset check now asserts that two containers
and one rasteriser produce identical pixels. And the gate's own sub-pixel property
caught eleven fixtures whose glyphs were **slivers**: a chain of curves that all
run up-and-right encloses almost no area with its closing line, and a sliver's
extreme row carries so little coverage that a quarter-pixel *horizontal* shift
decides whether it rounds to nothing. The property is about the rasteriser and is
right; a fixture too thin to state it weakens the gate.

The same three-gate split as the vectors, for the same reason:

| gate | finds | needs |
| --- | --- | --- |
| `testGolden` | a change to the rasteriser nobody meant | nothing |
| `check-golden` | a rendering that differs on a **big-endian** target | the workspace's `ghoti-xarch` container |
| `check-oracle-glyf` | an **outline** wrong about reality, over 37,218 glyphs | the `fonttools` image |
| `check-oracle-cff` | a **charstring** wrong about reality - its program, its path and the advance it states - over 4,172 glyphs | the `fonttools` image |

`check-golden` is the only thing here that can see a violation of the determinism
promise, and it was verified to: a `memcpy` of a `uint32_t`'s bytes planted in
`gfnt_coverage_hash()` is identical to the explicit shifts on this machine, so
the whole unit suite stayed green while that gate failed on s390x.

When a glyph comes out wrong, `font-render --art <glyph> <ppem>` draws it:

```bash
./build/linux/release/apps/examples/font-render x tests/data/fonts/outline-simple.ttf --art 2 24
```

`testVectors` is why the two container gates can fail rather than skip: something
still covers the tables without an engine. It reads every entry through the
public API - `post-v1.ttf` is the standard order, and
`name-mac-encodings.ttf` carries all 128 high bytes of each encoding - so nothing
here includes a generated header from a test.

Two things to know before changing `tools/vectors/`:

1. **The Macintosh encoding ID does not choose the table.** `platEncID` 0 is keyed
   by `langID`, and Icelandic, Turkish, Croatian, Central European and Romanian
   all live under it. Every Macintosh record in the 327-font corpus is
   `(0, Roman)`, so this is a rule only a fixture can check.
2. **Mac Roman is cross-checked against glibc's `MACINTOSH` charmap**, and the
   generator fails unless the disagreements are exactly the two it documents - or
   if one of them stops disagreeing. Generating from one source would be
   circular, since fontTools decodes `name` records with the same CPython codec.

## Line coverage

```bash
make coverage
```

Builds an instrumented tree of its own - never the release objects, for the
reason the target's comment gives - runs the suite, and prints per-file line
coverage, the lines no test executed, and every growth or reallocation line that
was never reached. **96.1% of 4,951 lines under `src/`**, from `make test` alone
with no container.

The report fails below a floor of 96%, and fails if it measured fewer than 4,500
executable lines - a control that has to be raised as the library grows, or it
stops being one. The second is the control: every other figure here is a ratio,
and a ratio is perfectly happy about a report that collapsed - a sweep that
measured one file would print 100% and clear any floor. Both were armed and
watched to fail before being committed green, and both can be moved for a one-off
run (`GFNT_COVERAGE_FLOOR`, `GFNT_COVERAGE_LINES`).

Three things to know before reading a number here.

1. **The report is read for the lines nothing reaches, not for the percentage.**
   The annotated source is left in `<object-dir>/coverage.txt`; `grep '#####'` is
   the triage. One finding came out of exactly that: no fixture carried a `REPEAT`
   flag, so ten lines of `glyf.c` were reached only through the oracle's real
   fonts (design.md section 18).
2. **`-fprofile-update=atomic` is deliberate.** One suite uses threads, and GCC's
   default counter update is unsynchronised: it reported an unreachable
   argument-validation line as executed - 12 times, then not at all, then 16 -
   and reported a line the threaded test really does reach as unexecuted. The
   aggregate moved by a tenth of a point, which is why nothing would have noticed
   it. Do not build the instrumented tree by hand without the flag.
3. **`gcov -t` is deliberate too.** gcov names its output after the source, so a
   header compiled into many objects had its report overwritten by whichever
   object was processed last, and `src/core/fixed.h`'s saturation lines flipped
   between covered and uncovered depending on nothing but `find`'s order. The
   annotated source now goes to one stream and the lines are merged.

### What the last 3.9% is

The 198 lines that remain are not a backlog. Each is one of the kinds below, and
none can be reached through the public API - so a test for one would have to call
a static function or fake a state the library does not produce, which is a test of
the test rather than of the library.

**More than half of them are one line of C.** 113 are a bare
`return result;` carrying another call's failure outward, and 42 more are the
first line of a refusal nothing reaches. Both grew sharply with phase 2, and the
reason is structural rather than a gap in the tests: a charstring interpreter and
a nest of CFF offsets are mostly *checks*, and a check whose predicate a
bounds-checked reader has already enforced cannot fire. The truncation sweep in
`test_cff.cpp` cuts that table to every length and asks five accessors of each
cut, which is what reaches as many of them as are reachable at all.

| kind | lines | example |
| --- | ---: | --- |
| `gcu_safe_*` overflow guards | 11 | `outline.c` reserving past `SIZE_MAX`, with `max_outline_points` two orders of magnitude below it |
| null-argument guards on internal functions | 8 | `gfnt_loca_range()`, whose three callers are in the same file |
| per-read arms behind an up-front check | ~20 | every `OS/2` field read, after `reader.length < gfnt_os2_lengths[version]` has already refused a short table |
| two passes disagreeing | 15 | `gfnt_face_glyph_name()` sizing a name and then writing it, from the same bytes |
| invariant reporters in the rasteriser | 7 | a cell outside the bitmap, which the bounds make impossible - kept because a bounds bug must be a reported failure and not a missing span |

Phase 2 added two kinds of its own:

| kind | lines | example |
| --- | ---: | --- |
| a refusal behind a bounds-checked read | ~90 | every `return result;` in `cff.c` after a reader call the table's own extent has already made safe |
| an operand-count refusal a writer cannot emit | ~25 | `rcurveline` with two operands, `sbw` with three - each has a test, and the ones left are the arms *inside* an operator whose earlier check already refused |

Two more are a measurement rather than an argument: `gfnt_bound_cubic()`'s
out-of-budget arm needs a cubic that is still not monotone after 16 subdivisions,
and the worst case found over 400,000 random cubics at five coordinate scales,
plus seven adversarial constructions, needed 13. Two are racy by nature - the
"another thread parsed this table first" arm in `tables.c` - and one is a
`switch`'s unreachable default.

**The distinction that matters is between an arm nothing can reach and an arm
nothing has tried to reach.** The list above is the first; everything in it has a
reason written next to it in the source. When a line joins it, say which kind it
is, and if it is none of them, it is the second and it wants a test.

## Memory

Every allocation goes through the `GFNT_Allocator` the caller supplied, which
is cutil's `GCU_Allocator`. `tests/unit/test_allocator.cpp` proves the default
is the suite's; as modules arrive, the counting allocator there proves each
returns everything, including on the error paths.
