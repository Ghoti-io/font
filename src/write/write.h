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

/**
 * Add to @p keep the glyphs the kept color glyphs paint with, until nothing is added:
 * a version 0 glyph's layers, a version 1 glyph's PaintGlyph outlines and the base
 * glyphs its PaintColrGlyph nodes name. A face with no `COLR` is left as it is.
 */
GFNT_Result gfnt_subset_colr_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_Error * error);

/**
 * `COLR` for a new numbering: the kept base glyphs only, their paints renumbered and
 * copied, the layer list and clip list rebuilt. Writes nothing when the face has none.
 * A table with variation data is ::GFNT_ERR_UNSUPPORTED.
 */
GFNT_Result gfnt_subset_colr(const GFNT_Face * face, const uint8_t * keep,
    const uint32_t * new_of_old, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_WBuf * out, GFNT_Error * error);

/**
 * Add to @p keep the glyphs the kept glyphs of a `MATH` table grow to and are built from: every
 * size in a kept glyph's construction and every part of its assembly, until nothing is added;
 * @p changed says whether anything was. A face with no `MATH`, or one that cannot be read, is
 * left as it is.
 */
GFNT_Result gfnt_subset_math_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Allocator * allocator, bool * changed, GFNT_Error * error);

/**
 * `SVG ` for a subset that retains its glyph ids: the documents that serve a kept glyph, each
 * with its range, written again into @p out (left empty when there are none or the table is of
 * a version this library does not read). With renumbered ids the table is not carried.
 */
GFNT_Result gfnt_subset_svg(const GFNT_Face * face, const uint8_t * keep, size_t count,
    const GFNT_Allocator * allocator, GFNT_WBuf * out, GFNT_Error * error);

/**
 * Add to @p keep the glyphs a kept CFF glyph builds an accented character from (`seac`,
 * or `endchar` given four operands), until nothing is added.
 */
GFNT_Result gfnt_subset_cff_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_Error * error);

/**
 * The `CFF ` table of a subset: the kept glyphs' charstrings with their subroutines
 * flattened, the charset, strings, Font DICTs and Private DICTs for them only.
 *
 * @param old_of_new Per new glyph its source glyph, or 0xFFFFFFFF for a retained id with
 *   no glyph (written empty).
 * @param count The source face's glyph count.
 * @param ps_name The name to give the font in place of its own (a `CFF ` Name INDEX), which also
 *   drops the Top DICT's FullName and FamilyName and each Font DICT's FontName; or NULL to keep them.
 * @param subroutines Put the sequences several charstrings repeat in global subroutines.
 * @param out Receives the table (appended).
 */
GFNT_Result gfnt_subset_cff(const GFNT_Face * face, const uint32_t * old_of_new,
    size_t new_count, size_t count, bool drop_hinting, bool subroutines, const char * ps_name,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_WBuf * out,
    GFNT_Error * error);

/**
 * Finds the sequences of operations that the flattened charstrings repeat and writes them as
 * global subroutines.
 *
 * @param cs The charstrings laid end to end, @p cs_at their @p glyphs + 1 offsets.
 * @param masks The length of each hint and counter mask in them, glyph by glyph (@p mask_at,
 *   @p glyphs + 1 offsets): a mask's length is the stem count where it stands.
 * @param max_depth How deeply the subroutines may call each other; 0 does nothing.
 * @param out_cs Receives the charstrings that call them, @p out_at their offsets.
 * @param out_subrs Receives the subroutine bodies, @p out_sub_at (allocated with @p a, for the
 *   caller to free) their @p out_count + 1 offsets.
 * @return ::GFNT_OK with @p out_count 0 when nothing was worth making or a charstring was not
 *   understood: the caller keeps its own.
 */
GFNT_Result gfnt_cff_subroutinize(const GFNT_Allocator * a, const uint8_t * cs,
    const size_t * cs_at, const uint8_t * masks, const size_t * mask_at, size_t glyphs,
    uint8_t max_depth, GFNT_WBuf * out_cs, size_t * out_at, GFNT_WBuf * out_subrs,
    size_t ** out_sub_at, size_t * out_count, GFNT_Error * error);

/**
 * `name` for a subset renamed to @p family: every record of the source is carried except the
 * ones that state the family, in any language - IDs 1, 3, 4, 6, 16, 18, 20, 21 and 25 - which
 * are written again for @p family, in Windows English, with the source's style.
 *
 * @param family The new family name, UTF-8; not empty.
 * @param out_ps_name Receives the PostScript name written to ID 6 (also what a `CFF ` is to be
 *   named), allocated with @p allocator and released by the caller.
 * @param out The table (appended).
 */
GFNT_Result gfnt_subset_name(const GFNT_Face * face, const char * family,
    const GFNT_Allocator * allocator, char ** out_ps_name, GFNT_WBuf * out, GFNT_Error * error);

/**
 * How long the item variation store at @p p is: its region list and every
 * ItemVariationData, whichever lies last.
 *
 * @return false for a store that runs off @p avail or is not format 1.
 */
bool gfnt_subset_ivs_extent(const uint8_t * p, size_t avail, size_t * out_length);

/**
 * `HVAR` or `VVAR` for a new numbering: the item variation store as it is, and the
 * delta-set index maps rebuilt with one entry for each kept glyph. Appends nothing when
 * the face has none; a table it cannot read is ::GFNT_ERR_CORRUPT.
 *
 * @param tag `HVAR` or `VVAR`.
 * @param old_of_new Per new glyph its source glyph, or 0xFFFFFFFF for none.
 */
GFNT_Result gfnt_subset_var_metrics(const GFNT_Face * face, GFNT_Tag tag,
    const uint32_t * old_of_new, size_t new_count, GFNT_WBuf * out, GFNT_Error * error);

/**
 * `gvar` for a new numbering: each kept glyph's variation data as it is, the shared
 * tuples whole. Appends nothing when the font has no `gvar`.
 */
GFNT_Result gfnt_subset_gvar(const GFNT_Face * face, const uint32_t * old_of_new,
    size_t new_count, GFNT_WBuf * out, GFNT_Error * error);

/** The bitmap strike tables of a subset, each empty (length 0) when not carried. */
typedef struct GFNT_SubsetStrikes {
  GFNT_WBuf eblc, ebdt, cblc, cbdt, sbix;
} GFNT_SubsetStrikes;

void gfnt_subset_strikes_init(GFNT_SubsetStrikes * s, const GFNT_Allocator * allocator);
void gfnt_subset_strikes_free(GFNT_SubsetStrikes * s);

/**
 * `EBLC`/`EBDT`, `CBLC`/`CBDT` and `sbix` for a new numbering: the kept glyphs' pictures,
 * indexed again. A composite bitmap glyph is ::GFNT_ERR_UNSUPPORTED; `EBSC` is not carried.
 */
GFNT_Result gfnt_subset_strikes(const GFNT_Face * face, const uint32_t * new_of_old,
    const uint32_t * old_of_new, size_t count, size_t new_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_SubsetStrikes * out,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_INTERNAL_H
