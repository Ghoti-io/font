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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/metrics.h>
#include "write.h"

/*
 * Bitmap font writers: BDF, PCF, PSF 1 and 2, and GNU Unifont's .hex.
 *
 * Every format is written from one neutral model of the face's strike - each
 * glyph's box, advance and rows, and the (codepoint, glyph) pairs - so the four
 * agree about what a glyph is, and what each format cannot say is refused in one
 * place rather than silently dropped.
 */

typedef struct BGlyph {
  bool present;
  uint32_t width, height;
  int32_t bearing_x, bearing_y, advance;
  size_t stride;
  const uint8_t * bits;
  char * name;
} BGlyph;

typedef struct Pair {
  uint32_t codepoint;
  uint32_t glyph;
} Pair;

typedef struct Model {
  const GFNT_Allocator * allocator;
  GFNT_Error * error;
  GFNT_Strike strike;
  BGlyph * glyphs;
  size_t count;
  Pair * pairs;
  size_t pair_count;
  bool states_encoding;
  uint32_t px;            ///< Pixel size for BDF's SIZE and PCF's PIXEL_SIZE.
} Model;

static GFNT_Result fail(const Model * m, GFNT_Result code, const char * message) {
  return gfnt_error_set(m->error, code, 0, 0, GFNT_GLYPH_NONE, message);
}

static int by_codepoint(const void * x, const void * y) {
  const Pair * a = x;
  const Pair * b = y;

  if (a->codepoint != b->codepoint) {
    return a->codepoint < b->codepoint ? -1 : 1;
  }
  return a->glyph < b->glyph ? -1 : a->glyph > b->glyph;
}

static void model_free(Model * m) {
  size_t i;

  if (m->glyphs) {
    for (i = 0; i < m->count; ++i) {
      gfnt_glyph_name_free(m->allocator, m->glyphs[i].name);
    }
  }
  m->allocator->free_fn(m->allocator->ctx, m->glyphs);
  m->allocator->free_fn(m->allocator->ctx, m->pairs);
}

static GFNT_Result model_build(Model * m, const GFNT_Face * face, size_t strike) {
  size_t count = 0;
  size_t pairs = 0;
  size_t i;
  GFNT_Result result;

  result = gfnt_face_strike_at(face, strike, &m->strike, m->error);
  if (result != GFNT_OK) {
    return result;
  }
  if (m->strike.bit_depth != 1) {
    return fail(m, GFNT_ERR_UNSUPPORTED,
        "only one-bit strikes can be written as a bitmap font");
  }
  result = gfnt_face_num_glyphs(face, &count, m->error);
  if (result != GFNT_OK) {
    return result;
  }
  if (count == 0 || count > 65535u) {
    return fail(m, GFNT_ERR_UNSUPPORTED, "no glyphs, or more than 65,535");
  }
  m->count = count;
  m->glyphs = m->allocator->calloc_fn(m->allocator->ctx, count, sizeof *m->glyphs);
  if (!m->glyphs) {
    return fail(m, GFNT_ERR_OOM, "allocating the glyph list");
  }
  m->px = m->strike.ppem_y ? m->strike.ppem_y
                           : (uint32_t)(m->strike.ascent - m->strike.descent);
  for (i = 0; i < count; ++i) {
    GFNT_BitmapGlyph bitmap;
    BGlyph * g = &m->glyphs[i];
    char * name = NULL;

    result = gfnt_face_glyph_bitmap(face, (uint32_t)i, strike, &bitmap, NULL);
    if (result == GFNT_ERR_UNSUPPORTED) {
      continue;
    }
    if (result != GFNT_OK) {
      return fail(m, result, "a glyph of the strike cannot be read");
    }
    g->present = true;
    g->width = bitmap.width;
    g->height = bitmap.height;
    g->bearing_x = bitmap.bearing_x;
    g->bearing_y = bitmap.bearing_y;
    g->advance = bitmap.advance;
    g->stride = bitmap.stride;
    g->bits = bitmap.bits;
    if (gfnt_face_glyph_name(face, (uint32_t)i, m->allocator, &name, NULL, NULL)
        == GFNT_OK) {
      g->name = name;
    }
  }
  // The encoding: the container's own, or the face's cmap by scanning it.
  result = gfnt_face_bitmap_encoding_count(face, &pairs, NULL);
  if (result == GFNT_OK) {
    m->states_encoding = true;
    m->pairs = m->allocator->calloc_fn(m->allocator->ctx, pairs ? pairs : 1,
        sizeof *m->pairs);
    if (!m->pairs) {
      return fail(m, GFNT_ERR_OOM, "allocating the encoding");
    }
    for (i = 0; i < pairs; ++i) {
      if (gfnt_face_bitmap_encoding_at(face, i, &m->pairs[i].codepoint,
              &m->pairs[i].glyph, NULL) != GFNT_OK) {
        return fail(m, GFNT_ERR_INTERNAL, "an encoding pair cannot be read");
      }
    }
    m->pair_count = pairs;
  }
  else if (gfnt_face_cmap_count(face, &pairs, NULL) == GFNT_OK && pairs > 0) {
    uint32_t cp;
    size_t used = 0;
    size_t capacity = 256;

    m->states_encoding = true;
    m->pairs = m->allocator->calloc_fn(m->allocator->ctx, capacity,
        sizeof *m->pairs);
    if (!m->pairs) {
      return fail(m, GFNT_ERR_OOM, "allocating the encoding");
    }
    for (cp = 0; cp <= 0x10FFFFu; ++cp) {
      uint32_t glyph = 0;

      if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK
          || glyph == 0 || glyph >= count || !m->glyphs[glyph].present) {
        continue;
      }
      if (used == capacity) {
        Pair * bigger = m->allocator->calloc_fn(m->allocator->ctx, capacity * 2,
            sizeof *bigger);

        if (!bigger) {
          return fail(m, GFNT_ERR_OOM, "growing the encoding");
        }
        memcpy(bigger, m->pairs, used * sizeof *bigger);
        m->allocator->free_fn(m->allocator->ctx, m->pairs);
        m->pairs = bigger;
        capacity *= 2;
      }
      m->pairs[used].codepoint = cp;
      m->pairs[used].glyph = glyph;
      ++used;
    }
    m->pair_count = used;
  }
  if (m->pair_count > 1) {
    qsort(m->pairs, m->pair_count, sizeof *m->pairs, by_codepoint);
  }
  for (i = 0; i < m->pair_count; ++i) {
    if (m->pairs[i].glyph >= count) {
      return fail(m, GFNT_ERR_CORRUPT, "an encoding names a glyph that is not there");
    }
  }
  return GFNT_OK;
}

static void put_le32(GFNT_WBuf * b, uint32_t v) {
  gfnt_wbuf_u8(b, v & 0xFF);
  gfnt_wbuf_u8(b, (v >> 8) & 0xFF);
  gfnt_wbuf_u8(b, (v >> 16) & 0xFF);
  gfnt_wbuf_u8(b, (v >> 24) & 0xFF);
}

static void text(GFNT_WBuf * b, const char * format, ...) {
  char line[512];
  va_list ap;
  int n;

  va_start(ap, format);
  n = vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  if (n > 0) {
    gfnt_wbuf_bytes(b, (const uint8_t *)line, (size_t)n < sizeof line ? (size_t)n
                                                                      : sizeof line - 1);
  }
}

static const char * glyph_name(const Model * m, size_t i, char * scratch,
    size_t size) {
  if (m->glyphs[i].name && m->glyphs[i].name[0]
      && !strpbrk(m->glyphs[i].name, " \t\r\n")) {
    return m->glyphs[i].name;
  }
  snprintf(scratch, size, "glyph%zu", i);
  return scratch;
}

/** The first codepoint that names @p glyph, or -1; @p more counts the others. */
static int64_t first_codepoint(const Model * m, size_t glyph) {
  size_t i;

  for (i = 0; i < m->pair_count; ++i) {
    if (m->pairs[i].glyph == glyph) {
      return m->pairs[i].codepoint;
    }
  }
  return -1;
}

/* ---- BDF ------------------------------------------------------------------ */

static void write_bdf(const Model * m, GFNT_WBuf * b) {
  int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  bool first = true;
  size_t i, k, chars = 0;
  char scratch[32];

  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];

    if (!g->present) {
      continue;
    }
    chars += 1;
    for (k = 0; k < m->pair_count; ++k) {
      if (m->pairs[k].glyph == i && m->pairs[k].codepoint != (uint32_t)first_codepoint(m, i)) {
        chars += 1;
      }
    }
    if (g->width && g->height) {
      int32_t x0 = g->bearing_x, x1 = g->bearing_x + (int32_t)g->width;
      int32_t y0 = g->bearing_y - (int32_t)g->height, y1 = g->bearing_y;

      if (first || x0 < min_x) min_x = x0;
      if (first || y0 < min_y) min_y = y0;
      if (first || x1 > max_x) max_x = x1;
      if (first || y1 > max_y) max_y = y1;
      first = false;
    }
  }
  text(b, "STARTFONT 2.1\n");
  if (m->states_encoding) {
    text(b, "FONT -Ghoti-Bitmap-Medium-R-Normal--%u-%u-75-75-C-%u-ISO10646-1\n",
        m->px, m->px * 10, m->px * 5);
  }
  else {
    text(b, "FONT bitmap\n");
  }
  text(b, "SIZE %u 75 75\n", m->px);
  text(b, "FONTBOUNDINGBOX %d %d %d %d\n", max_x - min_x, max_y - min_y, min_x, min_y);
  text(b, "STARTPROPERTIES 3\nFONT_ASCENT %d\nFONT_DESCENT %d\nPIXEL_SIZE %u\n"
      "ENDPROPERTIES\n", m->strike.ascent, -m->strike.descent, m->px);
  text(b, "CHARS %zu\n", chars);
  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];
    int64_t own = first_codepoint(m, i);
    size_t emitted = 0;

    if (!g->present) {
      continue;
    }
    for (k = 0; k <= m->pair_count; ++k) {
      int64_t encoding;
      size_t row;

      if (k < m->pair_count) {
        if (m->pairs[k].glyph != i) {
          continue;
        }
        encoding = m->pairs[k].codepoint;
      }
      else if (emitted == 0) {
        encoding = -1;
      }
      else {
        break;
      }
      text(b, "STARTCHAR %s\n", glyph_name(m, i, scratch, sizeof scratch));
      text(b, "ENCODING %lld\n", (long long)encoding);
      text(b, "SWIDTH %d 0\nDWIDTH %d 0\n",
          m->px ? (int)((int64_t)g->advance * 1000 / (int64_t)m->px) : 0, g->advance);
      if (g->width && g->height) {
        text(b, "BBX %u %u %d %d\n", g->width, g->height, g->bearing_x,
            g->bearing_y - (int32_t)g->height);
      }
      else {
        text(b, "BBX 0 0 0 0\n");
      }
      text(b, "BITMAP\n");
      if (g->width && g->height) {
        for (row = 0; row < g->height; ++row) {
          size_t byte;

          for (byte = 0; byte < g->stride; ++byte) {
            text(b, "%02X", g->bits[row * g->stride + byte]);
          }
          text(b, "\n");
        }
      }
      text(b, "ENDCHAR\n");
      ++emitted;
      (void)own;
    }
  }
  text(b, "ENDFONT\n");
}

/* ---- PCF ------------------------------------------------------------------ */

#define PCF_FORMAT 0x0000000Eu /* default layout, MSB bytes, MSB bits, rows padded to 4 */

typedef struct Toc {
  uint32_t type;
  GFNT_WBuf body;
} Toc;

static void be_i32(GFNT_WBuf * b, int32_t v) {
  gfnt_wbuf_u32(b, (uint32_t)v);
}

static void pcf_metric(GFNT_WBuf * b, int32_t lsb, int32_t rsb, int32_t width,
    int32_t ascent, int32_t descent) {
  gfnt_wbuf_u16(b, (uint32_t)lsb & 0xFFFFu);
  gfnt_wbuf_u16(b, (uint32_t)rsb & 0xFFFFu);
  gfnt_wbuf_u16(b, (uint32_t)width & 0xFFFFu);
  gfnt_wbuf_u16(b, (uint32_t)ascent & 0xFFFFu);
  gfnt_wbuf_u16(b, (uint32_t)descent & 0xFFFFu);
  gfnt_wbuf_u16(b, 0);
}

static GFNT_Result write_pcf(const Model * m, GFNT_WBuf * out) {
  Toc tables[8];
  size_t table_count = 0;
  size_t i, k;
  int32_t min_lsb = 0, min_rsb = 0, min_w = 0, min_a = 0, min_d = 0;
  int32_t max_lsb = 0, max_rsb = 0, max_w = 0, max_a = 0, max_d = 0;
  bool first = true;
  uint32_t min_byte2 = 0xFFFFFFFFu, max_byte2 = 0, min_byte1 = 0xFFFFFFFFu,
      max_byte1 = 0;
  size_t strings_length = 0;
  GFNT_WBuf * t;

  for (i = 0; i < 8; ++i) {
    gfnt_wbuf_init(&tables[i].body, m->allocator);
  }
  for (i = 0; i < m->pair_count; ++i) {
    if (m->pairs[i].codepoint > 0xFFFFu) {
      for (i = 0; i < 8; ++i) {
        gfnt_wbuf_free(&tables[i].body);
      }
      return fail(m, GFNT_ERR_UNSUPPORTED,
          "a PCF encodes two bytes; a codepoint above U+FFFF cannot be written");
    }
    min_byte1 = m->pairs[i].codepoint >> 8 < min_byte1 ? m->pairs[i].codepoint >> 8 : min_byte1;
    max_byte1 = m->pairs[i].codepoint >> 8 > max_byte1 ? m->pairs[i].codepoint >> 8 : max_byte1;
    min_byte2 = (m->pairs[i].codepoint & 0xFF) < min_byte2 ? (m->pairs[i].codepoint & 0xFF) : min_byte2;
    max_byte2 = (m->pairs[i].codepoint & 0xFF) > max_byte2 ? (m->pairs[i].codepoint & 0xFF) : max_byte2;
  }
  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];
    int32_t lsb = g->present ? g->bearing_x : 0;
    int32_t rsb = g->present ? g->bearing_x + (int32_t)g->width : 0;
    int32_t width = g->present ? g->advance : 0;
    int32_t ascent = g->present && g->height ? g->bearing_y : 0;
    int32_t descent = g->present && g->height ? (int32_t)g->height - g->bearing_y : 0;

    if (first || lsb < min_lsb) min_lsb = lsb;
    if (first || rsb < min_rsb) min_rsb = rsb;
    if (first || width < min_w) min_w = width;
    if (first || ascent < min_a) min_a = ascent;
    if (first || descent < min_d) min_d = descent;
    if (first || lsb > max_lsb) max_lsb = lsb;
    if (first || rsb > max_rsb) max_rsb = rsb;
    if (first || width > max_w) max_w = width;
    if (first || ascent > max_a) max_a = ascent;
    if (first || descent > max_d) max_d = descent;
    first = false;
  }

  /* Properties: the baseline and the size, as integers, and a name. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x1;
  put_le32(t, PCF_FORMAT);
  be_i32(t, 3);
  {
    static const char names[] = "FONT_ASCENT\0FONT_DESCENT\0PIXEL_SIZE";
    int32_t offsets[3] = {0, 12, 25};
    int32_t values[3];

    values[0] = m->strike.ascent;
    values[1] = -m->strike.descent;
    values[2] = (int32_t)m->px;
    for (k = 0; k < 3; ++k) {
      be_i32(t, offsets[k]);
      gfnt_wbuf_u8(t, 0);
      be_i32(t, values[k]);
    }
    gfnt_wbuf_align(t, 4);
    strings_length = sizeof names; /* includes the final NUL */
    be_i32(t, (int32_t)strings_length);
    gfnt_wbuf_bytes(t, (const uint8_t *)names, strings_length);
  }
  /* Accelerators: the baseline, and the extremes. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x100;
  put_le32(t, PCF_FORMAT);
  gfnt_wbuf_u8(t, 0);                    /* noOverlap */
  gfnt_wbuf_u8(t, min_w == max_w);       /* constantMetrics */
  gfnt_wbuf_u8(t, 0);                    /* terminalFont */
  gfnt_wbuf_u8(t, min_w == max_w);       /* constantWidth */
  gfnt_wbuf_u8(t, 1);                    /* inkInside */
  gfnt_wbuf_u8(t, 0);                    /* inkMetrics */
  gfnt_wbuf_u8(t, 0);                    /* drawDirection */
  gfnt_wbuf_u8(t, 0);
  be_i32(t, m->strike.ascent);
  be_i32(t, -m->strike.descent);
  be_i32(t, 0);
  pcf_metric(t, min_lsb, min_rsb, min_w, min_a, min_d);
  pcf_metric(t, max_lsb, max_rsb, max_w, max_a, max_d);
  /* Metrics, uncompressed. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x4;
  put_le32(t, PCF_FORMAT);
  be_i32(t, (int32_t)m->count);
  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];

    if (!g->present) {
      pcf_metric(t, 0, 0, 0, 0, 0);
    }
    else {
      pcf_metric(t, g->bearing_x, g->bearing_x + (int32_t)g->width, g->advance,
          g->height ? g->bearing_y : 0,
          g->height ? (int32_t)g->height - g->bearing_y : 0);
    }
  }
  /* Bitmaps: rows padded to four bytes. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x8;
  put_le32(t, PCF_FORMAT);
  be_i32(t, (int32_t)m->count);
  {
    size_t offset = 0;
    size_t sizes[4] = {0, 0, 0, 0};
    static const size_t pads[4] = {1, 2, 4, 8};

    for (i = 0; i < m->count; ++i) {
      const BGlyph * g = &m->glyphs[i];
      size_t stride4 = ((g->stride + 3u) / 4u) * 4u;

      be_i32(t, (int32_t)offset);
      if (g->present && g->height && g->width) {
        offset += stride4 * g->height;
        for (k = 0; k < 4; ++k) {
          sizes[k] += ((g->stride + pads[k] - 1u) / pads[k]) * pads[k] * g->height;
        }
      }
    }
    for (k = 0; k < 4; ++k) {
      be_i32(t, (int32_t)sizes[k]);
    }
    for (i = 0; i < m->count; ++i) {
      const BGlyph * g = &m->glyphs[i];
      size_t stride4 = ((g->stride + 3u) / 4u) * 4u;
      size_t row;

      if (!(g->present && g->height && g->width)) {
        continue;
      }
      for (row = 0; row < g->height; ++row) {
        gfnt_wbuf_bytes(t, g->bits + row * g->stride, g->stride);
        for (k = g->stride; k < stride4; ++k) {
          gfnt_wbuf_u8(t, 0);
        }
      }
    }
  }
  /* Encodings. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x20;
  put_le32(t, PCF_FORMAT);
  if (m->pair_count == 0) {
    gfnt_wbuf_u16(t, 0);
    gfnt_wbuf_u16(t, 0);
    gfnt_wbuf_u16(t, 0);
    gfnt_wbuf_u16(t, 0);
    gfnt_wbuf_u16(t, 0);
    gfnt_wbuf_u16(t, 0xFFFFu);
  }
  else {
    uint32_t b1, b2;

    gfnt_wbuf_u16(t, min_byte2);
    gfnt_wbuf_u16(t, max_byte2);
    gfnt_wbuf_u16(t, min_byte1);
    gfnt_wbuf_u16(t, max_byte1);
    gfnt_wbuf_u16(t, 0);
    for (b1 = min_byte1; b1 <= max_byte1; ++b1) {
      for (b2 = min_byte2; b2 <= max_byte2; ++b2) {
        uint32_t code = (b1 << 8) | b2;
        uint32_t found = 0xFFFFu;

        for (k = 0; k < m->pair_count; ++k) {
          if (m->pairs[k].codepoint == code) {
            found = m->pairs[k].glyph;
            break;
          }
        }
        gfnt_wbuf_u16(t, found);
      }
    }
  }
  /* Scalable widths. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x40;
  put_le32(t, PCF_FORMAT);
  be_i32(t, (int32_t)m->count);
  for (i = 0; i < m->count; ++i) {
    be_i32(t, m->px ? (int32_t)((int64_t)m->glyphs[i].advance * 1000 / (int64_t)m->px) : 0);
  }
  /* Glyph names. */
  t = &tables[table_count].body;
  tables[table_count++].type = 0x80;
  put_le32(t, PCF_FORMAT);
  be_i32(t, (int32_t)m->count);
  {
    GFNT_WBuf strings;
    char scratch[32];

    gfnt_wbuf_init(&strings, m->allocator);
    for (i = 0; i < m->count; ++i) {
      const char * name = glyph_name(m, i, scratch, sizeof scratch);

      be_i32(t, (int32_t)strings.length);
      gfnt_wbuf_bytes(&strings, (const uint8_t *)name, strlen(name) + 1);
    }
    be_i32(t, (int32_t)strings.length);
    gfnt_wbuf_bytes(t, strings.data, strings.length);
    gfnt_wbuf_free(&strings);
  }

  /* The file: signature, table of contents, then each table on a four-byte line. */
  put_le32(out, 0x70636601u);
  put_le32(out, (uint32_t)table_count);
  {
    size_t offset = 8u + 16u * table_count;

    for (i = 0; i < table_count; ++i) {
      gfnt_wbuf_align(&tables[i].body, 4);
      put_le32(out, tables[i].type);
      put_le32(out, PCF_FORMAT);
      put_le32(out, (uint32_t)tables[i].body.length);
      put_le32(out, (uint32_t)offset);
      offset += tables[i].body.length;
    }
  }
  for (i = 0; i < table_count; ++i) {
    gfnt_wbuf_bytes(out, tables[i].body.data, tables[i].body.length);
  }
  for (i = 0; i < 8; ++i) {
    if (tables[i].body.oom) {
      out->oom = true;
    }
    gfnt_wbuf_free(&tables[i].body);
  }
  return GFNT_OK;
}

/* ---- Cells: PSF and .hex --------------------------------------------------- */

/**
 * Every glyph placed in a cell of @p cell_width by @p cell_height, rows padded to
 * the byte. Returns false (with a message) when a glyph does not fit or the
 * advances differ, which a fixed cell cannot say.
 */
static GFNT_Result make_cells(const Model * m, uint32_t cell_width,
    uint32_t cell_height, uint8_t * cells, size_t cell_bytes, bool uniform) {
  size_t stride = (cell_width + 7u) / 8u;
  size_t i, row, col;

  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];
    int32_t top;

    if (!g->present || !g->width || !g->height) {
      continue;
    }
    if (uniform && g->advance != (int32_t)cell_width) {
      return fail(m, GFNT_ERR_UNSUPPORTED,
          "this format has one cell size; the strike's advances differ");
    }
    top = m->strike.ascent - g->bearing_y;
    if (g->bearing_x < 0 || top < 0
        || (uint32_t)g->bearing_x + g->width > cell_width
        || (uint32_t)top + g->height > cell_height) {
      return fail(m, GFNT_ERR_UNSUPPORTED,
          "a glyph does not fit the cell this format would give it");
    }
    for (row = 0; row < g->height; ++row) {
      for (col = 0; col < g->width; ++col) {
        if (g->bits[row * g->stride + col / 8u] & (0x80u >> (col % 8u))) {
          size_t x = (size_t)g->bearing_x + col;
          size_t y = (size_t)top + row;

          cells[i * cell_bytes + y * stride + x / 8u] |= (uint8_t)(0x80u >> (x % 8u));
        }
      }
    }
  }
  return GFNT_OK;
}

/** The advance every present glyph shares, or 0 when they differ or none exists. */
static uint32_t common_advance(const Model * m) {
  uint32_t advance = 0;
  size_t i;

  for (i = 0; i < m->count; ++i) {
    if (!m->glyphs[i].present || m->glyphs[i].advance <= 0) {
      continue;
    }
    if (advance == 0) {
      advance = (uint32_t)m->glyphs[i].advance;
    }
    else if (advance != (uint32_t)m->glyphs[i].advance) {
      return 0;
    }
  }
  return advance;
}

static void put_utf8(GFNT_WBuf * b, uint32_t c) {
  if (c < 0x80) {
    gfnt_wbuf_u8(b, c);
  }
  else if (c < 0x800) {
    gfnt_wbuf_u8(b, 0xC0 | (c >> 6));
    gfnt_wbuf_u8(b, 0x80 | (c & 0x3F));
  }
  else if (c < 0x10000) {
    gfnt_wbuf_u8(b, 0xE0 | (c >> 12));
    gfnt_wbuf_u8(b, 0x80 | ((c >> 6) & 0x3F));
    gfnt_wbuf_u8(b, 0x80 | (c & 0x3F));
  }
  else {
    gfnt_wbuf_u8(b, 0xF0 | (c >> 18));
    gfnt_wbuf_u8(b, 0x80 | ((c >> 12) & 0x3F));
    gfnt_wbuf_u8(b, 0x80 | ((c >> 6) & 0x3F));
    gfnt_wbuf_u8(b, 0x80 | (c & 0x3F));
  }
}

static GFNT_Result write_psf(const Model * m, GFNT_WBuf * b, bool version2) {
  uint32_t width = common_advance(m);
  uint32_t height = (uint32_t)(m->strike.ascent - m->strike.descent);
  size_t glyphs = m->count;
  size_t stride, cell_bytes, i, k;
  uint8_t * cells;
  GFNT_Result result;
  bool table = m->states_encoding && m->pair_count > 0;

  if (width == 0 || height == 0 || height > 255u * (version2 ? 1000u : 1u)) {
    return fail(m, GFNT_ERR_UNSUPPORTED,
        "a PSF needs one advance for every glyph and a cell height");
  }
  if (!version2) {
    if (width != 8 || height > 255u) {
      return fail(m, GFNT_ERR_UNSUPPORTED, "a PSF 1 cell is 8 pixels wide");
    }
    if (glyphs > 512u) {
      return fail(m, GFNT_ERR_UNSUPPORTED, "a PSF 1 holds 256 or 512 glyphs");
    }
    glyphs = glyphs <= 256u ? 256u : 512u;
  }
  stride = (width + 7u) / 8u;
  cell_bytes = stride * height;
  cells = m->allocator->calloc_fn(m->allocator->ctx, glyphs * cell_bytes, 1);
  if (!cells) {
    return fail(m, GFNT_ERR_OOM, "allocating the cells");
  }
  result = make_cells(m, width, height, cells, cell_bytes, true);
  if (result != GFNT_OK) {
    m->allocator->free_fn(m->allocator->ctx, cells);
    return result;
  }
  if (version2) {
    put_le32(b, 0x864AB572u);
    put_le32(b, 0);
    put_le32(b, 32);
    put_le32(b, table ? 1u : 0u);
    put_le32(b, (uint32_t)glyphs);
    put_le32(b, (uint32_t)cell_bytes);
    put_le32(b, height);
    put_le32(b, width);
  }
  else {
    gfnt_wbuf_u8(b, 0x36);
    gfnt_wbuf_u8(b, 0x04);
    gfnt_wbuf_u8(b, (glyphs == 512u ? 1u : 0u) | (table ? 2u : 0u));
    gfnt_wbuf_u8(b, height);
  }
  gfnt_wbuf_bytes(b, cells, glyphs * cell_bytes);
  m->allocator->free_fn(m->allocator->ctx, cells);
  if (table) {
    for (i = 0; i < glyphs; ++i) {
      for (k = 0; k < m->pair_count; ++k) {
        if (m->pairs[k].glyph != i) {
          continue;
        }
        if (version2) {
          put_utf8(b, m->pairs[k].codepoint);
        }
        else if (m->pairs[k].codepoint > 0xFFFEu) {
          return fail(m, GFNT_ERR_UNSUPPORTED, "a PSF 1 table holds UCS-2 only");
        }
        else {
          gfnt_wbuf_u8(b, m->pairs[k].codepoint & 0xFF);
          gfnt_wbuf_u8(b, m->pairs[k].codepoint >> 8);
        }
      }
      if (version2) {
        gfnt_wbuf_u8(b, 0xFF);
      }
      else {
        gfnt_wbuf_u8(b, 0xFF);
        gfnt_wbuf_u8(b, 0xFF);
      }
    }
  }
  return GFNT_OK;
}

static GFNT_Result write_hex(const Model * m, GFNT_WBuf * b) {
  size_t i;
  uint32_t cell_width;
  size_t stride, cell_bytes;
  uint8_t * cells;
  GFNT_Result result;

  if ((m->strike.ascent - m->strike.descent) != 16 || m->pair_count == 0) {
    return fail(m, GFNT_ERR_UNSUPPORTED,
        ".hex holds 16-row glyphs keyed by codepoint; this strike is not that");
  }
  /* The widest cell any glyph needs, to the byte; one width for the file. */
  cell_width = 8;
  for (i = 0; i < m->count; ++i) {
    const BGlyph * g = &m->glyphs[i];
    uint32_t need;

    if (!g->present) continue;
    need = (uint32_t)(g->advance > 0 ? g->advance : 0);
    if (g->bearing_x >= 0 && (uint32_t)g->bearing_x + g->width > need) {
      need = (uint32_t)g->bearing_x + g->width;
    }
    while (cell_width < need) cell_width += 8;
  }
  if (cell_width > 32) {
    return fail(m, GFNT_ERR_UNSUPPORTED, ".hex glyphs are at most 32 pixels wide");
  }
  stride = (cell_width + 7u) / 8u;
  cell_bytes = stride * 16u;
  cells = m->allocator->calloc_fn(m->allocator->ctx, m->count * cell_bytes, 1);
  if (!cells) {
    return fail(m, GFNT_ERR_OOM, "allocating the cells");
  }
  result = make_cells(m, cell_width, 16, cells, cell_bytes, false);
  if (result == GFNT_OK) {
    for (i = 0; i < m->pair_count; ++i) {
      size_t g = m->pairs[i].glyph;
      size_t k;
      uint32_t cp = m->pairs[i].codepoint;
      uint32_t row_bytes = 1;
      /* Each glyph is as wide as its own advance rounds up to, per the format. */
      uint32_t w = (uint32_t)(m->glyphs[g].advance > 0 ? m->glyphs[g].advance : 8);

      if (!m->glyphs[g].present) continue;
      row_bytes = (w + 7u) / 8u;
      if (row_bytes < 1) row_bytes = 1;
      if (row_bytes > stride) row_bytes = (uint32_t)stride;
      text(b, cp > 0xFFFFu ? "%06X:" : "%04X:", cp);
      for (k = 0; k < 16; ++k) {
        size_t byte;

        for (byte = 0; byte < row_bytes; ++byte) {
          text(b, "%02X", cells[g * cell_bytes + k * stride + byte]);
        }
      }
      text(b, "\n");
    }
  }
  m->allocator->free_fn(m->allocator->ctx, cells);
  return result;
}

GFNT_Result gfnt_write_bitmap_font(const GFNT_Face * face, size_t strike,
    GFNT_BitmapFormat format, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits defaults;
  Model m;
  GFNT_WBuf b;
  GFNT_Result result;

  if (!face || !out_blob || (int)format < 0 || format >= GFNT_BITMAP_FORMAT_COUNT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, nowhere to put the font, or a format that is not one");
  }
  if (!limits) {
    gfnt_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  memset(&m, 0, sizeof m);
  m.allocator = allocator;
  m.error = error;
  gfnt_wbuf_init(&b, allocator);
  result = model_build(&m, face, strike);
  if (result == GFNT_OK) {
    switch (format) {
      case GFNT_BITMAP_FORMAT_BDF:
        write_bdf(&m, &b);
        break;
      case GFNT_BITMAP_FORMAT_PCF:
        result = write_pcf(&m, &b);
        break;
      case GFNT_BITMAP_FORMAT_PSF1:
        result = write_psf(&m, &b, false);
        break;
      case GFNT_BITMAP_FORMAT_PSF2:
        result = write_psf(&m, &b, true);
        break;
      case GFNT_BITMAP_FORMAT_HEX:
        result = write_hex(&m, &b);
        break;
      default:
        break;
    }
  }
  if (result == GFNT_OK && b.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "building the font");
  }
  if (result == GFNT_OK) {
    result = gfnt_blob_create_memory(b.data, b.length, GFNT_BLOB_COPY, limits,
        allocator, out_blob, error);
  }
  gfnt_wbuf_free(&b);
  model_free(&m);
  return result;
}
