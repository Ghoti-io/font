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

printf("%s\n", gfnt_version_string());
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
| `make coverage` | Line coverage, per file |
| `make fuzz` | Build and run every fuzzer (`FUZZ_TIME=3600` for a real campaign) |
| `make docs` | Doxygen, into `./docs` |

## The API

Everything is prefixed `gfnt_` / `GFNT_`, under `<ghoti.io/font/...>`.

- **`core.h`** - `GFNT_Result`, `gfnt_result_string()`, `GFNT_Limits` with
  every cap design.md section 15.2 names, and the version.
- **`allocator.h`** - `GFNT_Allocator`, which is cutil's `GCU_Allocator`, so an
  allocator written for any library in the suite works with all of them.
- **`font.h`** - the umbrella for tier 0.

The modules design.md section 4.1 names - `blob.h`, `face.h`, `metrics.h`,
`cmap.h`, `glyph.h`, `outline.h`, `raster.h`, `shape.h`, `layout.h`,
`discover.h`, `write.h` and the rest - do not exist yet. Absent, not stubbed:
there is no function here that returns `GFNT_ERR_UNSUPPORTED` in place of a
parser.

## Status

Scaffold. The library builds, installs, and passes its gates - `check-symbols`,
`check-layering` (all five tiers and the writer, over headers that do not exist
yet), `check-aliasing`, `check-stamps` - with the core module and nothing else.
Phase 0 of [documentation/design.md](documentation/design.md) section 18 is
the next thing, and it waits on nothing in `unicode`; tier 2 does.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
