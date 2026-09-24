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
 * @file namespace.h
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GFNT_NAMESPACE_H
#define GHOTI_IO_GFNT_NAMESPACE_H

#include <ghoti.io/font/libver.h>

/// @cond HIDDEN_SYMBOLS

// Public types. Renamed as well as the functions, so that two versions whose
// structs differ in layout cannot be confused for one another. GCU_* names
// are deliberately absent: they are cutil's, and cutil has already renamed
// them.
#define GFNT_Allocator GHOTIIO_FONT(GFNT_Allocator)
#define GFNT_Limits GHOTIIO_FONT(GFNT_Limits)
#define GFNT_Result GHOTIIO_FONT(GFNT_Result)

// Public functions.
#define gfnt_allocator_default GHOTIIO_FONT(gfnt_allocator_default)
#define gfnt_limits_default GHOTIIO_FONT(gfnt_limits_default)
#define gfnt_result_string GHOTIIO_FONT(gfnt_result_string)
#define gfnt_version_number GHOTIIO_FONT(gfnt_version_number)
#define gfnt_version_string GHOTIIO_FONT(gfnt_version_string)
/// @endcond

#endif // GHOTI_IO_GFNT_NAMESPACE_H
