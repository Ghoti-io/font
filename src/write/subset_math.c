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
 * `MATH` for a subset.
 *
 * A glyph that is kept and has a construction (a parenthesis that can grow, a radical, a
 * big operator) brings the glyphs of every size it grows to and every piece it can be built
 * from, so the closure runs first and the table is written once the numbering is known. The
 * constants are carried as they are; the italics corrections, top accent attachments, kern
 * tables and variant constructions are written again for the kept glyphs under the new ids,
 * with the device tables behind them (a variation index is renumbered through the same map
 * as `GPOS`'s, so that `GDEF`'s store can be pruned to the rows named).
 */

#include <stdlib.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "lrewrite.h"

#define T_MATH GFNT_TAG('M', 'A', 'T', 'H')

/** MathConstants: four plain values, 51 value records and one more plain value. */
#define MATH_VALUE_RECORDS 51
#define MATH_CONSTANTS_SIZE (8 + 4 * MATH_VALUE_RECORDS + 2)

static bool healthy(const GFNT_RW * rw) {
  return !rw->bad && !rw->oom && !rw->overflow;
}

/* --- the closure ----------------------------------------------------------- */

static void add_glyph(GFNT_RW * rw, uint8_t * keep, uint32_t g, bool * grew) {
  if (g >= rw->count) {
    rw->bad = true;
    return;
  }
  if (!keep[g]) {
    keep[g] = 1;
    *grew = true;
  }
}

/** The variants and parts of one construction. */
static void close_construction(GFNT_RW * rw, uint8_t * keep, size_t at, bool * grew) {
  uint16_t assembly = gfnt_rw_u16(rw, at);
  uint16_t variants = gfnt_rw_u16(rw, at + 2);
  size_t i;

  for (i = 0; i < variants && !rw->bad; ++i) {
    add_glyph(rw, keep, gfnt_rw_u16(rw, at + 4 + 4 * i), grew);
  }
  if (assembly && !rw->bad) {
    size_t a = at + assembly;
    uint16_t parts = gfnt_rw_u16(rw, a + 4);

    for (i = 0; i < parts && !rw->bad; ++i) {
      add_glyph(rw, keep, gfnt_rw_u16(rw, a + 6 + 10 * i), grew);
    }
  }
}

/** One direction of MathVariants: its coverage and the constructions in the coverage's order. */
static void close_direction(GFNT_RW * rw, uint8_t * keep, size_t variants, size_t coverage,
    size_t constructions, uint16_t count, bool * grew) {
  size_t n = 0;
  size_t i;
  GFNT_CovEnt * ents;

  if (!coverage || !count) {
    return;
  }
  ents = gfnt_rw_cov_read(rw, variants + coverage, &n);
  for (i = 0; i < n && !rw->bad; ++i) {
    uint16_t offset;

    if (ents[i].glyph >= rw->count || !keep[ents[i].glyph]) {
      continue;
    }
    if (ents[i].index >= count) {
      rw->bad = true;
      break;
    }
    offset = gfnt_rw_u16(rw, constructions + 2 * ents[i].index);
    if (offset && !rw->bad) {
      close_construction(rw, keep, variants + offset, grew);
    }
  }
  rw->a->free_fn(rw->a->ctx, ents);
}

GFNT_Result gfnt_subset_math_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Allocator * allocator, bool * changed, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_RW rw;
  uint16_t variants;
  bool grew;

  *changed = false;

  if (!gfnt_face_has_table(face, T_MATH)
      || gfnt_face_table_reader(face, T_MATH, &table, NULL) != GFNT_OK) {
    return GFNT_OK;
  }
  table.error = NULL;
  memset(&rw, 0, sizeof rw);
  rw.a = allocator;
  rw.r = &table;
  rw.count = count;
  if (gfnt_rw_u16(&rw, 0) != 1) {
    return GFNT_OK;
  }
  variants = gfnt_rw_u16(&rw, 8);
  if (rw.bad || !variants) {
    return GFNT_OK;
  }
  // Until a pass adds nothing: a size a glyph grows to can have constructions of its own.
  do {
    uint16_t vcov = gfnt_rw_u16(&rw, variants + 2);
    uint16_t hcov = gfnt_rw_u16(&rw, variants + 4);
    uint16_t vcount = gfnt_rw_u16(&rw, variants + 6);
    uint16_t hcount = gfnt_rw_u16(&rw, variants + 8);

    grew = false;
    close_direction(&rw, keep, variants, vcov, variants + 10, vcount, &grew);
    close_direction(&rw, keep, variants, hcov, variants + 10 + 2 * (size_t)vcount, hcount, &grew);
    if (grew) {
      *changed = true;
    }
  } while (grew && healthy(&rw));
  if (rw.oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, T_MATH, 0, GFNT_GLYPH_NONE,
        "closing over MATH constructions");
  }
  // A table that cannot be read says nothing here, and is left out when the tables are written.
  return GFNT_OK;
}

/* --- the table ------------------------------------------------------------- */

/** A device table a value record names, to be put behind the structure that holds it. */
typedef struct {
  size_t field;    ///< Where in the buffer the offset goes.
  size_t source;   ///< The device table in the source table.
} MDev;

typedef struct {
  GFNT_RW * rw;
  MDev * dev;
  size_t ndev;
  size_t capdev;
} Mctx;

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

/** A MathValueRecord: the value, and an offset to a device table counted from @p origin. */
static void put_value(Mctx * c, GFNT_WBuf * b, size_t src, size_t origin) {
  uint16_t value = gfnt_rw_u16(c->rw, src);
  uint16_t device = gfnt_rw_u16(c->rw, src + 2);

  gfnt_wbuf_u16(b, value);
  if (device && !c->rw->bad) {
    if (c->ndev == c->capdev) {
      size_t cap = c->capdev ? c->capdev * 2 : 16;
      MDev * grown = c->rw->a->calloc_fn(c->rw->a->ctx, cap, sizeof *grown);

      if (!grown) {
        c->rw->oom = true;
        return;
      }
      if (c->ndev) {
        memcpy(grown, c->dev, c->ndev * sizeof *grown);
      }
      c->rw->a->free_fn(c->rw->a->ctx, c->dev);
      c->dev = grown;
      c->capdev = cap;
    }
    c->dev[c->ndev].field = b->length;
    c->dev[c->ndev].source = origin + device;
    ++c->ndev;
  }
  gfnt_wbuf_u16(b, 0);
}

/** Put the deferred device tables at the end of the table @p o holds, and link them. */
static void flush_devices(Mctx * c, GFNT_LOut * o) {
  size_t i;

  for (i = 0; i < c->ndev && healthy(c->rw); ++i) {
    size_t at = o->b.length;

    gfnt_rw_device(c->rw, c->dev[i].source, &o->b);
    absorb(c->rw, &o->b);
    gfnt_lout_link16(o, c->dev[i].field, 0, at);
  }
  c->ndev = 0;
}

/** The entries of a coverage whose glyph is kept, with their positions in the source. */
static size_t kept_entries(GFNT_RW * rw, size_t coverage, GFNT_CovEnt ** out) {
  size_t n = 0;
  size_t kept = 0;
  size_t i;
  GFNT_CovEnt * ents;

  *out = NULL;
  if (!coverage) {
    return 0;
  }
  ents = gfnt_rw_cov_read(rw, coverage, &n);
  for (i = 0; i < n; ++i) {
    if (gfnt_rw_kept(rw, ents[i].glyph)) {
      ents[kept++] = ents[i];
    }
  }
  if (!kept) {
    rw->a->free_fn(rw->a->ctx, ents);
    return 0;
  }
  *out = ents;
  return kept;
}

/** A coverage of the kept entries, under the new ids. */
static void put_coverage(GFNT_RW * rw, const GFNT_CovEnt * ents, size_t n, GFNT_WBuf * out) {
  uint32_t * ids = rw->a->calloc_fn(rw->a->ctx, n, sizeof *ids);
  size_t i;

  if (!ids) {
    rw->oom = true;
    return;
  }
  for (i = 0; i < n; ++i) {
    ids[i] = rw->new_of_old[ents[i].glyph];
  }
  gfnt_rw_cov_write(out, ids, n);
  rw->a->free_fn(rw->a->ctx, ids);
}

static void build_constants(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  GFNT_LOut o;
  size_t i;

  gfnt_lout_init(&o, rw->a);
  for (i = 0; i < 4; ++i) {
    gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, src + 2 * i));
  }
  for (i = 0; i < MATH_VALUE_RECORDS; ++i) {
    put_value(c, &o.b, src + 8 + 4 * i, src);
  }
  gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, src + 8 + 4 * MATH_VALUE_RECORDS));
  flush_devices(c, &o);
  lout_to_buf(rw, &o, out);
}

/** MathItalicsCorrectionInfo and MathTopAccentAttachment: a coverage and a value record each. */
static void build_values(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  GFNT_CovEnt * ents = NULL;
  GFNT_LOut o;
  GFNT_WBuf cov;
  uint16_t count = gfnt_rw_u16(rw, src + 2);
  size_t n = kept_entries(rw, src + gfnt_rw_u16(rw, src), &ents);
  size_t i;

  if (!n || !healthy(rw)) {
    rw->a->free_fn(rw->a->ctx, ents);
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&cov, rw->a);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, (uint32_t)n);
  for (i = 0; i < n; ++i) {
    if (ents[i].index >= count) {
      rw->bad = true;
      break;
    }
    put_value(c, &o.b, src + 4 + 4 * (size_t)ents[i].index, src);
  }
  if (healthy(rw)) {
    put_coverage(rw, ents, n, &cov);
    gfnt_lout_child16(&o, 0, 0, &cov);
    flush_devices(c, &o);
  }
  gfnt_wbuf_free(&cov);
  rw->a->free_fn(rw->a->ctx, ents);
  c->ndev = 0;
  lout_to_buf(rw, &o, out);
}

/** One MathKern: correction heights and kerns, with their device tables. */
static void build_kern(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  uint16_t heights = gfnt_rw_u16(rw, src);
  GFNT_LOut o;
  size_t i;

  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_u16(&o.b, heights);
  for (i = 0; i < (size_t)heights * 2 + 1 && healthy(rw); ++i) {
    put_value(c, &o.b, src + 2 + 4 * i, src);
  }
  if (healthy(rw)) {
    flush_devices(c, &o);
  }
  c->ndev = 0;
  lout_to_buf(rw, &o, out);
}

static void build_kern_info(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  GFNT_CovEnt * ents = NULL;
  GFNT_LOut o;
  GFNT_WBuf piece;
  uint16_t count = gfnt_rw_u16(rw, src + 2);
  size_t n = kept_entries(rw, src + gfnt_rw_u16(rw, src), &ents);
  size_t i;
  size_t j;

  if (!n || !healthy(rw)) {
    rw->a->free_fn(rw->a->ctx, ents);
    return;
  }
  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&piece, rw->a);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, (uint32_t)n);
  for (i = 0; i < n; ++i) {
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
    gfnt_wbuf_u16(&o.b, 0);
  }
  for (i = 0; i < n && healthy(rw); ++i) {
    size_t rec;

    if (ents[i].index >= count) {
      rw->bad = true;
      break;
    }
    rec = src + 4 + 8 * (size_t)ents[i].index;
    for (j = 0; j < 4 && healthy(rw); ++j) {
      uint16_t kern = gfnt_rw_u16(rw, rec + 2 * j);

      if (kern) {
        piece.length = 0;
        build_kern(c, src + kern, &piece);
        gfnt_lout_child16(&o, 4 + 8 * i + 2 * j, 0, &piece);
      }
    }
  }
  if (healthy(rw)) {
    GFNT_WBuf cov;

    gfnt_wbuf_init(&cov, rw->a);
    put_coverage(rw, ents, n, &cov);
    gfnt_lout_child16(&o, 0, 0, &cov);
    gfnt_wbuf_free(&cov);
  }
  gfnt_wbuf_free(&piece);
  rw->a->free_fn(rw->a->ctx, ents);
  lout_to_buf(rw, &o, out);
}

/** ExtendedShapeCoverage: the kept glyphs only. */
static void build_extended(GFNT_RW * rw, size_t src, GFNT_WBuf * out) {
  GFNT_CovEnt * ents = NULL;
  size_t n = kept_entries(rw, src, &ents);

  if (n && healthy(rw)) {
    put_coverage(rw, ents, n, out);
  }
  rw->a->free_fn(rw->a->ctx, ents);
}

static void build_glyph_info(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  GFNT_LOut o;
  GFNT_WBuf piece;
  size_t i;
  bool any = false;

  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&piece, rw->a);
  for (i = 0; i < 4; ++i) {
    gfnt_wbuf_u16(&o.b, 0);
  }
  for (i = 0; i < 4 && healthy(rw); ++i) {
    uint16_t offset = gfnt_rw_u16(rw, src + 2 * i);

    piece.length = 0;
    if (!offset) {
      continue;
    }
    if (i == 0 || i == 1) {
      build_values(c, src + offset, &piece);
    }
    else if (i == 2) {
      build_extended(rw, src + offset, &piece);
    }
    else {
      build_kern_info(c, src + offset, &piece);
    }
    if (piece.length) {
      gfnt_lout_child16(&o, 2 * i, 0, &piece);
      any = true;
    }
  }
  gfnt_wbuf_free(&piece);
  if (!any) {
    gfnt_lout_free(&o);
    return;
  }
  lout_to_buf(rw, &o, out);
}

/** A GlyphAssembly, or nothing when a part's glyph is gone. */
static void build_assembly(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  uint16_t parts = gfnt_rw_u16(rw, src + 4);
  GFNT_LOut o;
  size_t i;

  for (i = 0; i < parts && healthy(rw); ++i) {
    if (!gfnt_rw_kept(rw, gfnt_rw_u16(rw, src + 6 + 10 * i))) {
      return;
    }
  }
  if (!healthy(rw)) {
    return;
  }
  gfnt_lout_init(&o, rw->a);
  put_value(c, &o.b, src, src);
  gfnt_wbuf_u16(&o.b, parts);
  for (i = 0; i < parts && healthy(rw); ++i) {
    size_t at = src + 6 + 10 * i;
    size_t k;

    gfnt_wbuf_u16(&o.b, rw->new_of_old[gfnt_rw_u16(rw, at)]);
    for (k = 1; k < 5; ++k) {
      gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, at + 2 * k));
    }
  }
  if (healthy(rw)) {
    flush_devices(c, &o);
  }
  c->ndev = 0;
  lout_to_buf(rw, &o, out);
}

/** A MathGlyphConstruction: the sizes a glyph grows to that are kept, and its assembly. */
static void build_construction(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  uint16_t assembly = gfnt_rw_u16(rw, src);
  uint16_t variants = gfnt_rw_u16(rw, src + 2);
  GFNT_LOut o;
  GFNT_WBuf piece;
  size_t kept = 0;
  size_t i;

  gfnt_lout_init(&o, rw->a);
  gfnt_wbuf_init(&piece, rw->a);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, 0);
  for (i = 0; i < variants && healthy(rw); ++i) {
    uint16_t glyph = gfnt_rw_u16(rw, src + 4 + 4 * i);

    if (gfnt_rw_kept(rw, glyph)) {
      gfnt_wbuf_u16(&o.b, rw->new_of_old[glyph]);
      gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, src + 6 + 4 * i));
      ++kept;
    }
  }
  if (healthy(rw) && !gfnt_lout_failed(&o) && o.b.length >= 4) {
    gfnt_write_put16(o.b.data + 2, (uint32_t)kept);
    if (assembly) {
      build_assembly(c, src + assembly, &piece);
      gfnt_lout_child16(&o, 0, 0, &piece);
    }
  }
  gfnt_wbuf_free(&piece);
  lout_to_buf(rw, &o, out);
}

/**
 * One direction of MathVariants: the coverage of the kept glyphs, and a construction for each.
 *
 * @param cov_field Where in @p o the coverage offset goes.
 * @param slot Where in @p o the first construction's offset goes.
 * @param written Receives how many constructions were kept.
 */
static void build_direction(Mctx * c, GFNT_LOut * o, size_t variants, uint16_t coverage,
    uint16_t count, size_t constructions, size_t cov_field, size_t slot, size_t * written) {
  GFNT_RW * rw = c->rw;
  GFNT_CovEnt * ents = NULL;
  GFNT_WBuf piece;
  size_t n = kept_entries(rw, coverage ? variants + coverage : 0, &ents);
  size_t i;

  *written = 0;
  if (!n || !healthy(rw)) {
    rw->a->free_fn(rw->a->ctx, ents);
    return;
  }
  gfnt_wbuf_init(&piece, rw->a);
  for (i = 0; i < n && healthy(rw); ++i) {
    uint16_t offset;

    if (ents[i].index >= count) {
      rw->bad = true;
      break;
    }
    offset = gfnt_rw_u16(rw, constructions + 2 * (size_t)ents[i].index);
    piece.length = 0;
    if (offset) {
      build_construction(c, variants + offset, &piece);
    }
    gfnt_lout_child16(o, slot + 2 * i, 0, &piece);
  }
  if (healthy(rw)) {
    piece.length = 0;
    put_coverage(rw, ents, n, &piece);
    gfnt_lout_child16(o, cov_field, 0, &piece);
    *written = n;
  }
  gfnt_wbuf_free(&piece);
  rw->a->free_fn(rw->a->ctx, ents);
}

static void build_variants(Mctx * c, size_t src, GFNT_WBuf * out) {
  GFNT_RW * rw = c->rw;
  uint16_t vcov = gfnt_rw_u16(rw, src + 2);
  uint16_t hcov = gfnt_rw_u16(rw, src + 4);
  uint16_t vcount = gfnt_rw_u16(rw, src + 6);
  uint16_t hcount = gfnt_rw_u16(rw, src + 8);
  size_t vn = 0;
  size_t hn = 0;
  size_t vkept;
  size_t hkept;
  GFNT_LOut o;
  size_t i;

  if (!healthy(rw)) {
    return;
  }
  // The header states how many constructions each direction keeps before the offsets to them,
  // so the coverages are counted first and the constructions linked into the laid-out header.
  gfnt_lout_init(&o, rw->a);
  {
    GFNT_CovEnt * ents = NULL;

    vkept = kept_entries(rw, vcov ? src + vcov : 0, &ents);
    rw->a->free_fn(rw->a->ctx, ents);
    hkept = kept_entries(rw, hcov ? src + hcov : 0, &ents);
    rw->a->free_fn(rw->a->ctx, ents);
  }
  if (!healthy(rw)) {
    gfnt_lout_free(&o);
    return;
  }
  gfnt_wbuf_u16(&o.b, gfnt_rw_u16(rw, src));        // minConnectorOverlap
  gfnt_wbuf_u16(&o.b, 0);                           // vertGlyphCoverage
  gfnt_wbuf_u16(&o.b, 0);                           // horizGlyphCoverage
  gfnt_wbuf_u16(&o.b, (uint32_t)vkept);
  gfnt_wbuf_u16(&o.b, (uint32_t)hkept);
  for (i = 0; i < vkept + hkept; ++i) {
    gfnt_wbuf_u16(&o.b, 0);
  }
  build_direction(c, &o, src, vcov, vcount, src + 10, 2, 10, &vn);
  build_direction(c, &o, src, hcov, hcount, src + 10 + 2 * (size_t)vcount, 4,
      10 + 2 * vkept, &hn);
  if (vn != vkept || hn != hkept) {
    rw->bad = true;
  }
  lout_to_buf(rw, &o, out);
}

GFNT_Result gfnt_subset_math(const GFNT_Face * face, const uint8_t * keep,
    const uint32_t * new_of_old, const uint32_t * old_of_new, size_t count, size_t new_count,
    GFNT_IvsMap * ivs, GFNT_WBuf * out, const GFNT_Allocator * allocator, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_RW rw;
  Mctx c;
  GFNT_LOut o;
  GFNT_WBuf piece;
  uint16_t constants;
  uint16_t info;
  uint16_t variants;

  if (!gfnt_face_has_table(face, T_MATH)
      || gfnt_face_table_reader(face, T_MATH, &table, NULL) != GFNT_OK) {
    return GFNT_OK;
  }
  table.error = NULL;
  memset(&rw, 0, sizeof rw);
  rw.a = allocator;
  rw.r = &table;
  rw.shape = keep;
  rw.new_of_old = new_of_old;
  rw.old_of_new = old_of_new;
  rw.count = count;
  rw.new_count = new_count;
  rw.ivs = ivs;
  memset(&c, 0, sizeof c);
  c.rw = &rw;

  // A MATH this reader cannot make sense of says nothing, as HarfBuzz treats it.
  if (gfnt_rw_u16(&rw, 0) != 1) {
    return GFNT_OK;
  }
  constants = gfnt_rw_u16(&rw, 4);
  info = gfnt_rw_u16(&rw, 6);
  variants = gfnt_rw_u16(&rw, 8);
  if (rw.bad || (!constants && !info && !variants)) {
    return GFNT_OK;
  }
  gfnt_lout_init(&o, allocator);
  gfnt_wbuf_init(&piece, allocator);
  gfnt_wbuf_u32(&o.b, 0x00010000u);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, 0);
  gfnt_wbuf_u16(&o.b, 0);
  if (constants && healthy(&rw)) {
    if ((size_t)constants + MATH_CONSTANTS_SIZE > table.length) {
      rw.bad = true;
    }
    else {
      build_constants(&c, constants, &piece);
      gfnt_lout_child16(&o, 4, 0, &piece);
      piece.length = 0;
    }
  }
  if (info && healthy(&rw)) {
    build_glyph_info(&c, info, &piece);
    gfnt_lout_child16(&o, 6, 0, &piece);
    piece.length = 0;
  }
  if (variants && healthy(&rw)) {
    build_variants(&c, variants, &piece);
    gfnt_lout_child16(&o, 8, 0, &piece);
    piece.length = 0;
  }
  gfnt_wbuf_free(&piece);
  allocator->free_fn(allocator->ctx, c.dev);
  if (rw.bad) {
    // A MATH that read badly part way is left out whole.
    gfnt_lout_free(&o);
    return GFNT_OK;
  }
  lout_to_buf(&rw, &o, out);
  if (rw.oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, T_MATH, 0, GFNT_GLYPH_NONE, "rewriting MATH");
  }
  if (rw.overflow) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, T_MATH, 0, GFNT_GLYPH_NONE,
        "MATH does not fit in 16-bit offsets once rewritten");
  }
  return GFNT_OK;
}
