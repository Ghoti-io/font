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
 * The `hhea` table: horizontal line metrics, and how many entries `hmtx` has.
 *
 * documentation/design.md section 7.2.
 *
 * Reference: OpenType Specification 1.9, "hhea - Horizontal Header Table".
 */

#include <ghoti.io/font/macros.h>
#include "tables.h"

GFNT_Result gfnt_hhea_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
  static const GFNT_Tag tag = GFNT_TAG('h', 'h', 'e', 'a');
  GFNT_Hhea * hhea = out;
  GFNT_Reader reader;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  *hhea = (GFNT_Hhea) {0};
  if (gfnt_read_u16(&reader, &hhea->major_version) != GFNT_OK
      || gfnt_read_u16(&reader, &hhea->minor_version) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->ascender) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->descender) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->line_gap) != GFNT_OK
      || gfnt_read_u16(&reader, &hhea->advance_width_max) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->min_left_side_bearing) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->min_right_side_bearing) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->x_max_extent) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->caret_slope_rise) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->caret_slope_run) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->caret_offset) != GFNT_OK
      // Four reserved int16s, which the specification requires to be zero and
      // which this library does not check: nothing depends on them, and a font
      // that fills them in is not a font anyone should be refused.
      || gfnt_reader_skip(&reader, 8) != GFNT_OK
      || gfnt_read_s16(&reader, &hhea->metric_data_format) != GFNT_OK
      || gfnt_read_u16(&reader, &hhea->number_of_h_metrics) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  if (hhea->metric_data_format != 0) {
    // The only defined format. A different one describes an hmtx this library
    // cannot read, which is UNSUPPORTED rather than corrupt.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 32,
        GFNT_GLYPH_NONE, "hhea metricDataFormat is not the defined one");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_hhea_dump(const GFNT_Hhea * hhea, FILE * out) {
  if (!hhea || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out,
          "hhea: version %u.%u, ascender %d, descender %d, lineGap %d\n",
          hhea->major_version, hhea->minor_version, hhea->ascender,
          hhea->descender, hhea->line_gap)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "hhea: advanceWidthMax %u, minLSB %d, minRSB %d, xMaxExtent %d\n",
          hhea->advance_width_max, hhea->min_left_side_bearing,
          hhea->min_right_side_bearing, hhea->x_max_extent)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "hhea: caretSlope %d/%d, caretOffset %d, numberOfHMetrics %u\n",
          hhea->caret_slope_rise, hhea->caret_slope_run, hhea->caret_offset,
          hhea->number_of_h_metrics)
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
