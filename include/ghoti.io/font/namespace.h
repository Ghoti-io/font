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
#define GFNT_Box GHOTIIO_FONT(GFNT_Box)
#define GFNT_BitmapGlyph GHOTIIO_FONT(GFNT_BitmapGlyph)
#define GFNT_Blob GHOTIIO_FONT(GFNT_Blob)
#define GFNT_BlobOwnership GHOTIIO_FONT(GFNT_BlobOwnership)
#define GFNT_CmapSubtable GHOTIIO_FONT(GFNT_CmapSubtable)
#define GFNT_Face GHOTIIO_FONT(GFNT_Face)
#define GFNT_Error GHOTIIO_FONT(GFNT_Error)
#define GFNT_F16Dot16 GHOTIIO_FONT(GFNT_F16Dot16)
#define GFNT_F26Dot6 GHOTIIO_FONT(GFNT_F26Dot6)
#define GFNT_F2Dot14 GHOTIIO_FONT(GFNT_F2Dot14)
#define GFNT_GlyphKind GHOTIIO_FONT(GFNT_GlyphKind)
#define GFNT_Head GHOTIIO_FONT(GFNT_Head)
#define GFNT_Hhea GHOTIIO_FONT(GFNT_Hhea)
#define GFNT_LineMetrics GHOTIIO_FONT(GFNT_LineMetrics)
#define GFNT_LineMetricsPolicy GHOTIIO_FONT(GFNT_LineMetricsPolicy)
#define GFNT_NameRecord GHOTIIO_FONT(GFNT_NameRecord)
#define GFNT_Os2 GHOTIIO_FONT(GFNT_Os2)
#define GFNT_Outline GHOTIIO_FONT(GFNT_Outline)
#define GFNT_OutlineSink GHOTIIO_FONT(GFNT_OutlineSink)
#define GFNT_OutlineSpace GHOTIIO_FONT(GFNT_OutlineSpace)
#define GFNT_Point GHOTIIO_FONT(GFNT_Point)
#define GFNT_Coverage GHOTIIO_FONT(GFNT_Coverage)
#define GFNT_FillRule GHOTIIO_FONT(GFNT_FillRule)
#define GFNT_RasterOptions GHOTIIO_FONT(GFNT_RasterOptions)
#define GFNT_PointTag GHOTIIO_FONT(GFNT_PointTag)
#define GFNT_Post GHOTIIO_FONT(GFNT_Post)
#define GFNT_Limits GHOTIIO_FONT(GFNT_Limits)
#define GFNT_SfntTable GHOTIIO_FONT(GFNT_SfntTable)
#define GFNT_Result GHOTIIO_FONT(GFNT_Result)
#define GFNT_Strike GHOTIIO_FONT(GFNT_Strike)
#define GFNT_ScaledStrike GHOTIIO_FONT(GFNT_ScaledStrike)
#define GFNT_StrikePolicy GHOTIIO_FONT(GFNT_StrikePolicy)
#define GFNT_Tag GHOTIIO_FONT(GFNT_Tag)
#define GFNT_Variation GHOTIIO_FONT(GFNT_Variation)
#define GFNT_DeltaRounding GHOTIIO_FONT(GFNT_DeltaRounding)
#define GFNT_Axis GHOTIIO_FONT(GFNT_Axis)
#define GFNT_NamedInstance GHOTIIO_FONT(GFNT_NamedInstance)

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
#define gfnt_face_has_outlines GHOTIIO_FONT(gfnt_face_has_outlines)
#define gfnt_face_glyph_for_codepoint GHOTIIO_FONT(gfnt_face_glyph_for_codepoint)
#define gfnt_face_glyph_side_bearing GHOTIIO_FONT(gfnt_face_glyph_side_bearing)
#define gfnt_face_glyph_name GHOTIIO_FONT(gfnt_face_glyph_name)
#define gfnt_face_glyph_for_name GHOTIIO_FONT(gfnt_face_glyph_for_name)
#define gfnt_face_glyph_names_dump GHOTIIO_FONT(gfnt_face_glyph_names_dump)
#define gfnt_face_glyph_outline GHOTIIO_FONT(gfnt_face_glyph_outline)
#define gfnt_face_glyph_is_composite GHOTIIO_FONT(gfnt_face_glyph_is_composite)
#define gfnt_face_glyph_stated_box GHOTIIO_FONT(gfnt_face_glyph_stated_box)
#define gfnt_face_glyph_charstring GHOTIIO_FONT(gfnt_face_glyph_charstring)
#define gfnt_face_glyph_charstring_metrics GHOTIIO_FONT(gfnt_face_glyph_charstring_metrics)
#define gfnt_charstring_run GHOTIIO_FONT(gfnt_charstring_run)
#define gfnt_charstring_dump GHOTIIO_FONT(gfnt_charstring_dump)
#define gfnt_charstring_type_string GHOTIIO_FONT(gfnt_charstring_type_string)
#define gfnt_glyph_name_free GHOTIIO_FONT(gfnt_glyph_name_free)
#define gfnt_face_head GHOTIIO_FONT(gfnt_face_head)
#define gfnt_face_hhea GHOTIIO_FONT(gfnt_face_hhea)
#define gfnt_face_line_metrics GHOTIIO_FONT(gfnt_face_line_metrics)
#define gfnt_face_name GHOTIIO_FONT(gfnt_face_name)
#define gfnt_face_name_at GHOTIIO_FONT(gfnt_face_name_at)
#define gfnt_face_name_count GHOTIIO_FONT(gfnt_face_name_count)
#define gfnt_face_name_decode GHOTIIO_FONT(gfnt_face_name_decode)
#define gfnt_face_name_dump GHOTIIO_FONT(gfnt_face_name_dump)
#define gfnt_face_num_glyphs GHOTIIO_FONT(gfnt_face_num_glyphs)
#define gfnt_face_select_strike GHOTIIO_FONT(gfnt_face_select_strike)
#define gfnt_face_strike_at GHOTIIO_FONT(gfnt_face_strike_at)
#define gfnt_face_scaled_strike_at GHOTIIO_FONT(gfnt_face_scaled_strike_at)
#define gfnt_face_scaled_strike_count GHOTIIO_FONT(gfnt_face_scaled_strike_count)
#define gfnt_bitmap_dump GHOTIIO_FONT(gfnt_bitmap_dump)
#define gfnt_bitmap_pixel GHOTIIO_FONT(gfnt_bitmap_pixel)
#define gfnt_coverage_from_bitmap GHOTIIO_FONT(gfnt_coverage_from_bitmap)
#define gfnt_face_bitmap_encoding_at GHOTIIO_FONT(gfnt_face_bitmap_encoding_at)
#define gfnt_face_bitmap_encoding_count GHOTIIO_FONT(gfnt_face_bitmap_encoding_count)
#define gfnt_face_glyph_bitmap GHOTIIO_FONT(gfnt_face_glyph_bitmap)
#define gfnt_face_strike_count GHOTIIO_FONT(gfnt_face_strike_count)
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
#define gfnt_glyph_kind_string GHOTIIO_FONT(gfnt_glyph_kind_string)
#define gfnt_head_dump GHOTIIO_FONT(gfnt_head_dump)
#define gfnt_hhea_dump GHOTIIO_FONT(gfnt_hhea_dump)
#define gfnt_name_free GHOTIIO_FONT(gfnt_name_free)
#define gfnt_box_is_empty GHOTIIO_FONT(gfnt_box_is_empty)
#define gfnt_coverage_apply_table GHOTIIO_FONT(gfnt_coverage_apply_table)
#define gfnt_coverage_at GHOTIIO_FONT(gfnt_coverage_at)
#define gfnt_coverage_destroy GHOTIIO_FONT(gfnt_coverage_destroy)
#define gfnt_coverage_dump GHOTIIO_FONT(gfnt_coverage_dump)
#define gfnt_coverage_dump_art GHOTIIO_FONT(gfnt_coverage_dump_art)
#define gfnt_coverage_hash GHOTIIO_FONT(gfnt_coverage_hash)
#define gfnt_coverage_total GHOTIIO_FONT(gfnt_coverage_total)
#define gfnt_face_render_glyph GHOTIIO_FONT(gfnt_face_render_glyph)
#define gfnt_fill_rule_string GHOTIIO_FONT(gfnt_fill_rule_string)
#define gfnt_raster_outline GHOTIIO_FONT(gfnt_raster_outline)
#define gfnt_outline_add_point GHOTIIO_FONT(gfnt_outline_add_point)
#define gfnt_outline_begin_contour GHOTIIO_FONT(gfnt_outline_begin_contour)
#define gfnt_outline_bounds GHOTIIO_FONT(gfnt_outline_bounds)
#define gfnt_outline_clear GHOTIIO_FONT(gfnt_outline_clear)
#define gfnt_outline_contour_at GHOTIIO_FONT(gfnt_outline_contour_at)
#define gfnt_outline_contour_count GHOTIIO_FONT(gfnt_outline_contour_count)
#define gfnt_outline_control_box GHOTIIO_FONT(gfnt_outline_control_box)
#define gfnt_outline_create GHOTIIO_FONT(gfnt_outline_create)
#define gfnt_outline_decompose GHOTIIO_FONT(gfnt_outline_decompose)
#define gfnt_outline_destroy GHOTIIO_FONT(gfnt_outline_destroy)
#define gfnt_outline_dump GHOTIIO_FONT(gfnt_outline_dump)
#define gfnt_outline_path_dump GHOTIIO_FONT(gfnt_outline_path_dump)
#define gfnt_outline_point_at GHOTIIO_FONT(gfnt_outline_point_at)
#define gfnt_outline_point_count GHOTIIO_FONT(gfnt_outline_point_count)
#define gfnt_outline_scale GHOTIIO_FONT(gfnt_outline_scale)
#define gfnt_outline_space GHOTIIO_FONT(gfnt_outline_space)
#define gfnt_outline_space_string GHOTIIO_FONT(gfnt_outline_space_string)
#define gfnt_outline_transform GHOTIIO_FONT(gfnt_outline_transform)
#define gfnt_outline_translate GHOTIIO_FONT(gfnt_outline_translate)
#define gfnt_point_tag_string GHOTIIO_FONT(gfnt_point_tag_string)
#define gfnt_os2_dump GHOTIIO_FONT(gfnt_os2_dump)
#define gfnt_post_dump GHOTIIO_FONT(gfnt_post_dump)
#define gfnt_limits_default GHOTIIO_FONT(gfnt_limits_default)
#define gfnt_result_string GHOTIIO_FONT(gfnt_result_string)
#define gfnt_scale_for_ppem GHOTIIO_FONT(gfnt_scale_for_ppem)
#define gfnt_tag_string GHOTIIO_FONT(gfnt_tag_string)
#define gfnt_units_to_pixels GHOTIIO_FONT(gfnt_units_to_pixels)
#define gfnt_version_number GHOTIIO_FONT(gfnt_version_number)
#define gfnt_version_string GHOTIIO_FONT(gfnt_version_string)
#define gfnt_face_is_variable GHOTIIO_FONT(gfnt_face_is_variable)
#define gfnt_face_axis_count GHOTIIO_FONT(gfnt_face_axis_count)
#define gfnt_face_axis_at GHOTIIO_FONT(gfnt_face_axis_at)
#define gfnt_face_instance_count GHOTIIO_FONT(gfnt_face_instance_count)
#define gfnt_face_instance_at GHOTIIO_FONT(gfnt_face_instance_at)
#define gfnt_face_normalize GHOTIIO_FONT(gfnt_face_normalize)
#define gfnt_face_variation_dump GHOTIIO_FONT(gfnt_face_variation_dump)
#define gfnt_face_has_stat GHOTIIO_FONT(gfnt_face_has_stat)
#define gfnt_face_stat_axis_count GHOTIIO_FONT(gfnt_face_stat_axis_count)
#define gfnt_face_stat_axis_at GHOTIIO_FONT(gfnt_face_stat_axis_at)
#define gfnt_face_stat_value_count GHOTIIO_FONT(gfnt_face_stat_value_count)
#define gfnt_face_stat_value_at GHOTIIO_FONT(gfnt_face_stat_value_at)
#define gfnt_face_stat_value_pair GHOTIIO_FONT(gfnt_face_stat_value_pair)
#define gfnt_face_stat_elided_fallback GHOTIIO_FONT(gfnt_face_stat_elided_fallback)
#define gfnt_face_stat_match GHOTIIO_FONT(gfnt_face_stat_match)
#define gfnt_face_stat_dump GHOTIIO_FONT(gfnt_face_stat_dump)
#define gfnt_face_feature_variations_count GHOTIIO_FONT(gfnt_face_feature_variations_count)
#define gfnt_face_feature_variations_match GHOTIIO_FONT(gfnt_face_feature_variations_match)
#define gfnt_face_feature_substitution_count GHOTIIO_FONT(gfnt_face_feature_substitution_count)
#define gfnt_face_feature_substitution_at GHOTIIO_FONT(gfnt_face_feature_substitution_at)
#define gfnt_face_feature_substitution_lookup GHOTIIO_FONT(gfnt_face_feature_substitution_lookup)
#define gfnt_face_feature_variations_dump GHOTIIO_FONT(gfnt_face_feature_variations_dump)
#define gfnt_face_cvt_count GHOTIIO_FONT(gfnt_face_cvt_count)
#define gfnt_face_cvt_values GHOTIIO_FONT(gfnt_face_cvt_values)
#define GFNT_Direction GHOTIIO_FONT(GFNT_Direction)
#define GFNT_ShapeFeature GHOTIIO_FONT(GFNT_ShapeFeature)
#define GFNT_ShapeOptions GHOTIIO_FONT(GFNT_ShapeOptions)
#define GFNT_ShapedGlyph GHOTIIO_FONT(GFNT_ShapedGlyph)
#define GFNT_ShapedRun GHOTIIO_FONT(GFNT_ShapedRun)
#define gfnt_face_shape GHOTIIO_FONT(gfnt_face_shape)
#define gfnt_shaped_run_free GHOTIIO_FONT(gfnt_shaped_run_free)
#define gfnt_faces_shape GHOTIIO_FONT(gfnt_faces_shape)
#define gfnt_face_runs_free GHOTIIO_FONT(gfnt_face_runs_free)
#define gfnt_face_layout_dump GHOTIIO_FONT(gfnt_face_layout_dump)
#define gfnt_shape_script_of GHOTIIO_FONT(gfnt_shape_script_of)
#define gfnt_shape_script_direction GHOTIIO_FONT(gfnt_shape_script_direction)
/// @endcond

#endif // GHOTI_IO_GFNT_NAMESPACE_H
