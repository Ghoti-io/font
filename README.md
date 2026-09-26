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

Outlines, the rasteriser, shaping, layout, discovery and writing are not
built yet.

## Before you call it

- A table this library does not parse yet comes back as `GFNT_ERR_UNSUPPORTED` and names what it could not read. An empty result is reserved for a font that states the thing is absent.
- An error names the table, the offset within it, and the glyph.
- Mapping a file is a separate call from copying it, and its documentation says `SIGBUS`.
- Macintosh encoding 0 is not Roman for every language, and the language is what picks the right one.
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

From the workspace:

```bash
./bootstrap.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/font test PREFIX="$PWD/.local"
```

`make test` is the suite, and it needs no container. `make help` lists the
rest, including `make test-asan`, `make test-valgrind`, and the fixture and
vector generators.

| Target | What it does |
| --- | --- |
| `make examples` | The three programs under `examples/` |
| `make check-oracle` | Differentials against fontTools, in a pinned container |
| `make fuzz` | The sfnt and cmap fuzzers |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `gfnt_` / `GFNT_`, under `<ghoti.io/font/...>`.
`<ghoti.io/font/font.h>` is the umbrella for what is built.

- **`core.h`** — `GFNT_Result`, `GFNT_Error`, the fixed-point types (26.6, 16.16, 2.14), `GFNT_Limits`, and the version.
- **`allocator.h`** — `GFNT_Allocator`, which is cutil's `GCU_Allocator`.
- **`blob.h`** — the bytes of a font, copied or read from a file.
- **`face.h`** — one font from a blob and an index: the sfnt version, the table directory, collections, and variations.
- **`metrics.h`** — `head`, `hhea`, `OS/2`, `post`, `unitsPerEm`, and per-glyph advances from `hmtx`.
- **`cmap.h`** — code point to glyph, formats 0, 4, 6 and 12, and which subtable answered.
- **`name.h`** — name records decoded to UTF-8 by platform, encoding and language.
- **`glyph.h`** — glyph names from `post` formats 1.0 and 2.0. Bitmap strikes are not implemented.

[What is implemented](#what-is-implemented) is the inventory.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

Found through pkg-config, and the installed `.pc` file names it, so a
program that links `ghoti.io-font-0` links this too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator.

## Documentation

[documentation/design.md](documentation/design.md) is the design: the threat
model (the file is the attacker), what is implemented, and what is not.
`make docs` builds the manual.

## Status

The tables named above are parsed. Outlines, rasterisation, shaping, layout,
font discovery and writing are not implemented.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
