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
 * `COLR` under a new glyph numbering, and which glyphs a color glyph needs.
 *
 * A base glyph that is kept brings the glyphs it paints with: a version 0 glyph's
 * layers, and a version 1 glyph's PaintGlyph outlines and the base glyphs its
 * PaintColrGlyph nodes name, which are kept as base glyphs in turn. That is the
 * closure, run before the numbering and until nothing is added.
 *
 * The rewrite builds a new table rather than patching the old one in place, so
 * that what the kept glyphs do not reach is gone and not merely zeroed (a color
 * emoji font is megabytes of paints for a few thousand glyphs). Version 0's
 * arrays are rebuilt for the kept bases. Version 1's graph is copied paint by
 * paint from each kept base's root: a paint's size and where its child offsets
 * sit follow from its format, its glyph ids are renumbered, its layers become a
 * new, shorter layer list, and a shared paint is copied once per use because a
 * child's offset counts forward from its parent. The clip list is rebuilt for
 * the kept glyphs, runs of consecutive new ids that share a box becoming one
 * clip. `CPAL` is carried as it is by the caller: a palette entry's index does not
 * depend on any glyph.
 *
 * A variable table keeps its delta-set index map and item variation store whole,
 * behind the paints: a variation index in a paint, a color line or a clip box
 * addresses a row of the store (through the map) and names no glyph, so nothing in
 * either is renumbered. Unreferenced palette entries, and store rows no kept paint
 * uses, are not pruned.
 */

#include <stdlib.h>
#include <string.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "write.h"

#define T_COLR GFNT_TAG('C', 'O', 'L', 'R')
#define NO_GLYPH 0xFFFFFFFFu
#define MAX_NODES (1u << 20)

typedef struct Colr {
  GFNT_Reader r;
  size_t length;
  uint16_t version;
  uint32_t base_n;       // version 0 base glyph records
  uint32_t base_off;
  uint32_t layer_rec_off;
  uint32_t layer_rec_n;
  uint32_t bgl;          // BaseGlyphList, 0 for none
  uint32_t layer_list;
  uint32_t clip_list;
  uint32_t var_map;
  uint32_t var_store;
  uint32_t layer_list_n;
  size_t max_depth;
  uint32_t nodes;
  bool bad;
} Colr;

static uint32_t rd8(Colr * c, size_t at) {
  uint8_t v = 0;

  if (gfnt_reader_u8_at(&c->r, at, &v) != GFNT_OK) {
    c->bad = true;
  }
  return v;
}

static uint32_t rd16(Colr * c, size_t at) {
  uint16_t v = 0;

  if (gfnt_reader_u16_at(&c->r, at, &v) != GFNT_OK) {
    c->bad = true;
  }
  return v;
}

static uint32_t rd32(Colr * c, size_t at) {
  uint32_t v = 0;

  if (gfnt_reader_u32_at(&c->r, at, &v) != GFNT_OK) {
    c->bad = true;
  }
  return v;
}

/* An Offset24: three bytes, big-endian. */
static uint32_t rd24(Colr * c, size_t at) {
  return (rd8(c, at) << 16) | rd16(c, at + 1);
}

/* @p length bytes at an absolute offset of the table. */
static GFNT_Result bytes_at(const GFNT_Reader * r, size_t at, size_t length,
    const uint8_t ** out) {
  GFNT_Reader sub;
  GFNT_Result result = gfnt_reader_sub(r, at, length, &sub);

  return result == GFNT_OK ? gfnt_read_bytes(&sub, length, out) : result;
}

static GFNT_Result colr_open(const GFNT_Face * face, Colr * c, const GFNT_Limits * limits,
    GFNT_Error * error) {
  GFNT_Result r;

  memset(c, 0, sizeof *c);
  r = gfnt_face_table_reader(face, T_COLR, &c->r, error);
  if (r != GFNT_OK) {
    return r;
  }
  c->length = c->r.length;
  c->version = (uint16_t)rd16(c, 0);
  c->base_n = rd16(c, 2);
  c->base_off = rd32(c, 4);
  c->layer_rec_off = rd32(c, 8);
  c->layer_rec_n = rd16(c, 12);
  if (c->version >= 1) {
    c->bgl = rd32(c, 14);
    c->layer_list = rd32(c, 18);
    c->clip_list = rd32(c, 22);
    c->var_map = rd32(c, 26);
    c->var_store = rd32(c, 30);
    if (c->layer_list) {
      c->layer_list_n = rd32(c, c->layer_list);
    }
  }
  c->max_depth = limits->max_paint_depth;
  if (c->bad) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_COLR, 0, GFNT_GLYPH_NONE,
        "a COLR header past the table");
  }
  return GFNT_OK;
}

/* ------------------------------------------------------- paint formats */

typedef struct Shape {
  uint8_t size;       // the paint's own bytes
  uint8_t child;      // an Offset24 to a paint at byte 1 (PaintGlyph and the transforms)
  uint8_t line;       // an Offset24 to a color line at byte 1
  uint8_t var_line;   // that color line carries a variation index per stop
  uint8_t matrix;     // an Offset24 at byte 4 to a 2x3 matrix of this many bytes
  uint8_t glyph_at;   // a glyph id at this byte, or 0
  bool glyph_is_base; // and the glyph it names is itself a base glyph
} Shape;

static bool paint_shape(uint32_t format, Shape * s) {
  static const uint8_t size[33] = {
      0, 6, 5, 9, 16, 20, 16, 20, 12, 16, 6, 3, 7, 7, 8, 12, 8, 12, 12, 16, 6, 10,
      10, 14, 6, 10, 10, 14, 8, 12, 12, 16, 8};

  memset(s, 0, sizeof *s);
  if (format < 1 || format > 32) {
    return false;
  }
  s->size = size[format];
  if (format >= 4 && format <= 9) {
    s->line = 1;
    s->var_line = (uint8_t)(format & 1);
  }
  else if (format == 10) {
    s->child = 1;
    s->glyph_at = 4;
  }
  else if (format == 11) {
    s->glyph_at = 1;
    s->glyph_is_base = true;
  }
  else if (format == 12 || format == 13) {
    s->child = 1;
    s->matrix = format == 12 ? 24 : 28;
  }
  else if (format >= 14 && format <= 31) {
    s->child = 1;
  }
  return true;
}

/* A color line's byte length, from its stop count and whether its stops vary. */
static size_t line_size(Colr * c, size_t at, bool var) {
  return 3 + (size_t)rd16(c, at + 1) * (var ? 10u : 6u);
}

/* The offset of layer-list entry @p index's paint, from the start of the table. */
static size_t layer_paint_at(Colr * c, uint32_t index) {
  if (index >= c->layer_list_n) {
    c->bad = true;
    return 0;
  }
  return (size_t)c->layer_list + rd32(c, (size_t)c->layer_list + 4 + 4u * index);
}

/* ------------------------------------------------------------- closure */

typedef struct Scan {
  Colr * c;
  uint8_t * keep;
  size_t count;
  uint8_t * seen;     // one bit per byte of the table: a paint here was scanned
  bool added;
} Scan;

static void keep_glyph(Scan * s, uint32_t glyph) {
  if (glyph >= s->count) {
    s->c->bad = true;
    return;
  }
  if (!s->keep[glyph]) {
    s->keep[glyph] = 1;
    s->added = true;
  }
}

static GFNT_Result scan_paint(Scan * s, size_t at, size_t depth) {
  Colr * c = s->c;
  Shape sh;
  uint32_t format;

  if (depth > c->max_depth || ++c->nodes > MAX_NODES) {
    return GFNT_ERR_LIMIT;
  }
  if (at >= c->length) {
    return GFNT_ERR_CORRUPT;
  }
  if (s->seen[at >> 3] & (1u << (at & 7))) {
    return GFNT_OK;
  }
  s->seen[at >> 3] |= (uint8_t)(1u << (at & 7));
  format = rd8(c, at);
  if (!paint_shape(format, &sh) || at + sh.size > c->length) {
    return GFNT_ERR_CORRUPT;
  }
  if (format == 1) {
    uint32_t n = rd8(c, at + 1), first = rd32(c, at + 2);

    for (uint32_t i = 0; i < n && !c->bad; i++) {
      GFNT_Result r = scan_paint(s, layer_paint_at(c, first + i), depth + 1);

      if (r != GFNT_OK) {
        return r;
      }
    }
    return c->bad ? GFNT_ERR_CORRUPT : GFNT_OK;
  }
  if (sh.glyph_at) {
    keep_glyph(s, rd16(c, at + sh.glyph_at));
  }
  if (sh.child) {
    GFNT_Result r = scan_paint(s, at + rd24(c, at + 1), depth + 1);

    if (r != GFNT_OK) {
      return r;
    }
  }
  if (format == 32) {
    GFNT_Result r = scan_paint(s, at + rd24(c, at + 1), depth + 1);

    if (r == GFNT_OK) {
      r = scan_paint(s, at + rd24(c, at + 5), depth + 1);
    }
    return r;
  }
  return c->bad ? GFNT_ERR_CORRUPT : GFNT_OK;
}

/* The base glyph's version 0 record: where its layers start and how many; false for none. */
static bool v0_base(Colr * c, uint32_t glyph, uint32_t * first, uint32_t * n) {
  size_t lo = 0, hi = c->base_n;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t at = (size_t)c->base_off + mid * 6u;
    uint32_t g = rd16(c, at);

    if (glyph < g) {
      hi = mid;
    }
    else if (glyph > g) {
      lo = mid + 1;
    }
    else {
      *first = rd16(c, at + 2);
      *n = rd16(c, at + 4);
      return !c->bad;
    }
  }
  return false;
}

/* The base glyph's version 1 root paint, as an offset from the table's start. */
static bool v1_base(Colr * c, uint32_t glyph, size_t * root) {
  size_t lo = 0, hi;
  uint32_t count;

  if (!c->bgl) {
    return false;
  }
  count = rd32(c, c->bgl);
  hi = count;
  while (lo < hi && !c->bad) {
    size_t mid = lo + (hi - lo) / 2;
    size_t at = (size_t)c->bgl + 4 + mid * 6u;
    uint32_t g = rd16(c, at);

    if (glyph < g) {
      hi = mid;
    }
    else if (glyph > g) {
      lo = mid + 1;
    }
    else {
      *root = (size_t)c->bgl + rd32(c, at + 2);
      return !c->bad;
    }
  }
  return false;
}

GFNT_Result gfnt_subset_colr_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_Error * error) {
  Colr c;
  Scan s;
  uint8_t * done;
  GFNT_Result r;

  if (!gfnt_face_has_table(face, T_COLR)) {
    return GFNT_OK;
  }
  r = colr_open(face, &c, limits, error);
  if (r != GFNT_OK) {
    return r;
  }
  done = allocator->calloc_fn(allocator->ctx, count, 1);
  s.c = &c;
  s.keep = keep;
  s.count = count;
  s.seen = allocator->calloc_fn(allocator->ctx, c.length / 8 + 1, 1);
  if (!done || !s.seen) {
    allocator->free_fn(allocator->ctx, done);
    allocator->free_fn(allocator->ctx, s.seen);
    return gfnt_error_set(error, GFNT_ERR_OOM, T_COLR, 0, GFNT_GLYPH_NONE,
        "allocating the color closure");
  }
  // Until a pass adds nothing: a glyph a paint names can itself be a base glyph.
  do {
    s.added = false;
    for (uint32_t g = 0; g < count && r == GFNT_OK; g++) {
      uint32_t first = 0, n = 0;
      size_t root = 0;

      if (!keep[g] || done[g]) {
        continue;
      }
      done[g] = 1;
      if (v0_base(&c, g, &first, &n)) {
        if ((uint64_t)first + n > c.layer_rec_n) {
          r = GFNT_ERR_CORRUPT;
          break;
        }
        for (uint32_t i = 0; i < n; i++) {
          keep_glyph(&s, rd16(&c, (size_t)c.layer_rec_off + (size_t)(first + i) * 4u));
        }
      }
      if (v1_base(&c, g, &root)) {
        r = scan_paint(&s, root, 0);
      }
      if (c.bad) {
        r = GFNT_ERR_CORRUPT;
      }
    }
  } while (s.added && r == GFNT_OK);
  allocator->free_fn(allocator->ctx, done);
  allocator->free_fn(allocator->ctx, s.seen);
  if (r != GFNT_OK) {
    return gfnt_error_set(error, r, T_COLR, 0, GFNT_GLYPH_NONE,
        r == GFNT_ERR_LIMIT ? "a COLR paint graph past the depth or node limit"
                            : "a COLR table that cannot be read");
  }
  return GFNT_OK;
}

/* -------------------------------------------------------------- rewrite */

typedef struct Build {
  Colr * c;
  const uint32_t * new_of_old;
  size_t count;
  GFNT_WBuf p;           // the paints, each child after its parent
  uint32_t * layer_pos;  // the new layer list: where each layer's paint is in p
  size_t layer_n, layer_cap;
  const GFNT_Allocator * a;
  bool oom, overflow;
} Build;

static uint32_t remap(Build * b, uint32_t glyph) {
  if (glyph >= b->count || b->new_of_old[glyph] == NO_GLYPH) {
    b->c->bad = true;
    return 0;
  }
  return b->new_of_old[glyph];
}

static void put24(Build * b, size_t at, size_t value) {
  if (value > 0xFFFFFFu) {
    b->overflow = true;
    return;
  }
  b->p.data[at] = (uint8_t)(value >> 16);
  b->p.data[at + 1] = (uint8_t)(value >> 8);
  b->p.data[at + 2] = (uint8_t)value;
}

static void put16p(Build * b, size_t at, uint32_t v) {
  b->p.data[at] = (uint8_t)(v >> 8);
  b->p.data[at + 1] = (uint8_t)v;
}

static void append_from(Build * b, size_t at, size_t length) {
  const uint8_t * bytes = NULL;

  if (bytes_at(&b->c->r, at, length, &bytes) != GFNT_OK) {
    b->c->bad = true;
    return;
  }
  gfnt_wbuf_bytes(&b->p, bytes, length);
}

/* Copy the paint at @p at, and what it reaches, into p; its position in p. */
static size_t emit_paint(Build * b, size_t at, size_t depth) {
  Colr * c = b->c;
  Shape sh;
  uint32_t format;
  size_t pos = b->p.length;

  if (depth > c->max_depth || ++c->nodes > MAX_NODES) {
    c->nodes = MAX_NODES + 1;
    return 0;
  }
  format = rd8(c, at);
  if (c->bad || !paint_shape(format, &sh) || at + sh.size > c->length) {
    c->bad = true;
    return 0;
  }
  append_from(b, at, sh.size);
  if (b->p.oom || c->bad) {
    return 0;
  }
  if (format == 1) {
    uint32_t n = rd8(c, at + 1), first = rd32(c, at + 2);
    size_t base = b->layer_n;

    if ((uint64_t)first + n > c->layer_list_n) {
      c->bad = true;
      return 0;
    }
    if (base + n > b->layer_cap) {
      size_t cap = (base + n) * 2 + 8;
      uint32_t * grown = b->a->realloc_fn(b->a->ctx, b->layer_pos, cap * sizeof *grown);

      if (!grown) {
        b->oom = true;
        return 0;
      }
      b->layer_pos = grown;
      b->layer_cap = cap;
    }
    b->layer_n += n;
    b->p.data[pos + 2] = (uint8_t)(base >> 24);
    b->p.data[pos + 3] = (uint8_t)(base >> 16);
    b->p.data[pos + 4] = (uint8_t)(base >> 8);
    b->p.data[pos + 5] = (uint8_t)base;
    for (uint32_t i = 0; i < n; i++) {
      size_t child = emit_paint(b, layer_paint_at(c, first + i), depth + 1);

      if (c->bad || b->oom || c->nodes > MAX_NODES) {
        return 0;
      }
      b->layer_pos[base + i] = (uint32_t)child;
    }
    return pos;
  }
  if (sh.glyph_at) {
    put16p(b, pos + sh.glyph_at, remap(b, rd16(c, at + sh.glyph_at)));
  }
  if (sh.child) {
    size_t child = emit_paint(b, at + rd24(c, at + 1), depth + 1);

    if (c->bad || b->oom || c->nodes > MAX_NODES) {
      return 0;
    }
    put24(b, pos + 1, child - pos);
  }
  if (sh.line) {
    size_t line_at = at + rd24(c, at + 1), n = line_size(c, line_at, sh.var_line);
    size_t line_pos = b->p.length;

    append_from(b, line_at, n);
    put24(b, pos + 1, line_pos - pos);
  }
  if (sh.matrix) {
    size_t matrix_at = at + rd24(c, at + 4), matrix_pos = b->p.length;

    append_from(b, matrix_at, sh.matrix);
    put24(b, pos + 4, matrix_pos - pos);
  }
  if (format == 32) {
    size_t source = emit_paint(b, at + rd24(c, at + 1), depth + 1), backdrop;

    if (c->bad || b->oom || c->nodes > MAX_NODES) {
      return 0;
    }
    put24(b, pos + 1, source - pos);
    backdrop = emit_paint(b, at + rd24(c, at + 5), depth + 1);
    if (c->bad || b->oom || c->nodes > MAX_NODES) {
      return 0;
    }
    put24(b, pos + 5, backdrop - pos);
  }
  return pos;
}

typedef struct Base {
  uint32_t new_glyph;
  uint32_t pos;   // the root's place in p
} Base;

GFNT_Result gfnt_subset_colr(const GFNT_Face * face, const uint8_t * keep,
    const uint32_t * new_of_old, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_WBuf * out, GFNT_Error * error) {
  Colr c;
  Build b;
  GFNT_WBuf recs, layer_recs, bgl, lay, clips;
  Base * bases = NULL;
  size_t nbases = 0, nv0 = 0;
  uint32_t * clip_of_old = NULL;
  GFNT_Result r;
  size_t header, at;
  size_t v0_base_pos = 0, v0_layer_pos = 0, bgl_pos = 0, lay_pos = 0, clip_pos = 0, p_pos;
  const uint8_t * var_map_bytes = NULL;
  const uint8_t * var_store_bytes = NULL;
  size_t var_map_len = 0, var_store_len = 0, var_map_pos = 0, var_store_pos = 0, tail;

  if (!gfnt_face_has_table(face, T_COLR)) {
    return GFNT_OK;
  }
  r = colr_open(face, &c, limits, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (c.version >= 1 && (c.var_map || c.var_store)) {
    const uint8_t * bytes = NULL;

    if (c.var_map) {
      uint32_t format, entry_format, n;
      size_t head, len;

      if (c.var_map >= c.length || bytes_at(&c.r, c.var_map, 2, &bytes) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_COLR, 0, GFNT_GLYPH_NONE,
            "a COLR delta-set index map outside the table");
      }
      format = rd8(&c, c.var_map);
      entry_format = rd8(&c, (size_t)c.var_map + 1);
      head = format == 0 ? 4u : 6u;
      n = format == 0 ? rd16(&c, (size_t)c.var_map + 2) : rd32(&c, (size_t)c.var_map + 2);
      len = head + (size_t)n * (((entry_format >> 4) & 3u) + 1u);
      if (format > 1 || c.bad || len > c.length - c.var_map
          || bytes_at(&c.r, c.var_map, len, &bytes) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_COLR, 0, GFNT_GLYPH_NONE,
            "a COLR delta-set index map that cannot be read");
      }
      var_map_bytes = bytes;
      var_map_len = len;
    }
    if (c.var_store) {
      size_t len = 0;

      if (c.var_store >= c.length || bytes_at(&c.r, c.var_store, 8, &bytes) != GFNT_OK
          || bytes_at(&c.r, c.var_store, c.length - c.var_store, &bytes) != GFNT_OK
          || !gfnt_subset_ivs_extent(bytes, c.length - c.var_store, &len)) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_COLR, 0, GFNT_GLYPH_NONE,
            "a COLR item variation store that cannot be read");
      }
      var_store_bytes = bytes;
      var_store_len = len;
    }
  }
  memset(&b, 0, sizeof b);
  b.c = &c;
  b.new_of_old = new_of_old;
  b.count = count;
  b.a = allocator;
  gfnt_wbuf_init(&b.p, allocator);
  gfnt_wbuf_init(&recs, allocator);
  gfnt_wbuf_init(&layer_recs, allocator);
  gfnt_wbuf_init(&bgl, allocator);
  gfnt_wbuf_init(&lay, allocator);
  gfnt_wbuf_init(&clips, allocator);
  bases = allocator->calloc_fn(allocator->ctx, count + 1, sizeof *bases);
  if (!bases) {
    r = GFNT_ERR_OOM;
    goto done;
  }

  for (uint32_t g = 0; g < count && !c.bad; g++) {
    uint32_t first = 0, n = 0;
    size_t root = 0;

    if (!keep[g]) {
      continue;
    }
    if (v0_base(&c, g, &first, &n)) {
      if ((uint64_t)first + n > c.layer_rec_n) {
        c.bad = true;
        break;
      }
      gfnt_wbuf_u16(&recs, remap(&b, g));
      gfnt_wbuf_u16(&recs, (uint32_t)(layer_recs.length / 4));
      gfnt_wbuf_u16(&recs, n);
      for (uint32_t i = 0; i < n; i++) {
        size_t rec = (size_t)c.layer_rec_off + (size_t)(first + i) * 4u;

        gfnt_wbuf_u16(&layer_recs, remap(&b, rd16(&c, rec)));
        gfnt_wbuf_u16(&layer_recs, rd16(&c, rec + 2));
      }
      ++nv0;
    }
    if (v1_base(&c, g, &root)) {
      size_t pos = emit_paint(&b, root, 0);

      if (c.bad || b.oom || c.nodes > MAX_NODES) {
        break;
      }
      bases[nbases].new_glyph = remap(&b, g);
      bases[nbases].pos = (uint32_t)pos;
      ++nbases;
    }
  }
  if (c.nodes > MAX_NODES) {
    r = GFNT_ERR_LIMIT;
    goto done;
  }
  if (c.bad || b.overflow) {
    r = b.overflow ? GFNT_ERR_LIMIT : GFNT_ERR_CORRUPT;
    goto done;
  }
  if (b.oom || b.p.oom || recs.oom || layer_recs.oom) {
    r = GFNT_ERR_OOM;
    goto done;
  }
  if (nv0 > 0xFFFFu || layer_recs.length / 4 > 0xFFFFu) {
    r = GFNT_ERR_LIMIT;
    goto done;
  }

  // The clip list: a clip for each kept version 1 base glyph that has one, runs of
  // consecutive new ids sharing a box becoming one record.
  if (c.version >= 1 && c.clip_list && nbases) {
    uint32_t nclips = rd32(&c, (size_t)c.clip_list + 1);
    size_t nrec = 0, kinds = 0;
    uint32_t * run_first = allocator->calloc_fn(allocator->ctx, nbases + 1, sizeof(uint32_t));
    uint32_t * run_last = allocator->calloc_fn(allocator->ctx, nbases + 1, sizeof(uint32_t));
    uint32_t * run_box = allocator->calloc_fn(allocator->ctx, nbases + 1, sizeof(uint32_t));
    uint32_t * kind_old = allocator->calloc_fn(allocator->ctx, nbases + 1, sizeof(uint32_t));
    uint32_t * kind_pos = allocator->calloc_fn(allocator->ctx, nbases + 1, sizeof(uint32_t));

    clip_of_old = allocator->calloc_fn(allocator->ctx, count, sizeof *clip_of_old);
    if (!run_first || !run_last || !run_box || !kind_old || !kind_pos || !clip_of_old) {
      r = GFNT_ERR_OOM;
    }
    else {
      for (uint32_t i = 0; i < nclips && !c.bad; i++) {
        size_t rec = (size_t)c.clip_list + 5 + (size_t)i * 7u;
        uint32_t lo = rd16(&c, rec), hi = rd16(&c, rec + 2), box = rd24(&c, rec + 4);

        for (uint32_t g = lo; g <= hi && g < count; g++) {
          clip_of_old[g] = box + 1;   // zero means none
        }
      }
      for (size_t i = 0; i < nbases && !c.bad; i++) {
        uint32_t old = 0, box;

        while (old < count && (!keep[old] || new_of_old[old] != bases[i].new_glyph)) {
          ++old;
        }
        box = old < count ? clip_of_old[old] : 0;
        if (!box) {
          continue;
        }
        if (nrec && run_box[nrec - 1] == box && run_last[nrec - 1] + 1 == bases[i].new_glyph) {
          run_last[nrec - 1] = bases[i].new_glyph;
        }
        else {
          run_first[nrec] = run_last[nrec] = bases[i].new_glyph;
          run_box[nrec++] = box;
        }
      }
      if (nrec && !c.bad) {
        size_t records_end = 5 + nrec * 7u;

        gfnt_wbuf_u8(&clips, 1);
        gfnt_wbuf_u32(&clips, (uint32_t)nrec);
        for (size_t i = 0; i < nrec; i++) {
          gfnt_wbuf_u16(&clips, run_first[i]);
          gfnt_wbuf_u16(&clips, run_last[i]);
          gfnt_wbuf_u8(&clips, 0);
          gfnt_wbuf_u16(&clips, 0);
        }
        // Each distinct box once, behind the records.
        for (size_t i = 0; i < nrec && !c.bad && !clips.oom; i++) {
          size_t at_box = (size_t)c.clip_list + run_box[i] - 1;
          size_t k, place;

          for (k = 0; k < kinds && kind_old[k] != run_box[i]; k++) {
          }
          if (k == kinds) {
            uint32_t fmt = rd8(&c, at_box);
            size_t len = fmt == 1 ? 9u : fmt == 2 ? 13u : 0u;
            const uint8_t * bytes = NULL;

            if (!len || bytes_at(&c.r, at_box, len, &bytes) != GFNT_OK) {
              c.bad = true;
              break;
            }
            kind_old[kinds] = run_box[i];
            kind_pos[kinds] = (uint32_t)clips.length;
            ++kinds;
            gfnt_wbuf_bytes(&clips, bytes, len);
          }
          place = kind_pos[k];
          clips.data[5 + i * 7 + 4] = (uint8_t)(place >> 16);
          clips.data[5 + i * 7 + 5] = (uint8_t)(place >> 8);
          clips.data[5 + i * 7 + 6] = (uint8_t)place;
        }
        (void)records_end;
      }
    }
    allocator->free_fn(allocator->ctx, run_first);
    allocator->free_fn(allocator->ctx, run_last);
    allocator->free_fn(allocator->ctx, run_box);
    allocator->free_fn(allocator->ctx, kind_old);
    allocator->free_fn(allocator->ctx, kind_pos);
    if (r != GFNT_OK) {
      goto done;
    }
    if (c.bad || clips.oom || clips.length > 0xFFFFFFu) {
      r = c.bad ? GFNT_ERR_CORRUPT : clips.oom ? GFNT_ERR_OOM : GFNT_ERR_LIMIT;
      goto done;
    }
  }

  // The layout: header, version 0 arrays, the lists, then the paints.
  header = c.version >= 1 ? 34u : 14u;
  at = header;
  if (nv0) {
    v0_base_pos = at;
    at += recs.length;
    v0_layer_pos = at;
    at += layer_recs.length;
  }
  if (c.version >= 1 && nbases) {
    bgl_pos = at;
    at += 4 + nbases * 6u;
  }
  if (c.version >= 1 && b.layer_n) {
    lay_pos = at;
    at += 4 + b.layer_n * 4u;
  }
  if (clips.length) {
    clip_pos = at;
    at += clips.length;
  }
  p_pos = at;
  tail = (p_pos + b.p.length + 3) & ~(size_t)3;
  if (var_map_len) {
    var_map_pos = tail;
    tail = (tail + var_map_len + 3) & ~(size_t)3;
  }
  if (var_store_len) {
    var_store_pos = tail;
  }

  gfnt_wbuf_u16(out, c.version);
  gfnt_wbuf_u16(out, (uint32_t)nv0);
  gfnt_wbuf_u32(out, (uint32_t)v0_base_pos);
  gfnt_wbuf_u32(out, (uint32_t)v0_layer_pos);
  gfnt_wbuf_u16(out, (uint32_t)(layer_recs.length / 4));
  if (c.version >= 1) {
    gfnt_wbuf_u32(out, (uint32_t)bgl_pos);
    gfnt_wbuf_u32(out, (uint32_t)lay_pos);
    gfnt_wbuf_u32(out, (uint32_t)clip_pos);
    gfnt_wbuf_u32(out, (uint32_t)var_map_pos);
    gfnt_wbuf_u32(out, (uint32_t)var_store_pos);
  }
  if (nv0) {
    gfnt_wbuf_bytes(out, recs.data, recs.length);
    gfnt_wbuf_bytes(out, layer_recs.data, layer_recs.length);
  }
  if (bgl_pos) {
    gfnt_wbuf_u32(out, (uint32_t)nbases);
    for (size_t i = 0; i < nbases; i++) {
      gfnt_wbuf_u16(out, bases[i].new_glyph);
      gfnt_wbuf_u32(out, (uint32_t)(p_pos + bases[i].pos - bgl_pos));
    }
  }
  if (lay_pos) {
    gfnt_wbuf_u32(out, (uint32_t)b.layer_n);
    for (size_t i = 0; i < b.layer_n; i++) {
      gfnt_wbuf_u32(out, (uint32_t)(p_pos + b.layer_pos[i] - lay_pos));
    }
  }
  if (clips.length) {
    gfnt_wbuf_bytes(out, clips.data, clips.length);
  }
  gfnt_wbuf_bytes(out, b.p.data, b.p.length);
  if (var_map_len) {
    gfnt_wbuf_align(out, 4);
    gfnt_wbuf_bytes(out, var_map_bytes, var_map_len);
  }
  if (var_store_len) {
    gfnt_wbuf_align(out, 4);
    gfnt_wbuf_bytes(out, var_store_bytes, var_store_len);
  }
  r = out->oom ? GFNT_ERR_OOM : GFNT_OK;

done:
  allocator->free_fn(allocator->ctx, bases);
  allocator->free_fn(allocator->ctx, clip_of_old);
  allocator->free_fn(allocator->ctx, b.layer_pos);
  gfnt_wbuf_free(&b.p);
  gfnt_wbuf_free(&recs);
  gfnt_wbuf_free(&layer_recs);
  gfnt_wbuf_free(&bgl);
  gfnt_wbuf_free(&lay);
  gfnt_wbuf_free(&clips);
  if (r != GFNT_OK) {
    return gfnt_error_set(error, r, T_COLR, 0, GFNT_GLYPH_NONE,
        r == GFNT_ERR_OOM ? "building COLR" : r == GFNT_ERR_LIMIT
            ? "a COLR past the paint limits or too large to subset"
            : "a COLR table that cannot be read");
  }
  return GFNT_OK;
}
