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
 * The part of font matching the fallback shares. Not installed.
 */

#ifndef GHOTI_IO_GFNT_MATCH_INT_H
#define GHOTI_IO_GFNT_MATCH_INT_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/discover.h>
#include <ghoti.io/font/match.h>

/**
 * Narrow @p n faces (all of one family, or any set a caller wants compared) to the one the
 * query's width, style and weight choose.
 *
 * @param recs The candidates; at least one.
 * @param out_pick Receives the survivor's place in @p recs: the first that remains.
 * @param out_values Receives the axis values to apply, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for no candidates, ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_match_pick(const GFNT_FontRecord * const * recs, size_t n,
    const GFNT_FontQuery * query, const GFNT_Allocator * allocator, size_t * out_pick,
    GFNT_FontMatch * out_values);

#endif // GHOTI_IO_GFNT_MATCH_INT_H
