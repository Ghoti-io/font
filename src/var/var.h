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
 * Internal shape of `fvar` and `avar`, and the parsers for both.
 *
 * documentation/design.md section 7.7. The public face of this is
 * `variation.h`; what is here is what the face memoises and the one function
 * `gvar` needs that a caller does not: whether a face has a design space at all.
 */

#ifndef GHOTI_IO_GFNT_VAR_H
#define GHOTI_IO_GFNT_VAR_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/variation.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads first: `fvar`. */
#define GFNT_TAG_FVAR GFNT_TAG('f', 'v', 'a', 'r')
/** @brief And the one that bends it: `avar`. */
#define GFNT_TAG_AVAR GFNT_TAG('a', 'v', 'a', 'r')

/** @brief Bytes in the `fvar` header. */
#define GFNT_FVAR_HEADER_BYTES 16u
/** @brief Bytes in one `VariationAxisRecord`, which the format fixes. */
#define GFNT_FVAR_AXIS_BYTES 20u

/** One instance's fields besides its coordinates. */
typedef struct GFNT_FvarInstance {
  uint16_t name_id;
  uint16_t flags;
  uint16_t postscript_name_id;
} GFNT_FvarInstance;

/**
 * A parsed `fvar`.
 *
 * Everything is copied out of the blob, because the table is big-endian and a
 * caller is handed `const GFNT_F16Dot16 *` for an instance's coordinates: a
 * pointer into the file would be those values in the wrong byte order on a
 * little-endian host.
 */
typedef struct GFNT_Fvar {
  GFNT_Axis * axes;                 ///< One per axis, in file order.
  size_t axis_count;
  GFNT_FvarInstance * instances;    ///< One per named instance.
  GFNT_F16Dot16 * coordinates;      ///< `instance_count * axis_count` values.
  size_t instance_count;
} GFNT_Fvar;

/** One (from, to) pair of an `avar` segment map. */
typedef struct GFNT_AvarPair {
  GFNT_F2Dot14 from;
  GFNT_F2Dot14 to;
} GFNT_AvarPair;

/**
 * A parsed `avar`: one segment map per axis, in one array.
 *
 * `first[i]` and `count[i]` say where axis `i`'s pairs are in `pairs`. A count
 * of zero is an axis with no map, which normalises as the identity.
 */
typedef struct GFNT_Avar {
  GFNT_AvarPair * pairs;
  size_t * first;
  uint16_t * count;
  size_t axis_count;
  uint16_t major;        ///< 1, or 2 when the table carries a variation store.
  uint32_t map_offset;   ///< Version 2: the axis index map, from the table; 0 for none.
  uint32_t store_offset; ///< Version 2: the item variation store; 0 for none.
} GFNT_Avar;

/** The `fvar` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_fvar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);
/** The `avar` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_avar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);

/** Release what an `fvar` parse allocated. Harmless on a zeroed struct. */
void gfnt_fvar_release(const GFNT_Allocator * allocator, void * table);
/** Release what an `avar` parse allocated. Harmless on a zeroed struct. */
void gfnt_avar_release(const GFNT_Allocator * allocator, void * table);

/** The memoised `fvar`. */
GFNT_Result gfnt_face_fvar(const GFNT_Face * face, const GFNT_Fvar ** out_fvar,
    GFNT_Error * error);
/**
 * Whether a variation asks for anything but the default instance, and whether it
 * is a well-formed request at all.
 *
 * **One answer for every accessor that takes a variation**, because "a variation
 * that moves nothing is the default instance" is a rule, and a rule stated in two
 * places is two rules the first time one of them is changed. An outline and an
 * advance must agree about what a caller who passes all zeros has asked for.
 *
 * @param face The face.
 * @param glyph The glyph the call is about, for the diagnostic, or
 *   ::GFNT_GLYPH_NONE.
 * @param variation The variation, or NULL.
 * @param out_moves Receives false for NULL, for no coordinates, and for every
 *   coordinate zero; true when some coordinate is not. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for coordinates promised and not given or
 *   more of them than the face has axes; or what reading `fvar` returned.
 */
GFNT_Result gfnt_variation_moves(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, bool * out_moves, GFNT_Error * error);

/** The memoised `avar`. */
GFNT_Result gfnt_face_avar(const GFNT_Face * face, const GFNT_Avar ** out_avar,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_VAR_H
