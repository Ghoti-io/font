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
 * Shared by the walk and the platform directory list. Not installed.
 */

#ifndef GHOTI_IO_GFNT_DISCOVER_INT_H
#define GHOTI_IO_GFNT_DISCOVER_INT_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/discover.h>

/**
 * Directories ::gfnt_fontset_scan_system() walks.
 *
 * Only paths that exist are returned, already deduplicated by their
 * canonical path. An absent directory is omitted and is not an error.
 * The caller frees each string and the array with @p allocator.
 */
GFNT_Result gfnt_discover_system_dirs(const GFNT_Allocator * allocator,
    char *** out_dirs, size_t * out_count, GFNT_Error * error);

#endif // GHOTI_IO_GFNT_DISCOVER_INT_H
