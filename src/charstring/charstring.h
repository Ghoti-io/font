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
 * The one thing the charstring interpreter needs of itself that a caller does
 * not: a run that knows it is already inside an accented character.
 */

#ifndef GHOTI_IO_GFNT_CHARSTRING_INTERNAL_H
#define GHOTI_IO_GFNT_CHARSTRING_INTERNAL_H

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run a charstring, saying whether this run is a component of a `seac`.
 *
 * ::gfnt_charstring_run() is this with @p in_seac false. The flag is what makes
 * "an accented character whose accent is itself an accented character"
 * terminate: without it, two charstrings naming each other would recurse until
 * the stack ran out, and a limit high enough for real fonts is not a limit that
 * catches it quickly.
 *
 * @param type Which language.
 * @param bytes The charstring.
 * @param length How many bytes.
 * @param context Where the subroutines are.
 * @param outline Receives the contours, appended.
 * @param out_metrics Receives the advance and counts, or NULL.
 * @param in_seac Whether an accented character is already being assembled.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_charstring_run().
 */
GFNT_Result gfnt_charstring_run_nested(GFNT_CharstringType type,
    const uint8_t * bytes, size_t length,
    const GFNT_CharstringContext * context, GFNT_Outline * outline,
    GFNT_CharstringMetrics * out_metrics, bool in_seac, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CHARSTRING_INTERNAL_H
