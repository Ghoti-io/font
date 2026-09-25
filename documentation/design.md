# The design of ghoti.io-font

**Status:** design, with most of phase 0 built. Tier 0 reads the sfnt
container, the metric tables, `cmap` and `name`; §18.1 lists what of phase 0 is
built and what is not, and it is the authority on what exists. Nothing above
tier 0 exists at all. This page says what will exist and why, so
that the code can be judged against it rather than the other way round. A
change of mind lands here first, in the same commit as the code that needs it
(`CONVENTIONS.md` §9). The workspace's `notes/font/SCOPE.md` is the scoping record
that preceded this - the measured inventory and the format survey. The
`unicode` library (`libs/unicode/documentation/design.md`) is a prerequisite
for tiers 2 and 3.

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
| **Secure** | The file is the attacker (§1.2). Every read of file data passes through one bounds-checked reader (§6); every recursion has a limit the Standard sets and the library enforces regardless of what the file claims; every unbounded quantity is capped by `GFNT_Limits`; and there is no bytecode interpreter (§2 M3). Fuzzers per format from the first phase, because layers 0-2 are offset arithmetic over hostile input. |
| **Deterministic** | Outlines, shaping and rasterisation are integer arithmetic in fixed point (§5.2, §8). The same font, size and text produce **byte-identical coverage on every platform and architecture**, and a golden-bitmap gate proves it on x86-64 and on the big-endian 32-bit cross container (§14.4). No platform rasteriser, no platform shaper, no `float` in a core type. |
| **Cross-platform** | Tiers 0-3 (§4) touch no operating-system API and behave identically everywhere by construction. Tier 4 - font discovery - is the only code that does, one file per platform, with the Windows and macOS branches marked per `CONVENTIONS.md` §11. |
| **Dependency-light** | `cutil`, `unicode`, and `compress` (gzip for `.pcf.gz`/`.psf.gz`, zlib for WOFF 1). `image` is optional, for PNG-bearing colour strikes and as a raster surface. Nothing else: not FreeType, not HarfBuzz, not fontconfig, not ICU. Each is an oracle in the tests and none is linked (§14). |
| **Enterprise-ready** | No global state: no process-wide font cache, no default face, no environment read outside tier 4 (§15.3). A face is immutable after load and shareable across threads; caches are objects the caller owns. Embedding permissions are reported and never enforced (§17.9). "Which face answered this glyph" and "which strike answered this size" are queryable, because fallback that cannot be audited is a bug report waiting to happen (§2 M8). |
| **Useful** | Three consumers are enumerated in §13 with their exact needs, and the library is designed against all three, not the first one. It draws *something* for any glyph it cannot find - glyph 0 is synthesised (§5.4) - and bundles no font of its own (§14.6): the application's fonts and the system's, through tier 4, are the only fonts there are. |

### 1.1 The shape is borrowed, deliberately

FreeType's face/glyph/outline model, HarfBuzz's buffer-and-cluster model, and
fontTools' table-object model are the references for what a font library
exposes, and each has decades of consumers. Where this library departs, §2
names the reason; where it does not, the reader can assume the reference's
semantics and conformance behaviour are the target. The layout tier borrows
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
| M24 | A font library that bundles a font, so every consumer ships a third-party licence obligation it never chose | Toolkits that embed a fallback face; its notice then travels with every binary every downstream distributor produces | No font is bundled (§14.6). Glyph 0 is synthesised in code, so a face with no usable glyphs still draws boxes; an application ships its own fallback as its own asset, or finds the system's through tier 4 |

---

## 3. The format universe

"Font" is four unrelated families that share a purpose. `notes/font/SCOPE.md`
§3 has the survey with measurements; what follows is the verdict.

| Family | Formats | Verdict |
| --- | --- | --- |
| **sfnt-wrapped** | `.ttf` (`glyf`), `.otf` (`CFF `), `.ttc`/`.otc`, WOFF 1 | **required**, phases 0-2 |
| | `EBDT`/`EBLC`/`EBSC` strikes; `COLR` v0 + `CPAL` | **wanted**, phases 1b and 6 |
| | `CBDT`/`CBLC`, `sbix`, `COLR` v1 | phase 6, `?image` |
| | variations: `fvar`/`avar`/`gvar`/`HVAR`/`VVAR`/`MVAR`/`STAT` | phase 4; **the API shape from phase 0** (§7.7) |
| | OpenType layout: `GDEF`/`GSUB`/`GPOS`/`BASE`/`JSTF` | **required**, phase 3 |
| | WOFF 2 | after Brotli lands in `compress`; **not here** |
| | `CFF2`, AAT (`morx`/`kerx`/...), `SVG `, `.dfont`, `.eot`, hinting | absent, §16 |
| **standalone bitmap** | PCF (`.pcf.gz`), BDF, PSF 1/2 | **wanted**, phase 1b - 1,885 of them on a stock Linux box; cheap |
| | GNU Unifont `.hex` | wanted as fallback data |
| | Windows FNT/FON, raw ROM fonts | absent |
| **standalone outline** | Type 1 (`.pfb`/`.pfa` + `.afm`/`.pfm`), bare `CFF` | **wanted**, phase 2 - all of TeX, and PDF |
| | Type 3, Type 42, Multiple Master, standalone SVG fonts, Metafont | absent; Type 3 is a PDF library's (§13.3) |
| | Hershey strokes | optional; needs no rasteriser |
| **sidecars** | AFM/PFM, `fonts.dir`/`fonts.alias`, the AGL, the OpenType language registry | with the formats that need them |

---

## 4. Tiers and modules

Five tiers, split by what a consumer pays for and what each touches. **Nothing
in tier *n* includes a tier *n+1* header**, and `make check-layering` enforces
it, as `chron` does.

| Tier | Holds | Needs | Consumers |
| --- | --- | --- | --- |
| 0 | blob, the checked reader, every table parser, metrics, `cmap`, strikes, glyph access, the glyph union | `cutil`, `compress` | everyone |
| 1 | outlines as paths; rasterisation to coverage | tier 0 | `image`, `cjelly`, PDF |
| 2 | shaping: `GDEF`/`GSUB`/`GPOS`, the script shapers, the cluster map | tier 0, `unicode` | `cjelly`, PDF (for text extraction) |
| 3 | layout: itemisation, bidi, breaking, paragraphs, boxes, hit testing | tier 2, `unicode` | `cjelly`, `image` |
| 4 | discovery and fallback: directories, matching, the platform APIs | tier 0, the OS | `cjelly` |
| W | writing and subsetting | tier 0 | PDF, tools |

`image` links 0, 1 and 3 (it wants "draw this wrapped label") and never pays
for discovery; a PDF reader links 0-2 and W; `cjelly` links everything. The
tier boundary is what keeps "should layout be its own library" an open option
that costs nothing to keep open.

### 4.1 The modules

| Header | Tier | Holds |
| --- | :-: | --- |
| `core.h` | 0 | `GFNT_Result`, `GFNT_Limits`, `GFNT_Error`, the fixed-point types, version |
| `blob.h` | 0 | `GFNT_Blob`: bytes with ownership and length; file, memory, mmap |
| `face.h` | 0 | `GFNT_Face`: one font from a blob; the table directory; `numGlyphs`; the strike list; `GFNT_Variation` |
| `metrics.h` | 0 | `head`, `hhea`/`hmtx`, `vhea`/`vmtx`, `OS/2`, `post`, `maxp`; ascent/descent policies; per-glyph advances and bounds |
| `cmap.h` | 0 | codepoint → glyph, all subtable formats; variation selectors; reverse lookup |
| `name.h` | 0 | `name` records decoded to UTF-8 |
| `glyph.h` | 0 | `GFNT_Glyph`, the tagged union; the strike-selection policy |
| `bitmap.h` | 0 | `EBDT`/`EBLC`, PCF, BDF, PSF, `.hex` strikes |
| `color.h` | 0 | `COLR`/`CPAL`, `CBDT`, `sbix` |
| `outline.h` | 1 | `GFNT_Outline`: the path; `glyf` and charstring producers; transforms; bounds |
| `raster.h` | 1 | the scan converter; `GFNT_Coverage`; the `GIMG_Raster` bridge |
| `charstring.h` | 0 | the Type 1 and Type 2 interpreters, container-independent (§7.4) |
| `shape.h` | 2 | `GFNT_ShapedRun`, features, the language registry, the script-shaper vtable |
| `layout.h` | 3 | `GFNT_Paragraph`, `GFNT_Line`, boxes, hit testing, the providers |
| `discover.h` | 4 | `GFNT_FontSet`, directory scanning, matching, the default fallback provider |
| `write.h` | W | the sfnt serialiser, the subsetter, WOFF 1, the PDF helpers |
| `font.h` | 0 | umbrella for tier 0 |

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
from phase 0, before any variation table is parsed - because adding the
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
are a second reader; when Brotli lands, WOFF 2 is a phase.

**PCF, BDF, PSF, `.hex`** are their own containers and their own glyph sources,
read through the same reader (PCF, PSF) or the line reader (BDF, `.hex`),
gzipped forms through `compress`. Each produces a `GFNT_Face` with one strike
and no outlines.

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

**Built so far**, and two deferrals that are the same deferral. `post`'s
header fields are read and its **glyph names are not**: format 2.0 spells the
first 258 of them as indices into the standard Macintosh glyph order, and that
list is a 258-entry vector. Macintosh `name` records decode as far as ASCII and
no further, because the rest needs the 128-codepoint Mac Roman table. §14's rule
is that a vector comes from an oracle and is never written from memory, and
fontTools holds both of these, so both wait for the fixture generator that can
check them. `vhea`/`vmtx`, `gasp` and `kern` are not built either, and are
phases of their own.

**`name`**: every record decoded by `(platformID, encodingID)` - Unicode and
Windows UTF-16BE, Windows symbol, Macintosh Roman and the other Mac encodings
by table, format 1 language tags - to UTF-8, with the preference order Windows
English, then Unicode, then Macintosh, then anything, documented and
overridable by a language argument. Name IDs 0-25 by constant; family, style,
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

### 7.5 Bitmap strikes

`EBLC`/`EBDT` (and Apple's `bloc`/`bdat` spellings): the `bitmapSizeTable`s,
index subtable formats 1-5, glyph bitmap formats 1-9 including the composite
formats 8 and 9 and the metrics-in-data forms; `EBSC` scaled references.
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
`avar`. Every accessor took a `GFNT_Variation *` from phase 0; phase 4 makes
the parameter do something.

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
concern and belong with tier 4 when it wants them.

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

Text layout is in this library up to the paragraph and not past it.
`notes/font/SCOPE.md` §5 has the argument in full; the short form: a
constraint-based layout system needs **min-content and max-content** widths
from every piece of text, both of which require break analysis over shaped
runs, so either this library exposes them or the GUI toolkit grows a second
shaper. And the cluster map does not survive a line boundary someone else
drew.

### 10.1 What is here and what is `cjelly`'s

| # | Thing | Where |
| --- | --- | --- |
| 1 | Shaping | `font`, tier 2 |
| 2 | Run measurement: advance sum, ink extents | `font`, tier 2 |
| 3 | Itemisation: by script, direction, style span, and by which face has the glyph | `font`, tier 3 |
| 4 | Break opportunities: UAX #14 lines, UAX #29 graphemes and words | `unicode`, applied here |
| 5 | Paragraph layout: wrap, stack, justify, align, truncate, tabs | `font`, tier 3 |
| 6 | Inline layout: inline images and widgets, mixed sizes on one line, ruby | `cjelly` |
| 7 | Box model: margins, padding, floats, flex, grid, constraints | `cjelly` (its Task 3.1) |

### 10.2 The promises

- **Two boxes, always.** The *typographic* box (line count × line height,
  width from advances) answers "how much room do I reserve"; the *ink* box
  (the union of positioned glyph bounds) answers "which pixels get touched".
  They differ for nearly every string. Both are returned, both are named.
- **Measurement without rasterisation.** Layout never touches tier 1's scan
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
provider is a vtable the caller supplies; tier 4 ships one over a `GFNT_FontSet`
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

## 11. Discovery and fallback (tier 4)

The only tier that touches the operating system, kept to one file per
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

## 12. Writing and subsetting (tier W)

The library writes fonts because two consumers need it: a PDF writer must
embed a *subset* of each font it uses (§13.3), and every fixture in §14.5 is
produced by subsetting. Writing is tier W, on tier 0 only.

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
and is phase 5's second half.

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

Tiers 0-4 and, for its GPU path, an atlas. `cjelly` owns the atlas and the
draw batching; this library gives it `GFNT_Coverage` at any sub-pixel offset,
the shaped runs with cluster maps its accessibility tree needs
(`semantics.md`), the paragraph layout with both boxes and hit testing its
widgets need, min-content and max-content for its constraint solver (Task
3.1), and `GFNT_FontSet` for its font manager. The atlas *format* - glyph key
(face, glyph, ppem, sub-pixel bin, variation) to rectangle plus metrics - is
defined here as `GFNT_AtlasEntry` so that a build step can bake one and
`cjelly` can load it, and so that the same key serves as the glyph cache key.
`cjelly`'s v0.1 "text (Latin fallback)" milestone needs tiers 0-1 and nothing
else.

### 13.2 `image`

Tiers 0, 1 and 3: `gimg_draw_text()` is a paragraph laid out here and
rasterised into a `GIMG_Raster` through the bridge in §8.2, with the
compositing (colour, blending, effects) on `image`'s side. `image` never links
tier 4; the caller hands it a face.

### 13.3 A PDF library, should one come

Designed for now because its needs are concrete and cheap to keep open:

| PDF needs | Here |
| --- | --- |
| `FontFile` (Type 1), `FontFile2` (TrueType), `FontFile3` (`Type1C`, `CIDFontType0C`, `OpenType`) | §7.1, §7.4: Type 1, sfnt, bare CFF including CID-keyed |
| Fonts with no `cmap`, `name` or `OS/2` | §7.8: per-operation requirements |
| Glyph names → Unicode for text extraction | `post` names, the **Adobe Glyph List** (vendored, BSD-3), the `uniXXXX`/`uXXXX[XX]` conventions, `gXX`/`cidXX` forms |
| The standard 14 fonts' metrics without their files | AFM reading (§7.1); the Core 14 AFMs are a fixture the PDF library carries |
| `StandardEncoding`, `WinAnsiEncoding`, `MacRomanEncoding`, `MacExpertEncoding`, the Symbol and ZapfDingbats built-in encodings | vendored tables in `write.h`'s PDF helpers, phase 5 |
| Embedding a subset with `CIDToGIDMap` identity, or renumbered with a `Differences` array | §12.3's `retain_gids` and its inverse |
| A `ToUnicode` CMap and a `W`/`Widths` array for the subset | phase 5 helpers over the subset's cluster and advance data |
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
| coverage is **byte-identical across platforms** | this library on x86-64 versus this library in the cross container (big-endian, 32-bit) | golden hashes in `tests/data/golden/`, `make check-golden` | `ghoti-xarch`, `tools/xarch/Containerfile`, already in the workspace |
| shaping is identical | **HarfBuzz** `hb-shape --output-format=json`: glyph ids, advances, offsets and clusters for every string in a per-script corpus, per font | `tools/oracle/hb_diff.py`, `make check-oracle-hb` | `harfbuzz`, built here: `hb-shape` from the pinned apt package, driven by `--text-file` and `--output-format=json` (a batch protocol already) |
| subsetting keeps what it should | fontTools `pyftsubset` with matching options; the closure sets compared | `tools/oracle/subset_diff.py` | `fonttools` |
| written fonts are valid | read back by this library, fontTools and FreeType (§12.5) | `make check-writer` | `fonttools`, `freetype` |
| line breaking and bidi | `unicode`'s conformance gates, already passed there | - | none: `unicode`'s committed conformance files |
| paragraph layout | **Pango** `pango-view --output` positions for a paragraph corpus, with the line-breaking differences that come from Pango's ICU tailorings recorded as known | `tools/oracle/pango_diff.py` | `pango`, built here: `pango-view` from the pinned apt package, `C.UTF-8` pinned in the image (`CONTAINERS.md` §1.1 is why) |
| bitmap formats | `bdftopcf` (BDF → PCF, then both read here and compared); `psftools` | `tools/oracle/bitmap_diff.sh` | `xfonts`, built here: `bdftopcf` and `psftools` |
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

For every corpus font, a fixed set of glyphs at 8, 12, 16, 24, 48 and 96 ppem
and at sub-pixel offsets 0, 1/4, 1/2 and 3/4, rasterised and hashed;
the hashes committed. `make check-golden` regenerates and compares, on the
build host and in the cross container. A hash that differs between the two
is a host-dependent read or a `float` that crept in, and this is the only
gate that sees it.

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

**What exists today is the last paragraph and nothing else.**
`tests/sfnt_builder.h` is the organised form of those hand-built arrays: it
assembles an sfnt, a `ttcf`, and each table this library reads, so that a test
can break exactly one field and name what it broke. Neither the `fonttools`
image nor the committed fixtures nor any `check-oracle-*` target exists yet, so
every test in the suite is a synthetic one - which is precisely the half that
flatters (§14.5's own point), and the reason the phase table keeps `ttx_diff`
and `cmap_diff` as unbuilt phase 0 work rather than treating the unit suite as
a substitute for them.

### 14.6 No font is bundled

The first draft compiled a subset face into tier 0 so that the library could
always draw a diagnostic string. It does not, and the reason is M24: a font
embedded in a library ships inside every consumer's binary, and its licence
notice - Vera's, OFL's - then has to travel with every binary every downstream
distributor of every consumer produces. A font library has no business making
that decision for its consumers.

What the library promises instead is narrower and needs no licence. **Glyph 0
is synthesised in code** (§5.4), so a face with no usable glyphs still draws a
hollow box per character and a run that no face could map still has geometry.
**Tier 4 enumerates the system's fonts** (§11), so an application that wants a
fallback finds one where the operating system keeps them. An application that
must draw text with no fonts on the system - a container, a fresh Windows
image - ships a font as its own asset under its own licence, which is where
that decision belongs; `cjelly` may well do so, and that is `cjelly`'s call.

### 14.7 The oracles run in containers

`notes/suite/CONTAINERS.md` §2 and §4 record the pattern - prototyped on
`regex`, measured, and shown to find defects the host had hidden - and this
library adopts it unchanged: `tools/oracle/containers/IMAGES` pins every
reference; `oracle_env.py` is the one place a reference is spelled;
`oracle_run.py` resolves it, prints `oracle(container): <version>`, and fails
closed on a pin mismatch; `ORACLE_MODE` is `container` or `host` with no
silent fallback; the repository is mounted read-only at its own host path with
`--network none`; every driver speaks a batch protocol. `make test` needs no
container: it reads committed vectors and golden hashes, and the container
gates are the separate `check-oracle-*` and `vectors` targets, which is the
seam that note leaves open and this library takes.

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
- **Every built-here image pins `C.UTF-8`.** `CONTAINERS.md` §1.1 found fifty
  false disagreements in the vim oracle from an unset `LANG`; `pango-view` and
  the Python tools are exposed the same way, and the image is where the
  locale is written down.

The real fonts live only in the oracle image, installed from Debian's
packages; the synthetic fixtures live only in the repository, mounted
read-only at the same path on both sides. Neither crosses, and no licence
question arises in the repository (§14.5).

### 14.8 The gates are themselves tested

Every gate above has been observed to fail before it is trusted: a byte
flipped in a corpus font fails `ttx_diff`; a point moved in the outline
producer fails `ft_outline`; an edge deposited with the wrong sign fails the
golden hashes; a lookup skipped fails `hb_diff` on the string that needed it;
a glyph dropped from the closure fails `subset_diff`. §12.3 of `chron`'s design
and this suite's history say why.

---

## 15. Code layout

```
include/ghoti.io/font/
  macros.h  libver.h  libver_gen.h  namespace.h  allocator.h     (CONVENTIONS §4)
  core.h  blob.h  face.h  metrics.h  cmap.h  name.h  glyph.h
  bitmap.h  color.h  charstring.h                                [tier 0]
  outline.h  raster.h                                            [tier 1]
  shape.h                                                        [tier 2]
  layout.h                                                       [tier 3]
  discover.h                                                     [tier 4]
  write.h                                                        [tier W]
  font.h                                                         umbrella, tier 0
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
`max_outline_points` (65,536 per glyph), `max_contours` (4,096), `max_ppem`
(4,096), `max_raster_bytes` (64 MiB per glyph), `max_strikes` (256),
`max_name_records` (4,096), `max_axes` (64), `max_lookup_depth` (6),
`max_ops_per_glyph` (64), `max_paint_depth` (64), `max_run_bytes` (16 MiB),
`max_line_length` for BDF and `.hex` (4,096). Every parser takes one; `NULL`
means default. This is what makes the fuzzers meaningful: a limit is a stated
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
3. **Bitmap fonts are first-class, from phase 0's API.** Decided (§5.3, §5.4).
4. **Outlines and rasterisation both live here**, writing into `GIMG_Raster`
   when `image` is present and a byte buffer when not; `image` stays optional
   (§8.2).
5. **Shaping is ours**, script by script, declared. `cjelly`'s Overview says
   so and the cluster map is the reason (§9.2).
6. **`GFNT_Variation` on every accessor from phase 0** (§5.3).
7. **The charstring interpreters are container-independent** (§7.4).
8. **No `GFNT_Stream`; a blob and a checked reader** (§5.1, §6).
9. **`fsType` is reported, never enforced.** A library that refuses to render a
   font because of an embedding bit breaks every legitimate viewer; a library
   that embeds one into a document without telling the caller breaks the law
   for them. Reporting is the only defensible position, and the consumer that
   embeds (§13.3) decides.
10. **Fixed point everywhere; no `float` in a core type** (§5.2).
11. **Discovery is a tier, fontconfig is not linked** (§11).
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

---

## 18. Plan

Phases, in dependency order, with the milestone each unlocks. Sizes follow
`regex`'s `plan.md` and `chron`: S up to a week, M two to four, L four to
eight, for one engineer who knows the suite. **Tiers 0 and 1 need no Unicode at
all; tier 2 needs `unicode`'s phase B and tier 3 needs its B and C.** This
sentence had B and C the wrong way round until 2026-09-24: phase B is
normalisation, bidi and the shaping properties - joining, Indic, USE, emoji,
mirroring, vertical orientation - which is what a shaper consumes, and phase C
is segmentation, which is what a paragraph layout consumes. `unicode`'s design
§16 phase table is the authority on which is which.

As of 2026-09-24 `unicode`'s phases A, B, C and D are built and installed, so
every Unicode input tiers 2 and 3 need already exists: `guni_joining_type` and
`guni_joining_group`, the Indic categories, the emoji properties,
`guni_vertical_orientation`, `guni_bidi_levels` and `guni_bidi_reorder`,
`guni_normalize` in all four forms, and `break.h` with all four algorithms, the
iterator shape this library asked for, `GUNI_BreakProvider` for the dictionary
seam and CSS Text's line-break tailorings. Phases E and F are `regex` and
`ctang` dropping their duplicates and cannot change what this library consumes.
`unicode`'s §16 says nothing in `font` that needs Unicode starts before E, which
is a scheduling decision on that side rather than a technical block on this one;
either way phase 0 here needed none of it and was built against `cutil` alone.

The decided order across the two libraries is `unicode` first; this table records
the actual dependency so that the scheduling is a choice rather than a
constraint.

| Phase | Work | Size | Gate | Unlocks |
| --- | --- | --- | --- | --- |
| **0** | Scaffold from `model` per `CONVENTIONS.md` §12; `core.h`, the fixed-point types, `GFNT_Limits`, `GFNT_Error`; `blob.h`; **the checked reader and `check-reader`**; sfnt and `ttcf` directories; `head`, `maxp`, `hhea`/`hmtx`, `OS/2`, `post`, `name`; `cmap` 4 and 12; **the strike list, `GFNT_StrikePolicy`, `GFNT_GlyphKind` and `GFNT_Variation` in the API**; `_dump` for every table; `fuzz_sfnt`, `fuzz_cmap`; the fixture tool and the first synthetic fixtures; the oracle image with Debian's fonts; `ttx_diff` and `cmap_diff` | M | `check-symbols`, `check-layering`, `check-reader`; `ttx_diff` clean over the corpus; every gate observed to fail | **F1: "what is this file, what does it contain, which glyph is this codepoint, how wide is it"** |
| **1** | `glyf`/`loca` with composites; `outline.h`; the scan converter and `GFNT_Coverage`; the `GIMG_Raster` bridge; `ft_outline`, `ft_raster`; the golden-bitmap gate on both architectures; `fuzz_glyf`, `fuzz_raster`; the truncation sweep | L | outlines identical to FreeType for every glyph of every TrueType fixture; golden hashes identical across architectures | **F2: `cjelly` draws unhinted Latin. Its v0.1 "text (Latin fallback)" is reachable here** |
| **1b** | PSF, BDF, PCF over `compress`'s gzip; `.hex`; `EBLC`/`EBDT`; the strike policy exercised by real strikes; `bitmap_diff` | M | `bdftopcf` round trip; every strike of Terminus renders identically from BDF, PCF and PSF | **F3: the 1,885 bitmap fonts on a stock Linux box load; console and pixel UIs** |
| **2** | `charstring.h` with Type 2; `CFF ` and CID-keyed; bare CFF; Type 1 and AFM/PFM on the same module; `cmap` 0, 2, 6, 8, 10, 13, 14; `kern`; WOFF 1; `fuzz_cff`, `fuzz_type1`, `fuzz_woff` | L | FreeType outline differential over every CFF and Type 1 fixture; `ttx_diff` over the new tables | **F4: most desktop fonts, all of TeX, and every font program a PDF embeds** |
| **3** | `GDEF`/`GSUB`/`GPOS`/`BASE` parsing and the applier; the default shaper; the language registry; `GFNT_ShapedRun` **with the cluster map from the first line**; Latin, Greek, Cyrillic, Hebrew; fallback mark positioning; `hb_diff`; `fuzz_layout_tables`, `fuzz_shape` | L | `hb-shape` identical for the Latin/Greek/Cyrillic/Hebrew corpus on every fixture | **F5: ligatures, real kerning, marks; `cjelly`'s accessibility requirement is satisfiable** |
| **3b** | Tier 3: itemisation over `unicode`'s script runs and bidi; breaking with the three tailorings and the `SA` seam; `GFNT_Paragraph`, lines, both boxes, `min_content`/`max_content`, alignment, greedy wrap, truncation, tabs; hit testing both ways; `GFNT_FontProvider`; `pango_diff`; the layout properties | L | §14.1 properties 3 and 5 over random paragraphs; `pango_diff` with its known differences enumerated | **F6: `cjelly`'s constraint system can ask for min- and max-content; text wraps and can be selected** |
| **4** | `fvar`/`avar`/`gvar`/`HVAR`/`VVAR`/`MVAR`/`STAT`; `FeatureVariations`; the `GFNT_Variation` parameter made live; `fuzz_var` | L | FreeType outline differential at a lattice of instances; `hb_diff` at instances | **F7: one file per family; weight is a number** |
| **4b** | Tier 4: directory scanning, the record, `GFNT_FontSet` with serialisation, CSS matching, the default fallback provider; `TODO(windows)` and `TODO(macos)` entries in `notes/suite/WINDOWS-TODO.md` | M | `fc-list` agreement on the set of families found on Linux | **F8: "a font for this string"** |
| **5** | Tier W: the sfnt and `CFF` writers; the subsetter with composite and `GSUB` closure, `retain_gids`; WOFF 1 writing; the PDF helpers (AGL, the encodings, `ToUnicode`, widths); `subset_diff`; `check-writer`; `fuzz_writer`; the parse-write-parse property over the corpus | L | every written font read back by three readers; `pyftsubset` closure agreement | **F9: PDF embedding is possible; fixtures regenerate from inside the suite** |
| **6** | `COLR` v0/v1 and `CPAL`; `CBDT`/`CBLC` and `sbix` with the `image` bridge; `GFNT_AtlasEntry` and atlas emission; `fuzz_color` | L | fontTools-built `COLR` v1 fixture with every paint format parsed to the same graph as `ttx` shows | **F10: emoji; `cjelly`'s GPU path fed from a baked atlas** |
| **7** | Complex shapers, one per phase, each declared: Arabic (joining, `rlig`/`calt`, the no-`GSUB` fallback) M; Thai/Lao M; Hangul S; Devanagari L, then each further Indic script M; Khmer M; Myanmar M; the USE L | per script | `hb_diff` identical for that script's corpus before the shaper is declared | **F11: each script, as it lands** |
| **—** | Deliberately absent until argued for: §16 | | | |

### 18.1 What of phase 0 is built

Kept here rather than in a commit message because a reader asking "can it do X
yet" has this page open, and because the list is what the next phase starts
from. Last revised 2026-09-24.

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

**Not built, and phase 0 is not finished without it:** `tools/fixtures/`, the
committed fixtures, and every oracle - the `fonttools` image, `ttx_diff`,
`cmap_diff`, `check-fixtures`. Until those exist every test here is synthetic,
which is the half §14.5 says flatters, so the differentials are the remaining
work rather than a nicety. `post`'s glyph names and Mac Roman name records wait
on the same image (§7.2). `maxp` has no `_dump` because nothing reads its
fields beyond `numGlyphs`; it gets one when something does. `vhea`/`vmtx`,
`gasp` and `kern` are unbuilt, as is WOFF 1.

Each phase ends with `make test`, `test-valgrind`, `test-asan`, `fuzz`,
`check-symbols`, `check-layering`, `check-reader`, `check-fixtures` and
`check-golden` clean from an empty build directory, serially and under `-j`,
per `CONVENTIONS.md` §12 item 10 - and, for phases 1 onward, clean in the
cross container. Phases 1b, 4b, 5 and 6 are independent of one another and of
3/3b; the one hard ordering is 0 → 1 → 2 → 3 → 3b, with 4 needing 3 for
`FeatureVariations`.

Phases 0 and 1 are the smallest useful thing - between `model` and `cjelly` in
size, with the scan converter as the only genuinely hard part. Phases 0-2 cover
most of the world's desktop fonts. The whole table is the largest library in
the suite, and most of that is phase 7.

---

## References

- OpenType Specification 1.9 (Microsoft), and the Apple TrueType Reference
  Manual, for every sfnt table named above; the OpenType Layout Common Table
  Formats; the OpenType Font Variations Overview.
- Adobe Technical Note #5176 (The Compact Font Format), #5177 (The Type 2
  Charstring Format), #5015 (Type 1 Font Format supplement), the *Adobe Type 1
  Font Format* book; #5004 (AFM); the Adobe Glyph List and its specification.
- WOFF File Format 1.0 (W3C Recommendation); WOFF 2.0 for what is deferred.
- X11 *Bitmap Distribution Format 2.1*; the PCF format as documented in
  `libXfont`; the Linux `psf` format as documented with `kbd`.
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
  §8.5, §8.6, §12; `notes/font/SCOPE.md`; `libs/unicode/documentation/design.md`;
  `libs/cjelly/docs/Overview.md` §8 and §18, `docs/semantics.md`, and
  `docs/CurrentTask.md` Task 3.2.
