/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * One strike, however it was spelled: the shape every bitmap container parses
 * into, and the four that do.
 *
 * documentation/design.md sections 7.1 and 7.5. PCF, BDF, PSF and `.hex` have
 * nothing in common as files - one is a compiled table of contents, one is a
 * text grammar, one is a console header and one is a line per codepoint - and
 * they have everything in common as fonts: a pixel size, a list of glyphs each
 * with its own box and advance, a mapping from codepoint to glyph, and no
 * outlines anywhere. So there is one ::GFNT_BitmapFont and four parsers that
 * fill it, and every accessor above this file is written once.
 *
 * Two normalisations happen here rather than in a caller, and both are in
 * `bitmap.h`'s documentation because they are API:
 *
 *  - **Rows top to bottom, bits MSB-first, one row starting on a byte.** PCF is
 *    the reason: it stores bits either way round, in scan units of one, two or
 *    four bytes, with rows padded to one, two, four or eight, and the four
 *    choices are independent. A caller handed the raw rows would have to
 *    implement the cross-product to draw anything.
 *  - **Pixels, and no em.** There is no `head` in any of these files and nothing
 *    to scale from, so ::gfnt_face_units_per_em() refuses on such a face rather
 *    than answering 1000 (M9 is what that answer leads to).
 *
 * The **glyph order is this library's**, as it is for Type 1: nothing in a PSF
 * or a `.hex` says which glyph is number three. It is the order the file lists
 * them in - PCF's metrics order, PSF's glyph order, BDF's `CHARS` order, the
 * order `.hex` gives its lines - and one function per container decides it so
 * that there is one place to read.
 */

#ifndef GHOTI_IO_GFNT_BITMAP_INTERNAL_H
#define GHOTI_IO_GFNT_BITMAP_INTERNAL_H

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag of a BDF face's one synthetic directory entry: `BDF `. */
#define GFNT_TAG_BDF GFNT_TAG('B', 'D', 'F', ' ')
/** @brief The tag of a PSF face's one synthetic directory entry: `PSF `. */
#define GFNT_TAG_PSF GFNT_TAG('P', 'S', 'F', ' ')
/** @brief The tag of a `.hex` face's one synthetic directory entry: `HEX `. */
#define GFNT_TAG_HEX GFNT_TAG('H', 'E', 'X', ' ')

/**
 * The tags a PCF face's synthetic directory carries, one per table it found.
 *
 * PCF types are small integers rather than tags, so unlike a bare CFF's `CFF `
 * these are *invented* - and they are invented rather than avoided because the
 * alternative is one entry spanning the file, which would hand every PCF table
 * parse a reader over the whole thing. The point of the directory is that a
 * table cannot reach into its neighbour, and PCF is the container here with the
 * most neighbours to reach into.
 */
#define GFNT_TAG_PCF_PROPERTIES GFNT_TAG('P', 'R', 'O', 'P')
#define GFNT_TAG_PCF_ACCELERATORS GFNT_TAG('A', 'C', 'C', 'L')
#define GFNT_TAG_PCF_METRICS GFNT_TAG('M', 'T', 'R', 'C')
#define GFNT_TAG_PCF_BITMAPS GFNT_TAG('B', 'M', 'A', 'P')
#define GFNT_TAG_PCF_INK_METRICS GFNT_TAG('I', 'M', 'T', 'R')
#define GFNT_TAG_PCF_ENCODINGS GFNT_TAG('E', 'N', 'C', 'O')
#define GFNT_TAG_PCF_SWIDTHS GFNT_TAG('S', 'W', 'I', 'D')
#define GFNT_TAG_PCF_GLYPH_NAMES GFNT_TAG('G', 'N', 'A', 'M')
#define GFNT_TAG_PCF_BDF_ACCELERATORS GFNT_TAG('B', 'A', 'C', 'C')

/** What a record's `name` or a font's string field holds when there is none. */
#define GFNT_BITMAP_NO_STRING ((size_t)-1)

/**
 * Whether a strike carries a glyph, and whether its bytes made sense.
 *
 * Three states and not two, because M11 is a policy of this library: per-glyph
 * corruption is per glyph, so one unreadable glyph must not condemn the strike it
 * is in - a face with one bad `loca` entry loads and answers for every other
 * glyph, and a strike works the same way.
 *
 * Zero is ::GFNT_BITMAP_ABSENT so that a zeroed record is an absent one, which is
 * what ::gfnt_bitmap_build_absent() relies on.
 *
 * A standalone container has no absent glyphs - the file *is* the glyph list - and
 * no corrupt ones either: it refuses the whole file rather than one glyph of it,
 * because its glyph count comes from the same bytes as its glyphs. Both of the
 * other states belong to `EBLC`, where a strike is sparse over a glyph count that
 * came from `maxp`.
 */
typedef enum {
  /** This strike has no bitmap for the glyph. Not an error; another may.    */
  GFNT_BITMAP_ABSENT = 0,
  /** The glyph is here and was read.                                        */
  GFNT_BITMAP_PRESENT,
  /** The strike claims the glyph and its bytes do not make sense (M11).     */
  GFNT_BITMAP_CORRUPT
} GFNT_BitmapPresence;

/**
 * One glyph: where its rows are, how big its box is, and where the box sits.
 *
 * `offset` indexes ::GFNT_BitmapFont::pixels, which holds every glyph's rows
 * back to back in normalised form. A glyph with no pixels - a space, or a PCF
 * glyph whose box is empty - has a `height` of zero, and then `offset` is not
 * read.
 */
typedef struct GFNT_BitmapRecord {
  uint32_t width;     ///< Pixels across.
  uint32_t height;    ///< Rows.
  size_t stride;      ///< Bytes per row: `(width + 7) / 8`.
  size_t offset;      ///< Where the rows start in the arena.
  int32_t bearing_x;  ///< Left side bearing, pixels; may be negative.
  int32_t bearing_y;  ///< Top row's edge above the baseline, y-up.
  int32_t advance;    ///< Pen movement, pixels.
  size_t name;        ///< Offset into `text`, or ::GFNT_BITMAP_NO_STRING.
  GFNT_BitmapPresence presence; ///< Whether this strike has the glyph, and readably.
  /**
   * Why a ::GFNT_BITMAP_CORRUPT glyph is corrupt, or NULL.
   *
   * Every diagnostic in this library is a string literal, so keeping one per glyph
   * costs a pointer and no allocation - and without it a per-glyph refusal would
   * report "something was wrong with this glyph" where the parse knew exactly
   * what. The first version lost that, and the test table that asserts nine
   * different sentences collapsed to nine copies of one.
   */
  const char * reason;
} GFNT_BitmapRecord;

/** One codepoint the font maps, and the glyph it maps to. */
typedef struct GFNT_BitmapMapping {
  uint32_t codepoint; ///< The character.
  uint32_t glyph;     ///< The glyph index it names.
} GFNT_BitmapMapping;

/**
 * A parsed bitmap font: one strike, its glyphs, and its own idea of itself.
 *
 * Three arenas, each allocated once and owned by the face: `pixels` for the
 * normalised rows, `text` for every string (glyph names and the font's own
 * names, NUL-terminated, offsets into it), and `map` for the encoding sorted by
 * codepoint. A record points into them rather than holding pointers so that
 * growing an arena cannot leave a record dangling - which is the defect a
 * reallocation in the middle of a parse writes.
 */
typedef struct GFNT_BitmapFont {
  GFNT_Tag container;  ///< Which format: ::GFNT_FLAVOUR_PCF and friends.
  GFNT_Strike strike;  ///< The one strike, with its ppem and its baseline.

  GFNT_BitmapRecord * glyphs; ///< Per glyph, in this library's order.
  size_t glyph_count;

  uint8_t * pixels;    ///< Every glyph's rows, normalised, back to back.
  size_t pixel_length;

  char * text;         ///< NUL-terminated strings, indexed by offset.
  size_t text_length;

  GFNT_BitmapMapping * map; ///< Sorted by codepoint, then by glyph.
  size_t map_count;

  /**
   * Whether the file states a mapping from characters to glyphs at all.
   *
   * False for a PSF with no Unicode table, whose glyph indices are positions in
   * a console's character generator and not characters. Reporting them as
   * codepoints would be inventing a mapping the font does not have, so
   * ::gfnt_face_glyph_for_codepoint() refuses on such a face by name.
   */
  bool states_encoding;
  /**
   * Whether the file states where the baseline is.
   *
   * False for `.hex`, which is a codepoint and a row of bits and nothing else.
   * The strike then reports the box it does state - an ascent of the full height
   * and a descent of zero - and this flag is why that is not a claim.
   */
  bool states_baseline;

  bool has_default_char;  ///< Whether a default character is stated.
  uint32_t default_char;  ///< The codepoint of it, when one is.

  size_t family;    ///< Family name in `text`, or ::GFNT_BITMAP_NO_STRING.
  size_t full_name; ///< The font's own full name, or none.
  size_t copyright; ///< A copyright or licence notice, or none.
  size_t weight;    ///< A weight name, or none.
} GFNT_BitmapFont;

/**
 * Whether these bytes are one of the four bitmap containers, and which.
 *
 * Ordered strongest magic first, as ::gfnt_face_bare_container() is: PSF and PCF
 * identify themselves in their first bytes, BDF in a keyword, and `.hex` has no
 * magic at all - it is recognised by its own grammar, which is why it is last
 * and why the probe insists on several whole lines of it.
 *
 * @param blob A reader over the whole blob.
 * @param out_flavour Receives ::GFNT_FLAVOUR_PCF and friends, on a match.
 * @param out_tag Receives the tag for the synthetic entry, on a match.
 * @return Whether anything matched.
 */
bool gfnt_bitmap_looks_like(const GFNT_Reader * blob, GFNT_Tag * out_flavour,
    GFNT_Tag * out_tag);

/**
 * Give a bitmap face its directory, and parse its strike into the memo.
 *
 * Called from the face load, after ::gfnt_bitmap_looks_like() said which
 * container this is. PCF builds a directory of its own from its table of
 * contents; the other three take one entry spanning the file.
 *
 * @param face The face. Its blob, allocator, limits and lock must be set.
 * @param flavour Which container, from the probe.
 * @param tag The synthetic entry's tag, from the probe.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or what the container's own parse returned.
 */
GFNT_Result gfnt_bitmap_derive(GFNT_Face * face, GFNT_Tag flavour, GFNT_Tag tag,
    GFNT_Error * error);

/**
 * The face's parsed strike, parsing it on first use.
 *
 * @param face The face.
 * @param out_font Receives it. Borrowed; it lives as long as the face.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face that is not one of these
 *   containers; or what the parse returned.
 */
GFNT_Result gfnt_face_bitmap(const GFNT_Face * face,
    const GFNT_BitmapFont ** out_font, GFNT_Error * error);

/**
 * Release everything a parse allocated. Harmless on a zeroed struct.
 *
 * `void *` so that it is ::gfnt_table_cached()'s release hook itself; see
 * ::gfnt_eblc_release() for why a typed wrapper would be unreachable code.
 */
void gfnt_bitmap_release(const GFNT_Allocator * allocator, void * table);

/**
 * This glyph's name, for `glyph.h`'s name accessors.
 *
 * @param font The parsed font.
 * @param glyph The glyph index.
 * @param out_name Receives a pointer into the font's own arena, NUL-terminated.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph past the count;
 *   ::GFNT_ERR_UNSUPPORTED for a container that states no glyph names - PSF and
 *   `.hex` state none at all, and a PCF may leave the table out.
 */
GFNT_Result gfnt_bitmap_glyph_name(const GFNT_BitmapFont * font, uint32_t glyph,
    const char ** out_name);

/**
 * The glyph a codepoint maps to, or ::GFNT_ERR_INVALID when none does.
 *
 * A bisection over the sorted map. Where several codepoints name one glyph each
 * is present; where one codepoint names several glyphs - which no format here
 * permits and a corrupt one can still spell - the lowest glyph wins, because the
 * map is sorted by glyph within a codepoint and the answer has to be stable.
 */
GFNT_Result gfnt_bitmap_glyph_for_codepoint(const GFNT_BitmapFont * font,
    uint32_t codepoint, uint32_t * out_glyph);

/** One of the font's own strings, or NULL when it states none. */
const char * gfnt_bitmap_string(const GFNT_BitmapFont * font, size_t offset);

/**
 * A builder the four parsers share, so that the arenas are grown one way.
 *
 * Each parser walks its own format and calls ::gfnt_bitmap_build_glyph() per
 * glyph and ::gfnt_bitmap_build_map() per mapping; this holds the buffers those
 * append to and the counts that bound them.
 */
typedef struct GFNT_BitmapBuild GFNT_BitmapBuild;

/**
 * Start building a font of at most @p glyph_count glyphs.
 *
 * @param build Receives the builder, allocated on @p face's allocator.
 * @param face The face, for its allocator and its limits.
 * @param container Which format, for the diagnostics.
 * @param glyph_count How many glyphs the file claims. Checked against
 *   ::GFNT_Limits::max_glyphs here, once, so that no parser has to.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_bitmap_build_start(GFNT_BitmapBuild ** build,
    const GFNT_Face * face, GFNT_Tag container, size_t glyph_count,
    GFNT_Error * error);

/**
 * Add one glyph, given its rows in the container's own layout.
 *
 * This is where the normalisation happens, once for all four formats: @p rows is
 * @p height rows of @p source_stride bytes each, and @p bit_order, @p byte_order
 * and @p scan_unit say how the container laid the bits out. A format with no
 * choice in the matter passes ::GFNT_ORDER_MSB_FIRST and a scan unit of 1.
 *
 * @param build The builder.
 * @param rows The glyph's rows, or NULL for a glyph with no pixels.
 * @param source_stride Bytes per row as the file has them, padding included.
 * @param bit_order Which end of a byte the leftmost pixel is at.
 * @param byte_order Which end of a scan unit the first byte is at.
 * @param scan_unit Bytes per scan unit: 1, 2 or 4.
 * @param metrics The box, the bearings and the advance.
 * @param name The glyph's name, or NULL.
 * @param name_length Its length, excluding any NUL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_bitmap_build_glyph(GFNT_BitmapBuild * build,
    const uint8_t * rows, size_t source_stride, GFNT_ByteOrder bit_order,
    GFNT_ByteOrder byte_order, unsigned scan_unit,
    const GFNT_BitmapRecord * metrics, const char * name, size_t name_length,
    GFNT_Error * error);

/**
 * Add a glyph this strike does not carry.
 *
 * Only an `EBLC` strike has these: it is sparse over the face's glyph count, and
 * the records are indexed by the face's glyph id so that a lookup needs no
 * translation. The record is zeroed with ::GFNT_BitmapRecord::present false.
 */
GFNT_Result gfnt_bitmap_build_absent(GFNT_BitmapBuild * build,
    GFNT_Error * error);

/**
 * Add a glyph the strike claims and whose bytes do not make sense.
 *
 * M11: the refusal is recorded against *that glyph* and the rest of the strike
 * still answers. ::gfnt_face_glyph_bitmap() returns ::GFNT_ERR_CORRUPT for it.
 */
GFNT_Result gfnt_bitmap_build_corrupt(GFNT_BitmapBuild * build,
    const char * reason, GFNT_Error * error);

/**
 * Unpack bit-aligned rows into byte-aligned ones.
 *
 * `EBDT`'s image formats split on exactly this: 2, 5, 7 and 9 pack the next row
 * at the next *bit*, where 1, 6 and 8 start each row on a byte. Everything above
 * this file wants byte-aligned rows (`bitmap.h` promises them), and
 * ::gfnt_bitmap_build_glyph() takes them, so the bit-aligned formats are widened
 * here first and then go through the same path as every other container.
 *
 * @param out Receives `height * ((width + 7) / 8)` bytes, padding bits zero.
 * @param source The packed bits, MSB-first, starting at the first pixel.
 * @param available How many bytes of @p source may be read.
 * @param width Pixels per row.
 * @param height Rows.
 * @return false when @p available does not hold `width * height` bits.
 */
bool gfnt_bitmap_widen_rows(uint8_t * out, const uint8_t * source,
    size_t available, uint32_t width, uint32_t height);

/**
 * Borrow the builder's scratch **canvas**, grown to @p bytes and **zeroed**.
 *
 * A second buffer, and it has to be: `EBDT`'s composite image formats 8 and 9
 * build a glyph by OR-ing their components into one destination, and each
 * component's own rows are normalised through
 * ::gfnt_bitmap_build_rows()'s buffer on the way in. One buffer for both would
 * have each component clobber the composite it is being drawn into - and would do
 * it invisibly, because the first component of a composite would still look
 * right.
 *
 * Zeroed, because a composite *accumulates*: the buffer outlives one glyph and a
 * composite that did not clear it would inherit the previous glyph's pixels
 * wherever its own components did not reach.
 *
 * The pointer is valid until the next call to this.
 */
GFNT_Result gfnt_bitmap_build_canvas(GFNT_BitmapBuild * build, size_t bytes,
    uint8_t ** out_canvas, GFNT_Error * error);

/**
 * Borrow the builder's scratch row buffer, grown to @p bytes.
 *
 * The builder already keeps one for the normalisation, sized to the widest source
 * row it has seen. `EBDT`'s bit-aligned formats need a whole glyph's worth rather
 * than a row's, and they need it *before* calling
 * ::gfnt_bitmap_build_glyph() - so the buffer is shared rather than allocated per
 * glyph, which for a 27,000-glyph strike is 27,000 allocations saved.
 *
 * The pointer is valid until the next call to this or to
 * ::gfnt_bitmap_build_glyph(), both of which may grow it.
 */
GFNT_Result gfnt_bitmap_build_rows(GFNT_BitmapBuild * build, size_t bytes,
    uint8_t ** out_rows, GFNT_Error * error);

/**
 * Map @p codepoint to @p glyph.
 *
 * Order does not matter; the map is sorted when the build finishes.
 */
GFNT_Result gfnt_bitmap_build_map(GFNT_BitmapBuild * build, uint32_t codepoint,
    uint32_t glyph, GFNT_Error * error);

/**
 * Add a string to the arena and return its offset, for a font's own names.
 *
 * @p text must not be NULL: a caller with no string writes
 * ::GFNT_BITMAP_NO_STRING itself, because "states no name" and "states the empty
 * name" are different facts and only the caller knows which it has.
 */
GFNT_Result gfnt_bitmap_build_string(GFNT_BitmapBuild * build,
    const char * text, size_t length, size_t * out_offset, GFNT_Error * error);

/** How many glyphs have been added so far. */
size_t gfnt_bitmap_build_count(const GFNT_BitmapBuild * build);

/**
 * Finish: sort the map, hand the arenas to @p out_font, and free the builder.
 *
 * Called on success and on failure both - on failure with @p keep false, which
 * frees the arenas instead of handing them over - so that a parser has one exit.
 *
 * @param build The builder. Freed either way; the pointer must not be used
 *   again.
 * @param out_font Where to put the font, when @p keep.
 * @param keep Whether the parse succeeded.
 */
void gfnt_bitmap_build_finish(GFNT_BitmapBuild * build,
    GFNT_BitmapFont * out_font, bool keep);

/** The builder's strike, for a parser to fill in as it learns the numbers. */
GFNT_Strike * gfnt_bitmap_build_strike(GFNT_BitmapBuild * build);

/** The builder's font, for the fields a parser sets directly. */
GFNT_BitmapFont * gfnt_bitmap_build_font(GFNT_BitmapBuild * build);

/**
 * Whether this face is one of the standalone bitmap containers.
 *
 * Answered from the flavour, so it costs nothing: every caller asks it in order
 * to decide whether to begin a bitmap parse at all.
 */
bool gfnt_face_is_bitmap(const GFNT_Face * face);

/**
 * How many glyphs a bitmap face has, for the numGlyphs minimum (M12).
 *
 * The same shape as ::gfnt_type1_glyph_bound() and ::gfnt_cff_glyph_bound(): a
 * container with no `maxp` states its count somewhere else, and there is exactly
 * one source, so it is the count rather than a bound that could disagree.
 *
 * @return false for a face that is not one of these containers, or whose parse
 *   failed - in which case it constrains nothing.
 */
bool gfnt_bitmap_glyph_bound(const GFNT_Face * face, size_t * out_bound);

/**
 * One glyph's name, counted when @p out is NULL and written otherwise.
 *
 * The two-pass contract `glyph/names.c` gives every name source.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph past the count;
 *   ::GFNT_ERR_UNSUPPORTED for a container that states no names;
 *   ::GFNT_ERR_LIMIT for a name longer than @p capacity.
 */
GFNT_Result gfnt_bitmap_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error);

/** Which of a bitmap font's own names is wanted. */
typedef enum {
  GFNT_BITMAP_STRING_FAMILY,    ///< `FAMILY_NAME`.
  GFNT_BITMAP_STRING_FULL,      ///< The XLFD name, or PCF's `FONT_NAME`.
  GFNT_BITMAP_STRING_COPYRIGHT, ///< `COPYRIGHT`.
  GFNT_BITMAP_STRING_WEIGHT     ///< `WEIGHT_NAME`.
} GFNT_BitmapString;

/**
 * One of the font's own names, for `name.h`'s accessors.
 *
 * @param out_text Receives a pointer into the font's arena, NUL-terminated and
 *   living as long as the face.
 * @return ::GFNT_OK, or ::GFNT_ERR_UNSUPPORTED when the font states no such name.
 */
GFNT_Result gfnt_bitmap_string_for(const GFNT_Face * face,
    GFNT_BitmapString which, const char ** out_text, GFNT_Error * error);

/* The four containers. Each takes a face whose directory is already built and
 * fills in the builder; `looks_like` reads only the first bytes of a blob. */

bool gfnt_pcf_looks_like(const GFNT_Reader * blob);
GFNT_Result gfnt_pcf_directory(GFNT_Face * face, GFNT_Error * error);
GFNT_Result gfnt_pcf_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error);

bool gfnt_psf_looks_like(const GFNT_Reader * blob);
GFNT_Result gfnt_psf_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error);

bool gfnt_bdf_looks_like(const GFNT_Reader * blob);
GFNT_Result gfnt_bdf_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error);

bool gfnt_hex_looks_like(const GFNT_Reader * blob);
GFNT_Result gfnt_hex_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_BITMAP_INTERNAL_H
