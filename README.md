# Ghoti.io Font

Font files in - every family of them, not one - and positioned glyphs,
coverage bitmaps and laid-out paragraphs out, with the mapping from characters
to glyphs surviving the trip. sfnt (`glyf` and `CFF`), the bitmap formats a
stock Linux box actually has in bulk (PCF, BDF, PSF), Type 1, WOFF; a checked
reader and no hinting interpreter, because the file is the attacker;
fixed-point throughout, so the same font produces byte-identical pixels on
every platform; shaping and paragraph layout up to, and not past, the
paragraph.

It exists because `cjelly` has to draw text, `image` cannot, and a PDF library
would need to read four font formats and write two. The design, the tiers, the
threat model and the plan are in
[documentation/design.md](documentation/design.md).

```c
#include <ghoti.io/font/font.h>

GFNT_Blob * blob = NULL;
GFNT_Face * face = NULL;
GFNT_Error error;
uint32_t glyph = 0;
int32_t advance = 0;

if (gfnt_blob_create_file("Example.ttf", NULL, NULL, &blob, &error) == GFNT_OK
    && gfnt_face_load(blob, 0, NULL, NULL, &face, &error) == GFNT_OK
    && gfnt_face_glyph_for_codepoint(face, 'A', &glyph, &error) == GFNT_OK
    && gfnt_face_glyph_advance(face, glyph, NULL, &advance, &error) == GFNT_OK) {
  printf("'A' is glyph %u, %d font units wide\n", glyph, advance);
}
else {
  gfnt_error_dump(&error, stderr);
}
gfnt_face_free(face);
gfnt_blob_destroy(blob);
```

## Building

Requires [cutil](https://github.com/Ghoti-io/cutil), found through pkg-config.
That is the only way it is looked for: a dependency pkg-config cannot find is
a hard error naming the fix, rather than a fallback to a checkout next door
that only an in-tree build would ever exercise.

```bash
make            # shared and static libraries
make test       # unit tests and the gates
sudo make install
```

| Target | What it does |
| --- | --- |
| `make test` | Run the unit tests and every gate in `TEST_GATES` |
| `make test-quiet` | One line per suite |
| `make test-valgrind-quiet` | Same, under Valgrind |
| `make test-asan` | Rebuild with ASan+UBSan and run the suite |
| `make check-layering` | Fail if a lower tier includes a higher tier's header |
| `make check-reader` | Fail if anything reads font bytes around the checked reader |
| `make check-oracle` | Both differentials against fontTools, in its pinned container |
| `make oracle-build` | Build that container from its pinned Containerfile |
| `make coverage` | Line coverage, per file |
| `make fuzz` | Build and run every fuzzer (`FUZZ_TIME=3600` for a real campaign) |
| `make docs` | Doxygen, into `./docs` |

## The API

Everything is prefixed `gfnt_` / `GFNT_`, under `<ghoti.io/font/...>`.

- **`core.h`** - `GFNT_Result` and `gfnt_result_string()`; the three
  fixed-point types (26.6, 16.16, 2.14) and their arithmetic, which is where
  the byte-identical-everywhere claim comes from; `GFNT_Tag`; `GFNT_Error`,
  which carries the table, the offset within it and the glyph, because
  "corrupt" is not a diagnostic; `GFNT_Limits` with every cap design.md section
  15.2 names; and the version.
- **`allocator.h`** - `GFNT_Allocator`, which is cutil's `GCU_Allocator`, so an
  allocator written for any library in the suite works with all of them.
- **`blob.h`** - the bytes of a font, copied, borrowed, read from a file, or
  mapped. Reading is the default and mapping is a separate call whose
  documentation says `SIGBUS`.
- **`face.h`** - one font from a blob and an index into it: the sfnt version,
  the table directory, table ranges and checksums, `ttcf` collections, and
  `GFNT_Variation`, which every accessor a variation could change already takes.
- **`metrics.h`** - `head`, `hhea`, `OS/2` and `post`; `unitsPerEm`; the glyph
  count as the minimum across every table that indexes glyphs; per-glyph
  advances and side bearings from `hmtx`; and line metrics under a named policy
  whose zero is the font's own request.
- **`cmap.h`** - codepoint to glyph through formats 0, 4, 6 and 12, the
  documented subtable preference order, and which subtable answered.
- **`name.h`** - name records decoded to UTF-8 by platform and encoding, with
  the preference order and a language override.
- **`glyph.h`** - `GFNT_GlyphKind`, `GFNT_Strike` and `GFNT_StrikePolicy`: the
  shape of "which strike answers this size", in the API from the start because
  adding it later would break every caller.
- **`font.h`** - the umbrella for tier 0.

The modules design.md section 4.1 names that do **not** exist yet: `bitmap.h`,
`color.h`, `charstring.h`, `outline.h`, `raster.h`, `shape.h`, `layout.h`,
`discover.h`, `write.h`. Absent, not stubbed: there is no function here that
returns `GFNT_ERR_UNSUPPORTED` in place of a parser. Where a table this library
does not parse yet would change an answer - a font whose strikes are in an
`EBLC`, a `cmap` subtable in format 13, a Macintosh name above ASCII - the call
says `GFNT_ERR_UNSUPPORTED` and names what it could not read, rather than
reporting an empty result that cannot be told from the truth.

## Status

**Most of phase 0.** Tier 0 opens an sfnt or a `ttcf` collection, walks the
table directory, and answers the four questions phase 0 exists for: what is
this file, what does it contain, which glyph is this codepoint, and how wide is
it. `head`, `maxp`, `hhea`, `hmtx`, `OS/2`, `post`, `cmap` (formats 0, 4, 6 and
12) and `name` are parsed, each on first use and memoised; every read goes
through the checked reader, and `check-reader` fails the build on one that does
not. The gates are `check-symbols`, `check-layering`, `check-aliasing`,
`check-stamps` and `check-reader`, all run by `make test`, each observed to fail
on a planted defect before it was trusted. Two fuzzers (`fuzz_sfnt`,
`fuzz_cmap`) run clean under ASan and UBSan over a smoke-length campaign, and
the truncation sweep cuts every table to every length it could have.

Nothing above tier 0 exists: no outlines, no rasteriser, no shaping, no layout,
no discovery, no writer.

**The differentials are built.** `make check-oracle` compares this library
against fontTools in a pinned container over 327 real fonts - DejaVu, Liberation,
the URW base 35 in CFF, Noto core, Noto Color Emoji - which live in the image and
never in this repository. Every field of every table this library parses agrees
(26,908 fields), and so does every codepoint of every `cmap`: 587,186 in the
quick run and all 364,314,624 in `make check-oracle-cmap-exhaustive`. `make test`
needs no container and says so; the oracle targets are separate.

**What phase 0 still owes** is the fixture generator:
`tools/fixtures/make_fixtures.py`, the committed fixtures, and `check-fixtures`.
Until those exist a unit fixture is assembled in the test binary rather than
committed as bytes, and the two deferrals in section 7.2 - `post`'s glyph names
and Mac Roman - have no pinned generator to produce their tables. Section 18.1 of
[documentation/design.md](documentation/design.md) lists the remainder item by
item.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
