# Ghoti.io Font

Font files in, and glyph identities, advances and names out. The aim is every
family a document might name — sfnt (`glyf` and `CFF`), the bitmap formats,
Type 1, WOFF — a checked reader and no hinting interpreter, fixed-point
rasterisation, shaping, and paragraph layout that stops at the paragraph.

## What is implemented

This is what the library implements.

- An sfnt or a `ttcf` collection: what the file is, and which tables it contains.
- `head`, `maxp`, `hhea`, `hmtx`, `OS/2`, `post`, `cmap` (formats 0, 4, 6 and 12) and `name`, each parsed on first use.
- Which glyph a code point maps to, and how wide that glyph is.
- A glyph's outline, from `glyf` and `loca`, with composites resolved: every flag, point matching, and all three transform encodings.
- A glyph's outline from a **charstring**: `CFF ` inside an sfnt, CID-keyed fonts included, with the Type 2 and Type 1 interpreters behind it.
- A **variable font's design space** — the axes and named instances from `fvar`, `avar` (versions 1 and 2), and the conversion from the coordinates a person types to the normalised ones — and a `glyf` glyph's outline **at a location in it**, moved by `gvar`: every tuple form, inferred points, composite offsets. A **metric at a location** is answered too - an advance from `HVAR` (or, in a font without one, from the phantom points `gvar` carries), a line's extent from `MVAR`, and a side bearing where `HVAR` maps one - and is refused rather than answered with the default's where the font states nothing that says. The control values `cvar` moves, the names `STAT` gives a place in the space, and the `GSUB`/`GPOS` `FeatureVariations` record that applies at one are read too.
- That outline rasterised to 8-bit coverage, at any pixel size and any sub-pixel offset.
- A **bare `CFF `** font program and a **Type 1** one — `.pfb`, `.pfa` or raw — each a face with no sfnt directory at all, stating its em, its glyph count, its names and its advances out of the program itself.
- The four **standalone bitmap** containers: PCF, BDF, PSF 1 and 2, and GNU Unifont's `.hex`. Each is a face with one strike and no outlines, and a glyph comes back as pixels with its own box and advance.
- A font that arrives **gzipped**, which is how a PCF almost always does: the wrapper is undone before the format is looked at, under the same size ceiling a file read from disk is held to.

`CFF2`, the bitmap strikes *inside* an sfnt (`EBDT`/`EBLC`, `CBDT`, `sbix`),
colour, shaping, layout, discovery and writing are not built.

## Before you call it

- A table this library does not parse yet comes back as `GFNT_ERR_UNSUPPORTED` and names what it could not read. An empty result is reserved for a font that states the thing is absent.
- An error names the table, the offset within it, and the glyph.
- Mapping a file is a separate call from copying it, and its documentation says `SIGBUS`.
- Macintosh encoding 0 is not Roman for every language, and the language is what picks the right one.
- An outline comes back in font units and has to be scaled before it can be rasterised. The refusal is deliberate: an unscaled outline would render a thousand pixels tall.
- A bitmap font's measurements are **pixels**, and it has no em. `gfnt_face_units_per_em()` and the font-unit metric calls refuse on such a face and say where the pixels are, rather than answering 1000 and letting the caller scale a strike as though it were an outline.
- Coverage is **linear** alpha. What colour space to blend it in is the caller's, and `gfnt_coverage_apply_table()` applies the caller's table.
- A coverage bitmap's row 0 is its top, and it reports the position of that row relative to the glyph origin, where y is up.
- `NULL` for an allocator is cutil's default.

## Examples

```c
#include <ghoti.io/font/font.h>
#include <stdio.h>

int main(void) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  uint32_t glyph = 0;
  int32_t advance = 0;

  if (gfnt_blob_create_file("Example.ttf", NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK
      || gfnt_face_glyph_for_codepoint(face, 'A', &glyph, &error) != GFNT_OK
      || gfnt_face_glyph_advance(face, glyph, NULL, &advance, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 1;
  }

  printf("'A' is glyph %u, %d font units wide\n", glyph, advance);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
```

The line it prints depends on `Example.ttf`.
`examples/font-info.c` prints the same facts for a file on the command line.
`examples/font-dump.c` and `examples/font-cmap.c` are the longer forms.
`examples/font-outline.c` prints a glyph's points and its path, and
`examples/font-render.c --art` draws it:

```
.....-+-
...=%@@+
..+@@@@=
.-@@@@@-
.*@@@@@:
.%@@@@#.
-@@@@@=.
=@@@@#..
+@@@#...
+%*=....
```

## Compile and link

Once the library is installed, pkg-config carries the include path, the
library, and its dependencies:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-font-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-font-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil) must already be installed where
pkg-config can see it. A dependency it cannot find is a hard error naming
the fix.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/font test PREFIX="$PWD/.local"
```

`make test` is the suite, and it needs no container. `make help` lists the
rest, including `make test-asan`, `make test-valgrind`, and the fixture and
vector generators.

| Target | What it does |
| --- | --- |
| `make examples` | The programs under `examples/` |
| `make check-oracle` | Differentials against fontTools, in a pinned container |
| `make check-golden` | The same pixels, rebuilt for three big-endian targets |
| `make fuzz` | The sfnt, cmap, glyf, raster, CFF and charstring fuzzers |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `gfnt_` / `GFNT_`, under `<ghoti.io/font/...>`.
`<ghoti.io/font/font.h>` is the umbrella for the reader. `outline.h` and
`raster.h` are included by name rather than through it, so a program that only
asks which glyph a code point maps to does not link a rasteriser.

- **`core.h`** — `GFNT_Result`, `GFNT_Error`, the fixed-point types (26.6, 16.16, 2.14), `GFNT_Limits`, and the version.
- **`allocator.h`** — `GFNT_Allocator`, which is cutil's `GCU_Allocator`.
- **`blob.h`** — the bytes of a font, copied or read from a file.
- **`face.h`** — one font from a blob and an index: the sfnt version, the table directory, collections, and the `GFNT_Variation` every accessor takes.
- **`variation.h`** — a variable font's axes and named instances, and `gfnt_face_normalize()`, which makes the normalised coordinates every accessor takes out of the ones a person types.
- **`stat.h`, `featurevar.h`, `cvt.h`** — what a variable font names, replaces and moves besides its outlines: `STAT`'s axes and axis values and which of them name a location; which `FeatureVariations` record applies at one, and the lookups it substitutes; and the `cvt ` control values at a location through `cvar`.
- **`metrics.h`** — `head`, `hhea`, `OS/2`, `post`, `unitsPerEm`, and per-glyph advances from `hmtx`.
- **`cmap.h`** — code point to glyph, formats 0, 4, 6 and 12, and which subtable answered.
- **`name.h`** — name records decoded to UTF-8 by platform, encoding and language.
- **`glyph.h`** — glyph names, from `post` formats 1.0 and 2.0, a CFF charset, a Type 1 `/CharStrings` or a BDF's `STARTCHAR`, whichever the font actually holds them in; and the strike list, with the policy that decides what answers a pixel size.
- **`bitmap.h`** — a glyph that is pixels: PCF, BDF, PSF and `.hex`, with the rows normalised to one layout and every measurement in pixels, because a strike has no em to scale from. The strikes inside an sfnt are not implemented and say so rather than reporting none.
- **`charstring.h`** — the Type 2 and Type 1 interpreters, which know nothing about any container: a program, its subroutines, and the outline it draws. What a PDF library needs for a `FontFile3` stream.
- **`outline.h`** — `GFNT_Outline`: a glyph as a path, from either producer. It holds the font's own points rather than a path derived from them, so `gfnt_outline_decompose()` is where `glyf`'s implicit on-curve points appear. Bounds come two ways, because the box `glyf` states is the box of the coordinates and the curve's own box is smaller; a CFF glyph states no box at all, and the refusal says so.
- **`raster.h`** — the scan converter and `GFNT_Coverage`: exact-area coverage, non-zero or even-odd, and no `float` between a font's bytes and a pixel.

[What is implemented](#what-is-implemented) is the inventory.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

Found through pkg-config, and the installed `.pc` file names it, so a
program that links `ghoti.io-font-0` links this too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator, the growable array, and the overflow-checked size arithmetic.
- [ghoti.io-compress](https://github.com/Ghoti-io/compress) — gzip, for the `.pcf.gz` and `.psf.gz` a bitmap font usually arrives in, and zlib when WOFF 1 lands.

## Documentation

[documentation/design.md](documentation/design.md) is the design: the threat
model (the file is the attacker), what is implemented, and what is not.
`make docs` builds the manual.

## Status

The tables named above are parsed, glyphs from `glyf` and from `CFF `
charstrings are turned into outlines, and those outlines are rasterised. Six
containers are read: an sfnt, a collection, a bare `CFF `, a Type 1 program, and
the four standalone bitmap formats, any of them gzipped. `CFF2`, the sfnt bitmap
strikes, colour, shaping, layout, font discovery and writing are not
implemented.

Every glyph of 312 real fonts is compared against fontTools — 1,999,069 fields
over 37,218 glyphs — and every charstring of another 43 is compared three ways:
its program operator by operator, the path it draws, and the advance it states.
Every pixel of 234 real bitmap fonts is compared against Pillow: 1,589,314 fields
over 73,361 glyphs. The committed renderings — outlines and strikes both — are
reproduced byte for byte on s390x, powerpc64 and sparc64, which is what the
fixed-point arithmetic is for.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
