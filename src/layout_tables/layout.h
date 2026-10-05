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
 * The OpenType Layout engine's internals: the table, the glyph buffer, and the
 * context a lookup is applied in.
 *
 * Read in place, like `STAT`: a subtable is an offset into the `GSUB` or `GPOS`
 * bytes, and a read past the table does not return a value - it sets the
 * context's sticky `bad` flag and returns zero, so a lookup that runs off its own
 * table finishes (it cannot loop on a zero) and the caller then refuses the run
 * with ::GFNT_ERR_CORRUPT and the offset. One flag rather than a result on every
 * read is what keeps a subtable's matching code readable.
 *
 * The buffer, the skipping iterator, the nested-lookup bookkeeping and the order
 * lookups are applied in follow HarfBuzz's, deliberately and to the detail that
 * can be seen in an answer: the specification leaves several of them open (which
 * glyph a mark attaches to when a ligature is between, what a context lookup does
 * after a nested lookup changed the length), and a shaper that decides them
 * differently produces different text from the one every browser does. The
 * differential against `hb-shape` is what holds this to it.
 */

#ifndef GHOTI_IO_GFNT_LAYOUT_H
#define GHOTI_IO_GFNT_LAYOUT_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFNT_TAG_GSUB GFNT_TAG('G', 'S', 'U', 'B')
#define GFNT_TAG_GPOS GFNT_TAG('G', 'P', 'O', 'S')
#define GFNT_TAG_GDEF GFNT_TAG('G', 'D', 'E', 'F')

/// The most glyphs one context rule may match, as HarfBuzz's HB_MAX_CONTEXT_LENGTH.
#define GFNT_LAYOUT_MAX_CONTEXT 64u
/// How deep a lookup may nest another. HarfBuzz's HB_MAX_NESTING_LEVEL.
#define GFNT_LAYOUT_MAX_NESTING 64u
/// A buffer's operation budget is this times its length, but never under the minimum.
#define GFNT_LAYOUT_OPS_FACTOR 64
#define GFNT_LAYOUT_OPS_MINIMUM 16384
/// The largest run this engine will shape. HarfBuzz's HB_BUFFER_MAX_LEN_DEFAULT.
#define GFNT_LAYOUT_MAX_LEN 0x3FFFFFFFu
/// A lookup may grow a run to this many times its length, but never to less than
/// the minimum: HarfBuzz's HB_BUFFER_MAX_LEN_FACTOR and HB_BUFFER_MAX_LEN_MIN.
#define GFNT_LAYOUT_LEN_FACTOR 64
#define GFNT_LAYOUT_LEN_MINIMUM 16384

/// A coverage lookup that found nothing.
#define GFNT_LAYOUT_NOT_COVERED (-1)

// Glyph properties: the low byte matches the lookup flags it is tested against.
#define GFNT_PROP_BASE 0x02u
#define GFNT_PROP_LIGATURE 0x04u
#define GFNT_PROP_MARK 0x08u
#define GFNT_PROP_CLASS_MASK 0x0Eu
#define GFNT_PROP_SUBSTITUTED 0x10u
#define GFNT_PROP_LIGATED 0x20u
#define GFNT_PROP_MULTIPLIED 0x40u
#define GFNT_PROP_PRESERVE 0x70u

// Lookup flags.
#define GFNT_LF_RIGHT_TO_LEFT 0x0001u
#define GFNT_LF_IGNORE_BASE 0x0002u
#define GFNT_LF_IGNORE_LIGATURES 0x0004u
#define GFNT_LF_IGNORE_MARKS 0x0008u
#define GFNT_LF_USE_MARK_FILTERING_SET 0x0010u
#define GFNT_LF_MARK_ATTACHMENT_TYPE 0xFF00u
#define GFNT_LF_IGNORE_FLAGS 0x000Eu

// A glyph's flags, from the code point it came from.
#define GFNT_GF_DEFAULT_IGNORABLE 0x01u
#define GFNT_GF_ZWJ 0x02u
#define GFNT_GF_ZWNJ 0x04u
#define GFNT_GF_HIDDEN 0x08u
#define GFNT_GF_MARK 0x10u          ///< General category Mn, Mc or Me.
#define GFNT_GF_CONTINUATION 0x20u  ///< Belongs to the grapheme before it.

/** One glyph of the run, in the order HarfBuzz keeps: parallel to ::GFNT_LPos. */
typedef struct GFNT_LInfo {
  uint32_t glyph;
  uint32_t cluster;
  uint32_t mask;      ///< Which features apply here: a bit field, see the plan.
  uint32_t unicode;   ///< The code point this glyph began as.
  uint16_t props;     ///< GFNT_PROP_*, and the mark attachment class in the high byte.
  uint8_t lig_props;  ///< Ligature id in the top three bits; see ::gfnt_l_lig_id().
  uint8_t flags;      ///< GFNT_GF_*.
  uint8_t gc;         ///< The Unicode general category, as GUNI_GC_*.
  uint8_t mcc;        ///< The modified combining class of a mark; 0 for any other.
  uint8_t space;      ///< A GFNT_SPACE_* the character falls back to, or 0.
  uint8_t syllable;   ///< The syllable it belongs to, serial and kind; 0 for none.
  uint8_t category;   ///< The shaper's category for the character.
  uint8_t position;   ///< The shaper's position in the syllable.
} GFNT_LInfo;

#define GFNT_ATTACH_MARK 1u
#define GFNT_ATTACH_CURSIVE 2u

/** One glyph's placement. All in font units. */
typedef struct GFNT_LPos {
  int32_t x_advance;
  int32_t y_advance;
  int32_t x_offset;
  int32_t y_offset;
  int32_t attach_chain; ///< Index delta to the glyph this one hangs from, or 0.
  uint8_t attach_type;
} GFNT_LPos;

/**
 * The run being shaped.
 *
 * `info` is the input and, while a lookup rewrites it, `out_info` is what has been
 * written so far: a substitution that does not change the length writes over
 * itself and the two are the same array, and the first one that does diverges
 * them. That is HarfBuzz's model, and a nested lookup's behaviour is defined in
 * terms of it ("the position in the output so far"), which is why it is not
 * replaced by a splice.
 */
typedef struct GFNT_LBuffer {
  const GFNT_Allocator * allocator;
  GFNT_LInfo * info;
  GFNT_LInfo * out_info;
  GFNT_LInfo * spare;   ///< The second array `out_info` diverges into.
  GFNT_LPos * pos;
  size_t len;
  size_t idx;
  size_t out_len;
  size_t capacity;
  bool have_output;
  bool oom;
  bool limit;           ///< The run tried to grow past max_len.
  size_t max_len;       ///< How long a lookup may make the run.
  bool has_attachment;
  int64_t max_ops;
  uint32_t serial;
} GFNT_LBuffer;

/** A table's GDEF: only what a lookup consults. */
typedef struct GFNT_Gdef {
  GFNT_Reader table;       ///< Empty when the face has no GDEF.
  uint32_t glyph_classes;  ///< Offset of the glyph ClassDef, or 0.
  uint32_t attach_classes; ///< Offset of the mark attachment ClassDef, or 0.
  uint32_t mark_sets;      ///< Offset of the MarkGlyphSets, or 0.
  uint32_t var_store;      ///< Offset of the item variation store, or 0.
  bool has_glyph_classes;
} GFNT_Gdef;

/** `GSUB` or `GPOS`, opened. */
typedef struct GFNT_LayoutTable {
  GFNT_Reader table;       ///< The whole table; its error pointer is NULL.
  GFNT_Tag tag;
  uint32_t script_list;
  uint32_t feature_list;
  uint32_t lookup_list;
  uint32_t lookup_count;
  uint32_t feature_count;
  uint32_t variations;     ///< Offset of FeatureVariations, or 0.
} GFNT_LayoutTable;

/** Where a failed read happened, and which table and glyph it was reading for. */
typedef struct GFNT_LFault {
  bool bad;
  GFNT_Tag table;
  size_t offset;
} GFNT_LFault;

/** What a lookup is applied in. */
typedef struct GFNT_LApply {
  bool per_syllable;   ///< The lookup being applied matches within a syllable.
  GFNT_LBuffer * buf;
  const GFNT_Face * face;
  const GFNT_Variation * variation;
  const GFNT_LayoutTable * lt;
  const GFNT_Gdef * gdef;
  GFNT_LFault fault;
  uint32_t lookup_index;
  uint32_t lookup_mask;
  uint32_t lookup_props;
  bool auto_zwj;
  bool auto_zwnj;
  bool is_gpos;
  bool rtl;
  bool vertical;       ///< The run is vertical: advances are y, cursive runs down.
  uint32_t nesting_left;
} GFNT_LApply;

/// What a skipping iterator may do with a glyph.
typedef enum { GFNT_SKIP_NO, GFNT_SKIP_YES, GFNT_SKIP_MAYBE } GFNT_Skip;

/// What a matcher says of a glyph.
typedef enum { GFNT_MATCH_NO, GFNT_MATCH_YES, GFNT_MATCH_MAYBE } GFNT_Match;

/** How a context rule's elements are compared with glyphs. */
typedef enum { GFNT_MATCH_GLYPH, GFNT_MATCH_CLASS, GFNT_MATCH_COVERAGE } GFNT_MatchKind;

/** A matcher: a kind, and the table offset its data is relative to. */
typedef struct GFNT_Matcher {
  GFNT_MatchKind kind;
  uint32_t origin; ///< Class def offset (CLASS) or the offset coverage offsets are from.
} GFNT_Matcher;

/** The skipping iterator: HarfBuzz's skipping_iterator_t. */
typedef struct GFNT_LIter {
  GFNT_LApply * c;
  uint32_t lookup_props;
  bool has_match;
  GFNT_Matcher matcher;
  const uint16_t * elements;  ///< Unused: elements are read from the table.
  size_t elements_offset;     ///< Table offset of the element array (u16 each).
  bool ignore_zwnj;
  bool ignore_zwj;
  size_t idx;
  size_t end;
  size_t num_items;
  uint32_t mask;          ///< Which glyphs may match: all of them in a context.
  bool per_syllable;      ///< Matches do not cross a syllable.
  uint8_t syllable;       ///< The syllable the match began in, when it is held to one.
} GFNT_LIter;

/** An offset relative to a subtable, with 0 meaning "none" kept as 0. */
static inline size_t gfnt_l_rel(size_t base, uint32_t offset) {
  return offset ? base + offset : 0;
}

/* --- common.c --------------------------------------------------------- */

GFNT_Result gfnt_layout_open(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_LayoutTable * out, GFNT_Error * error);
void gfnt_gdef_open(const GFNT_Face * face, GFNT_Gdef * out);

uint16_t gfnt_lu16(GFNT_LApply * c, size_t offset);
int16_t gfnt_ls16(GFNT_LApply * c, size_t offset);
uint32_t gfnt_lu32(GFNT_LApply * c, size_t offset);

int32_t gfnt_l_coverage(GFNT_LApply * c, size_t coverage, uint32_t glyph);
uint32_t gfnt_l_class(GFNT_LApply * c, size_t classdef, uint32_t glyph);
bool gfnt_l_mark_set_covers(GFNT_LApply * c, uint32_t set, uint32_t glyph);
uint16_t gfnt_gdef_props(GFNT_LApply * c, uint32_t glyph);

/** Table-independent reads, for the callers that hold only a reader. */
uint16_t gfnt_lr_u16(const GFNT_Reader * r, size_t offset, bool * bad);
uint32_t gfnt_lr_u32(const GFNT_Reader * r, size_t offset, bool * bad);
int32_t gfnt_lr_coverage(const GFNT_Reader * r, size_t coverage,
    uint32_t glyph, bool * bad);
uint32_t gfnt_lr_class(const GFNT_Reader * r, size_t classdef, uint32_t glyph,
    bool * bad);

/** The lookup's own offset, type, flags and subtable count. */
bool gfnt_l_lookup(GFNT_LApply * c, uint32_t index, size_t * out_offset,
    uint16_t * out_type, uint16_t * out_flag, uint16_t * out_count);

/* --- would.c ---------------------------------------------------------- */

/**
 * Whether a lookup would substitute this run of glyphs, with nothing around it.
 * HarfBuzz's hb_ot_layout_lookup_would_substitute().
 */
bool gfnt_l_would_apply(GFNT_LApply * c, uint32_t lookup_index,
    const uint32_t * glyphs, size_t n, bool zero_context);

/* --- morx.c ----------------------------------------------------------- */

struct GFNT_ShapeFeature;

/** Whether the face has Apple's glyph substitution table. */
bool gfnt_morx_present(const GFNT_Face * face);
/**
 * Run every `morx` chain over the run, as the features select. Glyphs a ligature
 * absorbed are left as 0xFFFF until ::gfnt_morx_remove_deleted().
 */
GFNT_Result gfnt_morx_apply(const GFNT_Face * face, GFNT_LBuffer * buf,
    bool backward, bool vertical, const struct GFNT_ShapeFeature * features,
    size_t feature_count, GFNT_Error * error);
void gfnt_morx_remove_deleted(GFNT_LBuffer * buf);

/* --- buffer.c --------------------------------------------------------- */

bool gfnt_lbuf_init(GFNT_LBuffer * b, const GFNT_Allocator * allocator,
    size_t length);
void gfnt_lbuf_free(GFNT_LBuffer * b);
bool gfnt_lbuf_enlarge(GFNT_LBuffer * b, size_t size);
/** Begin writing to the output: HarfBuzz's clear_output(). */
void gfnt_lbuf_clear_output(GFNT_LBuffer * b);
/** Finish: the output becomes the input. */
void gfnt_lbuf_sync(GFNT_LBuffer * b);
bool gfnt_lbuf_move_to(GFNT_LBuffer * b, size_t i);
bool gfnt_lbuf_next_glyph(GFNT_LBuffer * b);
bool gfnt_lbuf_replace_glyph(GFNT_LBuffer * b, uint32_t glyph);
/** Put @p info at index @p at, between lookups (the run has no output half). */
bool gfnt_lbuf_insert(GFNT_LBuffer * b, size_t at, const GFNT_LInfo * info);
bool gfnt_lbuf_output_glyph(GFNT_LBuffer * b, uint32_t glyph);
bool gfnt_lbuf_skip_glyph(GFNT_LBuffer * b);
bool gfnt_lbuf_delete_glyph(GFNT_LBuffer * b);
void gfnt_lbuf_merge_clusters(GFNT_LBuffer * b, size_t start, size_t end);
size_t gfnt_lbuf_backtrack_len(const GFNT_LBuffer * b);
size_t gfnt_lbuf_lookahead_len(const GFNT_LBuffer * b);
uint32_t gfnt_lbuf_allocate_lig_id(GFNT_LBuffer * b);
void gfnt_lbuf_reverse(GFNT_LBuffer * b);

static inline GFNT_LInfo * gfnt_lbuf_cur(GFNT_LBuffer * b) {
  return &b->info[b->idx];
}

static inline uint32_t gfnt_l_lig_id(const GFNT_LInfo * i) {
  return (uint32_t)(i->lig_props >> 5);
}
static inline bool gfnt_l_is_lig_base(const GFNT_LInfo * i) {
  return (i->lig_props & 0x10u) != 0;
}
static inline uint32_t gfnt_l_lig_comp(const GFNT_LInfo * i) {
  return gfnt_l_is_lig_base(i) ? 0u : (uint32_t)(i->lig_props & 0x0Fu);
}
static inline uint32_t gfnt_l_lig_num_comps(const GFNT_LInfo * i) {
  return (i->props & GFNT_PROP_LIGATURE) && gfnt_l_is_lig_base(i)
      ? (uint32_t)(i->lig_props & 0x0Fu) : 1u;
}
static inline bool gfnt_l_is_mark(const GFNT_LInfo * i) {
  return (i->props & GFNT_PROP_MARK) != 0;
}
static inline bool gfnt_l_is_base(const GFNT_LInfo * i) {
  return (i->props & GFNT_PROP_BASE) != 0;
}
static inline bool gfnt_l_is_ligature(const GFNT_LInfo * i) {
  return (i->props & GFNT_PROP_LIGATURE) != 0;
}

/* --- apply.c ---------------------------------------------------------- */

void gfnt_liter_init(GFNT_LIter * it, GFNT_LApply * c, bool context_match);
void gfnt_liter_reset(GFNT_LIter * it, size_t start, size_t num_items);
bool gfnt_liter_next(GFNT_LIter * it);
bool gfnt_liter_prev(GFNT_LIter * it);
GFNT_Skip gfnt_liter_may_skip(GFNT_LIter * it, const GFNT_LInfo * info);
void gfnt_liter_set_match(GFNT_LIter * it, GFNT_MatchKind kind, uint32_t base,
    size_t elements);
bool gfnt_l_check_glyph_property(GFNT_LApply * c, const GFNT_LInfo * info,
    uint32_t match_props);

bool gfnt_l_apply_lookup_at_cursor(GFNT_LApply * c, uint32_t lookup_index);
bool gfnt_l_apply_subtables(GFNT_LApply * c, size_t lookup_offset);
void gfnt_l_apply_lookup_to_buffer(GFNT_LApply * c, uint32_t lookup_index);

/** Match a rule's input glyphs from the cursor; HarfBuzz's match_input(). */
bool gfnt_l_match_input(GFNT_LApply * c, uint32_t count, GFNT_Matcher matcher,
    size_t elements, size_t * end_position, size_t * positions,
    uint32_t * total_components);

/** What a context subtable (type 5/6 of GSUB, 7/8 of GPOS) does. */
bool gfnt_l_context_apply(GFNT_LApply * c, size_t subtable);
bool gfnt_l_chain_context_apply(GFNT_LApply * c, size_t subtable);

/* --- gsub.c, gpos.c --------------------------------------------------- */

bool gfnt_gsub_apply_subtable(GFNT_LApply * c, uint16_t type, size_t subtable);
bool gfnt_gpos_apply_subtable(GFNT_LApply * c, uint16_t type, size_t subtable);
bool gfnt_gsub_is_reverse(uint16_t type);

/** The `kern` table's pair kerning, applied to the run (kern.c). */
GFNT_Result gfnt_kern_apply(const GFNT_Face * face, GFNT_LBuffer * b,
    const GFNT_Gdef * gdef, uint32_t kern_mask, GFNT_Error * error);

void gfnt_gpos_position_start(GFNT_LBuffer * b);
void gfnt_gpos_position_finish_offsets(GFNT_LBuffer * b, bool rtl,
    bool vertical);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_LAYOUT_H
