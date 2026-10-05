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
 * The Type 2 and Type 1 charstring interpreters, which know nothing about any
 * container.
 *
 * documentation/design.md sections 4.1 and 7.4, and decision 17.7. A charstring
 * is a program that draws one glyph, and five different containers hold them:
 * `CFF ` inside an sfnt, bare CFF as a PDF embeds it, a Type 1 font program,
 * and `CFF2`, whose charstrings move through a variation store. Each of those is
 * a different way of finding bytes and subroutines; the drawing is the same, so
 * it is here, and the container passes it a ::GFNT_CharstringContext saying where
 * the subroutines are.
 *
 * That split is what makes Type 1 cheap after Type 2, and it is what a PDF
 * library needs: a `FontFile3` stream is a bare CFF, and a `FontFile` is a Type
 * 1 program, and neither arrives wrapped in an sfnt this library could load as
 * a face.
 *
 * **This is tier 1** (::GFNT_Outline is what a charstring draws into), so it is
 * not in `font.h`. Include it by name.
 */

#ifndef GHOTI_IO_GFNT_CHARSTRING_H
#define GHOTI_IO_GFNT_CHARSTRING_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/outline.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which charstring language a program is written in.
 *
 * Type 1 and Type 2 are not versions of one language: Type 1 carries its advance and left
 * side bearing in `hsbw`, has no subroutine bias, no `hintmask`, no operand
 * stack arithmetic to speak of, and reaches flex and hint replacement through
 * `callothersubr`. They share the curve operators and little else, which is why
 * one function takes the language as an argument rather than the caller picking
 * an entry point: a container that guesses wrong should get a refusal from the
 * first byte it cannot read, not a plausible wrong shape.
 */
typedef enum {
  GFNT_CHARSTRING_TYPE2 = 0, ///< Type 2, as CFF holds.
  GFNT_CHARSTRING_TYPE1,     ///< Type 1, as a Type 1 font program holds.
  /**
   * Type 2 as `CFF2` holds it: no width, no `endchar`, an operand stack of 513
   * and two operators more - `vsindex` and `blend`.
   */
  GFNT_CHARSTRING_CFF2,
  GFNT_CHARSTRING_TYPE_COUNT ///< Closes the enum for the string table's test.
} GFNT_CharstringType;

/**
 * @brief Name a charstring language, for diagnostics and dumps.
 *
 * @param type The language.
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_charstring_type_string(GFNT_CharstringType type);

/**
 * @brief Where an interpreter finds the subroutines it is told to call.
 *
 * `count` is not a convenience: Type 2's subroutine numbers are **biased** by
 * how many subroutines there are (107 for fewer than 1240, 1131 below 33900,
 * else 32768), so a caller that supplied an accessor and no count would make
 * every subroutine call in every real font land somewhere else.
 */
typedef struct GFNT_CharstringSubrs {
  /**
   * Borrow subroutine @p index's bytes.
   *
   * @param user The `user` field below.
   * @param index Which one, already unbiased, from 0.
   * @param out_bytes Receives a pointer that outlives the run.
   * @param out_length Receives its length.
   * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT for an index that is not there.
   */
  GFNT_Result (*at)(void * user, uint32_t index, const uint8_t ** out_bytes,
      size_t * out_length);
  void * user;  ///< Passed to `at`.
  size_t count; ///< How many there are, which is what the bias is computed from.
} GFNT_CharstringSubrs;

/**
 * @brief Where a `CFF2` charstring's `blend` finds how far inside each region the
 *   location is.
 *
 * A `blend` adds to each of its values the sum of that many deltas, each scaled by
 * the scalar of one region of the font's variation store, and *which* regions is
 * the charstring's `vsindex` - the Private DICT's, unless the charstring chose
 * another. The interpreter knows neither the store nor the location, so the
 * container supplies this.
 */
typedef struct GFNT_CharstringBlend {
  /**
   * The scalars of the regions one ItemVariationData names, at the location.
   *
   * @param user The `user` field below.
   * @param vsindex Which ItemVariationData.
   * @param out_scalars Receives one scalar per region, in the data's order, each
   *   with 24 fractional bits (2^24 is 1).
   * @param capacity How many @p out_scalars holds.
   * @param out_count Receives how many the data names.
   * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT for an index the store does not
   *   have.
   */
  GFNT_Result (*scalars)(void * user, uint32_t vsindex, int64_t * out_scalars,
      size_t capacity, size_t * out_count);
  void * user;            ///< Passed to `scalars`.
  uint32_t default_vsindex; ///< The `vsindex` a charstring has until it says otherwise.
} GFNT_CharstringBlend;

/**
 * @brief Everything an interpreter needs that is not the charstring itself.
 */
typedef struct GFNT_CharstringContext {
  GFNT_CharstringSubrs local;  ///< The font's, or this FD's, local subroutines.
  GFNT_CharstringSubrs global; ///< The font's global subroutines. Type 2 only.

  /**
   * Find the charstring of the glyph a Standard Encoding code names.
   *
   * `seac` - and `endchar` given four or five arguments, which is the same
   * construction spelled differently - builds an accented character out of two
   * other glyphs, named by **Standard Encoding code** whatever the font's own
   * encoding is. Only the container can resolve that, because only it has the
   * charset. A NULL accessor makes such a charstring ::GFNT_ERR_UNSUPPORTED
   * rather than a glyph with its accent missing.
   */
  GFNT_Result (*standard_code)(void * user, uint8_t code,
      const uint8_t ** out_bytes, size_t * out_length);
  void * standard_user; ///< Passed to `standard_code`.

  /** `nominalWidthX`, 16.16 font units: what a stated width is a delta from. */
  GFNT_F16Dot16 nominal_width;
  /** `defaultWidthX`, 16.16 font units: the advance of a charstring that states none. */
  GFNT_F16Dot16 default_width;

  /**
   * Caps on points, contours, subroutine depth and operator count, or NULL.
   *
   * Applied to @p outline as well as to the run, so that one statement of the
   * caps governs both. NULL leaves the outline's own alone and uses the defaults
   * for the run.
   */
  const GFNT_Limits * limits;

  /**
   * `CFF2` only: where `blend` finds its scalars. NULL makes a `blend` or a
   * `vsindex` a refusal, which is the right answer for a font with no variation
   * store.
   */
  const GFNT_CharstringBlend * blend;
} GFNT_CharstringContext;

/**
 * @brief What a charstring said, besides its shape.
 *
 * The advance is the point of this structure. A CFF glyph's advance is in
 * `hmtx` *and* implied by its charstring, and the two are allowed to disagree;
 * a Type 1 glyph's advance is **only** in the charstring, where `hsbw` puts it.
 * A reader that dropped it would have nowhere to get the second from.
 */
typedef struct GFNT_CharstringMetrics {
  GFNT_F16Dot16 width;      ///< The advance, 16.16 font units.
  bool width_stated;        ///< Whether the charstring carried one itself.
  GFNT_F16Dot16 side_bearing; ///< Type 1's `hsbw` left side bearing; 0 for Type 2.
  size_t stems;             ///< Stem hints declared, which is what sizes a `hintmask`.
  size_t operators;         ///< Operators executed, subroutines included.
  bool seac;                ///< Whether it assembled an accented character.
} GFNT_CharstringMetrics;

/**
 * @brief Run a charstring, appending its contours to an outline.
 *
 * The outline is **appended to**, not cleared, so that a container can place
 * two charstrings in one outline - which is exactly what `seac` does, and doing
 * it any other way would need a second entry point.
 *
 * Coordinates arrive in the charstring's own units and are written as
 * ::GFNT_OUTLINE_UNITS, 26.6. A charstring's arithmetic is fixed point
 * throughout, `div` included: no `float` appears between a font's bytes and a
 * pixel, which is design.md section 1's determinism promise.
 *
 * @param type Which language the bytes are in.
 * @param bytes The charstring. Type 1 bytes must already be decrypted, which is
 *   the container's job.
 * @param length How many bytes.
 * @param context Where the subroutines are, and the width defaults.
 * @param outline Receives the contours.
 * @param out_metrics Receives the advance and the counts, or NULL.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_CORRUPT for a program that
 *   runs off its own end, underflows the stack or calls a subroutine that is
 *   not there; ::GFNT_ERR_UNSUPPORTED for an operator this library refuses by
 *   name - `random`, which no deterministic renderer can honour, and the
 *   deprecated `seac` spelling when the container supplied no resolver;
 *   ::GFNT_ERR_LIMIT; or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_charstring_run(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringContext * context, GFNT_Outline * outline,
    GFNT_CharstringMetrics * out_metrics, GFNT_Error * error);

/**
 * @brief Write one glyph's charstring to a stream, one operator per line.
 *
 * What a differential and a bug report want: the *program*, not the path it
 * draws. A charstring that produces the wrong shape is a program with one wrong
 * operand, and no picture of the outline says which.
 *
 * **Why it takes the metrics.** A `hintmask`'s mask is as many bytes as the
 * declared stem hints need, and a *subroutine* may declare them - so a walk that
 * does not run subroutines cannot divide the bytes after such a mask into
 * operators at all. ::GFNT_CharstringMetrics::stems from a previous
 * ::gfnt_charstring_run() over the same program is that count, which is why this
 * takes it rather than counting stems a second way. Passing NULL is allowed and
 * honest: the dump then stops at the first `hintmask` that follows a subroutine
 * call and says on the line why, rather than guessing a width and printing
 * operators that are not there.
 *
 * @param type Which language the bytes are in.
 * @param bytes The charstring.
 * @param length How many bytes.
 * @param metrics What a run of this same program reported, or NULL.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_charstring_dump(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringMetrics * metrics, FILE * out);

/**
 * @brief This glyph's charstring, borrowed from the face's bytes.
 *
 * The two functions below are the only face-level ones in this header, and they
 * are here rather than in `outline.h` because what they answer is a fact about a
 * *charstring* - `outline.h` would have to include this header to spell their
 * types, and this header already includes it.
 *
 * What it is for: a subsetter or a PDF writer wants the program, not the path,
 * and a differential wants to compare the program this library read against the
 * reference's decompilation of the same bytes. A glyph whose shape is wrong
 * because one operand was misread looks like a drawing bug in the path and like
 * exactly what it is in the program.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_type Receives which language the bytes are in, or NULL.
 * @param out_bytes Receives a pointer into the face's blob, valid as long as the
 *   blob is. A glyph whose charstring is empty gets NULL and a length of zero,
 *   which is a glyph that draws nothing rather than an error.
 * @param out_length Receives how many bytes.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face whose glyphs are not
 *   charstrings at all; ::GFNT_ERR_INVALID for a glyph the face does not have;
 *   or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_glyph_charstring(const GFNT_Face * face,
    uint32_t glyph, GFNT_CharstringType * out_type, const uint8_t ** out_bytes,
    size_t * out_length, GFNT_Error * error);

/**
 * @brief What this glyph's charstring says about itself.
 *
 * Its advance above all, which is a **different fact** from `hmtx`'s and is
 * allowed to disagree with it: a CFF font carries both, and a Type 1 font
 * carries only this one. A caller reporting on a font can compare them; a caller
 * laying out text wants ::gfnt_face_glyph_advance(), which is the metric table's
 * answer and the one a shaper uses.
 *
 * Running the program is what answers this - the advance may be stated by an
 * operator reached through a subroutine - so it costs what drawing the glyph
 * costs.
 *
 * @param face The face.
 * @param glyph The glyph index.
 * @param out_metrics Receives the advance, the side bearing and the counts.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_glyph_charstring().
 */
GFNT_API GFNT_Result gfnt_face_glyph_charstring_metrics(const GFNT_Face * face,
    uint32_t glyph, GFNT_CharstringMetrics * out_metrics, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CHARSTRING_H
