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
 * `GSUB` and `GPOS` under a new glyph numbering.
 *
 * The lookups that matter are those the closure found active; each is rewritten
 * subtable by subtable (subset_lookups.c) and the lists around them are rebuilt:
 * the script list as it was, the feature list with its lookup indices
 * renumbered (a feature the caller did not ask for stays, with no lookups, so
 * that which features exist - which some shapers consult - does not change), and
 * the lookup list, in extension form when the plain form does not fit in 16-bit
 * offsets.
 */

#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "lrewrite.h"

typedef struct BuiltLookup {
  uint16_t type;       ///< Effective type; an extension is rewrapped when written.
  uint16_t flags;
  uint16_t filter_set;
  GFNT_WBuf * subs;
  size_t nsubs;
  size_t cap;
} BuiltLookup;

static bool healthy(const GFNT_RW * rw) {
  return !rw->bad && !rw->oom && !rw->overflow;
}

static void absorb(GFNT_RW * rw, const GFNT_WBuf * b) {
  if (b->oom) {
    rw->oom = true;
  }
}

static void put16_at(GFNT_RW * rw, GFNT_WBuf * b, size_t field, size_t value) {
  if (value > 0xFFFFu) {
    rw->overflow = true;
    return;
  }
  if (!b->oom && field + 2 <= b->length) {
    gfnt_write_put16(b->data + field, (uint32_t)value);
  }
}

static void put32_at(GFNT_WBuf * b, size_t field, size_t value) {
  if (!b->oom && field + 4 <= b->length) {
    gfnt_write_put32(b->data + field, (uint32_t)value);
  }
}

static bool tag_wanted(const GFNT_Tag * features, size_t count, GFNT_Tag tag) {
  size_t i;

  if (!features || count == 0) {
    return true;
  }
  for (i = 0; i < count; ++i) {
    if (features[i] == tag) {
      return true;
    }
  }
  return false;
}

/* --- script list --------------------------------------------------------- */

static void build_script(GFNT_RW * rw, size_t script, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint16_t default_ls = gfnt_rw_u16(rw, script);
  uint16_t count = gfnt_rw_u16(rw, script + 2);
  uint16_t i;

  if (rw->bad) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, count);
  for (i = 0; i < count; ++i) {
    gfnt_wbuf_u32(&o.b, gfnt_rw_u32(rw, script + 4 + 6 * (size_t)i));
    gfnt_wbuf_u16(&o.b, 0);
  }
  for (i = 0; i <= count && !rw->bad && !gfnt_lout_failed(&o); ++i) {
    size_t ls = i == count ? default_ls : gfnt_rw_u16(rw, script + 4 + 6 * (size_t)i + 4);
    GFNT_WBuf blob;
    uint16_t features;
    uint16_t k;

    if (!ls || rw->bad) {
      continue;
    }
    ls += script;
    features = gfnt_rw_u16(rw, ls + 4);
    gfnt_wbuf_init(&blob, rw->a);
    gfnt_wbuf_u16(&blob, 0);
    gfnt_wbuf_u16(&blob, gfnt_rw_u16(rw, ls + 2));
    gfnt_wbuf_u16(&blob, features);
    for (k = 0; k < features; ++k) {
      gfnt_wbuf_u16(&blob, gfnt_rw_u16(rw, ls + 6 + 2 * (size_t)k));
    }
    absorb(rw, &blob);
    gfnt_lout_child16(&o, i == count ? 0 : 4 + 6 * (size_t)i + 4, 0, &blob);
    gfnt_wbuf_free(&blob);
  }
  if (o.overflow) {
    rw->overflow = true;
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw)) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

static void build_script_list(GFNT_RW * rw, size_t list, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint16_t count = gfnt_rw_u16(rw, list);
  uint16_t i;

  if (rw->bad) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, count);
  for (i = 0; i < count; ++i) {
    gfnt_wbuf_u32(&o.b, gfnt_rw_u32(rw, list + 2 + 6 * (size_t)i));
    gfnt_wbuf_u16(&o.b, 0);
  }
  for (i = 0; i < count && healthy(rw) && !gfnt_lout_failed(&o); ++i) {
    size_t script = list + gfnt_rw_u16(rw, list + 2 + 6 * (size_t)i + 4);
    GFNT_WBuf blob;

    gfnt_wbuf_init(&blob, rw->a);
    build_script(rw, script, &blob);
    gfnt_lout_child16(&o, 2 + 6 * (size_t)i + 4, 0, &blob);
    gfnt_wbuf_free(&blob);
  }
  if (o.overflow) {
    rw->overflow = true;
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw)) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

/* --- feature list -------------------------------------------------------- */

/** A Feature table with its lookups renumbered; a feature not wanted keeps none. */
static void build_feature_table(GFNT_RW * rw, size_t feature, bool wanted, GFNT_WBuf * blob) {
  uint16_t n = gfnt_rw_u16(rw, feature + 2);
  GFNT_WBuf idx;
  size_t kept = 0;
  uint16_t k;

  gfnt_wbuf_init(&idx, rw->a);
  for (k = 0; k < n && wanted && !rw->bad; ++k) {
    uint16_t lookup = gfnt_rw_u16(rw, feature + 4 + 2 * (size_t)k);

    if (!rw->bad && lookup < rw->lookup_count && rw->active[lookup]) {
      gfnt_wbuf_u16(&idx, rw->lookup_map[lookup]);
      ++kept;
    }
  }
  gfnt_wbuf_u16(blob, 0);
  gfnt_wbuf_u16(blob, (uint32_t)kept);
  gfnt_wbuf_bytes(blob, idx.data, idx.length);
  absorb(rw, blob);
  absorb(rw, &idx);
  gfnt_wbuf_free(&idx);
}

static void build_feature_list(GFNT_RW * rw, size_t list, const GFNT_Tag * features,
    size_t feature_count, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint16_t count = gfnt_rw_u16(rw, list);
  uint16_t i;

  if (rw->bad) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, count);
  for (i = 0; i < count; ++i) {
    gfnt_wbuf_u32(&o.b, gfnt_rw_u32(rw, list + 2 + 6 * (size_t)i));
    gfnt_wbuf_u16(&o.b, 0);
  }
  for (i = 0; i < count && healthy(rw) && !gfnt_lout_failed(&o); ++i) {
    GFNT_Tag tag = gfnt_rw_u32(rw, list + 2 + 6 * (size_t)i);
    size_t feature = list + gfnt_rw_u16(rw, list + 2 + 6 * (size_t)i + 4);
    GFNT_WBuf blob;

    gfnt_wbuf_init(&blob, rw->a);
    build_feature_table(rw, feature, tag_wanted(features, feature_count, tag), &blob);
    gfnt_lout_child16(&o, 2 + 6 * (size_t)i + 4, 0, &blob);
    gfnt_wbuf_free(&blob);
  }
  if (o.overflow) {
    rw->overflow = true;
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw)) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

/* --- feature variations -------------------------------------------------- */

/** A condition table. Formats 1 and 2 are all there are to a font without axes' worth of structure. */
static void build_condition(GFNT_RW * rw, size_t cond, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, cond);

  if (rw->bad) {
    return;
  }
  if (format == 1) {
    gfnt_wbuf_u16(out, 1);
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, cond + 2));
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, cond + 4));
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, cond + 6));
  }
  else if (format == 2) {
    gfnt_wbuf_u16(out, 2);
    gfnt_wbuf_u16(out, gfnt_rw_u16(rw, cond + 2));
    gfnt_wbuf_u32(out, gfnt_rw_u32(rw, cond + 4));
  }
  else {
    rw->unsupported = true;
  }
}

static void build_condition_set(GFNT_RW * rw, size_t set, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint16_t n = gfnt_rw_u16(rw, set);
  uint16_t i;

  if (rw->bad) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u32(&o.b, 0);
  }
  for (i = 0; i < n && healthy(rw) && !rw->unsupported; ++i) {
    GFNT_WBuf cb;

    gfnt_wbuf_init(&cb, rw->a);
    build_condition(rw, set + gfnt_rw_u32(rw, set + 2 + 4 * (size_t)i), &cb);
    absorb(rw, &cb);
    if (healthy(rw) && !rw->unsupported) {
      gfnt_lout_link32(&o, 2 + 4 * (size_t)i, 0, gfnt_lout_add(&o, cb.data, cb.length));
    }
    gfnt_wbuf_free(&cb);
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw) && !rw->unsupported) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

static void build_substitution(GFNT_RW * rw, size_t table, size_t flist,
    const GFNT_Tag * features, size_t feature_count, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint16_t n = gfnt_rw_u16(rw, table + 4);
  uint16_t i;

  if (rw->bad) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, 1);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, table + 6 + 6 * (size_t)i));
    gfnt_wbuf_u32(&o.b, 0);
  }
  for (i = 0; i < n && healthy(rw) && !gfnt_lout_failed(&o); ++i) {
    uint16_t index = gfnt_rw_u16(rw, table + 6 + 6 * (size_t)i);
    size_t alt = table + gfnt_rw_u32(rw, table + 6 + 6 * (size_t)i + 2);
    GFNT_Tag tag = gfnt_rw_u32(rw, flist + 2 + 6 * (size_t)index);
    GFNT_WBuf fb;

    gfnt_wbuf_init(&fb, rw->a);
    build_feature_table(rw, alt, tag_wanted(features, feature_count, tag), &fb);
    if (healthy(rw)) {
      gfnt_lout_link32(&o, 6 + 6 * (size_t)i + 2, 0, gfnt_lout_add(&o, fb.data, fb.length));
    }
    gfnt_wbuf_free(&fb);
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw)) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

static void build_variations(GFNT_RW * rw, size_t fv, size_t flist,
    const GFNT_Tag * features, size_t feature_count, GFNT_WBuf * out) {
  GFNT_LOut o;
  uint32_t n = gfnt_rw_u32(rw, fv + 4);
  uint32_t i;

  if (rw->bad || gfnt_rw_u16(rw, fv) != 1) {
    rw->bad = true;
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, 1);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u32(&o.b, n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u32(&o.b, 0);
    gfnt_wbuf_u32(&o.b, 0);
  }
  for (i = 0; i < n && healthy(rw) && !rw->unsupported && !gfnt_lout_failed(&o); ++i) {
    size_t rec = fv + 8 + 8 * (size_t)i;
    uint32_t cs = gfnt_rw_u32(rw, rec);
    uint32_t st = gfnt_rw_u32(rw, rec + 4);
    GFNT_WBuf blob;

    if (rw->bad) {
      break;
    }
    if (cs) {
      gfnt_wbuf_init(&blob, rw->a);
      build_condition_set(rw, fv + cs, &blob);
      if (healthy(rw) && !rw->unsupported) {
        gfnt_lout_link32(&o, 8 + 8 * (size_t)i, 0, gfnt_lout_add(&o, blob.data, blob.length));
      }
      gfnt_wbuf_free(&blob);
    }
    if (st && healthy(rw) && !rw->unsupported) {
      gfnt_wbuf_init(&blob, rw->a);
      build_substitution(rw, fv + st, flist, features, feature_count, &blob);
      if (healthy(rw)) {
        gfnt_lout_link32(&o, 8 + 8 * (size_t)i + 4, 0, gfnt_lout_add(&o, blob.data, blob.length));
      }
      gfnt_wbuf_free(&blob);
    }
  }
  if (gfnt_lout_failed(&o)) {
    rw->oom = true;
  }
  if (healthy(rw) && !rw->unsupported) {
    gfnt_wbuf_bytes(out, o.b.data, o.b.length);
  }
  gfnt_lout_free(&o);
}

/* --- lookup list --------------------------------------------------------- */

static void write_lookup_list(GFNT_RW * rw, const BuiltLookup * L, size_t n, bool ext,
    uint16_t ext_type, GFNT_WBuf * out) {
  size_t * stubs = NULL;
  size_t total = 0;
  size_t used = 0;
  size_t i;
  size_t j;

  rw->overflow = false;
  out->length = 0;
  if (ext) {
    for (i = 0; i < n; ++i) {
      total += L[i].nsubs;
    }
    stubs = rw->a->calloc_fn(rw->a->ctx, total ? total : 1, sizeof *stubs);
    if (!stubs) {
      rw->oom = true;
      return;
    }
  }
  gfnt_wbuf_u16(out, (uint32_t)n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u16(out, 0);
  }
  for (i = 0; i < n; ++i) {
    size_t lpos = out->length;
    bool filter = (L[i].flags & GFNT_LF_USE_MARK_FILTERING_SET) != 0;

    put16_at(rw, out, 2 + 2 * i, lpos);
    gfnt_wbuf_u16(out, ext ? ext_type : L[i].type);
    gfnt_wbuf_u16(out, L[i].flags);
    gfnt_wbuf_u16(out, (uint32_t)L[i].nsubs);
    for (j = 0; j < L[i].nsubs; ++j) {
      gfnt_wbuf_u16(out, 0);
    }
    if (filter) {
      gfnt_wbuf_u16(out, L[i].filter_set);
    }
    for (j = 0; j < L[i].nsubs; ++j) {
      size_t spos = out->length;

      put16_at(rw, out, lpos + 6 + 2 * j, spos - lpos);
      if (ext) {
        gfnt_wbuf_u16(out, 1);
        gfnt_wbuf_u16(out, L[i].type);
        gfnt_wbuf_u32(out, 0);
        stubs[used++] = spos;
      }
      else {
        gfnt_wbuf_bytes(out, L[i].subs[j].data, L[i].subs[j].length);
      }
    }
  }
  if (ext) {
    used = 0;
    for (i = 0; i < n; ++i) {
      for (j = 0; j < L[i].nsubs; ++j) {
        size_t at = out->length;

        gfnt_wbuf_bytes(out, L[i].subs[j].data, L[i].subs[j].length);
        put32_at(out, stubs[used] + 4, at - stubs[used]);
        ++used;
      }
    }
    rw->a->free_fn(rw->a->ctx, stubs);
  }
  absorb(rw, out);
}

/* --- the table ----------------------------------------------------------- */

static void free_lookups(GFNT_RW * rw, BuiltLookup * L, size_t n) {
  size_t i;
  size_t j;

  for (i = 0; i < n; ++i) {
    for (j = 0; j < L[i].nsubs; ++j) {
      gfnt_wbuf_free(&L[i].subs[j]);
    }
    rw->a->free_fn(rw->a->ctx, L[i].subs);
  }
  rw->a->free_fn(rw->a->ctx, L);
}

/** Keep a finished subtable with its lookup, growing the list as needed. */
static void push_sub(GFNT_RW * rw, BuiltLookup * b, GFNT_WBuf * blob) {
  if (b->nsubs == b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4;
    GFNT_WBuf * grown = rw->a->calloc_fn(rw->a->ctx, cap, sizeof *grown);

    if (!grown) {
      rw->oom = true;
      gfnt_wbuf_free(blob);
      return;
    }
    if (b->nsubs) {
      memcpy(grown, b->subs, b->nsubs * sizeof *grown);
    }
    rw->a->free_fn(rw->a->ctx, b->subs);
    b->subs = grown;
    b->cap = cap;
  }
  b->subs[b->nsubs++] = *blob;
}

/**
 * Rewrite one subtable for the entries led by glyphs in [lo, hi). If it does not fit
 * 16-bit offsets, write it as two subtables for half the glyphs each: every glyph
 * is in exactly one half, so the first subtable that matches is the same one.
 */
static void build_windowed(GFNT_RW * rw, bool is_gpos, uint16_t type, size_t st,
    uint32_t lo, uint32_t hi, BuiltLookup * b, unsigned depth) {
  GFNT_WBuf blob;
  bool made;

  rw->win_lo = lo;
  rw->win_hi = hi;
  gfnt_wbuf_init(&blob, rw->a);
  made = gfnt_rw_subtable(rw, is_gpos, type, st, &blob);
  if (rw->overflow && !rw->bad && !rw->oom && hi - lo > 1 && depth < 17) {
    uint32_t mid = lo + (hi - lo) / 2;

    rw->overflow = false;
    gfnt_wbuf_free(&blob);
    build_windowed(rw, is_gpos, type, st, lo, mid, b, depth + 1);
    if (healthy(rw)) {
      build_windowed(rw, is_gpos, type, st, mid, hi, b, depth + 1);
    }
    return;
  }
  if (made && healthy(rw)) {
    push_sub(rw, b, &blob);
  }
  else {
    gfnt_wbuf_free(&blob);
  }
}

/** Rewrite lookup @p old into @p b. */
static void build_lookup(GFNT_RW * rw, bool is_gpos, size_t lookup_list, uint32_t old,
    BuiltLookup * b) {
  size_t lookup = lookup_list + gfnt_rw_u16(rw, lookup_list + 2 + 2 * (size_t)old);
  uint16_t type = gfnt_rw_u16(rw, lookup);
  uint16_t nsub = gfnt_rw_u16(rw, lookup + 4);
  uint16_t extension = is_gpos ? 9 : 7;
  bool have_type = false;
  uint16_t i;

  b->flags = gfnt_rw_u16(rw, lookup + 2);
  if (b->flags & GFNT_LF_USE_MARK_FILTERING_SET) {
    b->filter_set = gfnt_rw_u16(rw, lookup + 6 + 2 * (size_t)nsub);
  }
  b->type = type == extension ? 1 : type;
  for (i = 0; i < nsub && healthy(rw); ++i) {
    size_t st = lookup + gfnt_rw_u16(rw, lookup + 6 + 2 * (size_t)i);
    uint16_t effective = type;

    if (type == extension) {
      uint32_t offset;

      if (gfnt_rw_u16(rw, st) != 1) {
        rw->bad = true;
        return;
      }
      effective = gfnt_rw_u16(rw, st + 2);
      offset = gfnt_rw_u32(rw, st + 4);
      st += offset;
      if (have_type && effective != b->type) {
        rw->bad = true;
        return;
      }
      b->type = effective;
      have_type = true;
    }
    if (rw->bad) {
      return;
    }
    build_windowed(rw, is_gpos, effective, st, 0, 0x10000u, b, 0);
  }
}

GFNT_Result gfnt_subset_layout_rewrite(const GFNT_Face * face, GFNT_Tag tag,
    const uint8_t * shape, const uint32_t * new_of_old, const uint32_t * old_of_new,
    size_t count, size_t new_count, const uint8_t * active, uint32_t active_count,
    const GFNT_Tag * features, size_t feature_count, GFNT_WBuf * out,
    const GFNT_Allocator * allocator, GFNT_Error * error) {
  GFNT_LayoutTable lt;
  GFNT_RW rw;
  GFNT_Result result;
  BuiltLookup * lookups = NULL;
  uint32_t * map = NULL;
  uint32_t i;
  size_t nl = 0;
  bool is_gpos = tag == GFNT_TAG_GPOS;
  GFNT_WBuf scripts;
  GFNT_WBuf feats;
  GFNT_WBuf lkl;
  GFNT_WBuf fvb;

  result = gfnt_layout_open(face, tag, &lt, error);
  if (result != GFNT_OK) {
    return result;
  }
  memset(&rw, 0, sizeof rw);
  rw.a = allocator;
  rw.r = &lt.table;
  rw.shape = shape;
  rw.new_of_old = new_of_old;
  rw.old_of_new = old_of_new;
  rw.count = count;
  rw.new_count = new_count;
  rw.active = active;
  rw.lookup_count = active_count;
  gfnt_wbuf_init(&scripts, allocator);
  gfnt_wbuf_init(&feats, allocator);
  gfnt_wbuf_init(&lkl, allocator);
  gfnt_wbuf_init(&fvb, allocator);

  map = allocator->calloc_fn(allocator->ctx, active_count ? active_count : 1, sizeof *map);
  lookups = allocator->calloc_fn(allocator->ctx, active_count ? active_count : 1,
      sizeof *lookups);
  if (!map || !lookups) {
    rw.oom = true;
    goto done;
  }
  for (i = 0; i < active_count; ++i) {
    if (active[i]) {
      map[i] = (uint32_t)nl++;
    }
  }
  rw.lookup_map = map;
  nl = 0;
  for (i = 0; i < active_count && healthy(&rw); ++i) {
    if (active[i]) {
      build_lookup(&rw, is_gpos, lt.lookup_list, i, &lookups[nl]);
      ++nl;
    }
  }
  if (healthy(&rw)) {
    build_script_list(&rw, lt.script_list, &scripts);
  }
  if (healthy(&rw)) {
    build_feature_list(&rw, lt.feature_list, features, feature_count, &feats);
  }
  if (healthy(&rw) && lt.variations) {
    build_variations(&rw, lt.variations, lt.feature_list, features, feature_count, &fvb);
  }
  if (healthy(&rw)) {
    write_lookup_list(&rw, lookups, nl, false, 0, &lkl);
    if (rw.overflow && !rw.oom) {
      write_lookup_list(&rw, lookups, nl, true, is_gpos ? 9 : 7, &lkl);
    }
  }
  if (healthy(&rw) && !rw.unsupported) {
    size_t header = lt.variations ? 14 : 10;
    size_t at_scripts = header;
    size_t at_features = at_scripts + scripts.length;
    size_t at_lookups = at_features + feats.length;

    gfnt_wbuf_u32(out, lt.variations ? 0x00010001u : 0x00010000u);
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_u16(out, 0);
    if (lt.variations) {
      gfnt_wbuf_u32(out, 0);
    }
    put16_at(&rw, out, 4, at_scripts);
    put16_at(&rw, out, 6, at_features);
    put16_at(&rw, out, 8, at_lookups);
    gfnt_wbuf_bytes(out, scripts.data, scripts.length);
    gfnt_wbuf_bytes(out, feats.data, feats.length);
    gfnt_wbuf_bytes(out, lkl.data, lkl.length);
    if (lt.variations) {
      put32_at(out, 10, at_lookups + lkl.length);
      gfnt_wbuf_bytes(out, fvb.data, fvb.length);
    }
    absorb(&rw, out);
  }

done:
  gfnt_wbuf_free(&scripts);
  gfnt_wbuf_free(&feats);
  gfnt_wbuf_free(&lkl);
  gfnt_wbuf_free(&fvb);
  if (lookups) {
    free_lookups(&rw, lookups, nl < active_count ? active_count : nl);
  }
  allocator->free_fn(allocator->ctx, map);
  if (rw.oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
        "rewriting a layout table");
  }
  if (rw.unsupported) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "a FeatureVariations condition of a format this does not rewrite");
  }
  if (rw.overflow) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 0, GFNT_GLYPH_NONE,
        "a layout table does not fit in 16-bit offsets once rewritten");
  }
  if (rw.bad) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a layout table cannot be read");
  }
  return GFNT_OK;
}
