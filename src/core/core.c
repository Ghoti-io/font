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
 * Result strings, default limits, and the version.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/libver.h>

const char * gfnt_result_string(GFNT_Result result) {
  switch (result) {
    case GFNT_OK:
      return "No error";
    case GFNT_ERR_IO:
      return "I/O error";
    case GFNT_ERR_FORMAT:
      return "Not a recognised format";
    case GFNT_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GFNT_ERR_LIMIT:
      return "Limit exceeded";
    case GFNT_ERR_CORRUPT:
      return "Corrupt data";
    case GFNT_ERR_OOM:
      return "Out of memory";
    case GFNT_ERR_INVALID:
      return "Invalid argument";
    case GFNT_ERR_INTERNAL:
      return "Internal error";
    case GFNT_RESULT_COUNT:
    default:
      return "Unknown error";
  }
}

void gfnt_limits_default(GFNT_Limits * limits) {
  if (!limits) {
    return;
  }

  *limits = (GFNT_Limits) {
    .max_blob_bytes = (size_t)256 * 1024 * 1024,
    .max_tables = 512,
    .max_glyphs = 65535,
    .max_composite_depth = 16,
    .max_outline_points = 65536,
    .max_contours = 4096,
    .max_ppem = 4096,
    .max_raster_bytes = (size_t)64 * 1024 * 1024,
    .max_strikes = 256,
    .max_name_records = 4096,
    .max_axes = 64,
    .max_lookup_depth = 6,
    .max_ops_per_glyph = 64,
    .max_paint_depth = 64,
    .max_run_bytes = (size_t)16 * 1024 * 1024,
    .max_line_length = 4096,
  };
}
