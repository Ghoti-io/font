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
 * Choosing a face from a ::GFNT_FontSet by family, weight, width and style.
 *
 * documentation/design.md section 11. This is the font matching algorithm of CSS Fonts
 * Level 4 (section 5.2, "Matching font styles"): a family list is walked in order, the
 * faces of the first family that has any are narrowed to one by width, then style, then
 * weight, and a face is *selected*, never synthesised. The steps are the specification's,
 * followed as written, with the one place it leaves to the user agent (which of several
 * faces that tie) decided as "the first in the set".
 *
 * **Names.** A family is matched against the typographic family (name ID 16, else 1) and,
 * when the face has a different one, the legacy family (name ID 1), by Unicode full case
 * folding without normalization - the "Default Caseless Matching" CSS asks for. A name no
 * family has is followed through the set's alias table once, so a generic family
 * (`sans-serif`, `monospace`) is whatever the caller mapped it to when it built the set;
 * nothing here knows what a generic family is.
 *
 * **Variable faces** take part as the range of values they cover, as the specification
 * says: a face whose `wght` axis spans 100 to 900 is a face at every weight in between,
 * and matching reports the value to set the axis to. ::gfnt_fontmatch_location() turns
 * that into the coordinates a shaper or an outline takes.
 *
 * **Not here.** Synthetic bold and oblique (design.md section 16) and `unicode-range`. Choosing a face for a *character* is the second half of
 * this header, ::GFNT_FontFallback.
 */

#ifndef GHOTI_IO_GFNT_MATCH_H
#define GHOTI_IO_GFNT_MATCH_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/discover.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The CSS keyword `oblique` with no angle: the angle a browser uses, 14 degrees. */
#define GFNT_OBLIQUE_DEFAULT_ANGLE (14 * 65536)

/**
 * @brief What is asked for. A zeroed query asks for the initial values of every property.
 *
 * Numbers are 16.16 and in CSS's units: `weight` 1 to 1000 (400 regular), `width` a
 * percentage (100 normal), `oblique_angle` degrees clockwise from upright. A zero `weight`
 * or `width` means the initial value, so a query that names only families is valid.
 */
typedef struct GFNT_FontQuery {
  const char * const * families; ///< The family list, in order. UTF-8.
  size_t family_count;           ///< How many.
  GFNT_F16Dot16 weight;          ///< `font-weight`; 0 for 400.
  GFNT_F16Dot16 width;           ///< `font-width` (`font-stretch`) in percent; 0 for 100.
  GFNT_Slant slant;              ///< `font-style`: roman is `normal`.
  /**
   * The angle of `font-style: oblique <angle>`. Read only for ::GFNT_SLANT_OBLIQUE, and
   * only when @p oblique_angle_set; otherwise the keyword's default,
   * ::GFNT_OBLIQUE_DEFAULT_ANGLE, is asked for.
   */
  GFNT_F16Dot16 oblique_angle;
  bool oblique_angle_set;        ///< Whether @p oblique_angle was given.
} GFNT_FontQuery;

/**
 * @brief A face that was chosen, and the values its variable axes are to be set to.
 *
 * `weight` and `width` are the request clamped into what the face covers (a static face
 * covers one value, so they are its own). `italic` is 0 or 1 and `oblique` is the angle,
 * both zero unless the match was made through that property; they are what `ital` and
 * `slnt` (the negated angle) are to be set to, for a face that has those axes.
 */
typedef struct GFNT_FontMatch {
  size_t index;           ///< The face's place in the set (::gfnt_fontset_at()).
  size_t family;          ///< Which family of the query it matched.
  GFNT_F16Dot16 weight;   ///< The weight to apply.
  GFNT_F16Dot16 width;    ///< The width to apply, in percent.
  GFNT_F16Dot16 italic;   ///< The `ital` axis value to apply.
  GFNT_F16Dot16 oblique;  ///< The oblique angle to apply, in degrees.
} GFNT_FontMatch;

/** @brief Fill @p query with the initial values and no families. */
GFNT_API void gfnt_fontquery_init(GFNT_FontQuery * query);

/**
 * @brief The best face of the first family in the query's list that has any.
 *
 * @param set The set.
 * @param query What is asked for; at least one family.
 * @param allocator For the working storage, or NULL for the default. Nothing is kept.
 * @param out_match Receives the choice. Written only when @p out_found is set.
 * @param out_found Receives whether any family had a face.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (found or not), ::GFNT_ERR_INVALID, ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_fontset_match(const GFNT_FontSet * set,
    const GFNT_FontQuery * query, const GFNT_Allocator * allocator,
    GFNT_FontMatch * out_match, bool * out_found, GFNT_Error * error);

/**
 * @brief The face a CSS `local()` source names: its full name (name ID 4) or its PostScript
 * name (name ID 6).
 * Compared by full Unicode case folding, as family names are. A face whose full name matches
 * wins over one whose PostScript name does, and the first such face in the set wins among
 * equals. Nothing is synthesised and no style is chosen: `local("Arial Bold")` is that face or
 * none.
 * @param set The set.
 * @param name The name. UTF-8.
 * @param out_index Receives the face's place in the set (::gfnt_fontset_at()) when found.
 * @param out_found Receives whether a face has the name.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (found or not) or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_fontset_match_local(const GFNT_FontSet * set, const char * name,
    size_t * out_index, bool * out_found, GFNT_Error * error);

/**
 * @brief The best face of one named family, whatever the query's own list says.
 *
 * `out_match->family` is 0.
 */
GFNT_API GFNT_Result gfnt_fontset_match_family(const GFNT_FontSet * set,
    const char * family, const GFNT_FontQuery * query, const GFNT_Allocator * allocator,
    GFNT_FontMatch * out_match, bool * out_found, GFNT_Error * error);

/**
 * @brief The normalised coordinates that apply a match to a face.
 *
 * Sets `wght`, `wdth`, `slnt` (the negated oblique angle) and `ital` from @p match, each
 * clamped to its axis, and leaves every other axis at its default: the CSS step "apply
 * font matching variations". A face with no variation axes gets a count of zero.
 *
 * @param face The face the match chose, opened by the caller from the record's path.
 * @param match The match.
 * @param out_coordinates Receives one 2.14 value per axis, for a ::GFNT_Variation.
 * @param capacity How many @p out_coordinates holds.
 * @param out_count Receives the axis count.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_LIMIT when @p capacity is short.
 */
GFNT_API GFNT_Result gfnt_fontmatch_location(const GFNT_Face * face,
    const GFNT_FontMatch * match, GFNT_F2Dot14 * out_coordinates, size_t capacity,
    size_t * out_count, GFNT_Error * error);


/**
 * @brief The families to try first for a script (and optionally a language).
 *
 * Han is the usual reason: a Japanese reader and a Chinese reader want different faces for the
 * same characters, and the faces on a machine do not say which is which. A preference with a
 * language is used for that language and one without for the script's other languages.
 */
typedef struct GFNT_FontScriptPreference {
  GFNT_Tag script;                ///< The OpenType script tag, `'hani'`.
  GFNT_Tag language;              ///< The OpenType language tag, or 0 for any.
  const char * const * families;  ///< In order of preference. UTF-8.
  size_t family_count;            ///< How many.
} GFNT_FontScriptPreference;

/** @brief What the fallback is given beyond the set. A zeroed one is valid. */
typedef struct GFNT_FontFallbackOptions {
  const GFNT_FontScriptPreference * preferences; ///< Or NULL.
  size_t preference_count;                       ///< How many.
} GFNT_FontFallbackOptions;

/**
 * @brief A face for a character the primary face lacks, drawn from a scanned set.
 *
 * Opaque. The faces it returns are opened from the files on first need and **stay open until
 * the object is freed**, so a pointer it gave out is good for as long as the object is: the
 * contract a paragraph's font provider needs. The set must outlive it.
 */
typedef struct GFNT_FontFallback GFNT_FontFallback;

/**
 * @brief Prepare a fallback over @p set.
 *
 * Nothing is opened yet. The families that preferences name are looked up by the same
 * caseless comparison matching uses; one the set does not have is simply never tried.
 */
GFNT_API GFNT_Result gfnt_fontfallback_create(const GFNT_FontSet * set,
    const GFNT_FontFallbackOptions * options, const GFNT_Allocator * allocator,
    GFNT_FontFallback ** out_fallback, GFNT_Error * error);

/**
 * @brief A face in the set that maps @p codepoint.
 *
 * Families are tried in this order: the script's preference list (the language's own first), the
 * family of @p wanted's first name, then every other family in the order the set lists them.
 * Within a family the face is the one CSS matching picks for @p wanted's weight, width and
 * style among the faces that map the character. Faces whose `OS/2` range bits claim the
 * character are tried before faces that do not (a font with no bits set claims nothing and
 * everything), and a face is chosen only if its `cmap` really maps the character: a claim is a
 * hint, never a proof.
 *
 * @param fallback The fallback.
 * @param codepoint The character.
 * @param script The OpenType script tag of the run, or 0.
 * @param language The OpenType language tag, or 0.
 * @param wanted The style asked for (weight, width, slant, and a first family to prefer), or
 *   NULL for the initial values.
 * @param out_face Receives the face, owned by @p fallback. Written only when found.
 * @param out_index Receives the face's place in the set, or NULL.
 * @param out_found Receives whether any face maps the character.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK (found or not), ::GFNT_ERR_INVALID, ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_fontfallback_face(GFNT_FontFallback * fallback, uint32_t codepoint,
    GFNT_Tag script, GFNT_Tag language, const GFNT_FontQuery * wanted,
    const GFNT_Face ** out_face, size_t * out_index, bool * out_found, GFNT_Error * error);

/** @brief Close every face the fallback opened and release it. NULL is ignored. */
GFNT_API void gfnt_fontfallback_free(GFNT_FontFallback * fallback);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_MATCH_H
