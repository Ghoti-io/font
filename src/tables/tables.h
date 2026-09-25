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
 * How a table gets parsed exactly once: the memo, the lock, and the one cast
 * that writes through a `const GFNT_Face *`.
 *
 * documentation/design.md sections 5.3, 7.8 and 15.3. Validation is lazy and
 * memoised per table, so a face load costs the directory and a caller that
 * never asks for `OS/2` never pays for it.
 */

#ifndef GHOTI_IO_GFNT_TABLES_H
#define GHOTI_IO_GFNT_TABLES_H

#include <ghoti.io/font/macros.h>
#include "../sfnt/sfnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse one table into caller-supplied storage.
 *
 * @param face The face, whose limits and blob the parser reads.
 * @param out Where the parsed table goes; each parser knows its own type.
 * @param error Receives a diagnostic on failure, or NULL.
 */
typedef GFNT_Result (*GFNT_TableParse)(const GFNT_Face * face, void * out,
    GFNT_Error * error);

/**
 * Parse a table once and answer from the memo thereafter.
 *
 * **The parse runs with no lock held**, into @p scratch, and only the
 * publication into @p storage takes the face's lock. That is not an
 * optimisation: one table's parse legitimately needs another's - the numGlyphs
 * minimum (M12) has to read `hhea` and `hmtx` to know what they imply - and a
 * parser that called back into this function while it held a non-recursive
 * mutex would deadlock against itself on the first font that had both tables.
 *
 * The cost is that two threads asking for the same table at the same moment
 * may both parse it. They parse the same immutable bytes with the same code
 * and reach the same answer, the first to finish publishes, and the second
 * takes the published memo - so the duplicate work is invisible and there is
 * no second write to @p storage to race with the first.
 *
 * @param face The face.
 * @param state The memo for this table, a field of @p face.
 * @param storage Where the parsed table lives, a field of @p face.
 * @param scratch Caller-owned storage of the same type, for the parse itself;
 *   the caller declares it locally, which is what keeps this function free of
 *   any knowledge of the types it copies.
 * @param size `sizeof` that type.
 * @param parse The parser to run on the first call.
 * @param error Receives the diagnostic - the first attempt's, replayed - when
 *   the result is a failure. May be NULL.
 * @return whatever the parse returned, the first time and every time after.
 */
GFNT_Result gfnt_table_cached(const GFNT_Face * face, GFNT_Cached * state,
    void * storage, void * scratch, size_t size, GFNT_TableParse parse,
    GFNT_Error * error);

/** The `head` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_head_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);
/** The `hhea` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_hhea_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);
/** The `OS/2` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_os2_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);
/** The `post` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_post_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_TABLES_H
