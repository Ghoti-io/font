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
#define GFNT_CmapSubtable GHOTIIO_FONT(GFNT_CmapSubtable)
#define GFNT_Face GHOTIIO_FONT(GFNT_Face)
#define GFNT_Error GHOTIIO_FONT(GFNT_Error)
#define GFNT_F16Dot16 GHOTIIO_FONT(GFNT_F16Dot16)
#define GFNT_F26Dot6 GHOTIIO_FONT(GFNT_F26Dot6)
#define GFNT_F2Dot14 GHOTIIO_FONT(GFNT_F2Dot14)
#define GFNT_Head GHOTIIO_FONT(GFNT_Head)
#define GFNT_Hhea GHOTIIO_FONT(GFNT_Hhea)
#define GFNT_LineMetrics GHOTIIO_FONT(GFNT_LineMetrics)
#define GFNT_LineMetricsPolicy GHOTIIO_FONT(GFNT_LineMetricsPolicy)
#define GFNT_NameRecord GHOTIIO_FONT(GFNT_NameRecord)
#define GFNT_Os2 GHOTIIO_FONT(GFNT_Os2)
#define GFNT_Post GHOTIIO_FONT(GFNT_Post)
#define GFNT_Limits GHOTIIO_FONT(GFNT_Limits)
#define GFNT_SfntTable GHOTIIO_FONT(GFNT_SfntTable)
#define GFNT_Result GHOTIIO_FONT(GFNT_Result)
#define GFNT_Tag GHOTIIO_FONT(GFNT_Tag)
#define GFNT_Variation GHOTIIO_FONT(GFNT_Variation)

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
#define gfnt_face_cmap_at GHOTIIO_FONT(gfnt_face_cmap_at)
#define gfnt_face_cmap_best GHOTIIO_FONT(gfnt_face_cmap_best)
#define gfnt_face_cmap_count GHOTIIO_FONT(gfnt_face_cmap_count)
#define gfnt_face_cmap_dump GHOTIIO_FONT(gfnt_face_cmap_dump)
#define gfnt_face_count GHOTIIO_FONT(gfnt_face_count)
#define gfnt_face_dump GHOTIIO_FONT(gfnt_face_dump)
#define gfnt_face_flavour GHOTIIO_FONT(gfnt_face_flavour)
#define gfnt_face_free GHOTIIO_FONT(gfnt_face_free)
#define gfnt_face_has_table GHOTIIO_FONT(gfnt_face_has_table)
#define gfnt_face_glyph_advance GHOTIIO_FONT(gfnt_face_glyph_advance)
#define gfnt_face_glyph_for_codepoint GHOTIIO_FONT(gfnt_face_glyph_for_codepoint)
#define gfnt_face_glyph_side_bearing GHOTIIO_FONT(gfnt_face_glyph_side_bearing)
#define gfnt_face_head GHOTIIO_FONT(gfnt_face_head)
#define gfnt_face_hhea GHOTIIO_FONT(gfnt_face_hhea)
#define gfnt_face_line_metrics GHOTIIO_FONT(gfnt_face_line_metrics)
#define gfnt_face_name GHOTIIO_FONT(gfnt_face_name)
#define gfnt_face_name_at GHOTIIO_FONT(gfnt_face_name_at)
#define gfnt_face_name_count GHOTIIO_FONT(gfnt_face_name_count)
#define gfnt_face_name_decode GHOTIIO_FONT(gfnt_face_name_decode)
#define gfnt_face_name_dump GHOTIIO_FONT(gfnt_face_name_dump)
#define gfnt_face_num_glyphs GHOTIIO_FONT(gfnt_face_num_glyphs)
#define gfnt_face_num_glyphs_disagreement GHOTIIO_FONT(gfnt_face_num_glyphs_disagreement)
#define gfnt_face_os2 GHOTIIO_FONT(gfnt_face_os2)
#define gfnt_face_post GHOTIIO_FONT(gfnt_face_post)
#define gfnt_face_units_per_em GHOTIIO_FONT(gfnt_face_units_per_em)
#define gfnt_face_index GHOTIIO_FONT(gfnt_face_index)
#define gfnt_face_load GHOTIIO_FONT(gfnt_face_load)
#define gfnt_face_table_checksum GHOTIIO_FONT(gfnt_face_table_checksum)
#define gfnt_face_table_count GHOTIIO_FONT(gfnt_face_table_count)
#define gfnt_face_table_range GHOTIIO_FONT(gfnt_face_table_range)
#define gfnt_face_table_tag_at GHOTIIO_FONT(gfnt_face_table_tag_at)
#define gfnt_error_dump GHOTIIO_FONT(gfnt_error_dump)
#define gfnt_error_set GHOTIIO_FONT(gfnt_error_set)
#define gfnt_f16dot16_div GHOTIIO_FONT(gfnt_f16dot16_div)
#define gfnt_f16dot16_mul GHOTIIO_FONT(gfnt_f16dot16_mul)
#define gfnt_f26dot6_ceil GHOTIIO_FONT(gfnt_f26dot6_ceil)
#define gfnt_f26dot6_floor GHOTIIO_FONT(gfnt_f26dot6_floor)
#define gfnt_f26dot6_round GHOTIIO_FONT(gfnt_f26dot6_round)
#define gfnt_f2dot14_to_f16dot16 GHOTIIO_FONT(gfnt_f2dot14_to_f16dot16)
#define gfnt_cmap_lookup GHOTIIO_FONT(gfnt_cmap_lookup)
#define gfnt_head_dump GHOTIIO_FONT(gfnt_head_dump)
#define gfnt_hhea_dump GHOTIIO_FONT(gfnt_hhea_dump)
#define gfnt_name_free GHOTIIO_FONT(gfnt_name_free)
#define gfnt_os2_dump GHOTIIO_FONT(gfnt_os2_dump)
#define gfnt_post_dump GHOTIIO_FONT(gfnt_post_dump)
#define gfnt_limits_default GHOTIIO_FONT(gfnt_limits_default)
#define gfnt_result_string GHOTIIO_FONT(gfnt_result_string)
#define gfnt_scale_for_ppem GHOTIIO_FONT(gfnt_scale_for_ppem)
#define gfnt_tag_string GHOTIIO_FONT(gfnt_tag_string)
#define gfnt_units_to_pixels GHOTIIO_FONT(gfnt_units_to_pixels)
#define gfnt_version_number GHOTIIO_FONT(gfnt_version_number)
#define gfnt_version_string GHOTIIO_FONT(gfnt_version_string)
/// @endcond

#endif // GHOTI_IO_GFNT_NAMESPACE_H
