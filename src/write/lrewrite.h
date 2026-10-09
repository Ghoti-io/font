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
 * What rewriting a layout table under a new glyph numbering shares: the reader
 * of the source table, the buffer a subtable is built in, and the encoders for
 * the structures every lookup type uses (coverage, class definitions, device
 * tables, anchors, sequence lookup records). Internal to `src/write/`.
 */

#ifndef GHOTI_IO_GFNT_WRITE_LREWRITE_H
#define GHOTI_IO_GFNT_WRITE_LREWRITE_H

#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"
#include "write.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFNT_RW_NONE 0xFFFFFFFFu

/** The source table, the glyph numbering, and what has gone wrong. */
typedef struct GFNT_RW {
  const GFNT_Allocator * a;
  const GFNT_Reader * r;        ///< The source table, whole.
  const uint8_t * shape;        ///< Per old glyph: text can reach it, so entries naming it stay.
  const uint32_t * new_of_old;  ///< Per old glyph: its new id, or ::GFNT_RW_NONE.
  const uint32_t * old_of_new;  ///< Per new glyph: its old id, or ::GFNT_RW_NONE.
  size_t count;                 ///< Old glyph count.
  size_t new_count;             ///< New glyph count.
  const uint8_t * active;       ///< Per old lookup: kept. NULL where lookups do not matter.
  const uint32_t * lookup_map;  ///< Per old lookup: its new index.
  uint32_t lookup_count;
  bool bad;       ///< The source table could not be read.
  bool oom;
  bool overflow;  ///< A 16-bit offset did not fit.
  bool unsupported; ///< A structure this does not rewrite.
  uint32_t win_lo;  ///< Only entries whose first glyph is in [win_lo, win_hi) are written:
  uint32_t win_hi;  ///< how a subtable too big for 16-bit offsets is split in two.
} GFNT_RW;

/** A buffer a subtable is built in, with children shared by content. */
typedef struct GFNT_LKid {
  size_t pos;
  size_t len;
  uint32_t hash;
} GFNT_LKid;

typedef struct GFNT_LPending {
  size_t field;   ///< Where in the buffer the offset goes.
  size_t source;  ///< Absolute offset in the source table of the device table.
} GFNT_LPending;

typedef struct GFNT_LOut {
  GFNT_WBuf b;
  const GFNT_Allocator * a;
  GFNT_LKid * kids;
  size_t nkids;
  size_t capkids;
  uint32_t * table;   ///< Open-addressing slots holding kid index + 1.
  size_t table_cap;
  GFNT_LPending * pend;
  size_t npend;
  size_t cappend;
  bool oom;
  bool overflow;
} GFNT_LOut;

void gfnt_lout_init(GFNT_LOut * o, const GFNT_Allocator * a);
void gfnt_lout_free(GFNT_LOut * o);
/** Whether any allocation behind this buffer failed. */
bool gfnt_lout_failed(const GFNT_LOut * o);
/** Append @p len bytes - or reuse an identical earlier child - and say where they are. */
size_t gfnt_lout_add(GFNT_LOut * o, const uint8_t * data, size_t len);
/** Store, at @p field, the distance from @p origin to @p pos as 16 bits. */
void gfnt_lout_link16(GFNT_LOut * o, size_t field, size_t origin, size_t pos);
void gfnt_lout_link32(GFNT_LOut * o, size_t field, size_t origin, size_t pos);
/** Add a child and link it in one step. Empty children are not added (offset 0). */
void gfnt_lout_child16(GFNT_LOut * o, size_t field, size_t origin, const GFNT_WBuf * child);

uint16_t gfnt_rw_u16(GFNT_RW * rw, size_t offset);
uint32_t gfnt_rw_u32(GFNT_RW * rw, size_t offset);
/** Whether the text can reach this old glyph. */
bool gfnt_rw_kept(const GFNT_RW * rw, uint32_t old_glyph);
/** Whether an entry led by this old glyph is written: reachable, and in the window. */
bool gfnt_rw_first(const GFNT_RW * rw, uint32_t old_glyph);

/** One glyph of a coverage table, and its coverage index. */
typedef struct GFNT_CovEnt {
  uint32_t glyph;
  uint32_t index;
} GFNT_CovEnt;

/** Read a coverage table; the caller frees the result with the allocator. NULL for none. */
GFNT_CovEnt * gfnt_rw_cov_read(GFNT_RW * rw, size_t coverage, size_t * n);
/** Write a coverage table for ascending new glyph ids. */
void gfnt_rw_cov_write(GFNT_WBuf * out, const uint32_t * ids, size_t n);
/**
 * Write a coverage table of the source coverage's reachable glyphs, renumbered.
 *
 * @return How many glyphs it holds; when zero nothing is written.
 */
size_t gfnt_rw_cov_filtered(GFNT_RW * rw, size_t coverage, GFNT_WBuf * out);
/**
 * Write a class definition for the reachable glyphs.
 *
 * @param map Old class to new class, or NULL to keep the classes.
 * @param olds Ascending old glyph ids to describe, or NULL for every reachable glyph.
 * @param n How many @p olds.
 */
void gfnt_rw_classdef(GFNT_RW * rw, size_t source, const uint16_t * map,
    const uint32_t * olds, size_t n, GFNT_WBuf * out);
/** Append the lookup records that name a kept lookup, renumbered; how many were written. */
size_t gfnt_rw_records(GFNT_RW * rw, size_t at, uint16_t count, GFNT_WBuf * out);
/** Append a device table, byte for byte. */
void gfnt_rw_device(GFNT_RW * rw, size_t source, GFNT_WBuf * out);
/** Append an anchor table with its device tables behind it. */
void gfnt_rw_anchor(GFNT_RW * rw, size_t source, GFNT_WBuf * out);
/** How many bytes a value record of this format takes. */
size_t gfnt_rw_value_size(uint16_t format);
/**
 * Append a value record read from @p source, deferring its device tables.
 *
 * @param origin Where the source's device offsets are counted from.
 */
void gfnt_rw_value(GFNT_RW * rw, GFNT_LOut * o, size_t source, uint16_t format,
    size_t origin);
/** Place the deferred device tables behind the buffer's content and link them. */
void gfnt_rw_flush_devices(GFNT_RW * rw, GFNT_LOut * o);

/**
 * Rewrite one lookup subtable under the new numbering.
 *
 * @param type The effective lookup type (an extension subtable already unwrapped).
 * @param out Receives the subtable's bytes, offsets counted from its start.
 * @return true when something was written; false when nothing in the subtable can
 *   match any reachable glyph, or when @p rw failed (see its flags).
 */
bool gfnt_rw_subtable(GFNT_RW * rw, bool is_gpos, uint16_t type, size_t st,
    GFNT_WBuf * out);

/** A rewritten GSUB, GPOS or GDEF is built by these; each returns a result code. */
GFNT_Result gfnt_subset_layout_rewrite(const GFNT_Face * face, GFNT_Tag tag,
    const uint8_t * shape, const uint32_t * new_of_old, const uint32_t * old_of_new,
    size_t count, size_t new_count, const uint8_t * active, uint32_t active_count,
    const GFNT_Tag * features, size_t feature_count, GFNT_WBuf * out,
    const GFNT_Allocator * allocator, GFNT_Error * error);

GFNT_Result gfnt_subset_gdef(const GFNT_Face * face, const uint8_t * shape,
    const uint32_t * new_of_old, const uint32_t * old_of_new, size_t count,
    size_t new_count, GFNT_WBuf * out, const GFNT_Allocator * allocator,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_LREWRITE_H
