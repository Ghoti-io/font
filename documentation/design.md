# Design

What is implemented reads the sfnt container, the metric tables, `cmap`
and `name`. §18 lists that, and what of it is not built. Outlines,
rasterisation, shaping, layout, font discovery and writing are not
implemented. Shaping and layout need the `unicode` library. Reading a file
and drawing an outline do not.

`font` is the suite's font library: font files in - every family of them, not
one - and positioned glyphs, coverage bitmaps and laid-out paragraphs out, with
the mapping from characters to glyphs surviving the trip. It reads and writes.
It exists because `cjelly` has to draw text and has written down that it
intends to own its text stack (`libs/cjelly/docs/Overview.md` §8: *"we will
build our own text shaper and font manager over time"*); because `image` has
no way to put text into a raster; and because a PDF library, should one come,
needs to read four font formats and write two of them.

The prefix is `GFNT_` / `gfnt_`. The package is `ghoti.io-font-0`, the include
path `<ghoti.io/font/...>`.

---

## 1. What the library is for

The brief is a correct, cross-platform, dependency-light C library for fonts
and text layout that an enterprise can build on, that produces the same pixels
on every machine, and that can be handed a hostile file without consequence.
Each word is a mechanism:

| Property | Mechanism |
| --- | --- |
| **Correct** | Every format is parsed byte by byte from its specification with explicit shifts (`CONVENTIONS.md` §8), and every parse is checked against an outside reader - fontTools, FreeType, HarfBuzz - never against this library's own opinion (§14). Every quantity that is a policy (which ascent, which strike, which line-break strictness) is a named enum with a documented default (§2 M4, M9). Measurement and painting are one code path (§10.2). |
| **Secure** | The file is the attacker (§1.2). Every read of file data passes through one bounds-checked reader (§6); every recursion has a limit the Standard sets and the library enforces regardless of what the file claims; every unbounded quantity is capped by `GFNT_Limits`; and there is no bytecode interpreter (§2 M3). Fuzzers cover each format that is implemented, because the reads are offset arithmetic over hostile input. |
| **Deterministic** | Outlines, shaping and rasterisation are integer arithmetic in fixed point (§5.2, §8). The same font, size and text produce **byte-identical coverage on every platform and architecture**, and a golden-bitmap gate proves it on x86-64 and on the big-endian 32-bit cross container (§14.4). No platform rasteriser, no platform shaper, no `float` in a core type. |
| **Cross-platform** | Reading, outlines, shaping and layout touch no operating-system API and behave identically everywhere by construction. Font discovery is the only code that does, one file per platform, with the Windows and macOS branches marked per `CONVENTIONS.md` §11. |
| **Dependency-light** | `cutil`, `unicode`, and `compress` (gzip for `.pcf.gz`/`.psf.gz`, zlib for WOFF 1). `image` is optional, for PNG-bearing colour strikes and as a raster surface. Nothing else: not FreeType, not HarfBuzz, not fontconfig, not ICU. Each is an oracle in the tests and none is linked (§14). |
| **Enterprise-ready** | No global state: no process-wide font cache, no default face, no environment read outside font discovery (§15.3). A face is immutable after load and shareable across threads; caches are objects the caller owns. Embedding permissions are reported and never enforced (§17.9). "Which face answered this glyph" and "which strike answered this size" are queryable, because fallback that cannot be audited is a bug report waiting to happen (§2 M8). |
| **Useful** | Three consumers are enumerated in §13 with their exact needs, and the library is designed against all three, not the first one. It draws *something* for any glyph it cannot find - glyph 0 is synthesised (§5.4) - and bundles no font of its own (§14.6): the application's fonts and the system's, once discovery exists, are the only fonts there are. |

### 1.1 The shape is borrowed, deliberately

FreeType's face/glyph/outline model, HarfBuzz's buffer-and-cluster model, and
fontTools' table-object model are the references for what a font library
exposes, and each has decades of consumers. Where this library departs, §2
names the reason; where it does not, the reader can assume the reference's
semantics and conformance behaviour are the target. Layout borrows
its vocabulary - min-content, max-content, line-break strictness, the
typographic and ink boxes - from CSS Text and CSS Inline Layout, because those
are the specifications a GUI toolkit's authors already know.

### 1.2 The threat model

A font file is an attacker-controlled program of offsets. Every table is a
directory of pointers into itself and into other tables, every count is a
loop bound, every glyph can reference another glyph, every charstring can call
a subroutine, and every one of those values arrives from the file. The
historical record is unambiguous: font parsers were, for two decades, the most
productive memory-safety bug class in every operating system, to the point
that Windows moved its parser out of the kernel into a sandboxed process.

Therefore: no struct overlays, no host-endian reads, no pointer arithmetic on
file data outside the reader (§6). Every offset is validated against the
*table's* length, not the file's, so a table cannot reach into its neighbour.
Every recursion - composite glyphs, charstring subroutines, extension lookups,
`COLR` v1 paint graphs - carries a depth counter with a limit that does not
come from the file. Every count is capped by `GFNT_Limits`. And the one
component that is a bytecode interpreter over file-supplied programs - the
TrueType hinting VM - does not exist here (§2 M3). A malformed font produces a
named result and a diagnostic naming the table and offset (§5.6); it never
produces a crash, a hang, or a read past a buffer, and the fuzzers exist to
keep that true.

---

## 2. Mistakes this library exists not to repeat

The first four are this suite's own lessons applied here; the rest are the
field's, and font engineering has a long list.

| # | The mistake | Where it happened | What `font` does instead |
| --- | --- | --- | --- |
| M1 | Reading file data through a struct cast, or trusting an offset because the table directory said so | The font-parser CVE class, every major parser, for two decades | One bounds-checked reader (§6); offsets validated against the table's own extent |
| M2 | Unbounded recursion through file-supplied references | Composite glyphs referencing themselves; charstring subroutines; nested lookups | A depth limit on every recursion, set by the Standard or by `GFNT_Limits`, never by the file (§6.2) |
| M3 | A bytecode interpreter over programs the file supplies, run in a trusted context | TrueType instructions; Type 1 `OtherSubrs`; the kernel-mode parsers that led to Windows' `fontdrvhost` sandbox | No hinting interpreter (§8.5). Outlines are rendered unhinted with good anti-aliasing, which is what the web and every modern GUI toolkit now ship |
| M4 | Line height from whichever metrics table the first platform used | Windows using `usWinAscent`/`usWinDescent`, macOS `hhea`, browsers a compromise; the same font a different height on each | `GFNT_LineMetrics` policy (§10.3), with the documented default following `OS/2` `fsSelection` bit 7 (`USE_TYPO_METRICS`) as the font's own request |
| M5 | Measuring with fractional advances and painting with rounded ones | Every text box whose last glyph is clipped | Measure and paint are one code path in 26.6 fixed point (§10.2). Rounding, if any, is a named policy applied in both |
| M6 | Shaping output that discards which characters made which glyphs | Every renderer that could draw text but not select it; `cjelly`'s `semantics.md` names this as the rewrite to avoid | The cluster map is a field of `GFNT_ShapedRun`, not an option (§9.3) |
| M7 | A process-wide font cache and a default face | fontconfig's `FcConfig` global; every toolkit with a singleton font manager | No globals (§15.3). Caches are objects; discovery returns a set the caller owns |
| M8 | Silent substitution: a glyph drawn from a font the caller did not name, with no way to find out | Every fallback chain that "just works" until the wrong glyph ships | Fallback is a provider the caller supplies, and every glyph in a shaped run records the face that answered it (§10.5) |
| M9 | A bitmap strike scaled as though it were an outline, or an outline ignored because a strike existed | Blurry 13px text; crisp 12px next to smeared 14px | Strikes are a first-class list; "which strike answers this size" is `GFNT_StrikePolicy` (§5.3) |
| M10 | `cmap` format 4 `idRangeOffset` arithmetic done in 16 bits, or relative to the wrong base | The single most-implemented and most-misimplemented subtable | Checked arithmetic through `safemath`; the subtable validated against fontTools over every codepoint (§14) |
| M11 | Refusing a whole face because one glyph is corrupt, or accepting the whole face and crashing on that glyph | Fonts in the wild routinely have one bad `loca` entry | Face load validates only what indexing needs; each glyph fails on its own with `ERR_CORRUPT` and renders as `.notdef` under a named policy (§7.8) |
| M12 | `numGlyphs` taken from `maxp` and trusted against `loca`, `hmtx`, `post` and `CFF` | Fonts where the tables disagree, and the parser that read past the shorter one | The minimum governs; the disagreement is a diagnostic (§7.2) |
| M13 | Assuming every font has a `cmap`, a `name`, an `OS/2` | Fonts embedded in PDFs are subsets with none of them | Required tables are per *operation*, not per file (§7.8). Glyph-id access is first-class; `cmap` is optional |
| M14 | `.notdef` treated as an error | Callers that crash on glyph 0 | Glyph 0 always exists; the library synthesises a box if the font has none (§5.4) |
| M15 | `name` records decoded without regard to platform and encoding IDs | Family names in Mac Roman read as Latin-1; UTF-16BE read as bytes | Decoded by `(platform, encoding, language)` to UTF-8 with a documented preference order (§7.2) |
| M16 | Kerning applied twice - from `kern` and from GPOS | Fonts carrying both for compatibility | GPOS `kern` disables the `kern` table, as HarfBuzz does (§9.4) |
| M17 | Advance widths from `hmtx` on a variable font at a non-default instance | Text that reflows when the weight changes | Every metric accessor takes a `GFNT_Variation *`; `HVAR`/`VVAR`/`MVAR` are consulted (§7.7) |
| M18 | Sign conventions mixed between tables | `hhea` descender negative, `usWinDescent` positive, CFF `FontBBox` y-up | One convention in the API: y-up, ascent positive, descent negative, in every function (§5.5) |
| M19 | The same Han codepoint rendered with the wrong regional form | `locl` ignored; no language tag | Shaping takes a language; the OpenType language-system registry is vendored (§9.5) |
| M20 | A writer whose checksums, padding or table order are wrong | Passes every reader that ignores them (most) and fails the ones that do not, which is how it ships | Every written font is read back by this library and by fontTools and FreeType before the writer is trusted (§12.5) |
| M21 | Subsetting that drops glyphs only reachable through `GSUB` | Ligatures missing from PDFs for a decade | Glyph closure walks composites and, by option, the layout tables (§12.3) |
| M22 | Rasterising at a size the caller passed straight from untrusted input | A 65,535-ppem glyph is a gigabyte | `GFNT_Limits::max_ppem` and `max_raster_bytes` (§15.2) |
| M23 | Memory-mapping a font file that is then truncated | `SIGBUS` on the next read; no C-level recovery | `GFNT_Blob` copies by default; mapping is an explicit choice with the hazard in its documentation (§5.1) |
| M24 | A font library that bundles a font, so every consumer ships a third-party licence obligation it never chose | Toolkits that embed a fallback face; its notice then travels with every binary every downstream distributor produces | No font is bundled (§14.6). Glyph 0 is synthesised in code, so a face with no usable glyphs still draws boxes; an application ships its own fallback as its own asset, or finds the system's through font discovery |

---

## 3. The format universe

"Font" is four unrelated families that share a purpose. What follows is
which of them this library takes, and which of those it reads today.

| Family | Formats | Verdict |
| --- | --- | --- |
| **sfnt-wrapped** | `.ttf`, `.otf`, `.ttc`/`.otc` | **required.** The container, the metric tables, `cmap` and `name` are implemented. `glyf` and `CFF ` are not |
| | WOFF 1 | **required**, not implemented |
| | `EBDT`/`EBLC`/`EBSC` strikes; `COLR` v0 + `CPAL` | **wanted**, not implemented |
| | `CBDT`/`CBLC`, `sbix`, `COLR` v1 | wanted, not implemented; colour bitmaps need `image` |
| | variations: `fvar`/`avar`/`gvar`/`HVAR`/`VVAR`/`MVAR`/`STAT` | not implemented. The accessor already takes a `GFNT_Variation *` (§7.7) |
| | OpenType layout: `GDEF`/`GSUB`/`GPOS`/`BASE`/`JSTF` | **required**, not implemented |
| | WOFF 2 | not here; it waits on Brotli in `compress` |
| | `CFF2`, AAT (`morx`/`kerx`/...), `SVG `, `.dfont`, `.eot`, hinting | absent, §16 |
| **standalone bitmap** | PCF, BDF, PSF 1/2 | **implemented**, including the `.gz` they usually arrive in. 1,885 of them on a stock Linux box |
| | GNU Unifont `.hex` | implemented |
| | Windows FNT/FON, raw ROM fonts | absent |
| **standalone outline** | Type 1 (`.pfb`/`.pfa` + `.afm`/`.pfm`), bare `CFF` | **wanted**, not implemented |
| | Type 3, Type 42, Multiple Master, standalone SVG fonts, Metafont | absent; Type 3 is a PDF library's (§13.3) |
| | Hershey strokes | optional; needs no rasteriser |
| **sidecars** | AFM/PFM, `fonts.dir`/`fonts.alias`, the AGL, the OpenType language registry | with the formats that need them |

---

## 4. Modules

What is implemented is the reader: a blob, the checked sfnt reader, metrics,
`cmap`, names and glyph access. The rest of this table is not implemented.
A header does not include one it does not need, and `make check-layering`
enforces that, as `chron` does.

| What | Holds | Needs | Who uses it | Implemented |
| --- | --- | --- | --- | --- |
| Reading | blob, the checked reader, every table parser, metrics, `cmap`, strikes, glyph access, the glyph union | `cutil`, `compress` | everyone | the sfnt container, metric tables, `cmap` and `name`. §18 is the list |
| Outlines and raster | outlines as paths; rasterisation to coverage | the reader | `image`, `cjelly`, PDF | `glyf`/`loca`, `CFF ` charstrings and the scan converter. §18 is the list |
| Shaping | `GDEF`/`GSUB`/`GPOS`, the script shapers, the cluster map | the reader, `unicode` | `cjelly`, PDF (for text extraction) | no |
| Layout | itemisation, bidi, breaking, paragraphs, boxes, hit testing | shaping, `unicode` | `cjelly`, `image` | no |
| Discovery | directories, matching, the platform APIs | the reader, the OS | `cjelly` | no |
| Writing | writing and subsetting | the reader | PDF, tools | no |

`image` wants the reader, rasterisation and layout ("draw this wrapped
label") and never pays for discovery; a PDF reader wants the reader, shaping
and writing; `cjelly` wants all of it. Keeping layout in its own headers
leaves "should layout be its own library" an open option that costs nothing
to keep open.

### 4.1 The modules

| Header | Holds |
| --- | --- |
| `core.h` | `GFNT_Result`, `GFNT_Limits`, `GFNT_Error`, the fixed-point types, version |
| `blob.h` | `GFNT_Blob`: bytes with ownership and length; file, memory, mmap |
| `face.h` | `GFNT_Face`: one font from a blob; the table directory; `numGlyphs`; the strike list; `GFNT_Variation` |
| `metrics.h` | `head`, `hhea`/`hmtx`, `vhea`/`vmtx`, `OS/2`, `post`, `maxp`; ascent/descent policies; per-glyph advances and bounds |
| `cmap.h` | codepoint → glyph, all subtable formats; variation selectors; reverse lookup |
| `name.h` | `name` records decoded to UTF-8 |
| `glyph.h` | `GFNT_Glyph`, the tagged union; the strike-selection policy |
| `bitmap.h` | `EBDT`/`EBLC`, PCF, BDF, PSF, `.hex` strikes. The four standalone containers are implemented; `EBDT`/`EBLC` are not |
| `color.h` | `COLR`/`CPAL`, `CBDT`, `sbix`. Not implemented |
| `outline.h` | `GFNT_Outline`: the path; `glyf` and charstring producers; transforms; bounds. Both producers are implemented |
| `raster.h` | the scan converter; `GFNT_Coverage`; the `GIMG_Raster` bridge. The scan converter and `GFNT_Coverage` are implemented; the bridge waits on `image` |
| `charstring.h` | the Type 1 and Type 2 interpreters, container-independent (§7.4). Implemented; of the four containers that feed them, `CFF ` inside an sfnt is read |
| `shape.h` | `GFNT_ShapedRun`, features, the language registry, the script-shaper vtable. Not implemented |
| `layout.h` | `GFNT_Paragraph`, `GFNT_Line`, boxes, hit testing, the providers. Not implemented |
| `discover.h` | `GFNT_FontSet`, directory scanning, matching, the default fallback provider. Not implemented |
| `write.h` | the sfnt serialiser, the subsetter, WOFF 1, the PDF helpers. Not implemented |
| `font.h` | umbrella for what is implemented |

---

## 5. The object model

### 5.1 `GFNT_Blob`

Bytes plus length plus ownership. Created from a memory buffer (borrowed or
copied), from a file read whole through `cutil`'s file module, or - as an
explicit, separately named call - from an `mmap`. **The default for a file is
to read it**: a mapped file that is truncated after mapping raises `SIGBUS` on
the next access and C has no way to recover (M23). `gfnt_blob_create_mmap()`
exists for trusted, large fonts (a 30 MB CJK collection) and its documentation
says the word `SIGBUS`.

There is no `GFNT_Stream`. `CONVENTIONS.md` §5 says a library that reads
input reads it through its own stream, and `regex` records the departure for a
related reason; here the reason is that a font is random-access by
construction - the table directory is a list of offsets - so a sequential
stream would be seeked past immediately. The blob and the reader (§6) are the
stream's replacement, and the departure is recorded in §13 of the conventions.
The two line-oriented text formats, BDF and `.hex`, read through a line reader
with `max_line_length`.

### 5.2 Fixed point, and no `float` in a core type

Three fixed-point types, named for their layout as the OpenType specification
names its own:

| Type | Layout | Used for |
| --- | --- | --- |
| `GFNT_F26Dot6` | `int32_t`, 1/64 | pixel-space coordinates and advances; font-unit outlines scaled by 64 so CFF and variation fractions survive |
| `GFNT_F16Dot16` | `int32_t`, 1/65536 | scale factors, transforms |
| `GFNT_F2Dot14` | `int16_t`, 1/16384 | variation coordinates and composite transforms, the format's own |

Every geometric quantity is one of these, every multiplication goes through
a 64-bit intermediate, and every one is checked by `cutil`'s `safemath` or a
signed helper beside it. This is what makes §1's determinism claim true rather
than aspirational: there is no `float` whose rounding differs between an x87
build and an SSE one, and no fused-multiply-add the compiler may or may not
contract. FreeType made the same choice for the same reason.

### 5.3 `GFNT_Face`, strikes and `GFNT_Variation`

A face is one font: a blob plus an index into a collection. Loading a face
parses the table directory and **nothing else** (§7.8); each table is parsed
and validated on first use and the result memoised, so that opening a 30 MB
collection to ask for one glyph costs one glyph.

A face exposes its **strike list**: the pixel sizes at which it carries
bitmaps, from `EBLC`/`CBLC`/`sbix`, or the single strike of a PCF/BDF/PSF
font, or none for a pure outline font. Every size-dependent call returns which
strike answered, or that the answer was scaled from outlines, and takes a
`GFNT_StrikePolicy`:

```c
typedef enum {
  GFNT_STRIKE_OUTLINES_ONLY = 0, /* ignore strikes; ERR_UNSUPPORTED if no outlines */
  GFNT_STRIKE_EXACT,             /* a strike at exactly this ppem, else outlines */
  GFNT_STRIKE_NEAREST,           /* the nearest strike, unscaled, else outlines */
  GFNT_STRIKE_PREFER_STRIKE      /* nearest strike, scaled if needed; last resort */
} GFNT_StrikePolicy;
```

Zero is the policy that cannot surprise a caller who never heard of strikes.

`GFNT_Variation` is a caller-owned array of normalised axis coordinates
(`F2Dot14`, `-1..1` after `avar`). **Every accessor that a variation can
change takes a `const GFNT_Variation *`**, `NULL` meaning the default instance -
before any variation table is parsed - because adding the
parameter later is a break across the whole API and adding it now is an
unused argument (§7.7).

### 5.4 `GFNT_Glyph` is a tagged union

Five different things can come back for one glyph id, and a caller that cannot
ask which cannot composite it:

```c
typedef enum {
  GFNT_GLYPH_OUTLINE,      /* a GFNT_Outline, quadratic or cubic          */
  GFNT_GLYPH_BITMAP_MONO,  /* 1-bit strike, with its own metrics          */
  GFNT_GLYPH_BITMAP_GRAY,  /* 2/4/8-bit strike                            */
  GFNT_GLYPH_BITMAP_PNG,   /* CBDT / sbix: bytes needing a decoder        */
  GFNT_GLYPH_COLR_LAYERS,  /* a COLR v0 layer list or a v1 paint graph    */
  GFNT_GLYPH_SVG           /* absent (§16); the arm exists so the enum is complete */
} GFNT_GlyphKind;
```

Each arm carries the strike that answered and the metrics *for that arm* - a
bitmap's advance is its own, not `hmtx`'s. **Glyph 0 always exists**: if the
font's `.notdef` is empty or missing, the library synthesises a hollow box in
the face's em (M14), and says so in the glyph's source field.

### 5.5 Units and conventions

Font units are the face's `unitsPerEm`, y-up, origin at the baseline and the
left side bearing origin, as every format's specification has them. Pixel
space is y-up too, in 26.6, with the caller applying whatever flip their
raster wants - `GFNT_Coverage` reports its origin and the raster bridge does
the flip. Ascent is positive, descent is **negative**, line gap is positive, in
every function and from every table, converted from each table's own
convention on the way out (M18).

### 5.6 Results and diagnostics

`GFNT_Result` is the fixed vocabulary of `CONVENTIONS.md` §5, unchanged:

| Result | Here means |
| --- | --- |
| `ERR_FORMAT` | not a font this library recognises: no sfnt magic, no PCF magic, no `%!PS-AdobeFont` |
| `ERR_UNSUPPORTED` | a font, with a feature outside §16's line: `CFF2`, a `morx` table, a hinting-only request |
| `ERR_CORRUPT` | a font, with bytes that violate its own specification: an offset past its table, a `loca` running backwards |
| `ERR_LIMIT` | a `GFNT_Limits` field exceeded, never by silently truncating |
| `ERR_INVALID` | a caller error: `NULL`, a glyph id past `numGlyphs`, a ppem of zero |

The distinctions must survive per table, or "this font is broken" and "we do
not do CFF2" become one answer. A missing *optional* table is not an error at
all. A `GFNT_Error` carries the table tag, the byte offset within it, the
glyph id if one was involved, and a static message, as `chron`'s `GCHRON_Error`
carries a position - because "corrupt" is not a diagnostic.

---

## 6. The checked reader

`GFNT_Reader` is a `(const uint8_t * base, size_t length, size_t cursor)`
over one table's extent, and **every read of file data in the library goes
through it**. It offers `u8`, `u16`, `u32`, `s16`, `s32`, `F2Dot14`,
`Fixed`, `LONGDATETIME`, `Tag`, `Offset16`, `Offset32` and `bytes(n)`, each of
which checks that the read fits before performing it and returns
`ERR_CORRUPT` with the offset if it does not. A sub-reader is derived from a
parent by `(offset, length)` and cannot exceed the parent. Table readers are
derived from the directory entry's `(offset, length)` against the blob, so a
table cannot read into its neighbour by construction.

This is not a style choice. It is the mechanism behind §1.2, and it is also
what makes the fuzzers meaningful: with one reader, a fuzzer that finds a
crash has found a defect in the reader or a read that bypassed it, and
`check-reader` - a grep for `base[`, `*(uint16_t *)` and their kin under
`src/` outside `reader.c` - fails the build on the second kind. **Built**, with
two fields beyond the three above: the reader carries the table's tag and the
caller's `GFNT_Error *`, which is what lets a failed read record which table
and which offset at the point of failure rather than at a call site that has
forgotten. `check-reader` also confines `gfnt_blob_data()` to the blob and the
reader, and runs each of its patterns against a planted violation in the same
invocation, so a pattern that has rotted into matching nothing fails the build
instead of reporting a clean tree.

### 6.1 Endianness and alignment

Every multi-byte read assembles bytes with shifts. There is no `ntohs`, no
`memcpy` into a `uint16_t`, no `#ifdef __BIG_ENDIAN__`. The cross container
(big-endian, 32-bit, strict-alignment) runs the full test suite and the
golden-bitmap gate (§14.4), which is how a read that accidentally depended on
the host would be caught.

### 6.2 Recursion budgets

| Recursion | Limit | Source |
| --- | ---: | --- |
| composite glyph nesting | 16 | `GFNT_Limits::max_composite_depth`; `maxp` is not consulted |
| charstring subroutine depth | 10 | Type 2 specification, appendix B; `GFNT_Limits` cannot raise it |
| charstring operand stack | 48 (Type 2), 513 (`CFF2`) | the specifications |
| `GSUB`/`GPOS` nested lookups | 6 | `GFNT_Limits::max_lookup_depth`; matches HarfBuzz |
| substitutions per glyph position | 64 | `GFNT_Limits::max_ops_per_glyph`, the loop guard for a recursive font |
| `COLR` v1 paint graph depth | 64 | `GFNT_Limits::max_paint_depth` |
| `EBDT` composite bitmap components | 16 | as composites |

Every limit is a counter on a stack frame, and exceeding one is `ERR_LIMIT`
for that glyph, not for the face.

---

## 7. Reading

### 7.1 Containers

**sfnt.** The offset table (`0x00010000`, `OTTO`, `true`, `typ1`) and the
table directory. `searchRange`, `entrySelector` and `rangeShift` are derived,
never read. Table offsets and lengths are validated against the blob; overlap
between tables is permitted (fonts in the wild do it) and noted. Checksums are
verified on request and reported, never enforced - a font with a bad checksum
renders in every other implementation and must here.

**Collections.** `ttcf` versions 1.0 and 2.0; faces share a blob and a parsed
directory cache; the `DSIG` in a 2.0 header is skipped. A face is
`(blob, index)`.

**WOFF 1.** The header, the table directory with `compLength`/`origLength`,
per-table zlib inflation through `compress`, into a reconstructed sfnt blob
that the ordinary path then reads. The metadata and private blocks are exposed
as bytes. **WOFF 2** is not here: it needs Brotli, which belongs in `compress`
as an eighth method, and it transforms `glyf`/`loca` and `hmtx` in ways that
are a second reader. It is not implemented.

**PCF, BDF, PSF, `.hex`** are their own containers and their own glyph sources,
read through the same reader (PCF, PSF) or the line reader (BDF, `.hex`). Each
produces a `GFNT_Face` with one strike and no outlines. PCF's table of contents
becomes a multi-entry synthetic directory, so each of its tables gets a reader
spanning itself; the other three take one entry over the file.

Two things are normalised on the way out and both are in `bitmap.h`'s
documentation, because they are API rather than implementation. **Rows run top to
bottom with bits most-significant-first**, one row starting on a byte: PCF alone
stores bits either way round, in scan units of one, two or four bytes, with rows
padded to one, two, four or eight, and the four choices are independent. And
**every measurement is pixels, with no em**: `gfnt_face_units_per_em()` refuses on
such a face rather than answering 1000, and so do the font-unit metric accessors,
because a strike's advance handed to a caller expecting font units is M9 with no
symptom until the text is the wrong size.

**Where the four formats differ they are made to say so.** BDF states a baseline
and PCF compiles one; PSF is a console cell and `.hex` is sixteen rows, and
neither says where the line is - so those two report the box where it is and a flag
records that this is the format's silence rather than a measurement. A PSF without
its Unicode table states no characters at all, and a codepoint lookup on one is
refused by name rather than pretending a cell index is a character.

**The gzip a PCF usually arrives in is read**, through `compress` (RFC 1952). It
is a **wrapper and not a container**: inflating a `.pcf.gz` produces a PCF, and
the probe that follows does not know the difference. So it happens once, in
`gfnt_face_load()`, before the flavour is looked at, and the inflated bytes are
*derived* in exactly the sense `GFNT_Face::bytes` already meant for a Type 1
program. The two stack - a `.pfb.gz` is inflated and then deciphered - and each
layer frees the one it consumed once it has read it.

The ceiling on the inflated size is `GFNT_Limits::max_blob_bytes`, the same cap
a file read from disk is held to, because what comes out of the inflater *is*
the font file from there on. `compress` checks it before every enlargement, so a
decompression bomb is refused by the documented number rather than by
exhausting memory.

**A PCF's encodings are positions in the font's own charset**, which its XLFD
`CHARSET_REGISTRY` and `CHARSET_ENCODING` properties name. For an `ISO10646-1`
font those are codepoints; for an `ISO8859-5` font they are not, and converting
them needs a charset table this library does not carry - the same gap Type 1's
`/Encoding` has without the Adobe Glyph List.

**Type 1.** PFB segment headers (`0x80 0x01/0x02/0x03`) and PFA cleartext;
`eexec` decryption (`r = 55665`), charstring decryption (`r = 4330`, `lenIV`);
the `/Encoding`, `/Subrs`, `/CharStrings` and `/Private` dictionaries parsed
with a small PostScript-token scanner that understands exactly what a Type 1
font program contains and nothing more - it is not a PostScript interpreter.
Metrics from an `.afm` or `.pfm` sidecar when given one; without one, from the
charstrings' `hsbw`.

### 7.2 Metrics, mapping, names

`head` (`unitsPerEm`, `indexToLocFormat`, the bounding box, `macStyle`),
`hhea`/`hmtx` and `vhea`/`vmtx` (`numberOfHMetrics` short of `numGlyphs` means
the last advance repeats), `maxp`, `OS/2` versions 0-5 (`fsType` reported,
`fsSelection`, the four vertical metrics, `xAvgCharWidth`, `capHeight` and
`xHeight` from version 2, the Unicode and code-page ranges, weight and width
class, PANOSE), `post` (format 2.0 glyph names; 1.0's standard 258; 3.0 none),
`gasp` (advisory), `kern` format 0 and Apple's format 2 (§9.4).

**`numGlyphs` is the minimum** across `maxp`, `loca`, `hmtx` and `CFF`'s
`CharStrings`, and a disagreement is a diagnostic on the face (M12).

**`cmap`**: formats 0, 2, 4, 6, 8, 10, 12, 13, 14, with the Unicode subtables
preferred in the order `(3,10)`, `(0,6)`, `(0,4)`, `(3,1)`, `(0,3)`, then the
symbol subtable `(3,0)` with its `0xF0xx` mapping, then Macintosh Roman.
Format 4's `idRangeOffset` is computed in `size_t` from the address of the
`idRangeOffset` entry itself, checked, and the whole subtable is compared with
fontTools over every codepoint (M10). Format 14's default and non-default
variation sequences are exposed as `gfnt_cmap_lookup_variation(face, cp, vs)`.
A reverse map (glyph → first codepoint) is built lazily for `post`-less fonts
and for PDF text extraction.

**Both of §7.2's deferrals are built, as of 2026-09-25.** `post` glyph names
and Macintosh `name` decoding were one deferral wearing two hats: each needed a
vector, and §14's rule is that a vector comes from an oracle and is never
written from memory. `tools/vectors/make_vectors.py` generates both from the
pinned image - the 258-entry standard Macintosh glyph order and the eight
single-byte Macintosh encodings - and three gates cover them
(§14.8). `gfnt_face_glyph_name`, `gfnt_face_glyph_for_name` and
`gfnt_face_glyph_names_dump` read format 1.0 and 2.0; 2.5 and 4.0 are refused by
name rather than guessed. `vhea`/`vmtx`, `gasp` and `kern` are not built.

**`name`**: every record decoded by `(platformID, encodingID, langID)` - Unicode
and Windows UTF-16BE, Windows symbol, and the single-byte Macintosh encodings by
generated table - to UTF-8, with the preference order Windows English, then
Unicode, then Macintosh, then anything, documented and overridable by a language
argument.

**The Macintosh encoding is not chosen by the encoding ID.** `platEncID` 0 is
keyed by `langID`: Mac Roman for most languages, and Icelandic, Turkish,
Croatian, Central European or Romanian for thirteen of them. Reading it as Mac
Roman throughout decodes those records to the wrong letters *without failing*,
which is the worst shape of wrong available here - so the relation is restricted
at both ends and the rule table is generated rather than written. The multi-byte
Macintosh and Microsoft encodings (Japanese, the two Chinese, Korean, Shift-JIS
and friends) are refused: they are a data set of their own, and a byte-per-
codepoint guess would produce plausible mojibake, which is worse than an error.

**Every one of the 557 Macintosh `name` records in the oracle corpus is
`(platEncID 0, langID 0)`** - Mac Roman - so the language-keyed rules and the
seven non-Roman tables have **no coverage from real fonts at all**. Measured
2026-10-01 over all 329 fonts of the corpus, every face of a collection included,
by counting `platformID 1` records with fontTools; this library's own dump counts
530 of them over face 0 of each file, and the two numbers differ by exactly the
collections' further faces. Both are stated because an earlier edition of this
paragraph gave a figure that neither instrument reproduces at any face policy,
and a count whose instrument is not named beside it cannot be checked where it is
read. `name-mac-encodings.ttf`
exists for them, and carries every one of the 128 high bytes in each encoding. Name IDs 0-25 by constant; family, style,
full, PostScript and the typographic and WWS families by function.

### 7.3 `glyf` and `loca`

Simple glyphs: the flag stream with `REPEAT`, `X_SHORT`/`Y_SHORT` and the
same-or-positive bits, implicit on-curve midpoints between consecutive
off-curve points, contours that begin off-curve. Composites: every flag -
`ARGS_ARE_XY_VALUES` and point matching when it is clear, `WE_HAVE_A_SCALE`,
`X_AND_Y_SCALE`, `TWO_BY_TWO`, `SCALED_COMPONENT_OFFSET` versus
`UNSCALED_COMPONENT_OFFSET` (the Apple/Microsoft difference, defaulting to
unscaled), `USE_MY_METRICS`, `ROUND_XY_TO_GRID` (ignored, no hinting) - to the
depth of §6.2. Phantom points are computed for variation support. Instructions
are skipped by length. A `loca` entry running backwards or past `glyf` makes
*that glyph* `ERR_CORRUPT` (M11).

### 7.4 Charstrings, independent of their container

`charstring.h` is one module holding the **Type 2** interpreter (the operators,
`hintmask`/`cntrmask` with the stem count that decides the mask width, the
subroutine bias by count, `endchar` with four or five arguments as `seac`, the
width parsing against `nominalWidthX`/`defaultWidthX`, the transient array,
`hflex`/`flex`/`hflex1`/`flex1`) and the **Type 1** interpreter (`hsbw`, `sbw`,
`seac`, `closepath`, `callothersubr` for flex and hint replacement, `div`,
`dotsection`), each producing a `GFNT_Outline`. Neither knows what an sfnt is.

Four containers feed them: `CFF ` inside an sfnt (header, the INDEX
structures, Top and Private DICTs, charsets, encodings, `FDArray`/`FDSelect`
for CID-keyed fonts), **bare CFF** (the same without the sfnt, as PDF embeds it
as `FontFile3`/`Type1C` and `CIDFontType0C`), Type 1 (§7.1), and - absent, §16 -
`CFF2`. This is decision §17.7 and it is what makes Type 1 cheap after CFF,
and what a PDF library would need (§13.3).

**Built: both interpreters, and the first of the four containers.** `CFF ` inside
an sfnt is read - every structure above, CID-keyed fonts included - and
`gfnt_face_glyph_outline()` answers for an OTTO face. `CharstringType 1`, which
puts Type 1 programs in a CFF, is honoured, so the Type 1 language has a container
and a differential even though the Type 1 *font format* does not.

**Bare CFF is built, as of 2026-09-28.** A ::GFNT_Face used to be an sfnt: it
holds a table directory, and every parser reaches its bytes through a reader
derived from a directory entry, so a container with no directory could not be a
face. What that needed turned out not to be a new abstraction but a **synthetic
directory**: one entry spanning the blob, carrying the format's real tag, after
which every table parse, memo, limit and producer dispatch above it works
unchanged and unaware. `gfnt_sfnt_single_table_directory()` is that, and it is
what the remaining directory-less containers will use - a container with several
regions (Type 1's cleartext and `eexec` portions, PCF's typed table of contents)
wants several entries and a variant of it.

Three things follow from there being no tables, and each is answered from the
font program rather than defaulted:

- **The em** comes from the `FontMatrix`, and a CFF that states none is a
  1000-unit font by the format's own default - exactly. A stated matrix is
  inverted only where 16.16 holds its reciprocal exactly, which it does for every
  power of two and does not for 1000: 0.001 is 65.536, a font writes 65 or 66,
  and those invert to 1008 and 993. Recognising 1000 *before* inverting is what
  makes both of those right. A matrix that skews, that scales the two axes
  differently, or whose reciprocal is not exact is refused by name, for the same
  reason a matrix disagreeing with `head.unitsPerEm` is: applying it would put
  these coordinates in a space no reference pen reports.
- **The glyph count** comes from `CharStrings`. With no `maxp` it is not a
  minimum to be reconciled (M12) - it is the count.
- **The name and the licence** come from the Top DICT's six strings and the Name
  INDEX, decoded as Latin-1 (which is what fontTools decodes them as, and
  fontTools is what every string here is compared against). A PDF identifies a
  `FontFile3` by that PostScript name, and a committed fixture has to state its
  own licence (§14.5) with no `name` table to state it in. A face carrying both
  sources reads its `name` table.

**Type 1 is built too, as of 2026-09-28**, and it needed one thing the bare CFF
did not: **its bytes do not exist in the file.** The private half of a font
program is `eexec`-encrypted and a PFA's is ASCII-hex on top of that, so there is
nothing for a reader to point at. A face therefore has two blob fields - `bytes`
is where the directory's offsets are and `owned` is what it must free - and the
loader decrypts the whole program into a blob of its own before the directory is
synthesised. WOFF will want exactly that when it arrives, for the same reason at
one remove: its tables are compressed.

- **Not a PostScript interpreter.** The scanner knows what a font program
  contains - names, numbers, arrays, procedures it skips, strings, and the
  `RD`/`ND`/`NP` idiom that introduces binary - in both spellings of each,
  because which one a font uses is its writer's habit and a reader that knew one
  would refuse half the fonts in the world.
- **Every charstring is deciphered once**, at parse time, into one arena. The
  interpreter's subroutine accessor promises a pointer that outlives the run, and
  a charstring deciphered into scratch has none to give.
- **Type 1 is name-keyed**, so the glyph *index* is this library's invention.
  `gfnt_type1_order()` is the only place that decides it: `.notdef` first because
  glyph 0 means "no glyph" everywhere else here, then the order `/CharStrings`
  gave them. A codepoint lookup is refused rather than guessed at - a Type 1
  `/Encoding` maps codes of the font's own choosing, and reaching Unicode from
  them needs the Adobe Glyph List, which is not here.
- **The advance is in the charstring.** There is no `hmtx` and never will be:
  each glyph states its own advance in `hsbw`, so reading a metric means running
  a program.

**The four bitmap containers of §7.1 are built too**, each producing a
`GFNT_Face` with one strike and no outlines, and neither the face nor the derived
bytes needed changing for them.

Three decisions the interpreters make, each of which could have gone the other
way and each of which is refused rather than guessed:

- **`random` is refused by name.** Section 1 promises one font at one size gives
  one bitmap on every platform for ever, and an operator whose value is by
  definition unpredictable cannot be part of that. Nothing in the corpus uses it.
- **A `FontMatrix` that is not `head.unitsPerEm`'s own scale is refused.** A
  charstring's coordinates are in the font's own charstring space, and for every
  font anybody ships that space is the em. One where they disagree would need the
  matrix applied - and applying it would put this library and every reference pen
  in different spaces, while ignoring it draws at the wrong size and reports
  success.
- **A CFF glyph states no bounding box**, so
  `gfnt_face_glyph_stated_box()` refuses and names `FontBBox`: that box is the
  whole font's, and answering with it would answer a question nobody asked with a
  number that looks like the one they did.

### 7.5 Bitmap strikes

`EBLC`/`EBDT` (and Apple's `bloc`/`bdat` spellings): the `bitmapSizeTable`s,
index subtable formats 1-5, glyph bitmap formats 1-9 including the composite
formats 8 and 9 and the metrics-in-data forms; `EBSC` scaled references.

**`EBSC` has a population now and did not when this was written.** Eleven files
in Debian carry one - Anonymous Pro's four and seven of Wine's core bitmap faces -
and what they state is a list of 28-byte `BitmapScale` records, each naming a ppem
that is to be drawn by *scaling* another ppem's `EBLC` strike. Anonymous Pro asks
for 7, 8 and 9 ppem from its 10 ppem strike; Wine's `system.ttf` names 21 of them,
8 through 40, drawn from its 16 and 18 ppem strikes. **FreeType does not read the
table at all**, so fontTools is the only reference for its fields and there is no
second reader of a scaled pixel anywhere - and the scaling rule itself is not in
the specification, which says only that the substitute strike is to be used.

**A composite's components are placed by their offsets and by nothing else.**
Image formats 8 and 9 carry no rows: past the metrics - and, for format 8, past a
**pad byte** that format 9 does not have - they are a `uint16` count and four
bytes per component, which are a glyph id and two signed offsets. Those offsets
are **destination pixel coordinates in the composite's own box**, x from its left
edge and y *down* from its top row, and the component's own bearings play no part
in positioning it. Components are OR-ed in, one canvas for the whole tree: a
nested composite contributes its components at its offsets plus theirs and has no
intermediate image, which is both what FreeType's
`tt_sbit_decoder_load_compound()` does and the only arrangement that is correct,
since a composite's box is its own and not the union of what lands in it.

Two consequences this library takes deliberately:

- **A component that falls outside the composite's box is refused**, not clipped.
  The font states the box and then states what goes in it; the two disagreeing is
  the font contradicting itself, and FreeType refuses the same case.
- **A cycle is named, and the depth budget is what is left.** A glyph that
  reaches itself - directly or through another glyph - is `ERR_CORRUPT` with that
  sentence, found by walking the chain of glyphs currently being painted rather
  than by recursing until the cap stops it. What `max_composite_depth` then
  refuses is an *acyclic* chain nested deeper than the caller allowed, which is
  `ERR_LIMIT` - the answer `gfnt_glyf_load()` gives for the same question, and one
  a caller can act on by raising the budget. `glyf`'s composite reader still names
  only the direct cycle and leaves the indirect one to its depth cap; the two
  should agree and this is the half that is right.

**The strike list is one memo and each strike's index is another.** `uming.ttc`
- four of the six faces in Debian that carry this table at all - has six strikes
of 27,123 glyphs with 2,305 index subtables each, so parsing the indexes to open
a face would cost 55,000 subtable headers before a caller asked for anything, and
§5.3 promises that opening a 30 MB collection to ask for one glyph costs one
glyph. `EBLC`'s header and its `bitmapSizeTable`s are therefore parsed together
and a strike's index when a glyph from that strike is first wanted.

**`indexTablesSize` is not to be trusted and the table's own extent is.** A
directory entry whose extent leaves the file fails the *load*, so every table a
parser is handed is inside the blob; a length stated *inside* `EBLC` has no such
guarantee, and `mona.ttf` - one of the 33 files in Debian with this table -
states three that overlap their neighbours by eight bytes each. fontTools refuses
the font for it and every renderer displays it, because no renderer reads the
field. So a subtable is bounded by the table and by what its own format needs.

**And the file itself is four bytes short of what that subtable needs**, which
is the defect underneath the overstated length rather than a second one.
`mona.ttf`'s last strike puts its format-1 `sbitOffsets` at 57,992 of an 86,900
byte `EBLC`; an array for glyphs 0..7,224 needs 28,912 bytes and 28,908 are
there, so the **sentinel entry that bounds the last glyph is missing**. The two
earlier strikes do have those four bytes, and what is in them is the next
strike's `indexSubTableArray` header - a number that reads as an offset of
7,224 where the previous entry is larger. So this library refuses glyph 7,224 of
each of the three strikes and no other glyph of any of them, for *two* different
stated reasons - "offsets run backwards" in strikes 0 and 1 and "past the end of
the table" in strike 2 - and answers for 7,224 of 7,225 glyphs three times over.
That is M11 earning its place: the alternative to a per-glyph refusal is
declining three strikes of a font that renders everywhere, which is what the
reference does.
`CBLC`/`CBDT` formats 17-19 carry PNG, and `sbix` carries PNG, JPEG or TIFF
per strike with `dupe` records; both come back as `GFNT_GLYPH_BITMAP_PNG` with
the bytes, and the raster bridge decodes them through `image` when it is
present and returns `ERR_UNSUPPORTED` when it is not. PCF (all the table
types, both byte orders, the `fcp` magic), BDF (the text grammar, `ENCODING`
`-1` glyphs, `SWIDTH`/`DWIDTH`), PSF 1 and 2 with their Unicode tables, and
`.hex`.

### 7.6 Colour

`COLR` v0: base glyph records to layer records, each a glyph id and a palette
index, drawn as stacked outlines. `CPAL`: palettes, palette types, the
usability flags, colour records to RGBA. `COLR` v1: the paint graph - solid,
linear, radial and sweep gradients with their extend modes, `PaintGlyph` clip
paths, `PaintColrGlyph` references, the transform paints, `PaintComposite`
with its modes, and the variable forms through the item variation store -
returned as a graph the caller's compositor walks, with §6.2's depth. This
library does not composite gradients: that is a graphics library's job, and
`GFNT_GLYPH_COLR_LAYERS` is the description, not the pixels.

### 7.7 Variations

`fvar` (axes with tag, range, flags, name; named instances), `avar` version 1
segment maps (version 2 is `ERR_UNSUPPORTED` until asked for), `gvar` (the
tuple variation store, shared point numbers, packed deltas, and IUP
interpolation of unreferenced points), `HVAR`/`VVAR` (advance, side bearing
and top-side-bearing deltas through the item variation store and delta-set
index maps), `MVAR` (the metric tags), `STAT` (axis values formats 1-4, for
naming an instance), and `GSUB`/`GPOS` `FeatureVariations`.
Normalisation: user coordinate to `-1..1` by the axis's min/default/max, then
`avar`. Every accessor takes a `GFNT_Variation *`. Nothing reads a variation
table yet, so the parameter selects the default instance.

### 7.8 Required tables are per operation

A font embedded in a PDF is a subset with, typically, `glyf`, `loca`, `head`,
`hhea`, `hmtx` and `maxp` and nothing else - no `cmap`, no `name`, no `OS/2`,
no `post`. The specification's "required tables" list describes a font a
foundry ships, not one a reader meets. So **face load requires only the table
directory**, and each operation requires what *it* needs: glyph access needs
`glyf`+`loca` or `CFF `; codepoint lookup needs `cmap`; family name needs
`name`; each is `ERR_UNSUPPORTED` (the font has no such table) rather than a
load failure. Validation is lazy and memoised per table.

Per-glyph corruption is per-glyph (M11): a face with one bad `loca` entry
loads, and that glyph returns `ERR_CORRUPT` from `gfnt_glyph_load()`. A
`GFNT_CorruptGlyphPolicy` on the face - `REPORT` (zero: the error), or `NOTDEF`
(substitute glyph 0 and set a flag) - is the caller's choice, and the
diagnostic is recorded either way.

---

## 8. Rasterisation

### 8.1 The scan converter

A cell-based exact-area coverage rasteriser in the lineage of libart and
FreeType's `smooth` module: each edge, after flattening, deposits signed
`cover` and `area` into the cells it crosses; a sweep per scanline accumulates
winding coverage across cells and applies the fill rule - non-zero by default,
even-odd by option - with saturation, producing 8-bit coverage per pixel. This
handles self-overlapping contours (CJK composites, badly drawn fonts) the way
every reference implementation does, computes exact area rather than
supersampling, and is O(edges + touched cells).

All of it in 26.6. Quadratics (from `glyf`) and cubics (from charstrings and
`COLR` v1) are flattened by adaptive subdivision to a tolerance in 26.6 units.
No `float` anywhere in the path from outline to pixel, which is §1's
determinism promise, and §14.4 is the gate that keeps it.

### 8.2 `GFNT_Coverage`

Rows of 8-bit linear coverage with a stride, a width and height, the integer
origin of the top-left pixel relative to the glyph origin, and the 26.6
sub-pixel offset the glyph was rendered at. **Coverage is linear alpha**;
blending it in the right colour space is the compositor's business, and a
gamma lookup helper is provided for compositors that want one. When `image` is
present, `gfnt_raster_to_gimg()` writes into a `GIMG_CHANNEL_GRAY` 8-bit
`GIMG_Raster`; a 1-bit strike is expanded to 8-bit on the way, since `image`
has no 1-bit depth.

### 8.3 Sub-pixel positioning

The render call takes a 26.6 origin, so a glyph can be rasterised at any
fractional x (and y) offset. A GPU atlas typically bins to thirds or quarters
of a pixel and caches per bin; that policy is the atlas's, and this API makes
any binning expressible. Vertical sub-pixel offsets are supported and usually
unwanted.

### 8.4 What is not here

No LCD/ClearType filtering (a three-channel coverage output would be a later
option), no stem darkening, no dropout control (which only matters for
un-anti-aliased rendering of hinted outlines, which is absent), no embolden or
oblique synthesis in the first release - synthetic styles are a fallback
concern and belong with font discovery when it wants them.

### 8.5 Hinting is absent, and stated absent

There is no TrueType instruction interpreter and no autohinter. The `gasp`
table is read and its per-range flags are exposed as advice. This is decision
§17.2 and M3: the interpreter is the largest single file in any library that
has one, a bytecode VM whose only observable output is pixel coverage, and a
correctness problem whose only oracle is a differential against FreeType's own
interpreter. Unhinted rendering with exact-area anti-aliasing at ordinary
sizes is what every browser and every modern toolkit ships. The `gasp`
advice, `ROUND_XY_TO_GRID` and the `cvt`/`fpgm`/`prep` tables are parsed for
completeness and ignored.

---

## 9. Shaping

### 9.1 The engine

An OpenType Layout engine: `GDEF` (glyph classes, mark attachment classes,
mark glyph sets, ligature carets, and its item variation store), `GSUB`
lookup types 1-8 (single, multiple, alternate, ligature, contextual, chaining
contextual, extension, reverse chaining single), `GPOS` lookup types 1-9
(single and pair adjustment with `ValueRecord`s and device/variation tables,
cursive attachment, mark-to-base, mark-to-ligature, mark-to-mark, contextual,
chaining, extension), coverage formats 1-2, class definitions 1-2, the
script/language-system/feature/lookup hierarchy, `FeatureVariations` for
variable fonts, and `BASE` for baseline alignment across scripts. `JSTF` is
parsed and exposed for the justifier (§10.6) and otherwise unused.

The applier walks a `GFNT_Buffer` of glyph infos - glyph id, cluster, mask,
the shaper's per-glyph scratch - applying the selected lookups in the order the
font's `LookupList` gives, with §6.2's nesting and per-position budgets.

### 9.2 The pipeline and the script shapers

`unicode` supplies the itemisation (script runs with `Script_Extensions`,
bidi levels) and the properties; the shaping pipeline for one run is:

1. **Normalise** for the font: NFD, then recompose where the font has the
   composite and decompose where it has the parts, per `cmap` - the
   HarfBuzz approach, because a font's coverage is the only correct
   normalisation target.
2. **Map** codepoints to glyphs through `cmap`, variation selectors first.
3. **Script-specific preprocessing**: the shaper vtable's `preprocess` -
   Arabic joining classes to `init`/`medi`/`fina`/`isol` masks; Indic
   syllable analysis and reordering; Thai/Lao mark handling; Hangul jamo
   composition; the Universal Shaping Engine's cluster model for everything
   else complex.
4. **`GSUB`** with the default features for the script plus the caller's.
5. **Script-specific postprocessing** (Indic final reordering, Arabic
   fallback shaping when the font has no `GSUB`).
6. **`GPOS`**; then `kern` if there was no `GPOS` kerning (§9.4).
7. **Fallback mark positioning** when the font has no mark features, from
   `unicode`'s combining classes and the glyph bounds - because Latin text
   with a stray combining mark should not render the mark on top of the
   base.

The shapers are a vtable per script, `GFNT_Shaper { preprocess, features,
postprocess }`, with a default shaper for every script not listed. **Complex
shapers land one at a time and are declared** (§18): a script with no shaper
uses the default and reports so in the run; there is no half-implemented Indic
shaper that silently produces wrong output.

### 9.3 `GFNT_ShapedRun` and the cluster map

The output of shaping one run: parallel arrays of glyph id, `x_advance`,
`y_advance`, `x_offset`, `y_offset` in 26.6 at the requested scale, **the
cluster of every glyph** (the byte offset in the source text of the cluster
it belongs to), the face that answered each glyph (§10.5), flags (unsafe to
break before, unsafe to concatenate, per HarfBuzz's definitions), and the
script, direction and language the run was shaped with.

Cluster semantics follow HarfBuzz's *monotone graphemes* level by default:
clusters are non-decreasing in logical order, a grapheme cluster is never
split across clusters, and a ligature merges the clusters of its components.
This is the level a text editor and an accessibility tree need. The other
levels are options. M6 is why the cluster array is not optional.

Shaping takes a `GFNT_Scale` (x and y, `F16Dot16` units per font unit) so that
a caller wanting unscaled advances (PDF's 1000/em widths) passes the identity
at `unitsPerEm` and a caller wanting pixels passes `ppem * 64 / upem`.

### 9.4 Kerning

`GPOS` pair adjustment is kerning. The `kern` table is consulted only when the
font has no `GPOS` `kern` feature for the script, never in addition (M16), and
its format 2 (Apple's class-based) is read as well as format 0.

### 9.5 Language

Shaping takes a BCP 47 tag. The vendored **OpenType language-system registry**
(the specification's table of ~500 tags, with the ISO 639 mappings) resolves
it to an OpenType script and language system, which selects `locl` and the
language-specific feature set - the mechanism behind M19. An unresolved tag
falls back to the script's default language system and the run says so.
`likelySubtags`, which would resolve `zh` to `zh-Hans`, is CLDR and is not
here; the caller supplies a full tag or accepts the default.

---

## 10. Layout

Text layout is in this library up to the paragraph and not past it. A
constraint-based layout system needs **min-content and max-content** widths
from every piece of text, both of which require break analysis over shaped
runs, so either this library exposes them or the GUI toolkit grows a second
shaper. And the cluster map does not survive a line boundary someone else
drew.

### 10.1 What is here and what is `cjelly`'s

| # | Thing | Where |
| --- | --- | --- |
| 1 | Shaping | `font` shaping, not implemented |
| 2 | Run measurement: advance sum, ink extents | `font` shaping, not implemented |
| 3 | Itemisation: by script, direction, style span, and by which face has the glyph | `font` layout, not implemented |
| 4 | Break opportunities: UAX #14 lines, UAX #29 graphemes and words | `unicode`, applied here |
| 5 | Paragraph layout: wrap, stack, justify, align, truncate, tabs | `font` layout, not implemented |
| 6 | Inline layout: inline images and widgets, mixed sizes on one line, ruby | `cjelly` |
| 7 | Box model: margins, padding, floats, flex, grid, constraints | `cjelly` (its Task 3.1) |

### 10.2 The promises

- **Two boxes, always.** The *typographic* box (line count × line height,
  width from advances) answers "how much room do I reserve"; the *ink* box
  (the union of positioned glyph bounds) answers "which pixels get touched".
  They differ for nearly every string. Both are returned, both are named.
- **Measurement without rasterisation.** Layout never touches the scan
  converter. Ink bounds come from outline bounds and strike metrics.
- **Measure and paint are one code path** (M5): the positions layout hands
  the painter are the positions it measured with, in 26.6, and any rounding
  is a `GFNT_AdvanceRounding` policy applied in layout so the painter sees
  rounded positions.
- **Shape once, break many.** A `GFNT_ShapedRun` outlives the line breaker; a
  paragraph re-broken at a new width re-uses its runs and re-shapes only
  where a break fell inside a cluster that the `unsafe_to_break` flag says
  cannot be split - the HarfBuzz protocol, which is what makes window resizing
  smooth.

### 10.3 Line metrics

`GFNT_LineMetrics`:

```c
typedef enum {
  GFNT_LINE_METRICS_DEFAULT = 0, /* OS/2 typo metrics if fsSelection bit 7,
                                    else hhea; the font's own request      */
  GFNT_LINE_METRICS_HHEA,
  GFNT_LINE_METRICS_TYPO,        /* OS/2 sTypoAscender/Descender/LineGap    */
  GFNT_LINE_METRICS_WIN          /* OS/2 usWinAscent/usWinDescent, gap 0    */
} GFNT_LineMetrics;
```

M4. The zero value is the one the font asked for, which is the only default
that is not "whichever platform was written first". A paragraph with runs from
several faces takes the maximum ascent and descent across them per line,
which is CSS's rule and the one users expect.

### 10.4 The paragraph

`GFNT_Paragraph` is built from UTF-8 text plus a list of style spans (face,
size, variation, features, language, letter-spacing) and paragraph properties
(base direction or `AUTO`, alignment, justification, tab stops, line-break
tailoring per `unicode`'s `GUNI_LineBreakTailoring`, the `GUNI_BreakProvider`
for `SA` scripts, a hyphenation provider). It is itemised and shaped once.
`gfnt_paragraph_layout(width)` produces `GFNT_Line`s - each a list of
positioned run slices with its own metrics, both boxes, and its logical range.
`gfnt_paragraph_min_content()` and `_max_content()` answer the constraint
system without producing lines.

Justification is space expansion in the first release, with `JSTF` and
kashida (Arabic) as a later option; alignment is start/end/left/right/center
with bidi-aware start/end; truncation is an ellipsis string shaped in the
paragraph's own style; tabs are stops or a default interval.

### 10.5 Itemisation and fallback

A paragraph is split by style span, then by script (`unicode`'s script runs,
with `Common` and `Inherited` merged into their neighbours), then by bidi level
(`unicode`'s UAX #9), then by **face**: for each glyph the span's face cannot
map, the `GFNT_FontProvider` is asked for one that can, given the codepoint,
script, language and style, and the run is split at face boundaries. The
provider is a vtable the caller supplies; discovery, when it exists, ships one over a `GFNT_FontSet`
(§11), and a caller with its own policy supplies its own. **Every glyph records
its face**, and a run whose face is not the span's is flagged - M8 - so that
"why did that glyph look wrong" has an answer.

### 10.6 Hit testing

Both directions, which is what the cluster map is for: a logical byte range to
a **list** of rectangles (several, once bidi puts one logical range in two
visual places); and a point to a byte offset plus a caret affinity (which side
of a direction boundary the caret belongs on). Caret positions inside a
ligature use `GDEF`'s ligature caret list when the font has one and even
division when it does not.

### 10.7 Deliberately not here

Hyphenation *data* (Liang patterns are a dictionary per language, each with its
own licence; `GFNT_Hyphenator` is the seam), dictionary word breaking for the
`SA` scripts (`unicode`'s seam, passed through; without a provider Thai does
not wrap, and the header says so), Knuth-Plass optimal breaking (greedy first;
an option later), vertical writing modes (`vhea`/`vmtx` *metrics* are read;
vertical *layout* is not), ruby, and anything that must know about a non-text
box sharing its line.

---

## 11. Discovery and fallback

Not implemented. The only part that touches the operating system, kept to one file per
platform. `GFNT_FontSet` is built by scanning directories - the caller's list,
or the platform's conventional ones: `$XDG_DATA_DIRS/fonts`,
`~/.local/share/fonts`, `~/.fonts`, `/usr/share/fonts` and its `X11` bitmap
trees with their `fonts.dir`/`fonts.alias` on Linux; `%WINDIR%\Fonts` and the
per-user fonts directory on Windows; the three `Library/Fonts` on macOS - and
reading each file's `name` and `OS/2` (or its BDF/PCF properties) into a
record: family, style, weight, width, slant, the Unicode ranges it claims, the
format, the path and index. The set is an object the caller owns and can
serialise, so that a scan of 2,000 files happens once.

**fontconfig is not linked and its configuration is not read**: its
configuration is XML, the suite has no XML parser, and `cjelly`'s stated intent
is to avoid external libraries. The directories fontconfig would scan are
scanned; its aliases and substitution rules are not applied. On Windows, the
registry's font list and DirectWrite's system collection are the sources, and
on macOS CoreText's; each is `TODO(windows)` / `TODO(macos)` per
`CONVENTIONS.md` §11 until run there.

**Matching** follows CSS Fonts level 4's algorithm: a family list is walked in
order; within a family, width, then style, then weight, by the specification's
distance rules; a face is selected, not synthesised. The default
`GFNT_FontProvider` over a set answers fallback by Unicode-range claim, then by
actual `cmap` coverage, with a per-script preference list the caller can set.
Synthetic bold and oblique are §16.

---

## 12. Writing and subsetting

The library writes fonts because two consumers need it: a PDF writer must
embed a *subset* of each font it uses (§13.3), and every fixture in §14.5 is
produced by subsetting. Writing is not implemented. It needs the reader and nothing else.

### 12.1 The sfnt serialiser

From a face's parsed tables - or from a `GFNT_FaceBuilder` holding new ones -
to bytes: tables sorted by tag, each padded to four bytes, the directory with
correct `searchRange`/`entrySelector`/`rangeShift`, per-table checksums, and
`head.checkSumAdjustment` computed last over the whole file (M20). `loca` is
regenerated from `glyf` in the short or long format the offsets require;
`maxp`, `hhea.numberOfHMetrics`, `post` format 2 or 3, `cmap` formats 4 and
12, `name`, `OS/2` and `hmtx` are all writable. Collections are written with
shared tables de-duplicated by content.

### 12.2 The `CFF` writer

INDEX structures, DICTs with the offset-size fixups their self-references
require, charsets, `FDArray`/`FDSelect` for CID output, and charstrings copied
verbatim or with subroutines **flattened** (the simple, always-correct choice
for a subset; re-subroutinisation is an optimisation for a later release).

### 12.3 The subsetter

Input: a face, a set of glyph ids or codepoints, and options. Output: a new
face. The glyph set is **closed** over composite components always, and over
`GSUB` reachability by option (default on: a subset that drops the `fi`
ligature glyph is M21). Glyph ids are either **renumbered** compactly or
**retained** (`retain_gids`), the latter being what a PDF `CIDFontType2` with
an identity `CIDToGIDMap` needs. `cmap` is rebuilt for the retained
codepoints or dropped; `name` is rewritten with a new family name and the
licence-required rename (§14.5); `hmtx`, `loca`, `glyf`, `CFF `, `post` and the
layout tables are pruned to the closure. Layout-table subsetting (pruning
lookups to the surviving glyphs while keeping them valid) is the hard part
and is not implemented.

### 12.4 Other writers

WOFF 1 (per-table zlib through `compress`, with the metadata block). BDF from
a bitmap face, so that a fixture can be regenerated from a font this library
subsetted. PCF and PSF writing are tools, not library (`bdftopcf` exists), and
WOFF 2 waits on Brotli.

### 12.5 The gate

Every writer is proven by reading its output back three ways: by this
library (the round trip must reproduce every table's parsed form), by
fontTools (`ttx` round-trip byte-comparison of the tables that have a
canonical form), and by FreeType (every glyph loads and renders to the same
coverage as from the original face). The parse-write-parse gate over the
whole corpus is what scores the writer for free, and a subsetter differential
against `pyftsubset` scores the closure.

---

## 13. The consumers, and what each one needs

### 13.1 `cjelly`

The reader, outlines, shaping, layout and discovery, and, for its GPU path, an atlas. `cjelly` owns the atlas and the
draw batching; this library gives it `GFNT_Coverage` at any sub-pixel offset,
the shaped runs with cluster maps its accessibility tree needs
(`semantics.md`), the paragraph layout with both boxes and hit testing its
widgets need, min-content and max-content for its constraint solver (Task
3.1), and `GFNT_FontSet` for its font manager. The atlas *format* - glyph key
(face, glyph, ppem, sub-pixel bin, variation) to rectangle plus metrics - is
defined here as `GFNT_AtlasEntry` so that a build step can bake one and
`cjelly` can load it, and so that the same key serves as the glyph cache key.
`cjelly`'s v0.1 "text (Latin fallback)" milestone needs the reader and
rasterisation, and nothing else.

### 13.2 `image`

The reader, rasterisation and layout: `gimg_draw_text()` is a paragraph laid out here and
rasterised into a `GIMG_Raster` through the bridge in §8.2, with the
compositing (colour, blending, effects) on `image`'s side. `image` never links
font discovery; the caller hands it a face.

### 13.3 A PDF library, should one come

Designed for now because its needs are concrete and cheap to keep open:

| PDF needs | Here |
| --- | --- |
| `FontFile` (Type 1), `FontFile2` (TrueType), `FontFile3` (`Type1C`, `CIDFontType0C`, `OpenType`) | §7.1, §7.4: Type 1, sfnt, bare CFF including CID-keyed |
| Fonts with no `cmap`, `name` or `OS/2` | §7.8: per-operation requirements |
| Glyph names → Unicode for text extraction | `post` names, the **Adobe Glyph List** (vendored, BSD-3), the `uniXXXX`/`uXXXX[XX]` conventions, `gXX`/`cidXX` forms |
| The standard 14 fonts' metrics without their files | AFM reading (§7.1); the Core 14 AFMs are a fixture the PDF library carries |
| `StandardEncoding`, `WinAnsiEncoding`, `MacRomanEncoding`, `MacExpertEncoding`, the Symbol and ZapfDingbats built-in encodings | vendored tables in `write.h`'s PDF helpers; not implemented |
| Embedding a subset with `CIDToGIDMap` identity, or renumbered with a `Differences` array | §12.3's `retain_gids` and its inverse |
| A `ToUnicode` CMap and a `W`/`Widths` array for the subset | not implemented; they would be built from the subset's cluster and advance data |
| Predefined CJK CMaps (`Adobe-Japan1-6`, ...) | **not here**: they are a large separate data set (`poppler-data` on Debian) and a provider seam; `Identity-H`/`V` are built in |
| Type 3 fonts | **not here**: their glyphs are PDF content streams, which is the PDF library's interpreter |
| `fsType` embedding permissions | reported, never enforced (§17.9); the PDF library decides |

---

## 14. Correctness: oracles and tests

The principle from `regex`'s `testing.md` and `chron` §12: **the oracle is the
authority, and every vector is generated from one, never written from
memory.** Fonts have three excellent oracles and one of them can also
*manufacture* test inputs.

| Claim | Oracle | Driver | Image |
| --- | --- | --- | --- |
| every table parses to the same fields | **fontTools** `ttx`: every table of every synthetic fixture and every real font in the image dumped to XML and compared field by field with this library's `_dump` | `tools/oracle/ttx_diff.py`, `make check-oracle-ttx` | `fonttools`, built here: `python` by digest + `fonttools==` and `brotli==` exact, plus Debian's font packages by full apt version; the **same image builds the synthetic fixtures** (§14.5) |
| `cmap` maps every codepoint identically | fontTools `getBestCmap()` and every subtable, over all 1,114,112 codepoints per font | `tools/oracle/cmap_diff.py` | `fonttools` |
| outlines are identical | **FreeType** `FT_Load_Glyph` with `FT_LOAD_NO_HINTING \| FT_LOAD_NO_SCALE`, every glyph of every corpus font, point by point | `tools/oracle/ft_outline.c` | `freetype`, built here: the driver links only FreeType and is compiled inside its image; apt version pinned in full |
| coverage is close | FreeType `FT_Render_Glyph` at several ppem, compared with a per-pixel tolerance (the two rasterisers are the same algorithm family and differ by rounding) | `tools/oracle/ft_raster.c` | `freetype` |
| coverage is **byte-identical across platforms** | this library on x86-64 versus this library in the cross container (big-endian, 32-bit) | golden hashes in `tests/data/golden/`, `make check-golden` | the `ghoti-xarch` cross image |
| shaping is identical | **HarfBuzz** `hb-shape --output-format=json`: glyph ids, advances, offsets and clusters for every string in a per-script corpus, per font | `tools/oracle/hb_diff.py`, `make check-oracle-hb` | `harfbuzz`, built here: `hb-shape` from the pinned apt package, driven by `--text-file` and `--output-format=json` (a batch protocol already) |
| subsetting keeps what it should | fontTools `pyftsubset` with matching options; the closure sets compared | `tools/oracle/subset_diff.py` | `fonttools` |
| written fonts are valid | read back by this library, fontTools and FreeType (§12.5) | `make check-writer` | `fonttools`, `freetype` |
| line breaking and bidi | `unicode`'s conformance gates, already passed there | - | none: `unicode`'s committed conformance files |
| paragraph layout | **Pango** `pango-view --output` positions for a paragraph corpus, with the line-breaking differences that come from Pango's ICU tailorings recorded as known | `tools/oracle/pango_diff.py` | `pango`, built here: `pango-view` from the pinned apt package, `C.UTF-8` pinned in the image |
| bitmap formats | **Pillow**'s `PcfFontFile` and `BdfFontFile`, every pixel of every glyph; a *second* reading of PCF and the only reading of PSF and `.hex` would need `bdftopcf` and `psftools` | `tools/oracle/bitmap_diff.py`, `make check-oracle-bitmap` | `fonttools` (Pillow is pinned in it); the `xfonts` image is not built |
| **embedded** bitmap strikes are read identically | **fontTools**' `EBLC`/`EBDT`: every strike's ppem both ways, depth and baseline, each strike's present/absent/corrupt glyph counts over *every* glyph, and every sampled glyph's box, bearings, advance and pixels | `tools/oracle/eblc_diff.py`, `make check-oracle-eblc` | `fonttools`; the image carries the **whole** Debian population of the table - 33 fonts, from a scan of all 589 font packages |
| a **composite** strike glyph is composed the same way, and `mona.ttf` is read at all | **FreeType**'s `ttsbit.c` through a driver compiled in its own image: the same line protocol, so the same comparison. The only second reader of a composite's pixels - fontTools parses a component list and does not compose one - and the only reader of `mona.ttf` besides this library | `tools/oracle/eblc_diff.py`, `make check-oracle-eblc` | `freetype`; carries no fonts of its own, reading the corpus `corpus.py` materialised for both |
| Type 1 | FreeType again, over fixtures converted from OFL fonts | `ft_outline.c` | `freetype` |

**None of the three principal oracles is installed on the development
machine today, and none needs to be** (§14.8). Every differential names its
image, prints the reference's version before its numbers, and counts its
skips, per `chron` §12.2; a CI with the images regenerates the vectors and
fails on a diff.

### 14.1 Properties

1. **Chunk and truncation invariance** (§14.3): a font truncated at any byte
   either loads with the same answers for every glyph whose data survived, or
   refuses with `ERR_CORRUPT` naming the table - never a third thing.
2. Parse → write → parse is the identity on every parsed field (§12.5).
3. Shaping is deterministic and independent of buffer boundaries: shaping a
   run is identical to shaping it as two runs split at a
   `safe_to_break` position and concatenating.
4. For every glyph, `outline_bounds ⊆ face_bbox` or a diagnostic; and
   `coverage.origin + coverage.size` lies within the outline bounds rounded
   out.
5. Layout: `min_content ≤ any laid-out width ≤ max_content`; every byte of the
   source text appears in exactly one line's logical range; hit-testing a
   returned rectangle's centre returns an offset inside its range.

### 14.2 Fuzzing

One harness per container and per table family, each with the options byte
driving `GFNT_Limits` and the policies: `fuzz_sfnt.cpp` (the directory and
every table parser through the face's `_dump`), `fuzz_glyf.cpp`,
`fuzz_cff.cpp` (charstrings and DICTs), `fuzz_type1.cpp`, `fuzz_cmap.cpp`,
`fuzz_bitmap.cpp` (PCF, BDF, PSF, `EBDT`), `fuzz_woff.cpp`, `fuzz_color.cpp`,
`fuzz_var.cpp`, `fuzz_layout_tables.cpp` (`GSUB`/`GPOS` applied to random glyph
strings), `fuzz_shape.cpp` (random text through the full pipeline against the
corpus fonts), `fuzz_raster.cpp` (random outlines), and `fuzz_writer.cpp`
(random subsets round-tripped). Seeded from the corpus; run under ASan and
UBSan; the writer fuzzer because a corpus carries only what the parser accepts
and the writer is how the parser meets what it does not.

### 14.3 Truncate each table, not the file

Cutting a font at its end shortens only its last table. The truncation sweep
therefore truncates *each table* to every length from 0 to its own, with the
directory intact, and asserts §14.1's first property for each - the lesson
this suite learned on a segmented image format applies to a font directly.

### 14.4 The golden-bitmap gate

Every glyph of every **fixture** that carries outlines of its own, at 8, 12, 16,
24, 48 and 96 ppem and at sub-pixel offsets 0, 1/4, 1/2 and 3/4 plus one
vertical, rasterised and hashed; the hashes committed to
`tests/data/golden/coverage.txt`. Two gates read that one file:

- `testGolden` re-renders on the build host. It needs no container, so it runs in
  `make test` on a fresh clone, and it catches an unintended change to the
  rasteriser.
- `make check-golden` cross-builds the library for s390x, powerpc64 and sparc64
  in the workspace's `ghoti-xarch` container and requires the same renderings
  byte for byte. A rendering that differs by platform is a host-dependent read,
  and this is the only gate that sees one.

Three things this says that an earlier draft of it did not.

**Fixtures rather than corpus fonts.** The corpus lives in the oracle image
(§14.5) and hashing it would tie the committed file to that pin and need a
container to regenerate - which makes a golden file nobody regenerates. The
fixtures are in the repository, so `make golden` needs nothing. Only the seven
with outlines of their own are committed; the rest draw `basic.ttf`'s five
shapes, and that exclusion is **probed** - each excluded fixture is rendered and
must introduce no rendering `basic.ttf` does not already have.

**What a platform difference implies, precisely.** Byte order, alignment and word
size are what this catches. It does *not* catch every `float`: two IEEE-754
targets rounding one expression agree. That no `float` is in the path at all is
kept by there being no floating-point type in these headers and by
`-Wfloat-conversion` on every translation unit.

**It cross-builds `font` alone**, with cutil's six entry points supplied by
`tools/golden/cross_shim.c`. Cross-building cutil would need cutil's generated
headers produced for the target, and a generated header produced for the wrong
target is exactly the class of defect this gate is looking for - it would make
the gate's answer depend on the thing under test. The shim's list is checked
against the library's undefined symbols, so a seventh dependency fails the gate
rather than quietly linking the host's cutil.

### 14.5 Fixtures are ours; real fonts stay in the oracle image

Every other library here commits fixtures freely, and a font library's first
instinct is to commit fonts. It is the wrong instinct: fonts are licensed,
large, and not needed in the repository at all, because the two jobs a corpus
does want different fonts.

- **Unit tests want fixtures that exercise a code path** - a format-2 `cmap`,
  a composite three deep, a `loca` running backwards, a `numGlyphs` that
  disagrees across tables, a `COLR` v1 graph with every paint format, an
  `sbix` strike, a variable font with three axes. No shipping font has most
  of these, and the ones that exist are megabytes. `tools/fixtures/
  make_fixtures.py` builds them with `fontTools.fontBuilder` from a handful of
  outlines drawn here, in the `fonttools` image so that the bytes are
  reproducible, and they are committed under `tests/data/fonts/`: a few
  kilobytes each, this library's copyright, LGPL like everything around them,
  **no third-party licence anywhere in the tree**. `make check-fixtures`
  regenerates and fails on a byte difference.
- **Conformance wants the population that is not ours** - Noto Sans
  Devanagari through this shaper against HarfBuzz, DejaVu through this
  rasteriser against FreeType, every font a stock system has through
  `ttx_diff`. Those fonts live **in the oracle image only**, installed from
  Debian's font packages (`fonts-dejavu-core`, `fonts-noto-core`,
  `fonts-noto-cjk`, `fonts-noto-color-emoji`, `xfonts-terminus`, and whatever
  else a differential names) pinned by full apt version as the container
  prototype pins pcre2. They are never copied into the repository, so no
  licence question arises in it, and `make test` never needs them: only the
  `check-oracle-*` targets do.

The lesson this suite learned on its own corpora - one grown from your fixes
measures the fixes - is what the second half is for, and it makes the
differentials mandatory rather than nice to have. Synthetic fixtures alone
would flatter; the real-font differentials are the outside population, and a
shaper that passes every fixture and fails `hb_diff` on Noto is exactly the
case this arrangement exists to catch. A CI without the oracle image is
running half the gates and must say so (§14.7).

Two cases fall between:

- **A real font's quirk that needs a regression test.** Reproduce it
  synthetically: the quirk is a byte pattern, and `fontBuilder` can emit any
  byte pattern. If it genuinely cannot be reduced, a subset of the real font
  may be committed - renamed per its licence's reserved-name clause, with the
  licence text beside it - as a decision recorded in the commit, never as the
  habit.
- **The golden-bitmap gate (§14.4)** measures this rasteriser's determinism,
  not any font; the synthetic fixtures serve it.

Hand-built byte arrays remain for the refusal arms that even `fontBuilder`
will not emit - a bad checksum, a directory entry past the blob.

**All of this exists today.** `tests/sfnt_builder.h` is the organised form of
those hand-built arrays: it assembles an sfnt, a `ttcf`, and each table this
library reads, so that a test can break exactly one field and name what it broke.
`tools/fixtures/make_fixtures.py` writes the generated half - eighteen fonts and
a `MANIFEST`, in the same pinned image as the differentials - and `check-fixtures`
regenerates and compares bytes, so a fixture is a thing that can be rebuilt
rather than a byte array with extra steps. The `fonttools` image and both
differentials are built (§18), so the real-font population is reachable too.

The generator's own contract is determinism, and two of its pins are not
obvious. `FontBuilder` writes the current time into `head` whatever its defaults
table says, and a `TTFont` re-opened from a file has `recalcTimestamp` on - so
the collection, the one fixture re-read before it is written, differed on every
run while every other fixture was stable. And `head.created` cannot be 0:
fontTools reads any timestamp below 0x7C259DC0 as a misencoded unix timestamp
and adds that constant, so a fixture dated 1904 comes back as 1970 and every
fixture disagrees on `created` for neither side's fault.

### 14.6 No font is bundled

The first draft compiled a subset face into the reader so that the library could
always draw a diagnostic string. It does not, and the reason is M24: a font
embedded in a library ships inside every consumer's binary, and its licence
notice - Vera's, OFL's - then has to travel with every binary every downstream
distributor of every consumer produces. A font library has no business making
that decision for its consumers.

What the library promises instead is narrower and needs no licence. **Glyph 0
is synthesised in code** (§5.4), so a face with no usable glyphs still draws a
hollow box per character and a run that no face could map still has geometry.
**Discovery enumerates the system's fonts** (§11), so an application that wants a
fallback finds one where the operating system keeps them. An application that
must draw text with no fonts on the system - a container, a fresh Windows
image - ships a font as its own asset under its own licence, which is where
that decision belongs; `cjelly` may well do so, and that is `cjelly`'s call.

### 14.7 The oracles run in containers

The pattern was prototyped on `regex`, measured, and shown to find defects
the host had hidden, and this library adopts it unchanged:
`tools/oracle/containers/IMAGES` pins every
reference; `oracle_env.py` is the one place a reference is spelled;
`oracle_run.py` resolves it, prints `oracle(container): <version>`, and fails
closed on a pin mismatch; `ORACLE_MODE` is `container` or `host` with no
silent fallback; the repository is mounted read-only at its own host path with
`--network none`; every driver speaks a batch protocol. `make test` needs no
container: it reads committed vectors and golden hashes, and the container
gates are the separate `check-oracle-*` and `vectors` targets, which is the
seam that note leaves open and this library takes.

**Built, 2026-09-24**, and one thing was learned by building it. A four-font
sample of the corpus reported a clean run against a planted defect that only
affects `OS/2` version 1, because the four fonts sampled were all version 4 -
the corpus holds 271 version 4, 48 version 3 and 8 version 1, and no version 0,
2 or 5 at all. So `ttx_diff` prints which table versions it met and says which
the corpus lacks. A differential that does not report its coverage is one whose
clean result cannot be read, and thinning a corpus for speed is how a gate stops
covering the arm the defect is in.

Four things are specific to font oracles:

- **The fixture generator runs in the `fonttools` image, and that is what
  makes `check-fixtures` mean anything.** A built fixture's bytes depend on
  the fontTools version that produced them; a host upgrade would fail the
  byte-identity check for no reason, and a pinned image cannot drift. The
  same image answers `ttx_diff`, `cmap_diff` and `subset_diff`, so the
  fixture and the oracle agree on what a table *is*.
- **The FreeType drivers build inside their image** against FreeType alone, as
  `regex`'s `pcre2_match` builds against pcre2 alone, so the reference cannot
  reach the implementation it answers for. FreeType has no CLI that emits
  outlines; the driver is forty lines of `FT_Outline` walking that reads glyph
  ids from stdin and writes points to stdout.
- **`hb-shape` is already a batch tool** (`--text-file`, `--output-format=json`)
  and needs no driver; the image pins `harfbuzz-utils` by full apt version, as
  the prototype pins pcre2, so that the archive moving past it fails the build
  rather than silently reporting a newer HarfBuzz's answers as a regression.
- **Every built-here image pins `C.UTF-8`.** An unset `LANG` produced fifty
  false disagreements in the vim oracle; `pango-view` and
  the Python tools are exposed the same way, and the image is where the
  locale is written down.

The real fonts live only in the oracle image, installed from Debian's
packages; the synthetic fixtures live only in the repository, mounted
read-only at the same path on both sides. Neither crosses, and no licence
question arises in the repository (§14.5).

**Which fonts a differential covers is a list in the image, not a glob in the
driver**, so that "what did that run read" has one answer and adding a package is
one edit. There are three: `fonttools-corpus` is every sfnt,
`fonttools-corpus-bitmap` is the PCF strikes (234), and `fonttools-corpus-ebdt`
is the fonts with embedded bitmap strikes (33 - the whole Debian population).
Three things the third one does that the other two do not, and each was paid for:

- **It reads a table directory with `struct` and opens nothing.** A population
  chosen by the reader under test lets a reader that lost a table compare nothing
  and pass; a population chosen by the *reference* drops `mona.ttf`, the one font
  where the two disagree about whether the font is readable at all. Neither
  reader gets to pick.
- **It resolves symlinks and deduplicates.** Debian's alternatives put
  `fonts-japanese-gothic.ttf` beside `mona.ttf` as a link to it, so a list keyed
  on paths reports two fonts where there is one. `find -type f` already drops a
  link; the dedupe is what says so rather than assuming it.
- **It does not select by file extension, which cost it five fonts.** It matched
  `*.ttf`, `*.otf`, `*.ttc` and `*.otc`, and Debian's bitmap-only sfnts are named
  **`.otb`** - so the one shape in this population that is *nothing but* embedded
  strikes was the one shape the list could not see - and those five turn out to be
  the only faces from which FreeType reports a strike's own baseline. The same mistake in the same
  spelling is what made an earlier survey report two fonts where there are 33. An sfnt says what it is in its first four bytes; the
  extension decides nothing, and every regular file under the roots is now offered
  to a magic check instead.

  The other two lists still select by extension, and `fonttools-corpus` therefore
  still cannot see an `.otb`. That is a known hole rather than a decision: fixing
  it moves the denominator of four other differentials, so it belongs in a commit
  that re-runs them.

The same program is what selects the *fixtures* for that differential, so a new
strike fixture joins it by existing rather than by somebody remembering a second
list.

### 14.8 Generated vectors, and which gate covers what

Three vectors are generated from the pinned image and committed:
`src/tables/post_names.h` (the 258-entry standard Macintosh glyph order),
`src/name/mac_encodings.h` (the eight single-byte Macintosh encodings and the
`(platEncID, langID)` rules that choose between them), and
`src/cff/cff_strings.h` (the 391 CFF standard strings, the Standard Encoding, and
the two predefined charsets - §7.4). The generator is
`tools/vectors/make_vectors.py`; `make gen-vectors` installs, `make check-vectors`
verifies.

The CFF tables are cross-checked the way Mac Roman is, and across a boundary
that makes it mean something: fontTools keeps the strings in `cffLib` and the
Standard Encoding in `fontTools.encodings`, and the specification says codes 32 to
126 are SIDs 1 to 95. The generator requires that relation, requires every encoded
name to be one of the 391, and requires `cffISOAdobeStrings` - a second
transcription of the first 229 - to be a prefix of them. A transcription error on
either side breaks one of the three.

**Three gates, and the split is the design** - the same split `unicode` uses for
its UCD tables, and for the same reason:

| gate | finds | needs |
| --- | --- | --- |
| `check-vectors` | a table edited by hand, or a generator changed without regenerating | the image |
| `testVectors` | the same, on a fresh clone | nothing; runs in `make test` |
| `check-oracle-ttx` | a table **wrong about reality**, over 362 faces | the image |
| `check-oracle-cff` | the CFF tables wrong about reality: every glyph's name comes from the charset through the standard strings, and every accented character through the Standard Encoding | the image |

Only the third can find the generator wrong; only the first two can find a
table edited. `testVectors` is what makes the container gates' honesty free: they
can fail rather than skip when the image is absent, because something still
covers the tables without it. It reaches every entry through the public API
rather than by including the generated header - `post-v1.ttf` *is* the standard
order, so naming its 258 glyphs walks the whole vector, and
`name-mac-encodings.ttf` carries all 128 high bytes of each encoding.

The CFF tables are the exception, and the reason is worth stating rather than
working around: there is no public call that walks 391 strings or 256 encoding
codes, and a fixture naming every one of them would be a font built for one test.
So `testVectors` checks those two against the header directly and `test_cff.cpp`
covers their *use* through the public API - a font whose glyph names come out of
the standard strings and whose accented character is resolved through the Standard
Encoding.

**One source is not enough for a transcription, so Mac Roman is cross-checked.**
The table is generated from CPython's codecs, because that is what fontTools'
`name` decoding uses and therefore what a clean differential *means*; on its own
that is circular. So the generator also reads glibc's `MACINTOSH` charmap through
`iconv` - an independent transcription that happens to be in the same image - and
requires the disagreements to be exactly two, each with its reason recorded:

- **0xC6**: glibc U+0394 GREEK CAPITAL DELTA, CPython U+2206 INCREMENT. glibc is
  internally inconsistent here, giving 0xB7 as U+2211 N-ARY SUMMATION rather than
  Greek Sigma, so it splits a pair Apple's mapping keeps together.
- **0xF0**: the Apple logo, private use either way - U+F8FF (Apple's registered
  corporate-use codepoint) against glibc's U+E01E.

A third byte disagreeing fails the generator, and so does one of these two
*ceasing* to disagree: a category that excuses nothing is a category that stops
being read. The first attempt at this cross-check used fontTools' own
`encodings.MacRoman` through the Adobe Glyph List and reported 39 disagreements -
because that table is a glyph *order*, not the encoding, and fills the control
range with letters. Two tables under one name answering different questions is
not a second source.

### 14.9 The gates are themselves tested

Every gate above has been observed to fail before it is trusted: a byte
flipped in a corpus font fails `ttx_diff`; a point moved in the outline
producer fails `ft_outline`; an edge deposited with the wrong sign fails the
golden hashes; a lookup skipped fails `hb_diff` on the string that needed it;
a glyph dropped from the closure fails `subset_diff`. §12.3 of `chron`'s design
and this suite's history say why.

`make coverage` is one of them now: it fails below a line-coverage floor, and
fails again if it measured fewer executable lines than the library has. The
second is the control, because every other number it prints is a ratio and a
ratio is fine about a report that collapsed - a sweep that measured one file
would print 100% and clear any floor. Both were watched failing before being
committed green, which is the rule here and was worth applying to the instrument
as well as to the gates: this one was found reporting an unreachable line as
executed (documentation/development.md).

---

## 15. Code layout

```
include/ghoti.io/font/
  macros.h  libver.h  libver_gen.h  namespace.h  allocator.h     (CONVENTIONS §4)
  core.h  blob.h  face.h  metrics.h  cmap.h  name.h  glyph.h
  bitmap.h  color.h  charstring.h
  outline.h  raster.h
  shape.h
  layout.h
  discover.h
  write.h
  font.h                                                         umbrella for what is implemented
src/
  core/ reader/ blob/ sfnt/ woff/ tables/ cmap/ name/ glyf/ cff/ type1/
  charstring/ bitmap/ color/ var/ outline/ raster/ layout_tables/ shape/
  shapers/ layout/ discover/ write/ data/   (the language registry, the AGL,
                                             the encodings, the fallback face)
tools/
  fixtures/make_fixtures.py   oracle/{ttx_diff,cmap_diff,hb_diff,subset_diff,
  pango_diff}.py  oracle/{ft_outline,ft_raster}.c  bench/
tests/
  unit/  conformance/  fuzz/  data/fonts/<family>/  data/golden/  data/built/
```

### 15.1 Allocation

Everything the library allocates goes through `GFNT_Allocator`, a typedef of
`GCU_Allocator`. Faces, outlines, coverage, shaped runs and paragraphs are
`_create`/`_destroy` or `_load`/`_free` per `CONVENTIONS.md` §3. Property-like
queries - a glyph's advance, a name record - allocate nothing.

### 15.2 Limits

`GFNT_Limits` caps: `max_blob_bytes` (default 256 MiB), `max_tables` (512),
`max_glyphs` (65,535, the format's own), `max_composite_depth` (16),
`max_charstring_depth` (10, the Type 2 format's own), `max_charstring_ops`
(65,536 per glyph), `max_outline_points` (65,536 per glyph), `max_contours`
(4,096), `max_ppem` (4,096), `max_raster_bytes` (64 MiB per glyph),
`max_strikes` (256), `max_name_records` (4,096), `max_axes` (64),
`max_lookup_depth` (6), `max_ops_per_glyph` (64), `max_paint_depth` (64),
`max_run_bytes` (16 MiB), `max_line_length` for BDF and `.hex` (4,096). Every
parser takes one; `NULL` means default.

The two charstring caps are separate from `max_composite_depth` because they are
different recursions in different formats, and the second is separate from the
first because **depth alone does not bound the work**: Type 2 has no jump and no
loop, so a program's length bounds its own straight-line work - but a subroutine
may call two subroutines, each of which may call two more, and ten levels of that
is a thousandfold expansion from a few hundred bytes. This is what makes the fuzzers meaningful: a limit is a stated
promise and the options byte drives every one of them.

### 15.3 Threads and state

A face is immutable after load and its lazily validated tables are memoised
under a `cutil` mutex, so one face may be shared read-only across threads.
**The lock is never held across a parse**: one table's parse legitimately needs
another's - the `numGlyphs` minimum has to read `hhea` and `hmtx` to know what
they imply - and a non-recursive mutex held across that deadlocks against
itself on the first font that has both tables, on one thread and not only under
contention. So a parse runs into caller-supplied scratch storage and only the
publication takes the lock; two threads may therefore parse the same table at
once, reach the same answer from the same immutable bytes, and the first to
finish publishes.

**The loser has to free what it built.** A memo whose type owns memory - `EBLC`'s
strike list, and every one that follows it - leaves that memory in the scratch of
whichever thread did not publish, and dropping it leaks for the life of the face.
So `gfnt_table_cached()` takes a release hook, required of any such memo and NULL
only for the ones that are plain data or offsets into the blob. It went unnoticed
until `EBLC` because every allocating memo before it is parsed during the *load*,
where there is one thread and no race to lose.
Every other object - outline, coverage, buffer, shaped run, paragraph, font
set, cache - is used from one thread at a time. There is no process-wide
state: no default face, no global cache, no environment read outside
`discover.h`. Caches (glyph coverage, shaped runs) are objects the caller
creates, keyed by `GFNT_AtlasEntry`'s key, with a stated capacity and an LRU.

---

## 16. Non-goals for the first stable release

Absent, not stubbed; each returns `ERR_UNSUPPORTED` at the point where the
file or the request names it, and nowhere is there a function that pretends.

- **The TrueType hinting interpreter and any autohinter** (M3, §8.5).
- **WOFF 2** until `compress` has Brotli.
- **The `SVG ` table** until `text` has XML, and an SVG subset after that.
- **`CFF2`**, **AAT** (`morx`, `kerx`, `feat`, `trak`, ...), **`.dfont`**,
  **Windows FNT/FON**, **`.eot`**, **Type 3**, **Type 42**, **Multiple Master**.
- **Synthetic bold and oblique**, LCD filtering, stem darkening.
- **Hyphenation data**, **`SA` dictionaries**, **Knuth-Plass**, vertical layout,
  ruby.
- **Predefined CJK CMaps** for PDF.
- **Font *rendering* of colour**: `COLR` v1 is returned as a paint graph, not
  composited.

---

## 17. Decisions

Listed so that they were decided on purpose. 1-3 were put to the author and
answered on 2026-09-23/24; the rest stand as recommended.

1. **Layout is in this library, up to the paragraph.** Decided (§10).
2. **No hinting interpreter.** Decided (§8.5).
3. **Bitmap fonts are first-class in the API.** Decided (§5.3, §5.4). The strike tables are not read yet.
4. **Outlines and rasterisation both live here**, writing into `GIMG_Raster`
   when `image` is present and a byte buffer when not; `image` stays optional
   (§8.2).
5. **Shaping is ours**, script by script, declared. `cjelly`'s Overview says
   so and the cluster map is the reason (§9.2).
6. **`GFNT_Variation` on every accessor** (§5.3).
7. **The charstring interpreters are container-independent** (§7.4).
8. **No `GFNT_Stream`; a blob and a checked reader** (§5.1, §6).
9. **`fsType` is reported, never enforced.** A library that refuses to render a
   font because of an embedding bit breaks every legitimate viewer; a library
   that embeds one into a document without telling the caller breaks the law
   for them. Reporting is the only defensible position, and the consumer that
   embeds (§13.3) decides.
10. **Fixed point everywhere; no `float` in a core type** (§5.2).
11. **Discovery does not link fontconfig** (§11).
12. **No font is bundled, and no third-party font is committed** (§14.5,
    §14.6). The first draft compiled DejaVu Sans in and committed subset OFL
    fonts as fixtures; the licence obligation the first would have propagated
    to every consumer, and the fact that the second is not needed once the
    fixtures are built here and the real fonts live in the oracle image, are
    the reasons both were dropped. Decided 2026-09-24.
13. **Cluster level is monotone graphemes by default** (§9.3).
14. **The zero line-metrics policy is the font's own request** (§10.3), not a
    platform's.
15. **A table's parse runs outside the face's lock** (§15.3). Decided by a
    deadlock: the first implementation held the lock across the parse, and the
    `numGlyphs` minimum - which reads `hhea` and `hmtx` - hung the test suite on
    the first font that had both. Decided 2026-09-24.
16. **An unreadable strike list is `ERR_UNSUPPORTED`, never a count of zero**
    (§5.3). A library that answers "no strikes" for a font carrying `EBLC`
    tells its caller a bitmap font has no bitmaps, and nothing downstream can
    tell that from the truth. The same rule governs every accessor whose table
    is not parsed yet, which is what keeps "absent, not stubbed" (§16) from
    quietly becoming "absent, and reported as empty".
17. **`cmap` subtable selection passes over a format this library cannot read**
    (§7.2), and reports which subtable answered. Selecting the highest-preference
    subtable and then refusing every lookup would leave a caller with a font
    every other implementation maps; the audit trail is what keeps that from
    being a silent substitution (M8).
18. **A glyph name is what the file says, even when the file repeats it.** Four
    Liberation faces name two different glyphs `uni00AD`; fontTools renames the
    second to `uni00AD.1` because its glyph order must be a set of unique keys.
    This library reports the file's name, because a caller asking what a glyph is
    called wants the font's answer and not a disambiguator nobody wrote, and
    `gfnt_face_glyph_for_name` documents that it returns the first match.
    `ttx_diff` carries the difference as a **checked** category: the reference's
    name must be ours plus a `.<digits>` suffix *and* an earlier glyph must
    actually carry the bare name, so a font genuinely containing `foo.1` that
    this library misread as `foo` stays a disagreement. Decided 2026-09-25.
19. **The multi-byte Macintosh and Microsoft `name` encodings are refused**
    (§7.2), where the single-byte ones are tabulated. A byte-per-codepoint guess
    at Shift-JIS produces text that looks like text, which is worse than an
    error; the CJK mappings are a data set of their own and would arrive the same
    way the single-byte ones did, from an oracle. Decided 2026-09-25.
20. **A glyph name is allocated and freed, not borrowed.** Format 1.0 and the
    standard-order indices could hand back a pointer into a static table while
    format 2.0's stored strings cannot, and an ownership rule that depends on
    which branch answered is a rule callers get wrong. It follows
    `gfnt_face_name`/`gfnt_name_free`, already the library's precedent.
    The reverse lookup is a linear scan over a stack buffer of the format's own
    maximum, so *it* allocates nothing: an index would have to be built,
    memoised and invalidated for a question most callers never ask.
    Decided 2026-09-25.

---

## 18. What is implemented

**Built, with tests:** the scaffold; `core.h` with the three fixed-point types,
their arithmetic, `GFNT_Tag`, `GFNT_Error` and `GFNT_Limits`; `blob.h` over
memory, a file read whole, and an explicit `mmap`; the checked reader and
`check-reader`; the sfnt offset table and directory, `ttcf` versions 1.0 and
2.0, and table checksums reported rather than enforced; `head`, `maxp`,
`hhea`, `hmtx`, `OS/2` versions 0-5 and `post`'s header, each parsed once per
face and memoised; the `numGlyphs` minimum across `maxp` and `hmtx` (M12); the
line-metrics policies with the font's own request as their zero (M4); `cmap`
formats 0, 4, 6 and 12 with the preference order and the symbol subtable's
`0xF0xx` mapping; `name` decoded to UTF-8 by platform and encoding;
`GFNT_GlyphKind`, `GFNT_Strike`, `GFNT_StrikePolicy` and `GFNT_Variation` in
the API; a `_dump` for the blob, the face, `head`, `hhea`, `OS/2`, `post`,
`cmap` and `name`; `fuzz_sfnt` and `fuzz_cmap` with generated seeds; and the
truncation sweep of §14.3 over every length of every table.

**Phase 1 is built, as of 2026-09-26**: `outline.h` with `GFNT_Outline`, the
`glyf`/`loca` reader behind it, and `raster.h` with the scan converter.

- **`glyf` and `loca`** (§7.3): simple glyphs - the flag stream with `REPEAT`
  and the same-or-positive bits - and composites with every flag, including
  point matching, all three transform encodings, and the Apple/Microsoft
  scaled-offset difference defaulting to unscaled. Instructions are skipped by
  length and never run. A `loca` entry running backwards or past `glyf`
  condemns *that glyph* (M11). `head.glyphDataFormat` other than 0, and flag bit
  0x80, are the cubic `glyf` extension and are refused by name rather than drawn
  as quadratics - the two are checked separately, because a font can state one
  and contain the other.
- **An outline holds the font's own points**, not a path derived from them.
  `gfnt_outline_decompose()` is the one place `glyf`'s implicit on-curve
  midpoint and its contour-beginning-off-curve rule are applied. That leaves two
  different things to be right about instead of one: the points compare against
  the table byte for byte, and the path compares against a reference pen.
- **Bounds, twice.** `gfnt_outline_control_box()` is the box of the points and
  `gfnt_outline_bounds()` solves each quadratic's extremum exactly (a cubic's by
  subdivision). `glyf`'s stated `xMin`/`yMax` are the *coordinate* box by
  definition - the specification says "minimum x for coordinate data" - so it is
  the control box a reference can be compared against, and the curve's own box
  has no counterpart in the file at all.
- **The scan converter** (§8.1): cell-based exact area, `ftgrays`' arithmetic
  with six sub-pixel bits rather than eight, non-zero and even-odd fill,
  saturation, and adaptive subdivision of quadratics and cubics to a 26.6
  tolerance. `GFNT_Coverage` is 8-bit linear alpha with a stride, an origin and
  the sub-pixel offset it was rendered at, trimmed to the pixels that have any.
- **`fuzz_glyf` and `fuzz_raster`**, each through the door its bugs come in:
  `fuzz_glyf` takes a `loca` and a `glyf` together, because the interesting
  inputs are in the relationship between them; `fuzz_raster` takes no font at
  all, because the shapes that break a scan converter are degenerate rather than
  malformed.
- **The truncation sweep covers both tables**, and asking for an outline was not
  enough: it asks for the pixels too, because a truncation that produced a
  plausible outline rather than a refusal is caught only there.

**What phase 1 cost, in findings.** Each of these is why something above is
shaped the way it is:

- **The composite 2x2 was applied transposed.** The file stores `xscale`,
  `scale01`, `scale10`, `yscale` and applies them column-major; the struct is
  row-major. `WE_HAVE_A_SCALE` and `WE_HAVE_AN_X_AND_Y_SCALE` are diagonal and
  cannot tell the two readings apart, so only a sheared component can - and real
  fonts almost never shear, so the corpus would not have found it either. One
  fixture glyph does.
- **A rasteriser cannot be tested against itself**, and fontTools does not
  rasterise. `tests/unit/test_raster.cpp` carries a second implementation of the
  *definition* of coverage - sample a 16x16 grid per pixel and count winding
  numbers - and requires the two to agree within the sampler's own resolution.
  It found the sub-pixel origin being applied to the bitmap's extent and not to
  the geometry, which the trim then hid.
- **`make test-quiet` returned 0 with a failing suite.** The failure count came
  from parsing gtest's summary, and a suite that crashes before printing one
  parses as zero tests. Found by a planted rasteriser defect that crashed a
  suite: eight tests failed and the build was green. The verdict is the exit
  status now.
- **Paired defences shadow each other, three times.** The cubic refusal was
  reachable through `head` *or* the flag bit and the first hid the second; the
  point and contour caps were checked in `glyf.c` *and* in
  `gfnt_outline_reserve()`; cells are merged as they are made *and* per row. In
  each case removing one changed nothing, so each needed either a fixture that
  reaches only the second or the removal of the duplicate.
- **A glyph stride samples away the whole reason a fixture exists.** Three
  planted defects were each caught by exactly one fixture glyph, and at
  `glyf_diff`'s default stride of seven all three fell between samples. The
  fixtures are compared at every glyph now, whatever `--stride` says.
- **No fixture carried a repeated flag**, and the gates could not have told
  anyone. `glyf` may write one flag byte and a count in place of a run of
  identical flags, and every fixture glyph alternated - a zigzag changes its y
  delta at every point, a leaf turns at every corner - so fontTools compressed
  none of them. The arm that expands a run was reached only through the oracle's
  real fonts, where every font has one, which left the construction invisible to
  `make test` on a machine with no container engine. Found by measuring line
  coverage rather than by a plant, which is the case for measuring it: ten lines
  of `glyf.c` were unexecuted and nothing else said so. `outline-simple.ttf` now
  carries a bar whose two edges are runs of 259 identical flags - more than a
  one-byte count can express, so one run is two records - and `testGlyf` asserts
  the *records* as well as the expanded points, because compressing is the
  writer's choice and a test that read only the points would pass again the day
  fontTools stopped. The over-long count that has to be refused is hand-built
  with its own control, for the same reason the broken `loca` is patched bytes:
  no writer will produce it.

**What a coverage sweep found afterwards, 2026-09-27.** Reading the report by
line rather than by percentage took `make test` from 92.0% to 97.6% of the
library's lines, and the tests written on the way found four things that were not
about coverage at all:

- **`gfnt_face_has_outlines()` contradicted its own contract**, and did it
  differently for the two charstring tables. The header says the predicate
  answers "can asking for an outline succeed", and `CFF2` reported false for
  exactly that reason - while `CFF ` reported *true* for a face this library
  cannot draw. Nothing caught it because no test asked a `CFF ` face, and
  `gfnt_face_strike_at()` was telling the caller of an OTTO font that it had
  outlines to scale. `CFF ` now reports false until the charstring interpreter
  lands, which is one answer here that is expected to change (§7.4).
- **A refusal's specific message was unreachable.** `gfnt_name_transcode()`
  names the multi-byte Macintosh encodings it cannot decode; the decodability
  check in front of it asks the same question, so no caller ever saw that
  sentence and every one of them got "a name encoding this library does not
  decode yet". The specific message moved out to the check that fires, and the
  inner one stayed as a guard - what follows it indexes an array by the table
  number.
- **A composite's arguments have three encodings, not two**, and the third -
  byte-sized *point indices*, the only unsigned pair - had no cut test behind it,
  because the composite the sweep cut used the other two.
- **The instrument was wrong twice**, in both directions, and neither showed up
  in the aggregate. Threads plus GCC's default `-fprofile-update=single` made an
  unreachable line read as executed 12 times, then not at all, then 16; and
  `gcov`'s per-source output file meant a header compiled into many objects kept
  only the last object's report. The report is what a triage acts on, so an
  instrument that is right about the total and wrong about which lines is worse
  than a coarse one.

The 77 lines that remain are unreachable through the public API, in five named
kinds, and development.md lists them with their counts: an arm nothing can reach
is a different thing from an arm nothing has tried to reach, and only the second
is work.

**Phase 2's first container is built, as of 2026-09-28**: `charstring.h` with the
Type 2 and Type 1 interpreters, the `CFF ` container behind them, and an OTTO face
whose glyphs draw. §7.4 has what that includes and what it leaves - bare CFF and
Type 1 as *containers*, which both need a face whose tables do not come from an
sfnt directory, and that is one piece of work shared with the bitmap formats of
§7.1.

- **The interpreters know nothing about any container** (decision 17.7), which is
  what makes them testable at all: `gfnt_charstring_run()` takes bytes and two
  subroutine accessors, so the arms an interpreter mostly consists of - a stack
  that underflows, a subroutine that is not there, an operand that ends past the
  charstring - are reachable from a test and from no font. It is also what a PDF
  library needs, where a `FontFile3` is a bare CFF that never arrives wrapped in
  an sfnt.
- **Type 1 is not a dialect of Type 2**, and the code refuses each language the
  other's operators rather than drawing a plausible wrong shape. The advance and
  side bearing come from `hsbw`; `255` introduces a plain integer rather than a
  16.16 - the same five bytes differing by a factor of 65,536; subroutine numbers
  are unbiased; contours close with `closepath`; the accented character is `seac`
  with a side-bearing correction; and flex and hint replacement come through
  `callothersubr` and `pop`.
  - The split is a fact about the byte range, and it has to reach **every** place
    that reads one: the interpreter's dispatch, and the dump's operator-name
    table, which names only what the language it was handed actually has. A dump
    that named every byte in both would report that a font contains an operator
    its language does not have - and for `hintmask` it would read a mask, so one
    byte Type 1 reserves would make every operator printed after it fiction.
- **Glyph names have two sources now**, and the charset wins for a CFF face: the
  specification says such a font carries `post` version 3 and every reference
  takes its glyph order from the charset. The three public name calls moved out of
  `post.c` into `glyph/names.c` for it, and `gfnt_face_glyph_outline()` moved out
  of `glyf.c` into `outline/producer.c` for the same shape of reason - one
  question, two implementations, and the choice belongs in neither.
- **`CharStrings`'s count joins the numGlyphs minimum (M12)** and `loca`'s
  deliberately does not. A glyph past the end of `CharStrings` has no charstring
  at all, where a `loca` entry running backwards condemns *that glyph* and leaves
  the rest of the font answering (M11). The comment promising otherwise had been
  stale since phase 1.

**What phase 2 cost, in findings.** Each is why something above is shaped the way
it is:

- **A corpus survey before writing a fixture, for once in the right order.** The
  image's 35 CFF fonts use every curve operator, `hintmask`, `cntrmask` and both
  subroutine flavours - and between them not one flex, not one accented character,
  no CID font, no custom encoding, no charset in a format other than 0, and none
  of the arithmetic operators. A `T2CharStringPen` writes none of those either, so
  seven fixtures carry CFF tables assembled byte by byte inside the pinned image
  and wrapped in an sfnt fontTools builds: everything a pen can write comes from
  the reference and the rest is hand-built and named in the MANIFEST.
- **The thinned run was the hole again.** Applying `hhcurveto`'s leading operand
  to every group rather than to the first produced three disagreements in 24,957
  fields of real fonts and **none at all** at `--fonts 6`. With the fixtures in
  every run it is four at `--fonts 2`. That is the third time a defect has been
  invisible to a sample and visible to a fixture, after the `post` 2.0 boundary
  and `OS/2` version 2's fields.
- **Two defects the unit tests found that no font could have.** A charstring that
  draws before it moves opened its contour at the point the segment *ends*, which
  is a side bearing's worth of error in the one place Type 1 allows it. And the
  caps a caller states in a `GFNT_CharstringContext` were applied to the run and
  not to the outline it appends to, so a lowered `max_outline_points` was silently
  ignored - two places for one fact.
- **Three pieces of undefined behaviour the fuzzers found, none reachable from a
  font.** A left shift of a negative value, which C leaves undefined, in nine
  places - both number decoders, the dump's copy of one, `div` and `sqrt`, whose
  operands are negative by design. Signed overflow in `div` and `sqrt`, where an
  operand clamped to 2^61 was then multiplied by 65,536. And signed overflow
  building a DICT real's decimal scale, where nineteen multiplications by ten
  leave the type.
- **The fuzz object tree had no depfiles**, so a header change rebuilt only the
  objects whose own `.c` had changed. Adding two fields to `GFNT_Limits` and
  running a new harness reported "stack-buffer-overflow in gfnt_limits_default" on
  the fourth unit: one binary linked from two layouts of one struct, with a crash
  artifact to make it look reproducible. Worse than a stale result, because the
  report names a source line and accuses working code.
- **The golden gate's own property caught eleven thin fixtures.** A chain of
  curves that all run up-and-right encloses a sliver with its closing line, and a
  sliver's extreme row carries so little coverage that a quarter-pixel
  *horizontal* shift decides whether it rounds to nothing - so the vertical extent
  moved. The property is about the rasteriser and is right; a fixture too thin to
  state it weakens the gate, so each one now ends with a leg that gives its
  contour area.
- **`cff.otf` left the golden generator's "renders nothing" list**, and what
  replaced it is worth more than the refusals it used to contribute: its
  charstrings are the same five outlines `basic.ttf` holds as `glyf`, so the
  subset check now asserts that two containers and one rasteriser produce
  identical pixels.
- **The reference has two gaps of its own here**, both marked in its own source.
  `cffLib` ignores `CharstringType`, so a font carrying Type 1 programs
  decompiles as Type 2 and comes out as two leftover operands; and its Type 1
  `sbw` drops the operands, setting neither advance nor side bearing. The first is
  worked around by re-wrapping the bytes, which is what makes the Type 1 language
  comparable at all; the second is reported as a glyph the reference refused,
  because a zero this library disagrees with reads as a disagreement.

**The oracles are built, as of 2026-09-24.** `tools/oracle/` carries the
`fonttools` image - pinned base digest, `fonttools==4.66.0`, `brotli==1.2.0`,
and six Debian font packages by full apt version - with `oracle_env.py`,
`oracle_run.py`, the corpus extractor, and two differentials:

| gate | what it compares | result |
| --- | --- | ---: |
| `check-oracle-cmap` | every codepoint fontTools maps, both its neighbours, and a stride sample, per font | 327 fonts, 587,186 codepoints, **0** |
| `check-oracle-glyf` | every glyph's points, flags, contours, both boxes and its decomposed path | 312 fonts, 37,218 glyphs, 2,001,169 fields, **0** |
| `check-oracle-cff` | every charstring's program operator by operator, the path it draws, the advance it states, its charset name and its control box | 43 fonts, 4,172 glyphs, 24,957 fields, **0** |
| `check-golden` | the committed renderings, rebuilt for s390x, powerpc64 and sparc64 | 2,425 renderings x 3 targets, **0** |
| `check-oracle-cmap-exhaustive` | all 1,114,112 codepoints per font | 364,314,624 comparisons, **0** |
| `check-oracle-ttx` | every field of `head`, `hhea`, `OS/2`, `post`, `maxp`, the directory, the `cmap` inventory, every `name` record and every `post` glyph name | 352 faces, 292,609 fields, **0** |
| `check-fixtures` | every committed fixture against a fresh generation | 25 files, 56,968 bytes, **0** |
| `check-vectors` | the generated vectors against a fresh generation | 5 files, 54,939 bytes, **0** |

`ttx_diff`'s Mac Roman category is **gone**, which is the clearest measure of
what the vectors bought: it counted fourteen records it could not score, and now
scores them. What remains is one checked category of four glyph names (decision
18) and zero declined records.

**The fixtures are built, as of 2026-09-25**, and they changed what `ttx_diff`
can see. `tools/fixtures/make_fixtures.py` writes twenty-four fonts and a `MANIFEST`
from outlines in that file, in the same pinned image; `check-fixtures`
regenerates them and compares bytes; `testFixtures` loads every one of them
without a container, so the unit suite has an input it did not write itself.
Three findings came out of building them, and each is the reason a number above
moved:

- **`ttx_diff` had never compared twenty-two `OS/2` fields.** Subscript,
  superscript, strikeout, family class, PANOSE, both Unicode range pairs, both
  code page ranges, the default and break characters, max context, and both
  optical point sizes were parsed by this library, printed by its dump, and read
  by nothing. `os2-v5.ttf` was built to exercise the optical sizes and the
  differential reported it clean, which is how the gap surfaced: a field the
  reference does not emit reads exactly like a field that agrees. The comparison
  is 8,541 fields wider for it.
- **The corpus's missing `OS/2` versions now exist.** It holds versions 1, 3 and
  4 only. A planted defect that read version 2's fields from a version 1 table
  once survived a four-font sample (§14.7); with the fixtures in the population
  by default, that plant and an optical-size transposition are both caught by a
  `--fonts 4` run, because the fixtures are never what thinning drops.
- **A default in the reference produced 262 confident disagreements.** The
  PANOSE emitter spelled one member `bLetterform` where fontTools spells it
  `bLetterForm`, and read it with `getattr(..., 0)`; the default turned a typo
  into a zero and the gate blamed this library for a field it had right. The
  order now comes from fontTools' own struct description and the lookup has no
  default. An instrument with a fallback reports on the fallback.

**The vectors are built.**
`tools/vectors/make_vectors.py` generates the standard Macintosh glyph order and
the eight single-byte Macintosh encodings from the pinned image;
`gfnt_face_glyph_name`, `gfnt_face_glyph_for_name` and the Macintosh `name`
decode read them; `check-vectors`, `testVectors` and `ttx_diff` cover them from
three directions (§14.8). Four findings, each of which is why a figure above
moved:

- **The encoding ID does not choose the Macintosh table.** `platEncID` 0 is keyed
  by `langID` - Icelandic, Turkish, Croatian, Central European and Romanian all
  live under it - so "encoding 0 means Mac Roman" would have decoded thirteen
  languages' records to the wrong letters without ever failing. The rule table is
  generated for that reason, and all 658 Macintosh records in the real corpus are
  `(0, Roman)`, so nothing but a fixture could have caught it.
- **A `post` format 2.0 boundary nothing crossed.** A planted off-by-one at the
  standard/stored index boundary passed a two-font oracle run with zero
  disagreements: `post-v2.ttf` used indices 0-3 and 258 and never 257. 38 of the
  corpus's 291 format 2.0 fonts do use 257, so the full run catches it - but the
  fixtures are what travel with a thinned one, so the fixture now holds the
  boundary and the plant is caught at `--fonts 2`.
- **A second source has to answer the same question.** Cross-checking Mac Roman
  against fontTools' own `encodings.MacRoman` through the AGL reported 39
  disagreements, because that table is a glyph order rather than the encoding
  (§14.8). glibc's `MACINTOSH` charmap is a real second source and agrees on 126
  of 128.
- **fontTools renames duplicate glyph names.** Four Liberation faces genuinely
  name two glyphs `uni00AD`; the reference appends `.1` to keep its glyph order
  unique. Verified in the raw bytes before the category was written, which is the
  only way to tell that apart from being wrong (decision 18).

**Bare CFF is built too, as of 2026-09-28**: a face whose one table is
synthetic, whose em comes from the `FontMatrix`, whose glyph count comes from
`CharStrings`, and which names itself out of the Top DICT because it has no
`name` table to name itself in (§7.4).

**The Type 1 font format is built, as of 2026-09-28**: PFB and PFA framing,
`eexec`, the per-charstring cipher, and a scanner for the PostScript a font
program is written in - with its em, its glyph count, its names and each glyph's
advance all answered out of the program because there is no table to read any of
them from (§7.1).

**The four bitmap containers are built, as of 2026-09-28**: PCF, BDF, PSF 1 and
2, and GNU Unifont's `.hex`, each a face with one strike and no outlines, with
`bitmap.h` as the `GFNT_GLYPH_BITMAP_MONO` arm's data and `GFNT_Strike` finally
answering from something (§7.1, §7.5). **`ghoti.io-compress` is this library's
second dependency**, taken the same day for the gzip those fonts arrive in.

- **One internal representation, four parsers.** The formats share no bytes and
  every font underneath them is the same thing: a pixel size, glyphs with boxes
  and advances, a mapping from characters to glyphs. So the accessors above them -
  the strike list, the glyph name, the codepoint lookup, the font's own names - are
  written once.
- **The bit and byte orders are undone in one function**, because PCF's four
  layout choices are independent and a caller handed the raw rows would have to
  implement the cross-product to draw anything. `bitmap-lsb.pcf` and
  `bitmap-swap.pcf` are the same design in the other layouts, and that the three
  read identically is the only check on that arithmetic a little-endian machine can
  make.
- **`gfnt_sfnt_table_directory()`**, the multi-entry synthetic directory the bare
  CFF work left a note about, and a line reader on `GFNT_Reader` bounded by
  `max_line_length` for the two text formats - which §5.1 promised and nothing had
  needed.
- **`gfnt_coverage_from_bitmap()`** makes a strike's glyph the same
  `GFNT_Coverage` a rasterised outline produces, so a caller compositing a run has
  one code path. Nothing in it scales: whether to use a strike at a size it was not
  drawn for is `gfnt_face_select_strike()`'s decision and its policy's.
- **`fuzz_bitmap`** takes whole unaltered files and covers all four containers in
  one harness, because which one claims an input is itself a decision a malformed
  file steers.
- **The golden gate covers the strikes**, through the second driver: 9 fixtures,
  every glyph, reproduced on s390x, powerpc64 and sparc64. Every step from a file's
  bytes to a strike's pixels is an explicit shift, so they *should* be identical
  there - and that sentence was a claim until this measured it.
- **A gzipped font is inflated before its format is looked at**, so nothing below
  `gfnt_face_load()` knows the file was compressed. The wrapper reuses the derived-
  bytes field Type 1 introduced, and the two **stack**: a `.pfb.gz` is inflated and
  then deciphered, each layer freeing the one it consumed. The cross build for the
  golden gate shims the inflater out rather than cross-building `compress`, for the
  reason it shims `cutil`: a generated header built for the wrong target is the
  class of defect that gate exists to find.

**What the bitmap containers cost, in findings.** Each is why something above is
shaped the way it is:

- **`compress`'s gzip reported a full output buffer as a corrupt stream.** Its
  `finish()` ignored the output buffer, so "the input ended early" and "there is
  nowhere to put the rest" were one answer - and `gcomp_decode_alloc()`'s growth
  loop reads `GCOMP_ERR_LIMIT` as "grow and ask again", so a `.pcf.gz` over a
  caller's ceiling came back as a corrupt font. Fixed there rather than mapped
  around here, because §5.6's vocabulary only means anything if the two answers stay
  apart. Its own ceiling test used zstd, whose frame states its size and which
  therefore takes a different path; the test now sweeps all seven methods and six of
  them were already right.

- **`bdftopcf` overstates one table's size in every file it writes.** The last
  entry's size is that of an accelerator table *with* ink bounds whether it wrote
  those or not, so the final table of every Terminus font overruns the file by
  twenty-eight bytes. libXfont and FreeType both read such a file because neither
  compares a stated size against the file's length. This library refused all 234 of
  them until the differential met one; a PCF entry is clamped to the file now, and
  an offset past the end is still refused.
- **`gfnt_table_cached()` copies its scratch into the memo whatever the parse
  returned**, so a parser that refuses without writing publishes stack garbage -
  and for a memo holding arrays the face frees, closing the face then freed a wild
  pointer. Found by the first test to ask an outline face for a bitmap. The
  requirement is now written in `tables.h`, because Type 1 and CFF meet it only by
  convention.
- **A mutation harness scored a stale binary.** Two of four planted defects were
  reported as caught by the *previous* mutation's build: an `if (false)` edit left a
  parameter unused, `-Werror` failed the build, and the test binary from the run
  before was what ran. The harness checks the build's exit status now, and the
  mutations invert conditions rather than deleting them.
- **A fixture covered everything about the scan unit except the swap.** A scan
  unit's bytes are reversed only when the bit order and the byte order *differ*, and
  `bitmap-lsb.pcf` has them agreeing - so a reader that never implemented the
  reversal read it correctly. `bitmap-swap.pcf` exists because measuring the bytes
  is what found that.
- **A test asserted an outcome that was already true.** Clearing a row's bits past
  the glyph's width can be deleted with every test passing, because every row of
  every fixture already had zeros there. `bitmap-ink.bdf` now has a row that sets
  them.
- **A branch no input could distinguish.** A PCF encoding's code was computed as
  `low` for a single-byte font and `(high << 8) | low` otherwise - and for a
  single-byte font `high` is zero, so the two arms are the same expression. A
  mutation swapping them changed nothing. Removed, and a two-byte encoding is a
  test now, because no fixture has one.
- **`make golden` had been failing for two fixtures and nobody ran it.**
  `check-golden` reads the committed file and `testGolden` re-renders in C++, so
  the generator could rot unnoticed - and had, on a fixture that refuses every
  glyph and on one whose renderings duplicate another's. `check-golden`
  regenerates first now, the way `check-fixtures` and `check-vectors` do.
- **Pillow is a reference for half of each format.** Its BDF reader is sound. Its
  PCF `_load_encoding` indexes the offsets array by character code rather than by
  `code - firstCol`, so for a font whose codes start at 0x20 - most of them - every
  glyph is reported 32 positions from where it is; and `_load_bitmaps` leaves the
  byte-order bit commented out in its own source. So the differential compares PCF
  glyphs by *index*, counts a layout Pillow cannot lay out as declined, and prints
  what it could not check rather than a clean number that hides it.
- **Two formats cannot state their own licence.** §14.5 requires every committed
  fixture to name itself and its licence, and a `.hex` file is a codepoint, a colon
  and a row of bits while a PSF is a header and cells - neither has anywhere to put
  a string. For those four fixtures the statement is `MANIFEST` and this
  repository's licence, and `testFixtures` asserts the refusal rather than skipping
  them.

**`EBLC`'s strike list is built, as of 2026-10-01**: the `bitmapSizeTable`s
behind `gfnt_face_strike_count()` and `gfnt_face_strike_at()`, and
`gfnt_face_select_strike()`'s policies **choosing** rather than confirming. Until
this, every face that had a strike had exactly one - the file *was* the strike -
so `GFNT_STRIKE_NEAREST` had been documented and tested for three phases without
any input able to tell it from "the first strike", and the selection read
`gfnt_face_strike_at(face, 0)`. `strikes.ttf` has three, one pair of which is
equidistant from 11 ppem, so the tie rule is a rule. What that cost in findings:

- **A fixture set can make a policy untestable, and nothing says so.** The hole
  was not a defect in the selection - it was that no font this library could load
  had two strikes, so the search over a list was a search over one element. It is
  the `single-record-never-grows-array` shape at the level of a format: the
  population a gate samples has to contain the input that discriminates, and
  "every strike container is one strike" made that impossible rather than merely
  absent.
- **Nothing on this machine has the table, and the oracle corpus had nothing
  with it either.** The one font that looked like it was `NotoColorEmoji.ttf`,
  which is `CBLC` - the same layout with PNG payloads - so the corpus could not
  check a single `EBDT` glyph.
- **The population was then surveyed wrongly, and the wrong answer stood in this
  document.** It said two Debian packages carry the table, "found by installing
  candidates in a throwaway container rather than by reasoning about which fonts
  ought to have bitmaps". The container was real and the reasoning was still doing
  the work: the *candidates* were chosen from the guess that embedded strikes are
  a CJK thing. Scanning **every** one of Debian's 589 font packages - 5,927
  distinct sfnt files, table directories read with `struct` - gives **33 files
  with `EBLC`/`EBDT` and 11 of those with `EBSC`**, across fourteen packages.
  Three independent defects produced the earlier number, and each is worth keeping
  because each is a shape rather than a slip:
  - it globbed `*.tt[fc]`, so every **`.otb`** was invisible - and an `.otb` is a
    *bitmap-only* sfnt, the single most on-point shape for this table;
  - it swallowed a font fontTools cannot open in a bare `except: continue`, so a
    file vanished with no line of output;
  - and it walked `/usr/share/fonts` **inside an image whose packages that same
    guess had chosen**, so the question it answered was "which of the fonts I
    thought would have this do", which is not the question.

  `fonts-wqy-zenhei` was named in this document as the obvious guess, checked, and
  not carrying the table - "which is the only reason that is not now a wrong
  sentence in a design document". It carries it on **face 2** of three. The
  sentence was wrong, and the self-congratulation was the tell.
- **`(index 2, image 5)` is 54,268 of 57,000-odd index subtables - still the
  overwhelming majority.** A reader that implemented only that pair would read
  almost every real glyph. What the widened population changed is which cells rest
  on fixtures: index format **3** and image formats **1 and 2** are now covered by
  real fonts (Anonymous Pro, Computer Modern, Terminus, misaki, UKIJ), where this
  document said all of them occur nowhere. Index formats **4 and 5** and image
  formats **8 and 9** are what no Debian font uses, and image format 6 appears
  only in the one file fontTools cannot read.
- **The widened population immediately found a defect that two fixtures and
  2 real fonts had not.** `Konatu.ttf` has 13,249 zero-length glyphs in each of
  its fourteen strikes, and this library read a zero-length entry in an
  offset-array index format as a glyph that is *present with no pixels*: it
  reported 15,570 of 15,572 glyphs carried where fontTools and FreeType both say
  2,323 - one reference disagreeing is a question, and two agreeing is an answer.
  A unit test asserted the wrong reading **in a comment explaining why it was
  right**, which is why no gate had ever disagreed - `AGlyphOfZeroLengthIsASpaceAndKeepsItsAdvance`
  is now `AnOffsetFormatsZeroLengthGlyphIsOneTheStrikeDoesNotCarry`. What settles
  it besides the reference is the specification - the difference between
  consecutive offsets is the data size - and FreeType's source, whose format 1 and
  format 3 arms do the same comparison under the comment "missing glyph". The rule
  is per index format, not per length: formats 1, 3 and 4 state sizes as
  differences between offsets and a zero difference means no bitmap, while 2 and 5
  state one constant size whose zero is a statement about every glyph they cover.
  A mutation exempting format 4 escaped the whole suite until a case was written
  for it.
- **Two guards answering one question left the second unreachable.** The parse
  checked that `numSizes * 48` bytes were present *and* checked each field read,
  and the first refused exactly the inputs the second would have - so the
  per-strike arm could never be seen to work. The bulk check is gone; what bounds
  the allocation is `max_strikes`, which is the cap that matters, and the sweep
  now cuts a `bitmapSizeTable` to all 48 of its lengths.
- **`GFNT_Limits::max_strikes` had never been read.** It was declared in phase 0
  with a value of 256 and nothing consulted it, which is the
  `unread-table-constants` shape in a field rather than a table.
- **The memo helper leaked a losing parse, and this is the first memo that could
  tell.** `gfnt_table_cached()` parses with no lock held - deliberately, because
  one table's parse legitimately needs another's and a non-recursive mutex held
  across that deadlocks - so two threads may parse one table and only one
  publishes. The loser's scratch was dropped. That was invisible for every memo
  before this one: `head`, `hhea`, `OS/2`, `post`, the numGlyphs minimum, the
  `cmap` choice and `CFF ` own nothing but offsets into the blob, and the bitmap
  containers' and Type 1's arenas are built during the *load*, where there is one
  thread. `EBLC` is the first memo that is both lazy and owning. The helper takes
  a release hook now, required of any memo whose type owns memory, and §15.3 says
  so.
  - **The test for it could not see the leak until the race was forced.** Eight
    threads over `strikes.ttf` raced zero times in thirty-two passes - spawning a
    thread takes longer than parsing 152 bytes - so removing the hook changed
    nothing and the test passed either way. It needs a barrier releasing the
    threads together *and* a font with 256 strikes to make the parse long enough
    to overlap, and it counts how many passes actually raced so that a run which
    serialised reports a skip rather than a pass.
  - **And the instrument could not count.** `FailingAllocator` keeps plain
    `size_t` counters, which is right for every other sweep in the suite and wrong
    across eight threads: it reported a live count of -1 with the code correct and
    7 with it broken, from the same unsynchronised increments. The figure that
    matters came from an atomic counter written for this test.

**`EBDT`'s glyph data is built, as of 2026-10-01**: index subtable formats 1-5
and image formats 1, 2, 5, 6 and 7, so an `EBLC` face's glyphs come back as
::GFNT_BitmapGlyph through the same accessor a PCF's do. The index format says how
to find a glyph's bytes and the image format says what those bytes are, and the two
are **independent axes** - a reader that conflated them would work on the one
pairing that is 97.8% of the real population and fail on the rest.

- **Bit-aligned rows are widened and then go through the same path as every other
  container.** Image formats 2, 5, 7 and 9 pack the next row at the next *bit*;
  1, 6 and 8 start each row on a byte. `EBDT` is MSB-first with no scan unit, so
  the byte-aligned forms are `gfnt_bitmap_build_glyph()`'s identity case and the
  bit-aligned ones need one widening pass first.
- **A glyph is absent, present, or corrupt** - three states on a record, not two.
  A strike is sparse over the face's glyph count, so "this strike has no bitmap for
  that glyph" is a different fact from "that glyph's bitmap is empty", which a space
  legitimately is; and M11 means one unreadable glyph must not condemn the strike it
  is in. The first draft returned the failure from the strike parse, which made one
  wrong offset in a 27,000-glyph strike lose all 27,000 - caught because it also
  made a *correct* glyph of the same strike unreadable.
- **Grey strikes are listed and their glyphs are refused.** The strike list reports
  a bit depth of 2, 4 or 8 honestly, because that is what the table says; every row
  in this library is one bit per pixel, from `GFNT_BitmapRecord::stride` through the
  widening to `gfnt_coverage_from_bitmap()`, so the glyph data is declined by name
  rather than unpacked wrongly.

What that cost in findings, all of them about what a test could see:

- **A fixture 8 pixels wide cannot tell the two alignments apart.** At any width
  that is a multiple of eight, bit-aligned and byte-aligned rows are the *same
  bytes* - so a mutation routing image format 2 through the byte-aligned path passed
  the entire suite. `strike-formats.ttf` is 11 by 7 for that reason, and the comment
  that predicted the hole was in the test before the mutation found it.
- **The golden gate rendered one strike of three.** Its driver asked for strike 0,
  which was right while every strike container had exactly one, so two thirds of a
  three-strike fixture's pixels were never checked on a big-endian target. Both
  drivers iterate every strike now, and the ppem column already distinguished them.
- **Which driver produced a line was guessed from the file extension.** `.pcf`,
  `.bdf`, `.psf`, `.hex` meant "pixels" and anything else meant "paths" - and an
  sfnt with `EBLC` strikes is a `.ttf` whose pixels are committed. The cross-build
  rendered it as outlines and produced 422 lines the committed file does not have.
  Both checkers read the file's own section marker now, which cannot drift from the
  file the way a heuristic about names can.
- **`WITH_STRIKES` and `NO_OUTLINES` had been one list.** "Has a strike" and "has
  no outlines" named the same fixtures until an sfnt had both, and collapsing them
  again would have asserted that a font with both draws nothing.
- **A result code was overwritten with a constant.** `gfnt_face_glyph_bitmap()`
  returned `ERR_UNSUPPORTED` for every failure of the strike count, which was right
  while the only such failure was "this library cannot enumerate them". An `EBLC`
  parse can also run out of memory, and that arrived as UNSUPPORTED carrying an
  out-of-memory message - a code and a diagnostic disagreeing, which is worse than
  either being wrong alone.

**The strike differential is built, as of 2026-10-01**: `eblc_diff.py` against
fontTools **and FreeType**, over the whole Debian population of this table and
every strike fixture. Until it, every claim in the two blocks above rested on
fixtures this repository wrote and a reference reading them back.

**17,085,462 fields over 40 faces against fontTools and 17,539,615 over 41
against FreeType, 0 disagreements each**, exhaustively - every glyph of every
strike, not a stride - over 36 fonts.

**Two references, because each alone has a hole the other covers.** This is the
whole argument for the second oracle image, and it is not redundancy:

- fontTools parses every field and **composes no image**, so a composite's pixels
  were this repository's arithmetic on *both* sides of the comparison - the one
  thing a clean differential must not leave looking checked. FreeType composes,
  and `ttsbit.c` is where this library's placement rule came from, so the rule now
  has a reader that is not the document it was taken from. Every pixel of every
  composite in `strike-composite.ttf` agrees.
- fontTools **cannot parse `mona.ttf`** at all. FreeType reads all three strikes:
  346,780 protocol lines, 28 of which differ and none a pixel. "Read by this
  library and nobody else" is now true of nothing in the population.
- FreeType in exchange answers less, and **says so in its own output** rather than
  being exempted by a list here. It reports no per-strike bit depth; it reports a
  strike's baseline only for a face it judges bitmap-only, because
  `tt_size_select` prefers the scaled outline's for any other; it cannot separate
  an absent glyph from a corrupt one, or - on a bitmap-only face - from an empty
  one, because `ttgload.c` turns a missing glyph there into whitespace built from
  `hmtx`; and it **fills a stated advance of zero** from `hmtx` for a renderer's
  benefit. Each is a `census unanswered`, `census coarse` or `census fills` line
  from the driver, and the differential narrows exactly what the reference
  disclaims. A reference that stops disclaiming a key is compared on it from that
  run onward, which is the direction this has to fail in.

**Both arms of `FT_IS_SCALABLE` were a bug in the driver's first draft, and both
printed plausible numbers.** It read `size->metrics.ascender` for every face and
reported `ascent 11` for a mona.ttf strike whose byte says 10 - not a disagreement
about `EBLC`, but one reader answering a different question in the same units. And
it trusted a successful load to mean the strike carried the glyph, which on a
bitmap-only face it does not. `FT_IS_SCALABLE` is FreeType's judgement rather than
"has a `glyf` table": Wine's `system.ttf` has one and is bitmap-only by that test,
which is how 191 glyphs of its 18 ppem strike arrived as whitespace the table does
not contain.

**The version is pinned to the source that was read.** `src/bitmap/ebdt.c` cites
`ttsbit.c` for the composite placement, so the oracle is that code: 2.14.3, built
from a tarball pinned by a SHA-256 that Savannah and SourceForge independently
serve. The compound loader is unchanged from Debian's 2.13.3, which is worth
knowing on its own, so the citation names both. A source release also has no
expiry, unlike an apt pin into a distribution, and this is the one gate here whose
disagreement would mean a defect in this library.

- **It is a gate with no `-exhaustive` variant, deliberately.** Every other
  differential here samples because its corpus is 329 fonts; this one's is 36 and
  it runs in two minutes, so a strided default would leave the format's only real
  population sampled for no saving worth having.
- **The three glyph counts are over every glyph whatever the stride.** A stride
  samples pixels; a strike's glyph *set* is what the whole index subtable walk
  decides. Both mutations that moved a glyph's offset by one entry were caught by
  `present` and `corrupt` disagreeing before any pixel was compared - which is
  the check that survives a developer running `--stride 997`.
- **The population must not be chosen by either reader.** `fonttools-corpus-ebdt`
  reads a table directory with `struct` and opens nothing, because selecting with
  the reader under test lets a reader that lost `EBLC` support compare nothing and
  pass, and selecting with the reference drops `mona.ttf` - the one font the two
  disagree about being *readable*. The same program picks the fixtures, so a new
  strike fixture joins the differential by existing.
- **A font a reference declines is an assertion, not a skip**, and the table of
  them is now per reference. `mona.ttf` is named with the error fontTools must fail
  with; **FreeType's table is empty**, which is the assertion that every font in
  the population must reach it. If a future fontTools reads `mona.ttf`, the gate
  fails and says to move it to that reference's compared population; if it fails
  differently, the gate says that too; if FreeType declines anything at all, the
  gate names it. All of those arms were planted and seen to fire.
  - The "ours alone" line - what *this* library read of a face nothing compared -
    now prints only for a face **no** reference read. With one reference a decline
    and an unread face were the same thing; with two they are not, and `mona.ttf`
    is exactly the case that separates them, so printing "read by one reader and
    nobody else" beside it would now be false.
- **A strike-level key no face answered is a comparison that stopped happening**,
  and that is a guard of its own. FreeType answers the baseline only for a
  bitmap-only face, so that whole arm rests on which fonts the corpus holds - and
  the `.otb` files were invisible to the corpus list until it stopped selecting by
  file extension. Drop every bitmap-only face and the gate fails naming the
  packages; drop only the `.otb` files and it does not, because Wine's fonts are
  bitmap-only by FreeType's test too. Both of those were run.
- **The report names the cells of the grid the run visited**, from fontTools'
  census: `(2,5)` 54,268 subtables, `(1,7)` 1,739, `(1,2)` 850, `(3,1)` 50, and
  one each of `(1,1)`, `(3,6)`, `(4,2)`, `(5,5)`, `(1,8)` and `(1,9)`. Index
  formats **4 and 5** and image formats **8 and 9** are the fixture-only cells
  now; `(3,1)` and `(1,2)` came with the widened population. It also prints that
  191 strikes had `flags` of 1 and a depth of 1 and none had anything else, so the
  vertical `sbitLineMetrics` arm and every grey depth are fixture-only in a number
  rather than in a sentence.
- **40 faces over 36 distinct table extents.** `uming.ttc`'s four faces name the
  *same* `EBLC` and `EBDT` byte range, so three of those four re-check the
  collection path rather than the format, and the report prints both numbers so
  that 17 million fields cannot be read as more evidence than they are.
- **A run that compares nothing fails.** An image rebuilt without the two font
  packages still answers its version probe, so the corpus would come back empty
  and every number above would be zero - and a clean report of nothing reads
  exactly like a clean report of everything. The gate fails if no face was
  compared, and fails if every face compared was a fixture.

Adding two packages moved every differential's denominator, so all of them were
re-measured the same day and all are clean:

| gate | result, 2026-10-01 |
| --- | ---: |
| `check-oracle-eblc` | 6 faces, 650,980 glyphs, 7,246,262 fields, **0** |
| `check-oracle-ttx` | 362 faces, 349,372 fields, **0** (1 named skip: `mona.ttf`'s `OS/2` is short too) |
| `check-oracle-cmap` | 329 fonts, 634,109 codepoints, **0** |
| `check-oracle-glyf` | 316 fonts, 42,144 glyphs, 3,094,387 fields, **0** |
| `check-oracle-cff` | 47 fonts, 4,268 glyphs, 25,530 fields, **0** |
| `check-oracle-bitmap` | 239 fonts, 10,639 glyphs, 230,300 fields, **0** |
| `check-golden` | 6,421 renderings x 3 big-endian targets, **0** |

`mona.ttf` is compared by `glyf_diff` without complaint, which is worth noting
beside the three tables of it the reference cannot read: the font is short in
several places and sound everywhere else.

**And then the population turned out to be sixteen times larger**, so every
differential's denominator moved again the same day. Twelve more packages, the
sfnt corpus from 329 fonts to 485, and the embedded-strike corpus from 2 to 33:

| gate | result, 2026-10-01, over the full population |
| --- | ---: |
| `check-oracle-eblc` | vs fontTools 40 faces, 17,085,462 fields, **0**; vs FreeType 41 faces, 17,539,615 fields, **0** |
| `check-oracle-ttx` | 519 faces, 573,544 fields, **0** |
| `check-oracle-cmap` | 485 fonts, 1,128,934 codepoints, **0** |
| `check-oracle-glyf` | 473 fonts, 79,290 glyphs, 5,787,773 fields, **0** |
| `check-oracle-cff` | 47 fonts with charstrings of 534, 4,268 glyphs, 25,530 fields, **0** |
| `check-oracle-bitmap` | 239 fonts of 240, 10,639 glyphs, 230,300 fields, **0** |
| `check-golden` | 6,431 renderings x 3 big-endian targets, **0** |

Three of those were **not** clean on the first run, and all three were the
reference doing something other than reading the font:

- **`ttx_diff`, 9 fields:** three fonts store **empty** `post` glyph names - a
  Pascal string of length zero - and fontTools substitutes `glyph00800`, because
  its glyph order has to be a set of unique non-empty keys. Same class as the
  duplicate-name rename it already allowed, and narrowed the same way.
- **`ttx_diff`, 4 fields:** `Konatu.ttf` stores a `head.created` and `modified` of
  **zero**, and fontTools decides that is too low to be seconds since 1904 and
  reports it as though it were a Unix timestamp. It says so on its own stderr while
  doing it, so the adapter now captures that warning and emits
  `sanitised.head.created`, and the differential skips exactly the fields the
  reference admits it rewrote. An exemption list here would have gone on excusing
  the field after a fontTools release stopped needing it.
- **`glyf_diff`, 5 paths:** `UKIJTughra.ttf` has contours that **end in two
  coincident on-curve points** and begin off-curve, so the contour's start is one
  of an identical pair and the two readers pick different ones. One then walks from
  one duplicate to the other and emits a **zero-length line**; the other starts at
  the far one and emits none. Rotation could not reconcile that, because the
  segment *counts* differed. Both sides now drop a line to the point they already
  stand on, which is shape-preserving - no ink, no point moved - and the count
  (11,168 over the corpus) is printed rather than silent.

Each of those three allowances was planted and seen to fire. One clause did not:
the invented-name check also requires the reference's ordinal to be *this* glyph's
index, and making that unconditional changes no number in the gate, so it is a
precaution rather than a tested one. Said in the docstring rather than implied.

**A positive `descender` is resolved against `minAfterBL`, as of 2026-10-01.**
The specification does not state the sign of `sbitLineMetrics.descender`, and the
population writes it both ways: 30 faces negative, **Anonymous Pro's four
positive** with a `minAfterBL` of -2, and two zero. This library reported the byte,
so for those four it answered a `descent` of **+2** - against ::GFNT_Strike's own
documented convention, which says negative. A caller that trusted the
documentation put the descenders above the baseline.

`minAfterBL` is the same quantity measured a second way in the same record
("largest ink extent below the baseline"), so where the two disagree about
direction and it has committed to one, that is the statement to follow. FreeType
does the same comparison in `tt_face_load_strike_metrics` and comments it "fuzzy
wording in the EBLC documentation". The rule is deliberately narrow: a positive
descender whose `minAfterBL` is **not** negative is left alone, because there is no
second statement to prefer and flipping it would invent a direction.

Three things about how this is checked, because the obvious versions of each are
wrong:

- **The adapter must not apply the rule.** fontTools reports the byte, and an
  adapter that resolved it the way this library does would agree by construction
  and check nothing. It emits the two bytes instead, as a census line, and
  `eblc_diff` derives the expected descent from *those* - so the rule is checked
  against values the reference read on its own.
- **FreeType resolves it itself**, so for the faces where it reports a baseline at
  all - the bitmap-only ones - `descent` is compared directly and the agreement is
  real rather than arranged.
- **The narrow clause needed a unit test**, because dropping it changes no number
  in the whole population: nothing in Debian pairs a positive descender with a
  `minAfterBL` that is not negative. That mutation escaped the suite until a case
  was written, and the case cannot come from a real font. A third mutation -
  `>= 0` instead of `> 0` - escapes and should: negating zero is zero, so it is an
  equivalent mutant rather than a gap.

This was found while writing `EBSC`, whose records carry the same
`sbitLineMetrics` and inherited the same bug.

**`EBDT`'s composites are built, as of 2026-10-01**: image formats 8 and 9, so
every image format the specification defines except the two it calls obsolete is
read. A composite is a list of other glyphs of the same strike, each OR-ed into
the composite's own box at a signed pixel offset; nothing about that reaches the
public interface, because what comes back is composed pixels and a caller cannot
tell one from a glyph that stored its rows.

- **There is no second reader of these pixels anywhere.** Nothing in Debian has a
  composite - `uming.ttc` and `mona.ttf` between them carry none - and fontTools
  parses the component list without composing the image. So the gate is an
  identity: `strike-composite.ttf` pairs every composite with a plain glyph of the
  same strike that draws the same pixels, drawn from the generator's own
  arithmetic, and a transcription slip shows as two glyphs of one strike
  disagreeing. What fontTools *does* check is real and is the half that matters
  most: the component lists and their offsets come back identical, which is what
  pins **format 8's pad byte** - skip it and `numComponents` is read from the high
  half of the count.
- **The semantics came from FreeType's source, not from the specification's
  wording.** "Position of component left" does not say in what space or which way
  y runs. `tt_sbit_decoder_load_compound()` settles it: destination pixels, y
  down, OR-ed, the component's own bearings unused - so the three leaves of the
  fixture have bearings of (3,4), (-2,2) and (7,6) against the composites' (1,7),
  and a reader that positioned by them draws a different glyph.
- **A cycle is named rather than left to the depth cap**, by walking the chain of
  glyphs currently being painted. The cap alone would answer `ERR_LIMIT` after
  sixteen levels for `A -> B -> A`, which is true and useless: a caller can act on
  a limit by raising it and would raise it forever. `glyf` still names only the
  direct cycle, which is the same defect in the other composite reader and is
  recorded here rather than fixed in passing.
- **The canvas is a second scratch buffer, and it had to be.** A composite OR-s
  into one destination while each component's rows are normalised through the
  builder's existing row buffer on the way in; one buffer for both would have each
  component overwrite the composite it is being drawn into, and would do it
  invisibly, because the *first* component would still look right.

What that cost in findings:

- **A mutation that loosened a bounds check by one byte passed the whole suite.**
  Setting format 8's consumed-byte count to five instead of six leaves the
  *reading* correct - the reader has advanced past the pad either way - and only
  slackens the length check. Every refusal case in the composite table was a
  format 9 composite, so nothing exercised format 8's bound at all. The table has
  a format 8 case now, one byte short of its component.
- **A census line's keyword collided with another's.** `eblc_diff`'s report
  parses the reference's census by first word, and a new `census strike …
  composites …` line was read as the `census strike … flags …` line - so the
  report said a strike had `flags 4` and a bit depth of 6, which were the
  composite counts wearing those names. The numbers were nonsense and the table
  they printed still looked like a table.
- **Three fuzz seeds from the two commits before this one were never in the
  repository.** `tests/fuzz/corpus/.gitignore` excludes everything and re-admits
  `*.seed`, so that libFuzzer's findings stay out while the seeds go in; the EBLC
  seeds were named `seed-<fixture>.ttf`, which that pattern hides silently. They
  worked for whoever wrote them and did not exist for anybody else, and a figure
  recording half a million clean runs over one of them was a figure about a file
  no clone had. `make check-seeds` is now in `TEST_GATES` and fails on a file under
  the corpus whose name begins with `seed`, which is the spelling that was wrong.
- **`hasattr` on a fontTools bitmap glyph damages it.** Probing for
  `componentArray` to decide whether a glyph is a composite made a *format 1*
  glyph's metrics unreadable afterwards, and the failure surfaced on a different
  line than the probe: `BitmapGlyph.__getattr__` decompiles on a miss and consumes
  `self.data` doing it. The adapter decides from the subtable's `imageFormat` now,
  which is a fact about the font rather than about the object model.

**Not built:** `CFF2` (§16); `EBSC`,
and `CBDT` and `sbix` (§7.5); colour
(§7.6);
variations (§7.7); shaping, layout, discovery and the writer; the multi-byte
Macintosh and Microsoft `name` encodings (§7.2); `vhea`/`vmtx`, `gasp`, `kern`
and WOFF 1. `maxp` has no `_dump` because nothing
reads its fields beyond `numGlyphs`. `GFNT_Glyph`, the tagged union of §5.4, is
**still only its enum, and now deliberately rather than for want of a second
arm**: there are two kinds of glyph data, and each has an accessor of its own -
`gfnt_face_glyph_outline()` and `gfnt_face_glyph_bitmap()`. A caller that knows
which it wants asks for it; the union is for the caller that does not, which is a
layout engine, and one arrives with the shaper. Building it now would be inventing
the shape of a question nothing asks yet.

The `freetype` oracle `tools/oracle/containers/IMAGES` names - an outline and
coverage differential against FreeType's own `ftgrays` - is not built. It is the
next thing that would strengthen phase 1 rather than a gap in it: the outlines
are checked against fontTools over two million fields, and the coverage against
an independent sampler and three big-endian targets.

---

## References

- OpenType Specification 1.9 (Microsoft), and the Apple TrueType Reference
  Manual, for every sfnt table named above; the OpenType Layout Common Table
  Formats; the OpenType Font Variations Overview.
- X11 *Bitmap Distribution Format 2.1* and Adobe Technical Note #5005 (the BDF
  specification); the PCF format as documented in `libXfont`'s `pcfread.c`,
  `pcf.h` and `bdfToPcf`; `psf(5)` from `kbd` and `psf2.h` in its sources; GNU
  Unifont's `unifont-hex(5)` and its manual's "The .hex Format".
- Adobe Technical Note #5176 (The Compact Font Format), #5177 (The Type 2
  Charstring Format), #5015 (Type 1 Font Format supplement), the *Adobe Type 1
  Font Format* book; #5004 (AFM); the Adobe Glyph List and its specification.
- WOFF File Format 1.0 (W3C Recommendation); WOFF 2.0 for what is deferred.
- CSS Fonts Module Level 4 (matching); CSS Text Module Level 3 (`line-break`,
  min/max-content); CSS Inline Layout Module Level 3 (line metrics).
- UAX #9, #14, #24, #29 and the OpenType script development specifications
  (Arabic, Indic, USE), via `unicode`.
- HarfBuzz's documentation of clusters, `unsafe_to_break`, and its shaper
  pipeline; FreeType's `ftgrays.c` and the libart rasteriser it descends from;
  fontTools' `subset` and `fontBuilder` modules.
- PDF 32000-1:2008 §9.6-9.10 (simple, composite and Type 3 fonts, font
  descriptors, embedded font programs, `ToUnicode`).
- SIL Open Font License 1.1, in particular the Reserved Font Name clause; the
  Bitstream Vera licence; Apache-2.0.
- `CONVENTIONS.md` §4, §5, §7, §8, §11, §12; `libs/chron/documentation/design.md`
  §8.5, §8.6, §12; `libs/unicode/documentation/design.md`;
  `libs/cjelly/docs/Overview.md` §8 and §18, `docs/semantics.md`, and
  `docs/CurrentTask.md` Task 3.2.
