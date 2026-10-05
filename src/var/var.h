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
/** The memoised `avar`. */
GFNT_Result gfnt_face_avar(const GFNT_Face * face, const GFNT_Avar ** out_avar,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_VAR_H
