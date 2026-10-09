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
 * `GDEF` under a new glyph numbering.
 *
 * Glyph classes, attachment points, ligature carets, mark attachment classes
 * and mark glyph sets are carried for the glyphs the text can reach. The item
 * variation store (version 1.3) is not: a font that has one is variable, and
 * the subsetter does not take variable fonts.
 */

#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "../sfnt/sfnt.h"
#include "lrewrite.h"

static bool healthy(const GFNT_RW * rw) {
  return !rw->bad && !rw->oom && !rw->overflow;
}

static void absorb(GFNT_RW * rw, const GFNT_WBuf * b) {
  if (b->oom) {
    rw->oom = true;
  }
}

/** Move a finished LOut into @p out as a new buffer's content. */
static void lout_to_buf(GFNT_RW * rw, GFNT_LOut * o, GFNT_WBuf * out) {
  if (o->overflow) {
    rw->overflow = true;
  }
  if (gfnt_lout_failed(o)) {
    rw->oom = true;
  }
  if (healthy(rw)) {
    gfnt_wbuf_bytes(out, o->b.data, o->b.length);
    absorb(rw, out);
  }
  gfnt_lout_free(o);
}

static void build_attach_list(GFNT_RW * rw, size_t list, GFNT_WBuf * out) {
  size_t n = 0;
  size_t m = 0;
  size_t i;
  uint16_t count = gfnt_rw_u16(rw, list + 2);
  GFNT_CovEnt * ents = gfnt_rw_cov_read(rw, list + gfnt_rw_u16(rw, list), &n);
  uint32_t * ids;
  size_t * srcs;
  GFNT_LOut o;
  GFNT_WBuf cw;

  if (!ents) {
    return;
  }
  ids = rw->a->calloc_fn(rw->a->ctx, n ? n : 1, sizeof *ids);
  srcs = rw->a->calloc_fn(rw->a->ctx, n ? n : 1, sizeof *srcs);
  if (!ids || !srcs) {
    rw->oom = true;
  }
  else {
    for (i = 0; i < n && !rw->bad; ++i) {
      uint16_t off;

      if (!gfnt_rw_kept(rw, ents[i].glyph) || ents[i].index >= count) {
        continue;
      }
      off = gfnt_rw_u16(rw, list + 4 + 2 * (size_t)ents[i].index);
      if (!rw->bad && off) {
        ids[m] = rw->new_of_old[ents[i].glyph];
        srcs[m] = list + off;
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m && healthy(rw); ++i) {
      uint16_t points = gfnt_rw_u16(rw, srcs[i]);
      GFNT_WBuf pb;
      uint16_t k;

      gfnt_wbuf_init(&pb, rw->a);
      gfnt_wbuf_u16(&pb, points);
      for (k = 0; k < points; ++k) {
        gfnt_wbuf_u16(&pb, gfnt_rw_u16(rw, srcs[i] + 2 + 2 * (size_t)k));
      }
      absorb(rw, &pb);
      gfnt_lout_child16(&o, 4 + 2 * i, 0, &pb);
      gfnt_wbuf_free(&pb);
    }
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 0, 0, &cw);
    lout_to_buf(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  rw->a->free_fn(rw->a->ctx, ids);
  rw->a->free_fn(rw->a->ctx, srcs);
  rw->a->free_fn(rw->a->ctx, ents);
}

static void build_caret(GFNT_RW * rw, size_t caret, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, caret);

  if (rw->bad) {
    return;
  }
  gfnt_wbuf_u16(out, format);
  gfnt_wbuf_u16(out, gfnt_rw_u16(rw, caret + 2));
  if (format == 3) {
    uint16_t dev = gfnt_rw_u16(rw, caret + 4);

    if (dev && !rw->bad) {
      gfnt_wbuf_u16(out, 6);
      gfnt_rw_device(rw, caret + dev, out);
    }
    else {
      gfnt_wbuf_u16(out, 0);
    }
  }
  else if (format != 1 && format != 2) {
    rw->bad = true;
  }
}

static void build_lig_carets(GFNT_RW * rw, size_t list, GFNT_WBuf * out) {
  size_t n = 0;
  size_t m = 0;
  size_t i;
  uint16_t count = gfnt_rw_u16(rw, list + 2);
  GFNT_CovEnt * ents = gfnt_rw_cov_read(rw, list + gfnt_rw_u16(rw, list), &n);
  uint32_t * ids;
  size_t * srcs;
  GFNT_LOut o;
  GFNT_WBuf cw;

  if (!ents) {
    return;
  }
  ids = rw->a->calloc_fn(rw->a->ctx, n ? n : 1, sizeof *ids);
  srcs = rw->a->calloc_fn(rw->a->ctx, n ? n : 1, sizeof *srcs);
  if (!ids || !srcs) {
    rw->oom = true;
  }
  else {
    for (i = 0; i < n && !rw->bad; ++i) {
      uint16_t off;

      if (!gfnt_rw_kept(rw, ents[i].glyph) || ents[i].index >= count) {
        continue;
      }
      off = gfnt_rw_u16(rw, list + 4 + 2 * (size_t)ents[i].index);
      if (!rw->bad && off) {
        ids[m] = rw->new_of_old[ents[i].glyph];
        srcs[m] = list + off;
        ++m;
      }
    }
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cw, rw->a);
  if (m && healthy(rw)) {
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, (uint32_t)m);
    for (i = 0; i < m; ++i) {
      gfnt_wbuf_u16(&o.b, 0);
    }
    for (i = 0; i < m && healthy(rw); ++i) {
      uint16_t carets = gfnt_rw_u16(rw, srcs[i]);
      GFNT_LOut lg;
      GFNT_WBuf lb;
      uint16_t k;

      gfnt_lout_init(&lg, rw->a);
      gfnt_wbuf_init(&lb, rw->a);
      gfnt_wbuf_u16(&lg.b, carets);
      for (k = 0; k < carets; ++k) {
        gfnt_wbuf_u16(&lg.b, 0);
      }
      for (k = 0; k < carets && healthy(rw); ++k) {
        GFNT_WBuf cb;
        uint16_t coff = gfnt_rw_u16(rw, srcs[i] + 2 + 2 * (size_t)k);

        if (rw->bad || !coff) {
          continue;
        }
        gfnt_wbuf_init(&cb, rw->a);
        build_caret(rw, srcs[i] + coff, &cb);
        absorb(rw, &cb);
        gfnt_lout_child16(&lg, 2 + 2 * (size_t)k, 0, &cb);
        gfnt_wbuf_free(&cb);
      }
      lout_to_buf(rw, &lg, &lb);
      gfnt_lout_child16(&o, 4 + 2 * i, 0, &lb);
      gfnt_wbuf_free(&lb);
    }
    gfnt_rw_cov_write(&cw, ids, m);
    gfnt_lout_child16(&o, 0, 0, &cw);
    lout_to_buf(rw, &o, out);
  }
  else {
    gfnt_lout_free(&o);
  }
  gfnt_wbuf_free(&cw);
  rw->a->free_fn(rw->a->ctx, ids);
  rw->a->free_fn(rw->a->ctx, srcs);
  rw->a->free_fn(rw->a->ctx, ents);
}

static void build_mark_sets(GFNT_RW * rw, size_t list, GFNT_WBuf * out) {
  uint16_t format = gfnt_rw_u16(rw, list);
  uint16_t count = gfnt_rw_u16(rw, list + 2);
  GFNT_LOut o;
  uint16_t i;

  if (rw->bad || format != 1) {
    rw->bad = true;
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, 1);
  gfnt_wbuf_u16(&o.b, count);
  for (i = 0; i < count; ++i) {
    gfnt_wbuf_u32(&o.b, 0);
  }
  for (i = 0; i < count && healthy(rw) && !gfnt_lout_failed(&o); ++i) {
    uint32_t off = gfnt_rw_u32(rw, list + 4 + 4 * (size_t)i);
    GFNT_WBuf cw;
    size_t kept = 0;

    gfnt_wbuf_init(&cw, rw->a);
    if (off && !rw->bad) {
      kept = gfnt_rw_cov_filtered(rw, list + off, &cw);
    }
    if (kept == 0) {
      cw.length = 0;
      gfnt_wbuf_u16(&cw, 1);
      gfnt_wbuf_u16(&cw, 0);
    }
    absorb(rw, &cw);
    gfnt_lout_link32(&o, 4 + 4 * (size_t)i, 0, gfnt_lout_add(&o, cw.data, cw.length));
    gfnt_wbuf_free(&cw);
  }
  lout_to_buf(rw, &o, out);
}

GFNT_Result gfnt_subset_gdef(const GFNT_Face * face, const uint8_t * shape,
    const uint32_t * new_of_old, const uint32_t * old_of_new, size_t count,
    size_t new_count, GFNT_WBuf * out, const GFNT_Allocator * allocator,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_RW rw;
  GFNT_LOut o;
  GFNT_WBuf piece;
  uint16_t major;
  uint16_t minor;
  uint16_t classes;
  uint16_t attach;
  uint16_t carets;
  uint16_t marks;
  uint16_t sets = 0;
  size_t header;
  size_t i;

  if (!gfnt_face_has_table(face, GFNT_TAG_GDEF)
      || gfnt_face_table_reader(face, GFNT_TAG_GDEF, &table, NULL) != GFNT_OK) {
    return GFNT_OK;
  }
  table.error = NULL;
  memset(&rw, 0, sizeof rw);
  rw.a = allocator;
  rw.r = &table;
  rw.shape = shape;
  rw.new_of_old = new_of_old;
  rw.old_of_new = old_of_new;
  rw.count = count;
  rw.new_count = new_count;

  major = gfnt_rw_u16(&rw, 0);
  minor = gfnt_rw_u16(&rw, 2);
  classes = gfnt_rw_u16(&rw, 4);
  attach = gfnt_rw_u16(&rw, 6);
  carets = gfnt_rw_u16(&rw, 8);
  marks = gfnt_rw_u16(&rw, 10);
  if (minor >= 2) {
    sets = gfnt_rw_u16(&rw, 12);
  }
  // A GDEF this reader cannot make sense of says nothing, as HarfBuzz treats it.
  if (rw.bad || major != 1 || (!classes && !attach && !carets && !marks && !sets)) {
    return GFNT_OK;
  }
  header = sets ? 14 : 12;
  gfnt_lout_init(&o, allocator);
  gfnt_wbuf_u32(&o.b, sets ? 0x00010002u : 0x00010000u);
  for (i = 4; i < header; i += 2) {
    gfnt_wbuf_u16(&o.b, 0);
  }
  gfnt_wbuf_init(&piece, allocator);

  if (classes) {
    gfnt_rw_classdef(&rw, classes, NULL, NULL, 0, &piece);
    absorb(&rw, &piece);
    gfnt_lout_child16(&o, 4, 0, &piece);
    piece.length = 0;
  }
  if (attach && healthy(&rw)) {
    build_attach_list(&rw, attach, &piece);
    gfnt_lout_child16(&o, 6, 0, &piece);
    piece.length = 0;
  }
  if (carets && healthy(&rw)) {
    build_lig_carets(&rw, carets, &piece);
    gfnt_lout_child16(&o, 8, 0, &piece);
    piece.length = 0;
  }
  if (marks && healthy(&rw)) {
    gfnt_rw_classdef(&rw, marks, NULL, NULL, 0, &piece);
    absorb(&rw, &piece);
    gfnt_lout_child16(&o, 10, 0, &piece);
    piece.length = 0;
  }
  if (sets && healthy(&rw)) {
    build_mark_sets(&rw, sets, &piece);
    gfnt_lout_child16(&o, 12, 0, &piece);
    piece.length = 0;
  }
  gfnt_wbuf_free(&piece);
  if (rw.bad) {
    // A GDEF that read badly part way is left out whole.
    gfnt_lout_free(&o);
    return GFNT_OK;
  }
  lout_to_buf(&rw, &o, out);
  if (rw.oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GDEF, 0, GFNT_GLYPH_NONE,
        "rewriting GDEF");
  }
  if (rw.overflow) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_GDEF, 0, GFNT_GLYPH_NONE,
        "GDEF does not fit in 16-bit offsets once rewritten");
  }
  return GFNT_OK;
}
