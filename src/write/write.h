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
 * What the writer's files share: big-endian stores and the sfnt checksum.
 * Internal to `src/write/`.
 */

#ifndef GHOTI_IO_GFNT_WRITE_INTERNAL_H
#define GHOTI_IO_GFNT_WRITE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/write.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Store a 16-bit value big-endian. */
void gfnt_write_put16(uint8_t * p, uint32_t v);
/** Store a 32-bit value big-endian. */
void gfnt_write_put32(uint8_t * p, uint32_t v);
/** The sfnt checksum of @p length bytes: big-endian words, the tail zero-padded. */
uint32_t gfnt_write_checksum(const uint8_t * data, size_t length);

/**
 * Build the sfnt bytes. The shared body of ::gfnt_write_sfnt() and
 * ::gfnt_write_woff(); the caller frees @p out_bytes with @p allocator.
 */
GFNT_Result gfnt_write_build(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    uint8_t ** out_bytes, size_t * out_size, GFNT_Error * error);

/** A growable byte buffer. A failed append sets `oom` and drops the bytes. */
typedef struct GFNT_WBuf {
  const GFNT_Allocator * allocator;
  uint8_t * data;
  size_t length;
  size_t capacity;
  bool oom;
} GFNT_WBuf;

void gfnt_wbuf_init(GFNT_WBuf * b, const GFNT_Allocator * allocator);
void gfnt_wbuf_free(GFNT_WBuf * b);
void gfnt_wbuf_u8(GFNT_WBuf * b, uint32_t v);
void gfnt_wbuf_u16(GFNT_WBuf * b, uint32_t v);
void gfnt_wbuf_u32(GFNT_WBuf * b, uint32_t v);
void gfnt_wbuf_bytes(GFNT_WBuf * b, const uint8_t * data, size_t length);
/** Append zeros until the length is a multiple of @p unit. */
void gfnt_wbuf_align(GFNT_WBuf * b, size_t unit);

/**
 * Build a `cmap` table for the given (codepoint, glyph) pairs.
 *
 * @param codepoints Ascending and unique.
 * @param glyphs The glyph for each, in the output font.
 * @param count How many.
 * @param out Receives the table's bytes (appended).
 */
GFNT_Result gfnt_subset_cmap(const uint32_t * codepoints, const uint32_t * glyphs,
    size_t count, GFNT_WBuf * out, GFNT_Error * error);

/**
 * Add to @p keep every glyph `GSUB` can substitute for the ones already in it.
 *
 * @param keep One byte per glyph of the source face, non-zero for kept.
 * @param count The face's glyph count.
 * @param features Feature tags to restrict to, or NULL for all.
 * @param feature_count How many.
 * @return ::GFNT_OK (also when the face has no `GSUB`), ::GFNT_ERR_CORRUPT,
 *   ::GFNT_ERR_LIMIT, ::GFNT_ERR_OOM.
 */
GFNT_Result gfnt_subset_gsub_closure(const GFNT_Face * face, uint8_t * keep,
    size_t count, const GFNT_Tag * features, size_t feature_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Error * error);

/**
 * Build `hmtx` or `vmtx` for a new numbering: the long metrics, then the side
 * bearings, the trailing advance folded.
 *
 * @param data The source table's bytes; @p span its length; @p nhm its long-metric count.
 * @param old_of_new Per new glyph its source glyph, or 0xFFFFFFFF for none.
 * @param out_metrics Receives the long-metric count to put in `hhea` or `vhea`.
 */
GFNT_Result gfnt_subset_metrics(const GFNT_Allocator * a, const uint8_t * data,
    size_t span, size_t nhm, const uint32_t * old_of_new, size_t new_count,
    GFNT_WBuf * out, size_t * out_metrics, GFNT_Tag tag, GFNT_Error * error);

/** `VORG` for a new numbering. Writes nothing when the face has none. */
GFNT_Result gfnt_subset_vorg(const GFNT_Face * face, const uint32_t * new_of_old,
    size_t count, GFNT_WBuf * out, GFNT_Error * error);

/**
 * The legacy `kern` table for a new numbering: format 0 and class-based format 2
 * subtables, in the Microsoft and the Apple header. Writes nothing when the face
 * has none. A subtable of any other format is ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_Result gfnt_subset_kern(const GFNT_Face * face, const uint8_t * shape,
    const uint32_t * new_of_old, size_t count, GFNT_WBuf * out,
    const GFNT_Allocator * allocator, GFNT_Error * error);

/**
 * Find the lookups of `GSUB` or `GPOS` that text made of the kept glyphs can call.
 *
 * With @p grow this is also the `GSUB` closure: glyphs the active lookups can
 * produce are added to @p keep. Without it @p keep is only read.
 *
 * @param out_active Receives one byte per lookup (allocated with @p allocator), or
 *   NULL when the caller does not want it. Left NULL when the face has no such table.
 * @param out_count Receives the lookup count.
 */
GFNT_Result gfnt_subset_layout_scan(const GFNT_Face * face, GFNT_Tag tag,
    uint8_t * keep, size_t count, bool grow, const GFNT_Tag * features,
    size_t feature_count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    uint8_t ** out_active, uint32_t * out_count, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_INTERNAL_H
