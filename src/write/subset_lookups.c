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
 * Lookup subtables, rewritten for a new glyph numbering.
 *
 * Each builder reads one subtable of the source and writes the subtable a font
 * with fewer glyphs needs: entries that name a glyph the text cannot reach are
 * left out, the glyph ids that remain are the new ones, and what ends up empty
 * is not written. A rule survives exactly when the closure in subset_gsub.c
 * counted it (every glyph it names is reachable; class-based rules whenever
 * their coverage is reached), so a lookup the closure called does exist.
 *
 * Offsets are 16 bits, as in the source; a subtable that does not fit is an
 * error (::GFNT_ERR_LIMIT), not a silent truncation.
 */

#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "lrewrite.h"

/* --- small helpers ------------------------------------------------------- */

static void * xalloc(GFNT_RW * rw, size_t n) {
  void * p = rw->a->calloc_fn(rw->a->ctx, n ? n : 1, 1);

  if (!p) {
    rw->oom = true;
  }
  return p;
}

static void xfree(GFNT_RW * rw, void * p) {
  rw->a->free_fn(rw->a->ctx, p);
}

/** Fold a temporary buffer's allocation failure into the rewrite's. */
static void absorb(GFNT_RW * rw, const GFNT_WBuf * b) {
  if (b->oom) {
    rw->oom = true;
  }
}

static bool healthy(const GFNT_RW * rw) {
  return !rw->bad && !rw->oom && !rw->overflow;
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

/** Move a finished LOut into @p out; true when the rewrite is still sound. */
static bool finish(GFNT_RW * rw, GFNT_LOut * o, GFNT_WBuf * out) {
  bool ok;

  if (o->overflow) {
    rw->overflow = true;
  }
  if (gfnt_lout_failed(o)) {
    rw->oom = true;
  }
  ok = healthy(rw);
  if (ok) {
    gfnt_wbuf_bytes(out, o->b.data, o->b.length);
    absorb(rw, out);
    ok = healthy(rw);
  }
  gfnt_lout_free(o);
  return ok;
}

/** Parts of an array-of-offsets table (a rule set, a ligature set): count, offsets, bodies. */
typedef struct Parts {
  GFNT_WBuf body;
  size_t * offs;
  size_t n;
  size_t cap;
} Parts;

static void parts_init(GFNT_RW * rw, Parts * p, size_t cap) {
  gfnt_wbuf_init(&p->body, rw->a);
  p->cap = cap;
  p->n = 0;
  p->offs = xalloc(rw, cap * sizeof *p->offs);
  if (!p->offs) {
    p->cap = 0;
  }
}

static void parts_free(GFNT_RW * rw, Parts * p) {
  gfnt_wbuf_free(&p->body);
  xfree(rw, p->offs);
  p->offs = NULL;
}

/** Where the next part starts; pass it to parts_commit() to keep what was written. */
static size_t parts_begin(const Parts * p) {
  return p->body.length;
}

static void parts_commit(Parts * p, size_t start) {
  if (p->n < p->cap) {
    p->offs[p->n++] = start;
  }
}

static void parts_cancel(Parts * p, size_t start) {
  p->body.length = start;
}

/** Write count, offsets (counted from the table's start) and the bodies. */
static void parts_emit(GFNT_RW * rw, const Parts * p, GFNT_WBuf * out) {
  size_t i;

  gfnt_wbuf_u16(out, (uint32_t)p->n);
  for (i = 0; i < p->n; ++i) {
    size_t v = 2 + 2 * p->n + p->offs[i];

    if (v > 0xFFFFu) {
      rw->overflow = true;
      v = 0;
    }
    gfnt_wbuf_u16(out, (uint32_t)v);
  }
  gfnt_wbuf_bytes(out, p->body.data, p->body.length);
  absorb(rw, out);
}

static uint32_t map_glyph(const GFNT_RW * rw, uint32_t old) {
  return rw->new_of_old[old];
}

/* --- GSUB 1: single substitution ----------------------------------------- */

static bool gsub_single(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, st);
  size_t cov = st + gfnt_rw_u16(rw, st + 2);
  int32_t delta = 0;
  size_t n = 0;
  size_t m = 0;
  size_t i;
  GFNT_CovEnt * ents;
  uint32_t * from;
  uint32_t * to;
  bool uniform = true;
  uint32_t d0 = 0;
  GFNT_WBuf cw;
  bool ok = false;

  if (rw->bad) {
    return false;
  }
  if (format == 1) {
    delta = (int16_t)gfnt_rw_u16(rw, st + 4);
  }
  else if (format != 2) {
    rw->bad = true;
    return false;
  }
  ents = gfnt_rw_cov_read(rw, cov, &n);
  if (!ents) {
    return false;
  }
  from = xalloc(rw, n * sizeof *from);
  to = xalloc(rw, n * sizeof *to);
  gfnt_wbuf_init(&cw, rw->a);
  if (from && to) {
    for (i = 0; i < n && !rw->bad; ++i) {
      uint32_t g = ents[i].glyph;
      uint32_t o;

      if (!gfnt_rw_first(rw, g)) {
        continue;
      }
      o = format == 1 ? ((uint32_t)((int32_t)g + delta) & 0xFFFFu)
          : gfnt_rw_u16(rw, st + 6 + 2 * (size_t)ents[i].index);
      if (rw->bad || !gfnt_rw_kept(rw, o)) {
        continue;
      }
      from[m] = map_glyph(rw, g);
      to[m] = map_glyph(rw, o);
      if (m == 0) {
        d0 = (to[0] - from[0]) & 0xFFFFu;
      }
      else if (((to[m] - from[m]) & 0xFFFFu) != d0) {
        uniform = false;
      }
      ++m;
    }
  }
  if (from && to && m && !rw->bad) {
    gfnt_rw_cov_write(&cw, from, m);
    if (uniform) {
      gfnt_wbuf_u16(out, 1);
      gfnt_wbuf_u16(out, 6);
      gfnt_wbuf_u16(out, d0);
    }
    else {
      if (6 + 2 * m > 0xFFFFu) {
        rw->overflow = true;
      }
      gfnt_wbuf_u16(out, 2);
      gfnt_wbuf_u16(out, (uint32_t)(6 + 2 * m));
      gfnt_wbuf_u16(out, (uint32_t)m);
      for (i = 0; i < m; ++i) {
        gfnt_wbuf_u16(out, to[i]);
      }
    }
    gfnt_wbuf_bytes(out, cw.data, cw.length);
    absorb(rw, out);
    absorb(rw, &cw);
    ok = healthy(rw);
  }
  gfnt_wbuf_free(&cw);
  xfree(rw, from);
  xfree(rw, to);
  xfree(rw, ents);
  return ok;
}

/* --- GSUB 2, 3, 4: coverage-indexed sets --------------------------------- */

enum { SET_SEQUENCE, SET_ALTERNATE, SET_LIGATURE };

/** One entry's child table; false when nothing of it remains. */
static bool set_child(GFNT_RW * rw, int kind, size_t at, GFNT_WBuf * blob) {
  uint16_t n = gfnt_rw_u16(rw, at);
  uint16_t i;

  if (rw->bad) {
    return false;
  }
  if (kind == SET_SEQUENCE) {
    for (i = 0; i < n; ++i) {
      if (!gfnt_rw_kept(rw, gfnt_rw_u16(rw, at + 2 + 2 * (size_t)i))) {
        return false;
      }
    }
    gfnt_wbuf_u16(blob, n);
    for (i = 0; i < n; ++i) {
      gfnt_wbuf_u16(blob, map_glyph(rw, gfnt_rw_u16(rw, at + 2 + 2 * (size_t)i)));
    }
    return !rw->bad;
  }
  if (kind == SET_ALTERNATE) {
    size_t kept = 0;

    gfnt_wbuf_u16(blob, 0);
    for (i = 0; i < n; ++i) {
      uint32_t g = gfnt_rw_u16(rw, at + 2 + 2 * (size_t)i);

      if (!rw->bad && gfnt_rw_kept(rw, g)) {
        gfnt_wbuf_u16(blob, map_glyph(rw, g));
        ++kept;
      }
    }
    if (!blob->oom) {
      gfnt_write_put16(blob->data, (uint32_t)kept);
    }
    return kept > 0 && !rw->bad;
  }
  {
    Parts p;
    uint16_t k;

    parts_init(rw, &p, n);
    for (i = 0; i < n && !rw->bad; ++i) {
      size_t lig = at + gfnt_rw_u16(rw, at + 2 + 2 * (size_t)i);
      uint32_t out_glyph = gfnt_rw_u16(rw, lig);
      uint16_t comps = gfnt_rw_u16(rw, lig + 2);
      bool all = comps >= 1 && gfnt_rw_kept(rw, out_glyph) && !rw->bad;
      size_t start;

      for (k = 1; all && k < comps; ++k) {
        uint32_t g = gfnt_rw_u16(rw, lig + 4 + 2 * (size_t)(k - 1));

        all = !rw->bad && gfnt_rw_kept(rw, g);
      }
      if (!all) {
        continue;
      }
      start = parts_begin(&p);
      gfnt_wbuf_u16(&p.body, map_glyph(rw, out_glyph));
      gfnt_wbuf_u16(&p.body, comps);
      for (k = 1; k < comps; ++k) {
        gfnt_wbuf_u16(&p.body, map_glyph(rw, gfnt_rw_u16(rw, lig + 4 + 2 * (size_t)(k - 1))));
      }
      parts_commit(&p, start);
    }
    if (p.n && !rw->bad) {
      parts_emit(rw, &p, blob);
    }
    k = (uint16_t)p.n;
    parts_free(rw, &p);
    return k > 0 && !rw->bad;
  }
}

static bool gsub_sets(GFNT_RW * rw, int kind, size_t st, GFNT_WBuf * out) {
  size_t n = 0;
  size_t i;
  size_t m = 0;
  GFNT_CovEnt * ents;
  GFNT_WBuf * blobs;
  uint32_t * ids;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (gfnt_rw_u16(rw, st) != 1) {
    rw->bad = true;
    return false;
  }
  ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  if (!ents) {
    return false;
  }
  blobs = xalloc(rw, n * sizeof *blobs);
  ids = xalloc(rw, n * sizeof *ids);
  if (!blobs || !ids) {
    xfree(rw, blobs);
    xfree(rw, ids);
    xfree(rw, ents);
    return false;
  }
  for (i = 0; i < n && !rw->bad; ++i) {
    uint32_t g = ents[i].glyph;
    uint16_t off;

    if (!gfnt_rw_first(rw, g)) {
      continue;
    }
    off = gfnt_rw_u16(rw, st + 6 + 2 * (size_t)ents[i].index);
    if (rw->bad || !off) {
      continue;
    }
    gfnt_wbuf_init(&blobs[m], rw->a);
    if (set_child(rw, kind, st + off, &blobs[m])) {
      ids[m] = map_glyph(rw, g);
      ++m;
    }
    else {
      gfnt_wbuf_free(&blobs[m]);
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m; ++i) {
      gfnt_lout_child16(&o, 6 + 2 * i, 0, &blobs[i]);
    }
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  for (i = 0; i < m; ++i) {
    gfnt_wbuf_free(&blobs[i]);
  }
  xfree(rw, blobs);
  xfree(rw, ids);
  xfree(rw, ents);
  return ok;
}

/* --- GSUB 8: reverse chaining single substitution ------------------------ */

static bool coverage_array(GFNT_RW * rw, GFNT_LOut * o, size_t src_base, size_t src_at,
    size_t count, size_t field) {
  size_t i;

  for (i = 0; i < count; ++i) {
    GFNT_WBuf cw;
    uint16_t off = gfnt_rw_u16(rw, src_at + 2 * i);
    size_t kept;

    if (rw->bad || !off) {
      rw->bad = true;
      return false;
    }
    gfnt_wbuf_init(&cw, rw->a);
    kept = gfnt_rw_cov_filtered(rw, src_base + off, &cw);
    absorb(rw, &cw);
    if (kept == 0) {
      gfnt_wbuf_free(&cw);
      return false;
    }
    gfnt_lout_child16(o, field + 2 * i, 0, &cw);
    gfnt_wbuf_free(&cw);
  }
  return healthy(rw);
}

static bool gsub_reverse(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t back;
  uint16_t ahead;
  size_t la_at;
  size_t sub_at;
  size_t n = 0;
  size_t m = 0;
  size_t i;
  GFNT_CovEnt * ents;
  uint32_t * ids;
  uint32_t * subs;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (gfnt_rw_u16(rw, st) != 1) {
    rw->bad = true;
    return false;
  }
  back = gfnt_rw_u16(rw, st + 4);
  la_at = st + 6 + 2 * (size_t)back;
  ahead = gfnt_rw_u16(rw, la_at);
  sub_at = la_at + 2 + 2 * (size_t)ahead;
  if (rw->bad) {
    return false;
  }
  ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  if (!ents) {
    return false;
  }
  ids = xalloc(rw, n * sizeof *ids);
  subs = xalloc(rw, n * sizeof *subs);
  if (ids && subs) {
    for (i = 0; i < n && !rw->bad; ++i) {
      uint32_t g = ents[i].glyph;
      uint32_t s;

      if (!gfnt_rw_first(rw, g)) {
        continue;
      }
      s = gfnt_rw_u16(rw, sub_at + 2 + 2 * (size_t)ents[i].index);
      if (!rw->bad && gfnt_rw_kept(rw, s)) {
        ids[m] = map_glyph(rw, g);
        subs[m] = map_glyph(rw, s);
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (ids && subs && m && healthy(rw)) {
    size_t b_field = 6;
    size_t a_field = 6 + 2 * (size_t)back + 2;

    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, back);
    for (i = 0; i < back; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, ahead);
    for (i = 0; i < ahead; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, subs[i]);
    }
    if (coverage_array(rw, &o, st, st + 6, back, b_field)
        && coverage_array(rw, &o, st, la_at + 2, ahead, a_field)) {
      gfnt_rw_cov_write(&cw, ids, m);
      gfnt_lout_child16(&o, 2, 0, &cw);
      ok = finish(rw, &o, out);
    }
    else {
      gfnt_lout_free(&o);
    }
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  xfree(rw, ids);
  xfree(rw, subs);
  xfree(rw, ents);
  return ok;
}

/* --- contexts (GSUB 5 and 6, GPOS 7 and 8) ------------------------------- */

/** Copy @p n elements: glyph ids (checked, renumbered) or class values (as they are). */
static bool copy_elements(GFNT_RW * rw, size_t src, size_t n, bool glyphs, GFNT_WBuf * out) {
  size_t i;

  for (i = 0; i < n; ++i) {
    uint32_t v = gfnt_rw_u16(rw, src + 2 * i);

    if (rw->bad) {
      return false;
    }
    if (glyphs) {
      if (!gfnt_rw_kept(rw, v)) {
        return false;
      }
      v = map_glyph(rw, v);
    }
    gfnt_wbuf_u16(out, v);
  }
  return true;
}

/** One rule of any format. false: it names a glyph that cannot occur. */
static bool rule_body(GFNT_RW * rw, size_t rule, bool chain, bool glyphs, GFNT_WBuf * body) {
  GFNT_WBuf recs;
  size_t nrec;
  bool ok = true;

  gfnt_wbuf_init(&recs, rw->a);
  if (!chain) {
    uint16_t count = gfnt_rw_u16(rw, rule);
    uint16_t records = gfnt_rw_u16(rw, rule + 2);

    if (rw->bad || count == 0) {
      gfnt_wbuf_free(&recs);
      return false;
    }
    nrec = gfnt_rw_records(rw, rule + 4 + 2 * (size_t)(count - 1), records, &recs);
    gfnt_wbuf_u16(body, count);
    gfnt_wbuf_u16(body, (uint32_t)nrec);
    ok = copy_elements(rw, rule + 4, (size_t)count - 1, glyphs, body);
  }
  else {
    uint16_t back = gfnt_rw_u16(rw, rule);
    size_t at = rule + 2 + 2 * (size_t)back;
    uint16_t input = gfnt_rw_u16(rw, at);
    size_t la_at;
    uint16_t ahead;
    size_t rec_at;

    if (rw->bad || input == 0) {
      gfnt_wbuf_free(&recs);
      return false;
    }
    la_at = at + 2 + 2 * (size_t)(input - 1);
    ahead = gfnt_rw_u16(rw, la_at);
    rec_at = la_at + 2 + 2 * (size_t)ahead;
    nrec = gfnt_rw_records(rw, rec_at + 2, gfnt_rw_u16(rw, rec_at), &recs);
    gfnt_wbuf_u16(body, back);
    ok = copy_elements(rw, rule + 2, back, glyphs, body);
    gfnt_wbuf_u16(body, input);
    ok = ok && copy_elements(rw, at + 2, (size_t)input - 1, glyphs, body);
    gfnt_wbuf_u16(body, ahead);
    ok = ok && copy_elements(rw, la_at + 2, ahead, glyphs, body);
    gfnt_wbuf_u16(body, (uint32_t)nrec);
  }
  gfnt_wbuf_bytes(body, recs.data, recs.length);
  absorb(rw, &recs);
  gfnt_wbuf_free(&recs);
  return ok && !rw->bad;
}

/** A set of rules, as a blob. false: no rule of it can match. */
static bool rule_set(GFNT_RW * rw, size_t set, bool chain, bool glyphs, GFNT_WBuf * blob) {
  uint16_t rules = gfnt_rw_u16(rw, set);
  uint16_t k;
  Parts p;
  bool any;

  if (rw->bad) {
    return false;
  }
  parts_init(rw, &p, rules);
  for (k = 0; k < rules && !rw->bad && !rw->oom; ++k) {
    size_t rule = set + gfnt_rw_u16(rw, set + 2 + 2 * (size_t)k);
    size_t start = parts_begin(&p);

    if (rule_body(rw, rule, chain, glyphs, &p.body)) {
      parts_commit(&p, start);
    }
    else {
      parts_cancel(&p, start);
    }
  }
  any = p.n > 0 && !rw->bad;
  if (any) {
    parts_emit(rw, &p, blob);
  }
  parts_free(rw, &p);
  return any;
}

static bool context_format1(GFNT_RW * rw, size_t st, bool chain, GFNT_WBuf * out) {
  size_t n = 0;
  size_t m = 0;
  size_t i;
  uint16_t sets = gfnt_rw_u16(rw, st + 4);
  GFNT_CovEnt * ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  GFNT_WBuf * blobs;
  uint32_t * ids;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (!ents) {
    return false;
  }
  blobs = xalloc(rw, n * sizeof *blobs);
  ids = xalloc(rw, n * sizeof *ids);
  if (!blobs || !ids) {
    xfree(rw, blobs);
    xfree(rw, ids);
    xfree(rw, ents);
    return false;
  }
  for (i = 0; i < n && !rw->bad && !rw->oom; ++i) {
    uint32_t g = ents[i].glyph;
    uint16_t off;

    if (!gfnt_rw_first(rw, g) || ents[i].index >= sets) {
      continue;
    }
    off = gfnt_rw_u16(rw, st + 6 + 2 * (size_t)ents[i].index);
    if (rw->bad || !off) {
      continue;
    }
    gfnt_wbuf_init(&blobs[m], rw->a);
    if (rule_set(rw, st + off, chain, true, &blobs[m])) {
      ids[m] = map_glyph(rw, g);
      ++m;
    }
    else {
      gfnt_wbuf_free(&blobs[m]);
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m; ++i) {
      gfnt_lout_child16(&o, 6 + 2 * i, 0, &blobs[i]);
    }
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  for (i = 0; i < m; ++i) {
    gfnt_wbuf_free(&blobs[i]);
  }
  xfree(rw, blobs);
  xfree(rw, ids);
  xfree(rw, ents);
  return ok;
}

static bool context_format2(GFNT_RW * rw, size_t st, bool chain, GFNT_WBuf * out) {
  size_t classdefs = chain ? 3 : 1;
  size_t sets_at = chain ? st + 12 : st + 8;
  uint16_t sets = gfnt_rw_u16(rw, chain ? st + 10 : st + 6);
  GFNT_LOut o;
  GFNT_WBuf cw;
  size_t kept;
  size_t i;
  bool ok = false;

  if (rw->bad) {
    return false;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  kept = gfnt_rw_cov_filtered(rw, st + gfnt_rw_u16(rw, st + 2), &cw);
  absorb(rw, &cw);
  if (kept && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 2);
    gfnt_wbuf_u16(&o.b, 0);
    for (i = 0; i < classdefs; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, sets);
    for (i = 0; i < sets; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_lout_child16(&o, 2, 0, &cw);
    for (i = 0; i < classdefs; ++i) {
      uint16_t src = gfnt_rw_u16(rw, st + 4 + 2 * i);
      GFNT_WBuf cd;

      if (!src || rw->bad) {
        continue;
      }
      gfnt_wbuf_init(&cd, rw->a);
      gfnt_rw_classdef(rw, st + src, NULL, NULL, 0, &cd);
      absorb(rw, &cd);
      gfnt_lout_child16(&o, 4 + 2 * i, 0, &cd);
      gfnt_wbuf_free(&cd);
    }
    for (i = 0; i < sets && healthy(rw); ++i) {
      uint16_t off = gfnt_rw_u16(rw, sets_at + 2 * i);
      GFNT_WBuf blob;

      if (rw->bad || !off) {
        continue;
      }
      gfnt_wbuf_init(&blob, rw->a);
      if (rule_set(rw, st + off, chain, false, &blob)) {
        gfnt_lout_child16(&o, sets_at - st + 2 * i, 0, &blob);
      }
      gfnt_wbuf_free(&blob);
    }
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  return ok;
}

/** Coverage offsets of a format 3 rule, each filtered; false when one is empty. */
static bool context_coverages(GFNT_RW * rw, GFNT_LOut * o, size_t st, size_t src_at,
    size_t count, size_t field) {
  return coverage_array(rw, o, st, src_at, count, field);
}

static bool context_format3(GFNT_RW * rw, size_t st, bool chain, GFNT_WBuf * out) {
  GFNT_LOut o;
  GFNT_WBuf recs;
  size_t nrec;
  bool ok = false;

  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&recs, rw->a);
  if (!chain) {
    uint16_t glyphs = gfnt_rw_u16(rw, st + 2);
    uint16_t records = gfnt_rw_u16(rw, st + 4);
    size_t i;

    if (rw->bad || glyphs == 0) {
      goto done;
    }
    nrec = gfnt_rw_records(rw, st + 6 + 2 * (size_t)glyphs, records, &recs);
    gfnt_wbuf_u16(&o.b, 3);
    gfnt_wbuf_u16(&o.b, glyphs);
    gfnt_wbuf_u16(&o.b, (uint32_t)nrec);
    for (i = 0; i < glyphs; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_bytes(&o.b, recs.data, recs.length);
    if (!context_coverages(rw, &o, st, st + 6, glyphs, 6)) {
      goto done;
    }
  }
  else {
    uint16_t back = gfnt_rw_u16(rw, st + 2);
    size_t in_at = st + 4 + 2 * (size_t)back;
    uint16_t input = gfnt_rw_u16(rw, in_at);
    size_t la_at = in_at + 2 + 2 * (size_t)input;
    uint16_t ahead = gfnt_rw_u16(rw, la_at);
    size_t rec_at = la_at + 2 + 2 * (size_t)ahead;
    size_t i;
    size_t b_field = 4;
    size_t i_field = 4 + 2 * (size_t)back + 2;
    size_t a_field = i_field + 2 * (size_t)input + 2;

    if (rw->bad || input == 0) {
      goto done;
    }
    nrec = gfnt_rw_records(rw, rec_at + 2, gfnt_rw_u16(rw, rec_at), &recs);
    gfnt_wbuf_u16(&o.b, 3);
    gfnt_wbuf_u16(&o.b, back);
    for (i = 0; i < back; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, input);
    for (i = 0; i < input; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, ahead);
    for (i = 0; i < ahead; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    gfnt_wbuf_u16(&o.b, (uint32_t)nrec);
    gfnt_wbuf_bytes(&o.b, recs.data, recs.length);
    if (!context_coverages(rw, &o, st, st + 4, back, b_field)
        || !context_coverages(rw, &o, st, in_at + 2, input, i_field)
        || !context_coverages(rw, &o, st, la_at + 2, ahead, a_field)) {
      goto done;
    }
  }
  absorb(rw, &recs);
  ok = finish(rw, &o, out);
  gfnt_wbuf_free(&recs);
  return ok;

done:
  absorb(rw, &recs);
  gfnt_wbuf_free(&recs);
  gfnt_lout_free(&o);
  return false;
}

static bool context_subtable(GFNT_RW * rw, size_t st, bool chain, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, st);

  if (rw->bad) {
    return false;
  }
  switch (format) {
  case 1:
    return context_format1(rw, st, chain, out);
  case 2:
    return context_format2(rw, st, chain, out);
  case 3:
    return context_format3(rw, st, chain, out);
  default:
    rw->bad = true;
    return false;
  }
}

/* --- GPOS 1: single adjustment ------------------------------------------- */

static bool gpos_single(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, st);
  uint16_t vf = gfnt_rw_u16(rw, st + 4);
  size_t vsize = gfnt_rw_value_size(vf);
  size_t n = 0;
  size_t m = 0;
  size_t i;
  GFNT_CovEnt * ents;
  uint32_t * ids;
  size_t * srcs;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (rw->bad || (format != 1 && format != 2)) {
    rw->bad = true;
    return false;
  }
  ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  if (!ents) {
    return false;
  }
  ids = xalloc(rw, n * sizeof *ids);
  srcs = xalloc(rw, n * sizeof *srcs);
  if (ids && srcs) {
    for (i = 0; i < n; ++i) {
      if (gfnt_rw_first(rw, ents[i].glyph)) {
        ids[m] = map_glyph(rw, ents[i].glyph);
        srcs[m] = st + 8 + vsize * (size_t)ents[i].index;
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (ids && srcs && m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, format);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, vf);
    if (format == 1) {
      gfnt_rw_value(rw, &o, st + 6, vf, st);
    }
    else {
      gfnt_wbuf_u16(&o.b, (uint32_t)m);
      for (i = 0; i < m; ++i) {
        gfnt_rw_value(rw, &o, srcs[i], vf, st);
      }
    }
    gfnt_rw_flush_devices(rw, &o);
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  xfree(rw, ids);
  xfree(rw, srcs);
  xfree(rw, ents);
  return ok;
}

/* --- GPOS 2: pair adjustment --------------------------------------------- */

static bool pair_format1(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t vf1 = gfnt_rw_u16(rw, st + 4);
  uint16_t vf2 = gfnt_rw_u16(rw, st + 6);
  uint16_t sets = gfnt_rw_u16(rw, st + 8);
  size_t s1 = gfnt_rw_value_size(vf1);
  size_t s2 = gfnt_rw_value_size(vf2);
  size_t rec = 2 + s1 + s2;
  size_t n = 0;
  size_t m = 0;
  size_t i;
  size_t j;
  GFNT_CovEnt * ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  uint32_t * ids;
  size_t * sources;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (!ents) {
    return false;
  }
  ids = xalloc(rw, n * sizeof *ids);
  sources = xalloc(rw, n * sizeof *sources);
  if (ids && sources) {
    for (i = 0; i < n && !rw->bad; ++i) {
      uint16_t off;
      size_t set;
      uint16_t count;
      bool any = false;

      if (!gfnt_rw_first(rw, ents[i].glyph) || ents[i].index >= sets) {
        continue;
      }
      off = gfnt_rw_u16(rw, st + 10 + 2 * (size_t)ents[i].index);
      if (rw->bad || !off) {
        continue;
      }
      set = st + off;
      count = gfnt_rw_u16(rw, set);
      for (j = 0; j < count && !any && !rw->bad; ++j) {
        any = gfnt_rw_kept(rw, gfnt_rw_u16(rw, set + 2 + rec * j));
      }
      if (any) {
        ids[m] = map_glyph(rw, ents[i].glyph);
        sources[m] = set;
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (ids && sources && m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, vf1);
    gfnt_wbuf_u16(&o.b, vf2);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m && healthy(rw) && !gfnt_lout_failed(&o); ++i) {
      size_t set = sources[i];
      uint16_t count = gfnt_rw_u16(rw, set);
      size_t at = o.b.length;
      size_t kept = 0;

      put16_at(rw, &o.b, 10 + 2 * i, at);
      gfnt_wbuf_u16(&o.b, 0);
      for (j = 0; j < count && !rw->bad; ++j) {
        size_t r = set + 2 + rec * j;
        uint32_t second = gfnt_rw_u16(rw, r);

        if (rw->bad || !gfnt_rw_kept(rw, second)) {
          continue;
        }
        gfnt_wbuf_u16(&o.b, map_glyph(rw, second));
        gfnt_rw_value(rw, &o, r + 2, vf1, st);
        gfnt_rw_value(rw, &o, r + 2 + s1, vf2, st);
        ++kept;
      }
      put16_at(rw, &o.b, at, kept);
    }
    gfnt_rw_flush_devices(rw, &o);
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  xfree(rw, ids);
  xfree(rw, sources);
  xfree(rw, ents);
  return ok;
}

/** Number the classes in @p used, class 0 first and always. Returns how many. */
static size_t compact_classes(const uint8_t * used, size_t count, uint16_t * map) {
  size_t next = 1;
  size_t c;

  map[0] = 0;
  for (c = 1; c < count; ++c) {
    map[c] = used[c] ? (uint16_t)next++ : 0;
  }
  return next;
}

static bool pair_format2(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t vf1 = gfnt_rw_u16(rw, st + 4);
  uint16_t vf2 = gfnt_rw_u16(rw, st + 6);
  size_t cd1 = gfnt_rw_u16(rw, st + 8);
  size_t cd2 = gfnt_rw_u16(rw, st + 10);
  size_t c1n = gfnt_rw_u16(rw, st + 12);
  size_t c2n = gfnt_rw_u16(rw, st + 14);
  size_t s1 = gfnt_rw_value_size(vf1);
  size_t s2 = gfnt_rw_value_size(vf2);
  size_t n = 0;
  size_t m = 0;
  size_t i;
  size_t c;
  size_t d;
  GFNT_CovEnt * ents;
  uint32_t * ids = NULL;
  uint32_t * olds = NULL;
  uint8_t * used1 = NULL;
  uint8_t * used2 = NULL;
  uint16_t * map1 = NULL;
  uint16_t * map2 = NULL;
  size_t n1;
  size_t n2;
  GFNT_LOut o;
  GFNT_WBuf cw;
  GFNT_WBuf cdb;
  bool ok = false;

  if (rw->bad || c1n == 0 || c2n == 0) {
    return false;
  }
  ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  if (!ents) {
    return false;
  }
  ids = xalloc(rw, n * sizeof *ids);
  olds = xalloc(rw, n * sizeof *olds);
  used1 = xalloc(rw, c1n);
  used2 = xalloc(rw, c2n);
  map1 = xalloc(rw, c1n * sizeof *map1);
  map2 = xalloc(rw, c2n * sizeof *map2);
  if (!ids || !olds || !used1 || !used2 || !map1 || !map2) {
    goto cleanup;
  }
  for (i = 0; i < n && !rw->bad; ++i) {
    uint32_t cls;

    if (!gfnt_rw_first(rw, ents[i].glyph)) {
      continue;
    }
    cls = cd1 ? gfnt_lr_class(rw->r, st + cd1, ents[i].glyph, &rw->bad) : 0;
    if (cls >= c1n) {
      rw->bad = true;
      break;
    }
    used1[cls] = 1;
    ids[m] = map_glyph(rw, ents[i].glyph);
    olds[m] = ents[i].glyph;
    ++m;
  }
  used1[0] = 1;
  used2[0] = 1;
  for (i = 0; i < rw->new_count && !rw->bad; ++i) {
    uint32_t old = rw->old_of_new[i];
    uint32_t cls;

    if (!gfnt_rw_kept(rw, old)) {
      continue;
    }
    cls = cd2 ? gfnt_lr_class(rw->r, st + cd2, old, &rw->bad) : 0;
    if (cls >= c2n) {
      rw->bad = true;
      break;
    }
    used2[cls] = 1;
  }
  n1 = compact_classes(used1, c1n, map1);
  n2 = compact_classes(used2, c2n, map2);
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  gfnt_wbuf_init(&cdb, rw->a);
  if (m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 2);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, vf1);
    gfnt_wbuf_u16(&o.b, vf2);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)n1);
    gfnt_wbuf_u16(&o.b, (uint32_t)n2);
    for (c = 0; c < c1n; ++c) {
      if (!used1[c]) {
        continue;
      }
      for (d = 0; d < c2n; ++d) {
        size_t r;

        if (!used2[d]) {
          continue;
        }
        r = st + 16 + (c * c2n + d) * (s1 + s2);
        gfnt_rw_value(rw, &o, r, vf1, st);
        gfnt_rw_value(rw, &o, r + s1, vf2, st);
      }
    }
    gfnt_rw_flush_devices(rw, &o);
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    if (cd1) {
      gfnt_rw_classdef(rw, st + cd1, map1, olds, m, &cdb);
    }
    else {
      gfnt_wbuf_u16(&cdb, 2);
      gfnt_wbuf_u16(&cdb, 0);
    }
    absorb(rw, &cdb);
    gfnt_lout_child16(&o, 8, 0, &cdb);
    cdb.length = 0;
    if (cd2) {
      gfnt_rw_classdef(rw, st + cd2, map2, NULL, 0, &cdb);
    }
    else {
      gfnt_wbuf_u16(&cdb, 2);
      gfnt_wbuf_u16(&cdb, 0);
    }
    absorb(rw, &cdb);
    gfnt_lout_child16(&o, 10, 0, &cdb);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  gfnt_wbuf_free(&cdb);
cleanup:
  xfree(rw, ids);
  xfree(rw, olds);
  xfree(rw, used1);
  xfree(rw, used2);
  xfree(rw, map1);
  xfree(rw, map2);
  xfree(rw, ents);
  return ok;
}

static bool gpos_pair(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, st);

  if (rw->bad) {
    return false;
  }
  if (format == 1) {
    return pair_format1(rw, st, out);
  }
  if (format == 2) {
    return pair_format2(rw, st, out);
  }
  rw->bad = true;
  return false;
}

/* --- GPOS 3: cursive attachment ------------------------------------------ */

static bool gpos_cursive(GFNT_RW * rw, size_t st, GFNT_WBuf * out) {
  uint16_t count = gfnt_rw_u16(rw, st + 4);
  size_t n = 0;
  size_t m = 0;
  size_t i;
  GFNT_CovEnt * ents;
  uint32_t * ids;
  uint32_t * idx;
  GFNT_LOut o;
  GFNT_WBuf cw;
  bool ok = false;

  if (rw->bad || gfnt_rw_u16(rw, st) != 1) {
    rw->bad = true;
    return false;
  }
  ents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &n);
  if (!ents) {
    return false;
  }
  ids = xalloc(rw, n * sizeof *ids);
  idx = xalloc(rw, n * sizeof *idx);
  if (ids && idx) {
    for (i = 0; i < n; ++i) {
      if (gfnt_rw_first(rw, ents[i].glyph) && ents[i].index < count) {
        ids[m] = map_glyph(rw, ents[i].glyph);
        idx[m] = ents[i].index;
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (ids && idx && m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < 2 * m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m && healthy(rw); ++i) {
      int k;

      for (k = 0; k < 2; ++k) {
        uint16_t off = gfnt_rw_u16(rw, st + 6 + 4 * (size_t)idx[i] + 2 * (size_t)k);
        GFNT_WBuf ab;

        if (rw->bad || !off) {
          continue;
        }
        gfnt_wbuf_init(&ab, rw->a);
        gfnt_rw_anchor(rw, st + off, &ab);
        absorb(rw, &ab);
        gfnt_lout_child16(&o, 6 + 4 * i + 2 * (size_t)k, 0, &ab);
        gfnt_wbuf_free(&ab);
      }
    }
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 2, 0, &cw);
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  xfree(rw, ids);
  xfree(rw, idx);
  xfree(rw, ents);
  return ok;
}

/* --- GPOS 4, 5, 6: mark attachment --------------------------------------- */

enum { ATTACH_BASE, ATTACH_LIGATURE, ATTACH_MARK };

/** A row of anchors, one per used mark class, into @p o at @p field_base. */
static void anchor_row(GFNT_RW * rw, GFNT_LOut * o, size_t row_src, size_t table,
    size_t mcc, const uint8_t * used, size_t field_base) {
  size_t c;
  size_t k = 0;

  for (c = 0; c < mcc; ++c) {
    uint16_t off;
    GFNT_WBuf ab;

    if (!used[c]) {
      continue;
    }
    off = gfnt_rw_u16(rw, row_src + 2 * c);
    if (!rw->bad && off) {
      gfnt_wbuf_init(&ab, rw->a);
      gfnt_rw_anchor(rw, table + off, &ab);
      absorb(rw, &ab);
      gfnt_lout_child16(o, field_base + 2 * k, 0, &ab);
      gfnt_wbuf_free(&ab);
    }
    ++k;
  }
}

static bool gpos_mark_attach(GFNT_RW * rw, int kind, size_t st, GFNT_WBuf * out) {
  size_t mcc = gfnt_rw_u16(rw, st + 6);
  size_t marray = st + gfnt_rw_u16(rw, st + 8);
  size_t barray = st + gfnt_rw_u16(rw, st + 10);
  size_t nm = 0;
  size_t nb = 0;
  size_t mk = 0;
  size_t bk = 0;
  size_t i;
  size_t c;
  size_t m;
  GFNT_CovEnt * ments = NULL;
  GFNT_CovEnt * bents = NULL;
  uint32_t * mids = NULL;
  uint32_t * mrec = NULL;
  uint32_t * bids = NULL;
  uint32_t * bidx = NULL;
  uint8_t * used = NULL;
  uint16_t * map = NULL;
  GFNT_LOut o;
  GFNT_LOut ma;
  GFNT_LOut ba;
  GFNT_WBuf cw;
  GFNT_WBuf ab;
  bool ok = false;

  if (rw->bad || gfnt_rw_u16(rw, st) != 1 || mcc == 0) {
    rw->bad = true;
    return false;
  }
  ments = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 2), &nm);
  bents = gfnt_rw_cov_read(rw, st + gfnt_rw_u16(rw, st + 4), &nb);
  mids = xalloc(rw, (nm + 1) * sizeof *mids);
  mrec = xalloc(rw, (nm + 1) * sizeof *mrec);
  bids = xalloc(rw, (nb + 1) * sizeof *bids);
  bidx = xalloc(rw, (nb + 1) * sizeof *bidx);
  used = xalloc(rw, mcc);
  map = xalloc(rw, mcc * sizeof *map);
  if (!mids || !mrec || !bids || !bidx || !used || !map || !healthy(rw)) {
    goto cleanup;
  }
  for (i = 0; i < nm && !rw->bad; ++i) {
    uint16_t cls;

    if (!gfnt_rw_kept(rw, ments[i].glyph)) {
      continue;
    }
    cls = gfnt_rw_u16(rw, marray + 2 + 4 * (size_t)ments[i].index);
    if (rw->bad || cls >= mcc) {
      rw->bad = true;
      break;
    }
    used[cls] = 1;
    mids[mk] = map_glyph(rw, ments[i].glyph);
    mrec[mk] = ments[i].index;
    ++mk;
  }
  for (i = 0; i < nb; ++i) {
    if (gfnt_rw_first(rw, bents[i].glyph)) {
      bids[bk] = map_glyph(rw, bents[i].glyph);
      bidx[bk] = bents[i].index;
      ++bk;
    }
  }
  m = 0;
  for (c = 0; c < mcc; ++c) {
    map[c] = used[c] ? (uint16_t)m++ : 0;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_lout_init(&ma, rw->a);
  gfnt_lout_init(&ba, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  gfnt_wbuf_init(&ab, rw->a);
  if (mk && bk && healthy(rw)) {
    // The mark array: class (renumbered) and anchor for each kept mark.
    gfnt_wbuf_u16(&ma.b, (uint32_t)mk);
    for (i = 0; i < mk; ++i) {
      gfnt_wbuf_u16(&ma.b, 0);
      gfnt_wbuf_u16(&ma.b, 0);
    }
    for (i = 0; i < mk && healthy(rw); ++i) {
      size_t rec = marray + 2 + 4 * (size_t)mrec[i];
      uint16_t cls = gfnt_rw_u16(rw, rec);
      uint16_t off = gfnt_rw_u16(rw, rec + 2);

      put16_at(rw, &ma.b, 2 + 4 * i, map[cls]);
      if (!rw->bad && off) {
        ab.length = 0;
        gfnt_rw_anchor(rw, marray + off, &ab);
        absorb(rw, &ab);
        gfnt_lout_child16(&ma, 2 + 4 * i + 2, 0, &ab);
      }
    }
    // The base (mark, ligature) array.
    if (kind == ATTACH_LIGATURE) {
      gfnt_wbuf_u16(&ba.b, (uint32_t)bk);
      for (i = 0; i < bk; ++i) {
        gfnt_wbuf_u16(&ba.b, 0);
      }
      for (i = 0; i < bk && healthy(rw); ++i) {
        size_t attach = barray + gfnt_rw_u16(rw, barray + 2 + 2 * (size_t)bidx[i]);
        uint16_t comps = gfnt_rw_u16(rw, attach);
        GFNT_LOut la;
        GFNT_WBuf lab;
        size_t k;

        if (rw->bad) {
          break;
        }
        gfnt_lout_init(&la, rw->a);
        gfnt_wbuf_init(&lab, rw->a);
        gfnt_wbuf_u16(&la.b, comps);
        for (k = 0; k < (size_t)comps * m; ++k) {
          gfnt_wbuf_u16(&la.b, 0);
        }
        for (k = 0; k < comps && healthy(rw); ++k) {
          anchor_row(rw, &la, attach + 2 + 2 * mcc * k, attach, mcc, used,
              2 + 2 * m * k);
        }
        if (la.overflow) {
          rw->overflow = true;
        }
        lab.length = 0;
        gfnt_wbuf_bytes(&lab, la.b.data, la.b.length);
        absorb(rw, &lab);
        if (gfnt_lout_failed(&la)) {
          rw->oom = true;
        }
        gfnt_lout_child16(&ba, 2 + 2 * i, 0, &lab);
        gfnt_wbuf_free(&lab);
        gfnt_lout_free(&la);
      }
    }
    else {
      gfnt_wbuf_u16(&ba.b, (uint32_t)bk);
      for (i = 0; i < bk * m; ++i) {
        gfnt_wbuf_u16(&ba.b, 0);
      }
      for (i = 0; i < bk && healthy(rw); ++i) {
        anchor_row(rw, &ba, barray + 2 + 2 * mcc * (size_t)bidx[i], barray, mcc, used,
            2 + 2 * m * i);
      }
    }
    // The subtable.
    gfnt_wbuf_u16(&o.b, 1);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
    cw.length = 0;
    gfnt_rw_cov_write(&cw, mids, mk);
    gfnt_lout_child16(&o, 2, 0, &cw);
    cw.length = 0;
    gfnt_rw_cov_write(&cw, bids, bk);
    gfnt_lout_child16(&o, 4, 0, &cw);
    if (ma.overflow || ba.overflow) {
      rw->overflow = true;
    }
    if (gfnt_lout_failed(&ma) || gfnt_lout_failed(&ba)) {
      rw->oom = true;
    }
    if (healthy(rw)) {
      GFNT_WBuf blob;

      gfnt_wbuf_init(&blob, rw->a);
      gfnt_wbuf_bytes(&blob, ma.b.data, ma.b.length);
      gfnt_lout_child16(&o, 8, 0, &blob);
      blob.length = 0;
      gfnt_wbuf_bytes(&blob, ba.b.data, ba.b.length);
      gfnt_lout_child16(&o, 10, 0, &blob);
      gfnt_wbuf_free(&blob);
    }
    ok = finish(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_lout_free(&ma);
  gfnt_lout_free(&ba);
  gfnt_wbuf_free(&cw);
  gfnt_wbuf_free(&ab);
cleanup:
  xfree(rw, mids);
  xfree(rw, mrec);
  xfree(rw, bids);
  xfree(rw, bidx);
  xfree(rw, used);
  xfree(rw, map);
  xfree(rw, ments);
  xfree(rw, bents);
  return ok;
}

/* --- dispatch ------------------------------------------------------------ */

bool gfnt_rw_subtable(GFNT_RW * rw, bool is_gpos, uint16_t type, size_t st,
    GFNT_WBuf * out) {
  bool made = false;

  if (!is_gpos) {
    switch (type) {
    case 1:
      made = gsub_single(rw, st, out);
      break;
    case 2:
      made = gsub_sets(rw, SET_SEQUENCE, st, out);
      break;
    case 3:
      made = gsub_sets(rw, SET_ALTERNATE, st, out);
      break;
    case 4:
      made = gsub_sets(rw, SET_LIGATURE, st, out);
      break;
    case 5:
      made = context_subtable(rw, st, false, out);
      break;
    case 6:
      made = context_subtable(rw, st, true, out);
      break;
    case 8:
      made = gsub_reverse(rw, st, out);
      break;
    default:
      rw->bad = true;
      break;
    }
  }
  else {
    switch (type) {
    case 1:
      made = gpos_single(rw, st, out);
      break;
    case 2:
      made = gpos_pair(rw, st, out);
      break;
    case 3:
      made = gpos_cursive(rw, st, out);
      break;
    case 4:
      made = gpos_mark_attach(rw, ATTACH_BASE, st, out);
      break;
    case 5:
      made = gpos_mark_attach(rw, ATTACH_LIGATURE, st, out);
      break;
    case 6:
      made = gpos_mark_attach(rw, ATTACH_MARK, st, out);
      break;
    case 7:
      made = context_subtable(rw, st, false, out);
      break;
    case 8:
      made = context_subtable(rw, st, true, out);
      break;
    default:
      rw->bad = true;
      break;
    }
  }
  return made && healthy(rw);
}
