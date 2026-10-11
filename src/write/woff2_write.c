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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/font/macros.h>
#include "write.h"

/*
 * WOFF 2 writer: the forward half of src/blob/woff2.c.
 *
 * The tables are sorted by tag with `loca` moved to follow `glyf`, `glyf` and `loca`
 * are rewritten in the transformed form when the font is a TrueType font whose
 * `glyf` can be parsed, `hmtx` is rewritten when its bearings are the glyphs'
 * `xMin`, `head` gets bit 11 of its flags, and the lot is one Brotli stream.
 *
 * What a table the transform cannot take does: it is stored as it is (glyf and loca
 * with transform version 3). A font is never refused for being untransformable.
 */

#define TAG_GLYF GFNT_TAG('g', 'l', 'y', 'f')
#define TAG_LOCA GFNT_TAG('l', 'o', 'c', 'a')
#define TAG_HMTX GFNT_TAG('h', 'm', 't', 'x')
#define TAG_HHEA GFNT_TAG('h', 'h', 'e', 'a')
#define TAG_HEAD GFNT_TAG('h', 'e', 'a', 'd')
#define TAG_MAXP GFNT_TAG('m', 'a', 'x', 'p')

#define GLYPH_ON_CURVE 0x01u
#define GLYPH_X_SHORT 0x02u
#define GLYPH_Y_SHORT 0x04u
#define GLYPH_REPEAT 0x08u
#define GLYPH_X_SAME 0x10u
#define GLYPH_Y_SAME 0x20u
#define GLYPH_OVERLAP 0x40u

/** The 63 tags a directory entry can name by index. */
static const char known_tags[63][5] = {
  "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ",
  "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp",
  "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF",
  "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL",
  "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc",
  "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx",
  "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill",
};

/** One output table: the bytes the file states, and the bytes the stream holds. */
typedef struct W2Table {
  uint32_t tag;
  const uint8_t * data;     ///< The caller's bytes.
  size_t length;
  uint8_t * owned;          ///< Replacement bytes (head), or the transformed form.
  const uint8_t * original; ///< What `origLength` counts: data, or the edited copy.
  size_t original_length;
  const uint8_t * stream;   ///< What goes in the Brotli stream.
  size_t stream_length;
  uint8_t version;
  bool transformed;
} W2Table;

typedef struct Cur {
  const uint8_t * p;
  size_t n;
  size_t pos;
  bool bad;
} Cur;

static uint32_t cur_u8(Cur * c) {
  if (c->pos >= c->n) {
    c->bad = true;
    return 0;
  }
  return c->p[c->pos++];
}

static uint32_t cur_u16(Cur * c) {
  uint32_t hi = cur_u8(c);
  return (hi << 8) | cur_u8(c);
}

static int32_t cur_i16(Cur * c) {
  return (int16_t)cur_u16(c);
}

static void put_base128(GFNT_WBuf * b, uint32_t v) {
  uint8_t bytes[5];
  size_t n = 0;
  size_t i;

  do {
    bytes[n++] = (uint8_t)(v & 0x7F);
    v >>= 7;
  } while (v);
  for (i = n; i > 0; --i) {
    gfnt_wbuf_u8(b, bytes[i - 1] | (i > 1 ? 0x80u : 0u));
  }
}

static void put_255u16(GFNT_WBuf * b, uint32_t v) {
  if (v < 253) {
    gfnt_wbuf_u8(b, v);
  }
  else if (v < 506) {
    gfnt_wbuf_u8(b, 255);
    gfnt_wbuf_u8(b, v - 253);
  }
  else if (v < 762) {
    gfnt_wbuf_u8(b, 254);
    gfnt_wbuf_u8(b, v - 506);
  }
  else {
    gfnt_wbuf_u8(b, 253);
    gfnt_wbuf_u16(b, v);
  }
}

static uint32_t be16(const uint8_t * p) {
  return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t * p) {
  return (be16(p) << 16) | be16(p + 2);
}

static size_t pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

/** The class and data bytes that spell (dx, dy) in the triplet table. */
static void put_triplet(GFNT_WBuf * flags, GFNT_WBuf * glyph, int32_t dx,
    int32_t dy, bool on_curve) {
  uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx);
  uint32_t ay = (uint32_t)(dy < 0 ? -dy : dy);
  uint32_t sx = dx > 0 ? 1u : 0u;
  uint32_t sy = dy > 0 ? 1u : 0u;
  uint32_t signs = sx | (sy << 1);
  uint32_t index;

  if (dx == 0 && ay < 1280) {
    index = 2 * (ay >> 8) + sy;
    gfnt_wbuf_u8(glyph, ay & 0xFF);
  }
  else if (dy == 0 && ax < 1280) {
    index = 10 + 2 * (ax >> 8) + sx;
    gfnt_wbuf_u8(glyph, ax & 0xFF);
  }
  else if (ax >= 1 && ax <= 64 && ay >= 1 && ay <= 64) {
    index = 20 + ((((ax - 1) >> 4) << 4) | (((ay - 1) >> 4) << 2) | signs);
    gfnt_wbuf_u8(glyph, (((ax - 1) & 15) << 4) | ((ay - 1) & 15));
  }
  else if (ax >= 1 && ax <= 768 && ay >= 1 && ay <= 768) {
    index = 84 + 12 * ((ax - 1) >> 8) + 4 * ((ay - 1) >> 8) + signs;
    gfnt_wbuf_u8(glyph, (ax - 1) & 0xFF);
    gfnt_wbuf_u8(glyph, (ay - 1) & 0xFF);
  }
  else if (ax < 4096 && ay < 4096) {
    index = 120 + signs;
    gfnt_wbuf_u8(glyph, ax >> 4);
    gfnt_wbuf_u8(glyph, ((ax & 15) << 4) | (ay >> 8));
    gfnt_wbuf_u8(glyph, ay & 0xFF);
  }
  else {
    index = 124 + signs;
    gfnt_wbuf_u16(glyph, ax);
    gfnt_wbuf_u16(glyph, ay);
  }
  gfnt_wbuf_u8(flags, index | (on_curve ? 0u : 0x80u));
}

typedef struct Streams {
  GFNT_WBuf ncontour, npoints, flags, glyph, composite, bbox, instructions;
} Streams;

/**
 * Re-encode one glyph of a TrueType `glyf`. Returns false when the glyph does not
 * parse, which makes the whole font untransformable.
 */
static bool encode_glyph(const uint8_t * data, size_t length, Streams * s,
    int32_t * px, int32_t * py, uint8_t * on, uint16_t * ends,
    bool * explicit_box, bool * overlap, int16_t * xmin) {
  Cur c = {data, length, 0, false};
  int32_t nc;
  int32_t box[4];
  uint32_t k;

  *explicit_box = false;
  *overlap = false;
  *xmin = 0;
  if (length == 0) {
    gfnt_wbuf_u16(&s->ncontour, 0);
    return true;
  }
  nc = cur_i16(&c);
  for (k = 0; k < 4; ++k) {
    box[k] = cur_i16(&c);
  }
  if (c.bad) {
    return false;
  }
  if (nc == 0) {
    gfnt_wbuf_u16(&s->ncontour, 0);
    return true;
  }
  *xmin = (int16_t)box[0];
  if (nc < 0) {
    bool instructions = false;
    uint32_t flags;
    size_t start = c.pos;

    if (nc != -1) {
      return false;
    }
    do {
      flags = cur_u16(&c);
      (void)cur_u16(&c);
      c.pos += (flags & 0x0001u) ? 4u : 2u;
      if (flags & 0x0008u) {
        c.pos += 2;
      }
      else if (flags & 0x0040u) {
        c.pos += 4;
      }
      else if (flags & 0x0080u) {
        c.pos += 8;
      }
      if (c.pos > c.n) {
        return false;
      }
      instructions |= (flags & 0x0100u) != 0;
    } while (!c.bad && (flags & 0x0020u));
    if (c.bad) {
      return false;
    }
    gfnt_wbuf_u16(&s->ncontour, 0xFFFFu);
    gfnt_wbuf_bytes(&s->composite, data + start, c.pos - start);
    if (instructions) {
      uint32_t n = cur_u16(&c);

      if (c.bad || n > c.n - c.pos) {
        return false;
      }
      put_255u16(&s->glyph, n);
      gfnt_wbuf_bytes(&s->instructions, data + c.pos, n);
    }
    for (k = 0; k < 4; ++k) {
      gfnt_wbuf_u16(&s->bbox, (uint32_t)box[k] & 0xFFFFu);
    }
    *explicit_box = true;
    return true;
  }
  {
    uint32_t points;
    uint32_t instr_length;
    uint32_t previous = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t i;
    uint32_t filled = 0;
    int32_t computed[4];

    for (k = 0; k < (uint32_t)nc; ++k) {
      uint32_t end = cur_u16(&c);

      if (c.bad || (k > 0 && end < ends[k - 1])) {
        return false;
      }
      ends[k] = (uint16_t)end;
    }
    points = (uint32_t)ends[nc - 1] + 1u;
    instr_length = cur_u16(&c);
    if (c.bad || instr_length > c.n - c.pos) {
      return false;
    }
    {
      const uint8_t * instr = data + c.pos;

      c.pos += instr_length;
      // The flags, with their repeats, into `on` first, so that the coordinates
      // can be read against them; `on` becomes the on-curve bit afterwards.
      {
        uint8_t * raw = on;

        while (filled < points) {
          uint32_t f = cur_u8(&c);
          uint32_t repeat = 0;

          if (c.bad) {
            return false;
          }
          if (f & GLYPH_REPEAT) {
            repeat = cur_u8(&c);
          }
          if (c.bad || filled + 1u + repeat > points) {
            return false;
          }
          do {
            raw[filled++] = (uint8_t)f;
          } while (repeat-- > 0);
        }
        for (i = 0; i < points; ++i) {
          uint32_t f = raw[i];

          if (f & GLYPH_X_SHORT) {
            uint32_t d = cur_u8(&c);

            x += (f & GLYPH_X_SAME) ? (int32_t)d : -(int32_t)d;
          }
          else if (!(f & GLYPH_X_SAME)) {
            x += cur_i16(&c);
          }
          px[i] = x;
        }
        for (i = 0; i < points; ++i) {
          uint32_t f = raw[i];

          if (f & GLYPH_Y_SHORT) {
            uint32_t d = cur_u8(&c);

            y += (f & GLYPH_Y_SAME) ? (int32_t)d : -(int32_t)d;
          }
          else if (!(f & GLYPH_Y_SAME)) {
            y += cur_i16(&c);
          }
          py[i] = y;
        }
        if (c.bad) {
          return false;
        }
        *overlap = (raw[0] & GLYPH_OVERLAP) != 0;
        for (i = 0; i < points; ++i) {
          raw[i] = (raw[i] & GLYPH_ON_CURVE) ? 1 : 0;
        }
      }
      gfnt_wbuf_u16(&s->ncontour, (uint32_t)nc);
      for (k = 0; k < (uint32_t)nc; ++k) {
        put_255u16(&s->npoints, (uint32_t)ends[k] + 1u - previous);
        previous = (uint32_t)ends[k] + 1u;
      }
      {
        int32_t lx = 0;
        int32_t ly = 0;

        for (i = 0; i < points; ++i) {
          put_triplet(&s->flags, &s->glyph, px[i] - lx, py[i] - ly, on[i] != 0);
          lx = px[i];
          ly = py[i];
        }
      }
      put_255u16(&s->glyph, instr_length);
      gfnt_wbuf_bytes(&s->instructions, instr, instr_length);
    }
    computed[0] = computed[1] = computed[2] = computed[3] = 0;
    if (points > 0) {
      computed[0] = computed[2] = px[0];
      computed[1] = computed[3] = py[0];
      for (i = 1; i < points; ++i) {
        if (px[i] < computed[0]) computed[0] = px[i];
        if (px[i] > computed[2]) computed[2] = px[i];
        if (py[i] < computed[1]) computed[1] = py[i];
        if (py[i] > computed[3]) computed[3] = py[i];
      }
    }
    if (computed[0] != box[0] || computed[1] != box[1] || computed[2] != box[2]
        || computed[3] != box[3]) {
      for (k = 0; k < 4; ++k) {
        gfnt_wbuf_u16(&s->bbox, (uint32_t)box[k] & 0xFFFFu);
      }
      *explicit_box = true;
    }
    return true;
  }
}

/**
 * The transformed `glyf` for a TrueType font, or NULL (with @p ok true) when the
 * font cannot be transformed. @p index_format receives the format the rebuilt
 * `loca` will have.
 */
static GFNT_Result transform_glyf(const GFNT_Allocator * a, const W2Table * glyf,
    const W2Table * loca, uint32_t num_glyphs, uint32_t * index_format,
    uint8_t ** out, size_t * out_length, int16_t ** out_xmin, bool * ok) {
  Streams s;
  GFNT_WBuf result;
  uint8_t * bitmap = NULL;
  uint8_t * overlap_bits = NULL;
  int32_t * px = NULL;
  int32_t * py = NULL;
  uint8_t * on = NULL;
  uint16_t * ends = NULL;
  int16_t * xmin = NULL;
  uint32_t * offsets = NULL;
  uint32_t source_format = *index_format;
  size_t bitmap_bytes = 4u * ((num_glyphs + 31u) / 32u);
  size_t overlap_bytes = (num_glyphs + 7u) / 8u;
  bool any_overlap = false;
  bool fine = true;
  uint32_t i;
  GFNT_WBuf * all[7] = {&s.ncontour, &s.npoints, &s.flags, &s.glyph, &s.composite,
      &s.bbox, &s.instructions};

  *ok = false;
  *out = NULL;
  *out_xmin = NULL;
  memset(&s, 0, sizeof s);
  for (i = 0; i < 7; ++i) {
    gfnt_wbuf_init(all[i], a);
  }
  gfnt_wbuf_init(&result, a);
  offsets = a->calloc_fn(a->ctx, (size_t)num_glyphs + 1, sizeof *offsets);
  bitmap = a->calloc_fn(a->ctx, bitmap_bytes ? bitmap_bytes : 1, 1);
  overlap_bits = a->calloc_fn(a->ctx, overlap_bytes ? overlap_bytes : 1, 1);
  px = a->calloc_fn(a->ctx, 65537, sizeof *px);
  py = a->calloc_fn(a->ctx, 65537, sizeof *py);
  on = a->calloc_fn(a->ctx, 65537, 1);
  ends = a->calloc_fn(a->ctx, 65537, sizeof *ends);
  xmin = a->calloc_fn(a->ctx, num_glyphs ? num_glyphs : 1, sizeof *xmin);
  if (!offsets || !bitmap || !overlap_bits || !px || !py || !on || !ends || !xmin) {
    fine = false;
    goto done_oom;
  }
  // The offsets, from the font's own loca, which must be monotonic and inside.
  if (loca->length < ((size_t)num_glyphs + 1) * (source_format ? 4u : 2u)) {
    goto done;
  }
  for (i = 0; i <= num_glyphs; ++i) {
    offsets[i] = source_format ? be32(loca->data + 4u * i)
                               : 2u * be16(loca->data + 2u * i);
    if (offsets[i] > glyf->length || (i > 0 && offsets[i] < offsets[i - 1])) {
      goto done;
    }
  }
  for (i = 0; i < num_glyphs; ++i) {
    bool explicit_box;
    bool overlap;
    size_t length = offsets[i + 1] - offsets[i];

    if (!encode_glyph(glyf->data + offsets[i], length, &s, px, py, on, ends,
            &explicit_box, &overlap, &xmin[i])) {
      goto done;
    }
    if (explicit_box) {
      bitmap[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    if (overlap) {
      overlap_bits[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
      any_overlap = true;
    }
  }
  for (i = 0; i < 7; ++i) {
    if (all[i]->oom) {
      fine = false;
      goto done_oom;
    }
  }
  // A short `loca` holds half-offsets, so the rebuilt glyf, padded to four bytes a
  // glyph, has to fit 0x1FFFE. It is no larger than the source apart from that
  // padding; a font that is close switches to the long form rather than risk being
  // unreadable.
  if (source_format == 0
      && (uint64_t)glyf->length + 4ull * num_glyphs + 1024u > 0x1FFFEull) {
    *index_format = 1;
  }
  {
    GFNT_WBuf * body[7] = {&s.ncontour, &s.npoints, &s.flags, &s.glyph,
        &s.composite, &s.bbox, &s.instructions};
    size_t bbox_total = bitmap_bytes + s.bbox.length;

    gfnt_wbuf_u16(&result, 0);
    gfnt_wbuf_u16(&result, any_overlap ? 1u : 0u);
    gfnt_wbuf_u16(&result, num_glyphs);
    gfnt_wbuf_u16(&result, *index_format);
    for (i = 0; i < 7; ++i) {
      gfnt_wbuf_u32(&result, (uint32_t)(i == 5 ? bbox_total : body[i]->length));
    }
    for (i = 0; i < 7; ++i) {
      if (i == 5) {
        gfnt_wbuf_bytes(&result, bitmap, bitmap_bytes);
      }
      gfnt_wbuf_bytes(&result, body[i]->data, body[i]->length);
    }
    if (any_overlap) {
      gfnt_wbuf_bytes(&result, overlap_bits, overlap_bytes);
    }
  }
  if (result.oom) {
    fine = false;
    goto done_oom;
  }
  *out = a->calloc_fn(a->ctx, result.length ? result.length : 1, 1);
  if (!*out) {
    fine = false;
    goto done_oom;
  }
  memcpy(*out, result.data, result.length);
  *out_length = result.length;
  *out_xmin = xmin;
  xmin = NULL;
  *ok = true;

done_oom:
done:
  for (i = 0; i < 7; ++i) {
    gfnt_wbuf_free(all[i]);
  }
  gfnt_wbuf_free(&result);
  a->free_fn(a->ctx, offsets);
  a->free_fn(a->ctx, bitmap);
  a->free_fn(a->ctx, overlap_bits);
  a->free_fn(a->ctx, px);
  a->free_fn(a->ctx, py);
  a->free_fn(a->ctx, on);
  a->free_fn(a->ctx, ends);
  a->free_fn(a->ctx, xmin);
  return fine ? GFNT_OK : GFNT_ERR_OOM;
}

static int by_tag(const void * x, const void * y) {
  const W2Table * a = x;
  const W2Table * b = y;

  return a->tag < b->tag ? -1 : a->tag > b->tag;
}

static W2Table * find_table(W2Table * t, size_t n, uint32_t tag) {
  size_t i;

  for (i = 0; i < n; ++i) {
    if (t[i].tag == tag) {
      return &t[i];
    }
  }
  return NULL;
}

GFNT_Result gfnt_write_woff2(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits defaults;
  W2Table * t = NULL;
  GFNT_WBuf directory;
  GFNT_WBuf stream;
  GFNT_WBuf file;
  uint8_t * glyf_bytes = NULL;
  int16_t * xmin = NULL;
  void * packed = NULL;
  size_t packed_length = 0;
  GFNT_Result result = GFNT_OK;
  W2Table * glyf;
  W2Table * loca;
  W2Table * head;
  uint32_t num_glyphs = 0;
  uint32_t index_format = 0;
  uint32_t revision = 0;
  uint64_t sfnt_size;
  size_t i;

  if (!out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the font");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  gfnt_wbuf_init(&directory, allocator);
  gfnt_wbuf_init(&stream, allocator);
  gfnt_wbuf_init(&file, allocator);
  if (!tables || count == 0 || count > limits->max_tables) {
    return gfnt_error_set(error, count == 0 || !tables ? GFNT_ERR_INVALID
                                                      : GFNT_ERR_LIMIT,
        0, 0, GFNT_GLYPH_NONE, "no tables, or more than the limit allows");
  }
  t = allocator->calloc_fn(allocator->ctx, count, sizeof *t);
  if (!t) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the table list");
  }
  for (i = 0; i < count; ++i) {
    if (!tables[i].data && tables[i].length) {
      result = gfnt_error_set(error, GFNT_ERR_INVALID, tables[i].tag, 0,
          GFNT_GLYPH_NONE, "a table with a length and no bytes");
      goto done;
    }
    t[i].tag = tables[i].tag;
    t[i].data = tables[i].data;
    t[i].length = tables[i].length;
  }
  qsort(t, count, sizeof *t, by_tag);
  for (i = 1; i < count; ++i) {
    if (t[i].tag == t[i - 1].tag) {
      result = gfnt_error_set(error, GFNT_ERR_INVALID, t[i].tag, 0,
          GFNT_GLYPH_NONE, "a tag appears twice");
      goto done;
    }
  }
  // `loca` straight after `glyf`, as the format asks of the directory.
  glyf = find_table(t, count, TAG_GLYF);
  loca = find_table(t, count, TAG_LOCA);
  if (glyf && loca && loca != glyf + 1) {
    W2Table moved = *loca;
    size_t from = (size_t)(loca - t);
    size_t to = (size_t)(glyf - t) + 1;

    if (from > to) {
      memmove(&t[to + 1], &t[to], (from - to) * sizeof *t);
    }
    else {
      memmove(&t[from], &t[from + 1], (to - from - 1) * sizeof *t);
      to -= 1;
    }
    t[to] = moved;
    glyf = find_table(t, count, TAG_GLYF);
    loca = find_table(t, count, TAG_LOCA);
  }
  for (i = 0; i < count; ++i) {
    t[i].original = t[i].data;
    t[i].original_length = t[i].length;
    t[i].stream = t[i].data;
    t[i].stream_length = t[i].length;
  }

  head = find_table(t, count, TAG_HEAD);
  if (head && head->length >= 54) {
    revision = be32(head->data + 4);
    index_format = be16(head->data + 50) == 1 ? 1u : 0u;
  }
  {
    W2Table * maxp = find_table(t, count, TAG_MAXP);

    if (maxp && maxp->length >= 6) {
      num_glyphs = be16(maxp->data + 4);
    }
  }
  if (glyf && loca && head && head->length >= 54 && num_glyphs > 0
      && (flavour == 0x00010000u || flavour == GFNT_TAG('t', 'r', 'u', 'e'))
      && be16(head->data + 52) == 0) {
    bool ok = false;

    result = transform_glyf(allocator, glyf, loca, num_glyphs, &index_format,
        &glyf_bytes, &glyf->stream_length, &xmin, &ok);
    if (result != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, TAG_GLYF, 0, GFNT_GLYPH_NONE,
          "transforming glyf");
      goto done;
    }
    if (ok) {
      glyf->owned = glyf_bytes;
      glyf->stream = glyf_bytes;
      glyf->transformed = true;
      glyf->version = 0;
      loca->transformed = true;
      loca->version = 0;
      loca->stream = (const uint8_t *)"";
      loca->stream_length = 0;
      loca->original_length = ((size_t)num_glyphs + 1u) * (index_format ? 4u : 2u);
      loca->original = NULL;
    }
  }
  if (!glyf || !glyf->transformed) {
    if (glyf) {
      glyf->version = 3;
    }
    if (loca) {
      loca->version = 3;
    }
  }
  // `head`: bit 11 of the flags says the font was transformed, and the index format
  // follows the rebuilt `loca`.
  if (head && head->length >= 54) {
    uint8_t * copy = allocator->calloc_fn(allocator->ctx, head->length, 1);

    if (!copy) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, TAG_HEAD, 0, GFNT_GLYPH_NONE,
          "copying head");
      goto done;
    }
    memcpy(copy, head->data, head->length);
    copy[16] |= 0x08;
    if (glyf && glyf->transformed) {
      gfnt_write_put16(copy + 50, index_format);
    }
    head->owned = copy;
    head->original = copy;
    head->stream = copy;
  }
  // `hmtx`, when its bearings are the glyphs' xMin.
  if (glyf && glyf->transformed && xmin) {
    W2Table * hmtx = find_table(t, count, TAG_HMTX);
    W2Table * hhea = find_table(t, count, TAG_HHEA);

    if (hmtx && hhea && hhea->length >= 36) {
      uint32_t metrics = be16(hhea->data + 34);

      if (metrics >= 1 && metrics <= num_glyphs
          && hmtx->length == 4u * metrics + 2u * (num_glyphs - metrics)) {
        bool proportional = true;
        bool mono = true;
        uint8_t * out;
        size_t at = 1;
        uint32_t g;

        for (g = 0; g < num_glyphs; ++g) {
          uint32_t lsb = g < metrics ? be16(hmtx->data + 4u * g + 2u)
              : be16(hmtx->data + 4u * metrics + 2u * (g - metrics));

          if (lsb != (uint16_t)xmin[g]) {
            if (g < metrics) proportional = false;
            else mono = false;
          }
        }
        if (proportional || mono) {
          out = allocator->calloc_fn(allocator->ctx, hmtx->length + 1, 1);
          if (!out) {
            result = gfnt_error_set(error, GFNT_ERR_OOM, TAG_HMTX, 0,
                GFNT_GLYPH_NONE, "transforming hmtx");
            goto done;
          }
          out[0] = (uint8_t)((proportional ? 1 : 0) | (mono ? 2 : 0));
          for (g = 0; g < metrics; ++g) {
            memcpy(out + at, hmtx->data + 4u * g, 2);
            at += 2;
          }
          if (!proportional) {
            for (g = 0; g < metrics; ++g) {
              memcpy(out + at, hmtx->data + 4u * g + 2u, 2);
              at += 2;
            }
          }
          if (!mono) {
            memcpy(out + at, hmtx->data + 4u * metrics, 2u * (num_glyphs - metrics));
            at += 2u * (num_glyphs - metrics);
          }
          hmtx->owned = out;
          hmtx->stream = out;
          hmtx->stream_length = at;
          hmtx->transformed = true;
          hmtx->version = 1;
        }
      }
    }
  }

  // The directory and the stream.
  sfnt_size = 12u + 16u * (uint64_t)count;
  for (i = 0; i < count; ++i) {
    unsigned k;
    unsigned index = 63;
    size_t original = t[i].original_length;

    for (k = 0; k < 63; ++k) {
      if (t[i].tag == GFNT_TAG(known_tags[k][0], known_tags[k][1],
              known_tags[k][2], known_tags[k][3])) {
        index = k;
        break;
      }
    }
    gfnt_wbuf_u8(&directory, index | ((unsigned)t[i].version << 6));
    if (index == 63) {
      gfnt_wbuf_u32(&directory, t[i].tag);
    }
    put_base128(&directory, (uint32_t)original);
    if (t[i].transformed) {
      put_base128(&directory, (uint32_t)t[i].stream_length);
    }
    gfnt_wbuf_bytes(&stream, t[i].stream, t[i].stream_length);
    sfnt_size += pad4(original);
  }
  if (directory.oom || stream.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "building the WOFF 2 file");
    goto done;
  }
  {
    gcomp_options_t * options = NULL;
    gcomp_status_t status;

    if (gcomp_options_create(&options) != GCOMP_OK) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "allocating the options for a compress");
      goto done;
    }
    status = gcomp_encode_alloc(NULL, "brotli", options, stream.data,
        stream.length, &packed, &packed_length);
    gcomp_options_destroy(options);
    if (status != GCOMP_OK) {
      result = gfnt_error_set(error,
          status == GCOMP_ERR_MEMORY ? GFNT_ERR_OOM : GFNT_ERR_INTERNAL, 0, 0,
          GFNT_GLYPH_NONE, gcomp_status_to_string(status));
      goto done;
    }
  }
  {
    size_t total = pad4(48 + directory.length + packed_length);

    if (total > limits->max_blob_bytes) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "the font would be larger than the limit allows");
      goto done;
    }
    gfnt_wbuf_u32(&file, GFNT_TAG('w', 'O', 'F', '2'));
    gfnt_wbuf_u32(&file, flavour);
    gfnt_wbuf_u32(&file, (uint32_t)total);
    gfnt_wbuf_u16(&file, (uint32_t)count);
    gfnt_wbuf_u16(&file, 0);
    gfnt_wbuf_u32(&file, (uint32_t)sfnt_size);
    gfnt_wbuf_u32(&file, (uint32_t)packed_length);
    gfnt_wbuf_u16(&file, revision >> 16);
    gfnt_wbuf_u16(&file, revision & 0xFFFFu);
    for (i = 0; i < 5; ++i) {
      gfnt_wbuf_u32(&file, 0); // metadata offset, length, original length; private
    }
    gfnt_wbuf_bytes(&file, directory.data, directory.length);
    gfnt_wbuf_bytes(&file, packed, packed_length);
    gfnt_wbuf_align(&file, 4);
    if (file.oom) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "building the WOFF 2 file");
      goto done;
    }
  }
  result = gfnt_blob_create_memory(file.data, file.length, GFNT_BLOB_COPY, limits,
      allocator, out_blob, error);

done:
  if (packed) {
    gcomp_buffer_free(NULL, packed);
  }
  if (t) {
    for (i = 0; i < count; ++i) {
      allocator->free_fn(allocator->ctx, t[i].owned);
    }
  }
  allocator->free_fn(allocator->ctx, xmin);
  allocator->free_fn(allocator->ctx, t);
  gfnt_wbuf_free(&directory);
  gfnt_wbuf_free(&stream);
  gfnt_wbuf_free(&file);
  return result;
}
