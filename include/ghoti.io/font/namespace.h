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
#define GFNT_Blob GHOTIIO_FONT(GFNT_Blob)
#define GFNT_BlobOwnership GHOTIIO_FONT(GFNT_BlobOwnership)
#define GFNT_Error GHOTIIO_FONT(GFNT_Error)
#define GFNT_F16Dot16 GHOTIIO_FONT(GFNT_F16Dot16)
#define GFNT_F26Dot6 GHOTIIO_FONT(GFNT_F26Dot6)
#define GFNT_F2Dot14 GHOTIIO_FONT(GFNT_F2Dot14)
#define GFNT_Limits GHOTIIO_FONT(GFNT_Limits)
#define GFNT_Result GHOTIIO_FONT(GFNT_Result)
#define GFNT_Tag GHOTIIO_FONT(GFNT_Tag)

// Public functions.
#define gfnt_allocator_default GHOTIIO_FONT(gfnt_allocator_default)
#define gfnt_blob_create_file GHOTIIO_FONT(gfnt_blob_create_file)
#define gfnt_blob_create_memory GHOTIIO_FONT(gfnt_blob_create_memory)
#define gfnt_blob_create_mmap GHOTIIO_FONT(gfnt_blob_create_mmap)
#define gfnt_blob_data GHOTIIO_FONT(gfnt_blob_data)
#define gfnt_blob_destroy GHOTIIO_FONT(gfnt_blob_destroy)
#define gfnt_blob_dump GHOTIIO_FONT(gfnt_blob_dump)
#define gfnt_blob_is_mapped GHOTIIO_FONT(gfnt_blob_is_mapped)
#define gfnt_blob_size GHOTIIO_FONT(gfnt_blob_size)
#define gfnt_error_clear GHOTIIO_FONT(gfnt_error_clear)
#define gfnt_error_dump GHOTIIO_FONT(gfnt_error_dump)
#define gfnt_error_set GHOTIIO_FONT(gfnt_error_set)
#define gfnt_f16dot16_div GHOTIIO_FONT(gfnt_f16dot16_div)
#define gfnt_f16dot16_mul GHOTIIO_FONT(gfnt_f16dot16_mul)
#define gfnt_f26dot6_ceil GHOTIIO_FONT(gfnt_f26dot6_ceil)
#define gfnt_f26dot6_floor GHOTIIO_FONT(gfnt_f26dot6_floor)
#define gfnt_f26dot6_round GHOTIIO_FONT(gfnt_f26dot6_round)
#define gfnt_f2dot14_to_f16dot16 GHOTIIO_FONT(gfnt_f2dot14_to_f16dot16)
#define gfnt_limits_default GHOTIIO_FONT(gfnt_limits_default)
#define gfnt_result_string GHOTIIO_FONT(gfnt_result_string)
#define gfnt_scale_for_ppem GHOTIIO_FONT(gfnt_scale_for_ppem)
#define gfnt_tag_string GHOTIIO_FONT(gfnt_tag_string)
#define gfnt_units_to_pixels GHOTIIO_FONT(gfnt_units_to_pixels)
#define gfnt_version_number GHOTIIO_FONT(gfnt_version_number)
#define gfnt_version_string GHOTIIO_FONT(gfnt_version_string)
/// @endcond

#endif // GHOTI_IO_GFNT_NAMESPACE_H
