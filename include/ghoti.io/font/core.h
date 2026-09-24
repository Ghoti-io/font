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
 * Core types, result codes, limits and the version for the Ghoti.io Font
 * library.
 */

#ifndef GHOTI_IO_GFNT_CORE_H
#define GHOTI_IO_GFNT_CORE_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code for font library operations.
 *
 * The fixed vocabulary of CONVENTIONS.md section 5. Zero is success and
 * GFNT_RESULT_COUNT closes the enum so that a test can check the string table
 * is complete.
 */
typedef enum {
  GFNT_OK = 0,          ///< Operation succeeded.
  GFNT_ERR_IO,          ///< I/O error (read/write/seek failed).
  GFNT_ERR_FORMAT,      ///< Not a format this library recognises.
  GFNT_ERR_UNSUPPORTED, ///< This format, but a feature not implemented.
  GFNT_ERR_LIMIT,       ///< A GFNT_Limits field was exceeded.
  GFNT_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GFNT_ERR_OOM,         ///< The allocator returned NULL.
  GFNT_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GFNT_ERR_INTERNAL,    ///< The library's own invariant failed.
  GFNT_RESULT_COUNT
} GFNT_Result;

/**
 * @brief Convert a result code to a human-readable string.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @param result The result code.
 * @return A description of the result code, never NULL.
 */
GFNT_API const char * gfnt_result_string(GFNT_Result result);

/**
 * @brief Caps applied while reading a font, so that a hostile file
 * cannot make the library allocate, recurse or loop without bound. Every
 * parser takes one; NULL means the defaults. See documentation/design.md
 * section 15.2, and section 6.2 for the recursion budgets among them.
 */
typedef struct GFNT_Limits {
  size_t max_blob_bytes;      ///< Largest font file accepted. 256 MiB.
  size_t max_tables;          ///< sfnt table directory entries. 512.
  size_t max_glyphs;          ///< Glyphs per face; the format's own 65,535.
  size_t max_composite_depth; ///< Composite glyph nesting. 16.
  size_t max_outline_points;  ///< Points in one glyph's outline. 65,536.
  size_t max_contours;        ///< Contours in one glyph. 4,096.
  size_t max_ppem;            ///< Largest pixel size rasterised. 4,096.
  size_t max_raster_bytes;    ///< Coverage bytes for one glyph. 64 MiB.
  size_t max_strikes;         ///< Bitmap strikes per face. 256.
  size_t max_name_records;    ///< `name` table records. 4,096.
  size_t max_axes;            ///< Variation axes. 64.
  size_t max_lookup_depth;    ///< Nested GSUB/GPOS lookups. 6.
  size_t max_ops_per_glyph;   ///< Substitutions at one position. 64.
  size_t max_paint_depth;     ///< COLR v1 paint graph depth. 64.
  size_t max_run_bytes;       ///< Text handed to one shaping call. 16 MiB.
  size_t max_line_length;     ///< BDF and .hex line length. 4,096.
} GFNT_Limits;

/**
 * @brief Fill in the default limits.
 *
 * @param limits Structure to populate. NULL is ignored.
 */
GFNT_API void gfnt_limits_default(GFNT_Limits * limits);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * "0.0.0", or "0.0.0-dev" for a build with an overridden BRANCH. The same
 * string is GFNT_VERSION_STRING at compile time; this is the one the linked
 * library reports, which is the one that matters when the two differ.
 *
 * @return A static string, never NULL.
 */
GFNT_API const char * gfnt_version_string(void);

/**
 * @brief This build's version, packed as GFNT_MAKE_VERSION() packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GFNT_API unsigned gfnt_version_number(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CORE_H
