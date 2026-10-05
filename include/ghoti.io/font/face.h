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
 * @ref GFNT_Face "GFNT_Face": one font, from a blob and an index into it.
 *
 * documentation/design.md sections 5.3 and 7.1. Loading a face parses the
 * table directory and **nothing else**: every table is parsed and validated on
 * first use, so opening a 30 MB collection to ask for one glyph costs one
 * glyph. Section 7.8 is the rule behind that - the specification's "required
 * tables" list describes a font a foundry ships, not one a reader meets, and a
 * font embedded in a PDF has neither `cmap` nor `name` nor `OS/2` - so a
 * missing table is ::GFNT_ERR_UNSUPPORTED from the operation that needed it
 * rather than a load failure.
 */

#ifndef GHOTI_IO_GFNT_FACE_H
#define GHOTI_IO_GFNT_FACE_H

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One font: a blob, an index into it, and its table directory.
 *
 * A face is immutable after load, and its lazily parsed tables are memoised
 * under a lock, so one face may be shared read-only across threads
 * (documentation/design.md section 15.3).
 *
 * The face **borrows its blob**: the blob must outlive the face, and every
 * face of a collection shares one. That is what makes a collection cheap, and
 * it is the one lifetime rule this API has.
 */
typedef struct GFNT_Face GFNT_Face;

/** @brief The sfnt version of a TrueType-outline font: 0x00010000. */
#define GFNT_FLAVOUR_TRUETYPE 0x00010000u
/** @brief The sfnt version of a CFF-outline font: `OTTO`. */
#define GFNT_FLAVOUR_CFF GFNT_TAG('O', 'T', 'T', 'O')
/** @brief Apple's alternative TrueType version: `true`. */
#define GFNT_FLAVOUR_APPLE_TRUE GFNT_TAG('t', 'r', 'u', 'e')
/** @brief Apple's Type 1 sfnt version: `typ1`. */
#define GFNT_FLAVOUR_APPLE_TYPE1 GFNT_TAG('t', 'y', 'p', '1')
/** @brief The tag that opens a font collection: `ttcf`. */
#define GFNT_FLAVOUR_COLLECTION GFNT_TAG('t', 't', 'c', 'f')
/**
 * @brief A bare `CFF ` font program, which is not an sfnt at all: `CFF `.
 *
 * Not an sfnt version - a bare CFF has no offset table and no directory, and
 * this is the value that says so. It is reported rather than
 * ::GFNT_FLAVOUR_CFF because `OTTO` is a claim about a wrapper this font does
 * not have, and a tool that printed it would be printing something untrue.
 */
#define GFNT_FLAVOUR_BARE_CFF GFNT_TAG('C', 'F', 'F', ' ')
/**
 * @brief A Type 1 font program - PFB, PFA or raw - which is not an sfnt: `TYP1`.
 *
 * Not an sfnt version, and deliberately not ::GFNT_FLAVOUR_APPLE_TYPE1, which is
 * the `typ1` version of an *sfnt* whose tables hold Type 1 data. This is the
 * font program on its own, as a `.pfb` or `.pfa` file holds it and as a PDF
 * `FontFile` embeds it. The two differ by case, which is the only thing
 * separating two spellings of one word in a four-byte tag.
 */
#define GFNT_FLAVOUR_TYPE1 GFNT_TAG('T', 'Y', 'P', '1')
/**
 * @brief A PCF bitmap font, X11's compiled format: `PCF `.
 *
 * Not an sfnt version. The file's own magic is a byte `0x01` and then `fcp`,
 * which is not a tag a caller can print, so the flavour is the format's name
 * rather than its magic - the one place in this library where those differ.
 */
#define GFNT_FLAVOUR_PCF GFNT_TAG('P', 'C', 'F', ' ')
/** @brief A BDF bitmap font, the X11 text format: `BDF `. */
#define GFNT_FLAVOUR_BDF GFNT_TAG('B', 'D', 'F', ' ')
/** @brief A PSF console font, version 1 or 2: `PSF `. */
#define GFNT_FLAVOUR_PSF GFNT_TAG('P', 'S', 'F', ' ')
/** @brief A GNU Unifont `.hex` file: `HEX `. */
#define GFNT_FLAVOUR_HEX GFNT_TAG('H', 'E', 'X', ' ')

/**
 * @brief How a variation delta that is exactly halfway between two whole units rounds.
 *
 * The OpenType specification does not say. It fixes the rounding of a float to
 * 16.16 (half up) and of 16.16 to 2.14 (half up), and then states that, apart
 * from those, it "has no other requirements for instance coordinates, scaled
 * deltas or derived instance values to be rounded" (Font Variations overview,
 * "Coordinate Scales and Normalization"). Implementations differ: HarfBuzz and
 * fontTools round half up, FreeType half away from zero. The two agree on every
 * value that is not an exact tie and on every positive tie; they differ on a
 * negative one, where -37.5 is -37 half up and -38 half away.
 *
 * Applies where a *final* delta becomes a whole number of font units: an
 * advance or bearing (`HVAR`, `MVAR`), a `cvt ` value (`cvar`), and a `GPOS`
 * device-table adjustment. Intermediate divisions, the 2.14 normalisation and
 * glyph outlines are unchanged by it.
 */
typedef enum GFNT_DeltaRounding {
  /** Half up, towards positive infinity: HarfBuzz and fontTools. The default. */
  GFNT_DELTA_ROUND_HALF_UP = 0,
  /** Half away from zero: FreeType's rule. */
  GFNT_DELTA_ROUND_HALF_AWAY = 1,
} GFNT_DeltaRounding;

/**
 * @brief A point in a variable font's design space.
 *
 * Normalised axis coordinates in 2.14, in `fvar` order, each in -1..1 after
 * `avar`. The array is the caller's and is not copied.
 *
 * **Every accessor a variation can change takes one of these**, from phase 0
 * and before any variation table is parsed (documentation/design.md section
 * 5.3), because adding the parameter later is a break across the whole API and
 * adding it now is an unused argument. NULL means the default instance, which
 * is all this library answers today.
 */
typedef struct GFNT_Variation {
  const GFNT_F2Dot14 * coords; ///< Normalised coordinates, or NULL.
  size_t count;                ///< How many axes @p coords covers.
  /** How a delta that lands exactly halfway is rounded; zero is the default. */
  GFNT_DeltaRounding delta_rounding;
} GFNT_Variation;

/**
 * @brief How many faces a blob holds.
 *
 * One for an ordinary sfnt, `numFonts` for a `ttcf` collection.
 *
 * @param blob The blob.
 * @param limits Caps to apply, or NULL for the defaults.
 * @param out_count Receives the count. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_FORMAT if the bytes are
 *   not a font this library recognises, or ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_count(const GFNT_Blob * blob,
    const GFNT_Limits * limits, size_t * out_count, GFNT_Error * error);

/**
 * @brief Load one face from a blob.
 *
 * Parses the table directory and validates that every table lies inside the
 * blob. Nothing else is read: the tables themselves are parsed on first use.
 *
 * @param blob The blob, which must outlive the face.
 * @param index Which face, for a collection; 0 for an ordinary font.
 * @param limits Caps to apply, or NULL for the defaults. The face keeps a copy
 *   and applies it to every later parse.
 * @param allocator Allocator for the face, or NULL for the default.
 * @param out_face Receives the face, released with ::gfnt_face_free().
 *   Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID (including an @p index no font has),
 *   ::GFNT_ERR_FORMAT, ::GFNT_ERR_CORRUPT, ::GFNT_ERR_LIMIT or
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_face_load(const GFNT_Blob * blob, size_t index,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Face ** out_face, GFNT_Error * error);

/**
 * @brief Release a face. The blob is left alone.
 *
 * @param face The face, or NULL.
 */
GFNT_API void gfnt_face_free(GFNT_Face * face);

/**
 * @brief Which face of its collection this is.
 *
 * @param face The face, or NULL.
 * @return The index, or 0 for NULL.
 */
GFNT_API size_t gfnt_face_index(const GFNT_Face * face);

/**
 * @brief The sfnt version this face carries.
 *
 * ::GFNT_FLAVOUR_TRUETYPE, ::GFNT_FLAVOUR_CFF, ::GFNT_FLAVOUR_APPLE_TRUE or
 * ::GFNT_FLAVOUR_APPLE_TYPE1 - or ::GFNT_FLAVOUR_BARE_CFF or
 * ::GFNT_FLAVOUR_TYPE1 for a font that is not an sfnt and has no directory of
 * its own. It says which outline format to
 * expect and nothing more: what a face can actually do is decided by which
 * tables it has, and a container with no directory has the ones this library
 * synthesised for it.
 *
 * @param face The face, or NULL.
 * @return The version tag, or 0 for NULL.
 */
GFNT_API GFNT_Tag gfnt_face_flavour(const GFNT_Face * face);

/**
 * @brief How many tables the directory lists.
 *
 * @param face The face, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_face_table_count(const GFNT_Face * face);

/**
 * @brief The tag of the nth table, in directory order.
 *
 * Directory order rather than sorted order, because a font whose directory is
 * not sorted is a font this library reads and a tool that dumps it should show
 * what the file says.
 *
 * @param face The face.
 * @param index Which entry, from 0.
 * @param out_tag Receives the tag. Written only on success.
 * @return ::GFNT_OK, or ::GFNT_ERR_INVALID.
 */
GFNT_API GFNT_Result gfnt_face_table_tag_at(const GFNT_Face * face,
    size_t index, GFNT_Tag * out_tag);

/**
 * @brief Whether the face has a table.
 *
 * @param face The face, or NULL.
 * @param tag The table tag.
 * @return true if the directory lists it.
 */
GFNT_API bool gfnt_face_has_table(const GFNT_Face * face, GFNT_Tag tag);

/**
 * @brief Where a table is in the blob.
 *
 * For a consumer that needs the bytes themselves - a PDF writer embedding a
 * font program, a tool dumping a table this library does not parse.
 *
 * @param face The face.
 * @param tag The table tag.
 * @param out_offset Receives the offset from the start of the blob, or NULL.
 * @param out_length Receives the length, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_UNSUPPORTED if the
 *   face has no such table.
 */
GFNT_API GFNT_Result gfnt_face_table_range(const GFNT_Face * face,
    GFNT_Tag tag, size_t * out_offset, size_t * out_length);

/**
 * @brief Check a table's checksum, reporting both numbers.
 *
 * **Reported, never enforced.** A font with a bad checksum renders in every
 * other implementation, so refusing it here would make this library the only
 * one that cannot read a font the user can see elsewhere
 * (documentation/design.md section 7.1). The `head` table's stored checksum is
 * computed with `checkSumAdjustment` zeroed, as the specification requires,
 * and this call does the same so that the comparison means something.
 *
 * @param face The face.
 * @param tag The table tag.
 * @param out_stored Receives the directory's checksum, or NULL.
 * @param out_computed Receives the checksum of the bytes, or NULL.
 * @return ::GFNT_OK (whether or not the two agree), ::GFNT_ERR_INVALID, or
 *   ::GFNT_ERR_UNSUPPORTED if the face has no such table.
 */
GFNT_API GFNT_Result gfnt_face_table_checksum(const GFNT_Face * face,
    GFNT_Tag tag, uint32_t * out_stored, uint32_t * out_computed);

/**
 * @brief Write the face's directory to a stream, one table per line.
 *
 * The `_dump` every top-level parsed object has (CONVENTIONS.md section 3),
 * and the input `ttx_diff` compares against fontTools.
 *
 * @param face The face.
 * @param out The stream.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, or ::GFNT_ERR_IO.
 */
GFNT_API GFNT_Result gfnt_face_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_FACE_H
