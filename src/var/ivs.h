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
 * Item variation stores and delta-set index maps: the structure `HVAR` and `MVAR`
 * keep their deltas in.
 *
 * documentation/design.md section 7.7. A store is a list of regions of the
 * design space, and rows of deltas, one per item, with one delta per region the
 * row's group names. An item's value at a location is the sum of its deltas, each
 * scaled by how far inside its region the location is - the same rule `gvar`'s
 * tuples follow, and computed by the same function.
 *
 * Nothing here is copied out of the file. A store is read in place at each
 * lookup, because a lookup touches one row and a face has no use for a parsed
 * copy of thousands.
 *
 * Reference: OpenType Specification 1.9, "Item variation stores" and "Delta-set
 * index map" in the Font Variations Overview.
 */

#ifndef GHOTI_IO_GFNT_IVS_H
#define GHOTI_IO_GFNT_IVS_H

#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdint.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One item's delta at a location.
 *
 * @param face The face, whose `fvar` fixes the axis count the store must agree
 *   with.
 * @param store A reader whose extent **begins at the store**.
 * @param table The table the store is in, for the diagnostic.
 * @param coordinates The location: normalised 2.14, in `fvar` order, or NULL.
 * @param coordinate_count How many were given; axes past it are zero.
 * @param outer Which group of rows.
 * @param inner Which row in the group.
 * @param out_delta Receives the sum in font units with ::GFNT_GVAR_FRACTION_BITS
 *   fractional bits. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_CORRUPT for a store that contradicts itself or an
 *   index it does not have; ::GFNT_ERR_UNSUPPORTED for a store format other
 *   than 1; ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_ivs_delta(const GFNT_Face * face, const GFNT_Reader * store,
    GFNT_Tag table, const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    uint32_t outer, uint32_t inner, int64_t * out_delta, GFNT_Error * error);

/**
 * Which row a glyph (or any other index) is in, through a delta-set index map.
 *
 * An index past the map's end takes the **last** entry, which is the format's own
 * compression for a font whose trailing glyphs share one row.
 *
 * @param map A reader whose extent begins at the map.
 * @param table The table the map is in, for the diagnostic.
 * @param index The glyph.
 * @param out_outer Receives the outer index.
 * @param out_inner Receives the inner index.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_CORRUPT or ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_Result gfnt_ivs_map(const GFNT_Reader * map, GFNT_Tag table,
    uint32_t index, uint32_t * out_outer, uint32_t * out_inner,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_IVS_H
