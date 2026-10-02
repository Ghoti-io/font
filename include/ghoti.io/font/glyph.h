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
 * What a glyph can be, and which strike answers a pixel size.
 *
 * documentation/design.md sections 5.3 and 5.4, and decision 3: bitmap fonts
 * are first-class **from phase 0's API**, not a later feature. Five different
 * things can come back for one glyph index, a caller that cannot ask which
 * cannot composite it, and a strike list that arrives in phase 3 is a break
 * across the whole API. So the enums and the strike accessors are here now.
 *
 * The strike list has more than one entry as of `EBLC` (section 7.5), which is
 * when ::gfnt_face_select_strike()'s policies started choosing rather than
 * confirming: every standalone container is one strike, so until an sfnt's
 * strikes were readable no input could tell "the nearest strike" from "the first
 * one".
 *
 * What is **not** here is any glyph data: the bitmap parsers arrive in phase
 * 1b and outlines in phase 1. The accessors below are honest about that in the
 * one way that matters - a face carrying `EBLC`, `CBLC` or `sbix` reports
 * ::GFNT_ERR_UNSUPPORTED rather than "no strikes", because a library that
 * answered zero would be telling a caller a bitmap font has no bitmaps.
 */

#ifndef GHOTI_IO_GFNT_GLYPH_H
#define GHOTI_IO_GFNT_GLYPH_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a glyph turned out to be.
 *
 * The tag of the union design.md section 5.4 describes. A caller that cannot
 * ask which of these it has cannot composite the result, which is why the kind
 * is part of the API before any of the data is.
 */
typedef enum {
  GFNT_GLYPH_OUTLINE,     ///< A path, quadratic (`glyf`) or cubic (CFF).
  GFNT_GLYPH_BITMAP_MONO, ///< A 1-bit strike, with its own metrics.
  GFNT_GLYPH_BITMAP_GRAY, ///< A 2-, 4- or 8-bit strike.
  GFNT_GLYPH_BITMAP_PNG,  ///< `CBDT` or `sbix` bytes, needing a decoder.
  GFNT_GLYPH_COLR_LAYERS, ///< A `COLR` v0 layer list or a v1 paint graph.
  GFNT_GLYPH_SVG,         ///< Absent (section 16); the arm completes the enum.
  GFNT_GLYPH_KIND_COUNT   ///< Closes the enum, so a test can check the strings.
} GFNT_GlyphKind;

/**
 * @brief Name a glyph kind, for diagnostics and dumps.
 *
 * @param kind The kind.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_glyph_kind_string(GFNT_GlyphKind kind);

/**
 * @brief Which strike answers a pixel size, when a face has any.
 *
 * Zero is the policy that cannot surprise a caller who never heard of strikes:
 * it ignores them entirely. M9 is the mistake the other three exist to let a
 * caller avoid - a bitmap strike scaled as though it were an outline, or an
 * outline ignored because a strike existed, is how 13-pixel text ends up blurry
 * next to crisp 12-pixel text.
 */
typedef enum {
  /** Ignore strikes; ::GFNT_ERR_UNSUPPORTED if the face has no outlines. */
  GFNT_STRIKE_OUTLINES_ONLY = 0,
  /** A strike at exactly this ppem, else outlines. */
  GFNT_STRIKE_EXACT,
  /**
   * The nearest strike, used unscaled, else outlines.
   *
   * Nearest in vertical pixels per em, and **a tie goes to the larger strike** -
   * 11 ppem against strikes at 10 and 16 picks 10, against strikes at 10 and 12
   * picks 12. The rule is stated because a caller reporting that text looks wrong
   * at one size needs the same answer twice, and because the alternative throws
   * pixels away.
   */
  GFNT_STRIKE_NEAREST,
  /**
   * The nearest strike, scaled if it has to be; the last resort.
   *
   * Selects exactly as ::GFNT_STRIKE_NEAREST does today, because nothing in this
   * library scales a strike (`raster.h`). The two are separate so that the
   * distinction does not have to be added to the API later.
   */
  GFNT_STRIKE_PREFER_STRIKE
} GFNT_StrikePolicy;

/**
 * @brief One pixel size at which a face carries bitmaps.
 */
typedef struct GFNT_Strike {
  size_t index;        ///< Which strike of the face this is.
  uint32_t ppem_x;     ///< Horizontal pixels per em.
  uint32_t ppem_y;     ///< Vertical pixels per em.
  uint8_t bit_depth;   ///< 1, 2, 4, 8, or 32 for colour.
  GFNT_GlyphKind kind; ///< What glyphs from this strike come back as.
  /**
   * Pixels from the baseline to the top of the line, positive.
   *
   * The strike's own, not the face's: a bitmap font has no `hhea` and no em to
   * scale one from, and these are the numbers it states (M18's sign convention
   * holds here as everywhere - ascent up, descent down).
   *
   * A container that states neither reports the glyph box it does state, and
   * says so in its documentation rather than inventing a baseline.
   */
  int32_t ascent;
  int32_t descent;     ///< Pixels below the baseline, **negative**.
} GFNT_Strike;

/**
 * @brief One pixel size a face offers by **scaling** another size's bitmaps.
 *
 * `EBSC`, and only `EBSC`: documentation/design.md section 7.5. A face states
 * these beside its real strikes to say "I have no 8-pixel bitmaps, but make them
 * from the 10-pixel ones".
 *
 * **This library does not scale them, and that is deliberate.** The specification
 * names the substitute strike and says nothing about how to use it - no filter, no
 * rounding, nothing about what becomes of a bearing - so any pixels produced here
 * would be this library's invention wearing the font's name. FreeType does not
 * read the table at all and fontTools parses the records without scaling them, so
 * there is no second reader of a scaled pixel anywhere to contradict a guess.
 *
 * What is decidable is reported: the size offered, the size to make it from, and
 * which strike that is. A caller that wants the pixels has ::GFNT_Strike for the
 * substitute and its own choice of filter, which is a rendering decision rather
 * than a font-reading one.
 */
typedef struct GFNT_ScaledStrike {
  size_t index;                 ///< Which record of the face this is.
  uint32_t ppem_x;              ///< Horizontal pixels per em offered.
  uint32_t ppem_y;              ///< Vertical pixels per em offered.
  uint32_t substitute_ppem_x;   ///< The strike's horizontal size to scale from.
  uint32_t substitute_ppem_y;   ///< The strike's vertical size to scale from.
  /**
   * Which strike that is, as ::GFNT_Strike::index.
   *
   * Resolved when the table is read, so every caller gets the same answer and a
   * record naming a size no strike has is refused rather than handed over. It is
   * therefore always a strike this face has.
   */
  size_t substitute_index;
  /**
   * The baseline this record states, which is **its own** and not the
   * substitute's.
   *
   * A scaled size has its own `sbitLineMetrics`, because scaling a 10-pixel
   * strike to 8 does not scale its ascent to anything the font would have chosen.
   * M18's sign convention holds: ascent up, descent down.
   */
  int32_t ascent;
  int32_t descent;              ///< Pixels below the baseline, **negative**.
} GFNT_ScaledStrike;

/**
 * @brief How many scaled sizes the face offers.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK with a count of 0 for a face with no `EBSC`, or one whose
 *   `EBSC` lists no records - and for every standalone bitmap container, which
 *   has no such concept; the number of `BitmapScale` records otherwise; or what
 *   reading the table returned. A face with an `EBSC` but no readable `EBLC`
 *   reports the `EBLC`'s failure, because a scaled size that defers to a strike
 *   list nobody can read is not a size.
 */
GFNT_API GFNT_Result gfnt_face_scaled_strike_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth scaled size.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_scaled Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index the face does not have, or
 *   what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_scaled_strike_at(const GFNT_Face * face,
    size_t index, GFNT_ScaledStrike * out_scaled, GFNT_Error * error);

/**
 * @brief Whether the face carries outline glyph data this library reads.
 *
 * `glyf` with `loca` - both, since neither indexes glyphs without the other.
 *
 * A face whose outlines are charstrings reports **false**, `CFF ` and `CFF2`
 * alike, because what a caller needs to know is whether asking for an outline
 * can succeed and today it cannot (documentation/design.md section 7.4). This
 * becomes true for `CFF ` faces when the charstring interpreter lands, which is
 * the one answer here that is expected to change.
 *
 * @param face The face, or NULL.
 * @return true if outlines are there to be read.
 */
GFNT_API bool gfnt_face_has_outlines(const GFNT_Face * face);

/**
 * @brief How many strikes the face carries.
 *
 * @param face The face.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK with a count of 0 for a face with no bitmap tables at all,
 *   or with an `EBLC` that lists none; the number of `bitmapSizeTable`s for a
 *   face with an `EBLC` and the `EBDT` it indexes into; 1 for a standalone bitmap
 *   container, where the file is a strike; ::GFNT_ERR_INVALID; or
 *   ::GFNT_ERR_UNSUPPORTED for a face whose strikes are in a table this library
 *   does not parse - `bloc`, `CBLC`, `sbix`, or an `EBLC` whose `EBDT` is missing
 *   - which is not the same answer as zero, and must not be.
 */
GFNT_API GFNT_Result gfnt_face_strike_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error);

/**
 * @brief The nth strike.
 *
 * @param face The face.
 * @param index Which one, from 0.
 * @param out_strike Receives it. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for an index the face does not have,
 *   or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_API GFNT_Result gfnt_face_strike_at(const GFNT_Face * face, size_t index,
    GFNT_Strike * out_strike, GFNT_Error * error);

/**
 * @brief Decide what answers a pixel size: a strike, or the outlines.
 *
 * Every size-dependent call reports which strike answered, or that the answer
 * came from outlines (design.md section 5.3), because a caller that cannot find
 * out why 14-pixel text looks different from 12-pixel text has a bug report it
 * cannot act on.
 *
 * @param face The face.
 * @param ppem The pixel size wanted. Zero is a caller error.
 * @param policy Which way to resolve it; zero ignores strikes.
 * @param out_strike Receives the strike that answered, when one did. May be
 *   NULL.
 * @param out_from_outlines Receives true when the answer is "scale the
 *   outlines". May be NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_UNSUPPORTED when
 *   nothing in the face can answer at that size.
 */
GFNT_API GFNT_Result gfnt_face_select_strike(const GFNT_Face * face,
    uint32_t ppem, GFNT_StrikePolicy policy, GFNT_Strike * out_strike,
    bool * out_from_outlines, GFNT_Error * error);

/**
 * @brief This glyph's name, from the `post` table.
 *
 * documentation/design.md section 7.2. Format 2.0 names the first 258 glyphs by
 * index into the standard Macintosh glyph order and the rest by strings stored
 * in the table; format 1.0 *is* that order. The order itself is generated from
 * the reference (section 14) rather than written from memory.
 *
 * The caller owns the string and frees it with ::gfnt_glyph_name_free().
 *
 * @param face The face.
 * @param glyph The glyph.
 * @param allocator Allocator for the string, or NULL for the default.
 * @param out_name Receives a NUL-terminated name. Written only on success.
 * @param out_length Receives its length, excluding the NUL, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph this font does not name;
 *   ::GFNT_ERR_UNSUPPORTED when the font **states** it has no names (`post`
 *   format 3.0) or uses a `post` version whose names this library does not
 *   read; ::GFNT_ERR_CORRUPT, or ::GFNT_ERR_OOM.
 *
 * @note "This font has no glyph names" and "this library cannot read this
 *   font's glyph names" are both ::GFNT_ERR_UNSUPPORTED, and the diagnostic
 *   distinguishes them. Neither is an empty string, which would be a name.
 */
GFNT_API GFNT_Result gfnt_face_glyph_name(const GFNT_Face * face,
    uint32_t glyph, const GFNT_Allocator * allocator, char ** out_name,
    size_t * out_length, GFNT_Error * error);

/**
 * @brief Release a name from ::gfnt_face_glyph_name().
 *
 * @param allocator The same allocator that was passed in, or NULL.
 * @param name The string, or NULL.
 */
GFNT_API void gfnt_glyph_name_free(const GFNT_Allocator * allocator,
    char * name);

/**
 * @brief The first glyph with this name.
 *
 * A linear scan, deliberately: this is a text-extraction path asked once per
 * distinct name, and an index would have to be built, memoised and invalidated
 * for a question most callers never ask.
 *
 * @param face The face.
 * @param name The name to find, NUL-terminated.
 * @param out_glyph Receives the glyph. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID when no glyph has that name;
 *   ::GFNT_ERR_UNSUPPORTED when the font has no names to search.
 */
GFNT_API GFNT_Result gfnt_face_glyph_for_name(const GFNT_Face * face,
    const char * name, uint32_t * out_glyph, GFNT_Error * error);

/**
 * @brief Write every glyph's name to a stream, one per line.
 *
 * For the differentials: `tools/oracle/ttx_diff.py` compares this against
 * fontTools' glyph order. A font with no names says so on one line rather than
 * printing nothing, because a differential reads nothing as agreement.
 *
 * @param face The face.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_face_glyph_names_dump(const GFNT_Face * face,
    FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_GLYPH_H
