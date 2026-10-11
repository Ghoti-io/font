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
#include "../write/write.h"
#include "woff2.h"

#define GFNT_TAG_WOFF2 GFNT_TAG('W', 'O', 'F', '2')
#define GFNT_TAG_GLYF GFNT_TAG('g', 'l', 'y', 'f')
#define GFNT_TAG_LOCA GFNT_TAG('l', 'o', 'c', 'a')
#define GFNT_TAG_HMTX GFNT_TAG('h', 'm', 't', 'x')
#define GFNT_TAG_HHEA GFNT_TAG('h', 'h', 'e', 'a')
#define GFNT_TAG_HEAD GFNT_TAG('h', 'e', 'a', 'd')
#define GFNT_TAG_TTCF GFNT_TAG('t', 't', 'c', 'f')

#define GFNT_WOFF2_HEADER_BYTES 48u
#define GFNT_WOFF2_GLYF_HEADER_BYTES 36u

/** The 63 tags a directory entry can name by index (the specification's table). */
static const char gfnt_woff2_known[63][5] = {
  "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ",
  "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp",
  "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF",
  "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL",
  "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc",
  "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx",
  "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill",
};

/** One directory entry, and what becomes of its table. */
typedef struct GFNT_W2Entry {
  uint32_t tag;
  uint8_t version;        ///< Transform version from the flags byte.
  bool transformed;       ///< A transform other than the null one applies.
  uint32_t orig_length;
  uint32_t transform_length;
  size_t stream_length;   ///< Bytes it occupies in the decompressed stream.
  const uint8_t * data;   ///< Those bytes, once decompressed.
  uint8_t * owned;        ///< The reconstructed table, when there is one.
  const uint8_t * out;    ///< What goes in the sfnt.
  size_t out_length;
  int16_t * xmin;         ///< `glyf` only: each glyph's xMin, for `hmtx`.
  uint32_t num_glyphs;    ///< `glyf` only.
  struct GFNT_W2Entry * paired; ///< `loca` only: the glyf it was rebuilt with.
  uint32_t checksum;
  size_t offset;          ///< Where it lands in the rebuilt font.
} GFNT_W2Entry;

/** One font of a collection (or the only font): its flavour and its tables. */
typedef struct GFNT_W2Font {
  uint32_t flavor;
  size_t count;
  uint16_t * index;       ///< Entries, as positions in the directory.
  size_t offset;          ///< Where its offset table lands.
} GFNT_W2Font;

typedef struct GFNT_W2 {
  const GFNT_Limits * limits;
  const GFNT_Allocator * allocator;
  GFNT_Error * error;
  const uint8_t * bytes;
  size_t size;
  GFNT_W2Entry * entries;
  size_t entry_count;
  GFNT_W2Font * fonts;
  size_t font_count;
  bool collection;
  uint32_t ttc_version;
  uint32_t flavor;
  size_t compressed_offset;
  size_t compressed_length;
  uint8_t * stream;       ///< The decompressed table data.
  size_t stream_length;
} GFNT_W2;

/** A cursor over a byte range. A read past the end sets @c bad and returns 0. */
typedef struct GFNT_W2Cur {
  const uint8_t * p;
  size_t n;
  size_t pos;
  bool bad;
} GFNT_W2Cur;

static GFNT_Result gfnt_w2_fail(const GFNT_W2 * w, GFNT_Result code, uint32_t tag,
    size_t offset, const char * message) {
  return gfnt_error_set(w->error, code, tag, offset, GFNT_GLYPH_NONE, message);
}

static uint32_t gfnt_w2_u8(GFNT_W2Cur * c) {
  if (c->pos >= c->n) {
    c->bad = true;
    return 0;
  }
  return c->p[c->pos++];
}

static uint32_t gfnt_w2_u16(GFNT_W2Cur * c) {
  uint32_t hi = gfnt_w2_u8(c);
  return (hi << 8) | gfnt_w2_u8(c);
}

static uint32_t gfnt_w2_u32(GFNT_W2Cur * c) {
  uint32_t hi = gfnt_w2_u16(c);
  return (hi << 16) | gfnt_w2_u16(c);
}

static const uint8_t * gfnt_w2_take(GFNT_W2Cur * c, size_t count) {
  const uint8_t * at = c->p + c->pos;

  if (c->bad || count > c->n - c->pos) {
    c->bad = true;
    return NULL;
  }
  c->pos += count;
  return at;
}

/** UIntBase128: at most five bytes, no leading zero, no overflow of 32 bits. */
static uint32_t gfnt_w2_base128(GFNT_W2Cur * c) {
  uint32_t accum = 0;
  unsigned i;

  for (i = 0; i < 5; ++i) {
    uint32_t b = gfnt_w2_u8(c);

    if (c->bad || (i == 0 && b == 0x80) || (accum & 0xFE000000u)) {
      c->bad = true;
      return 0;
    }
    accum = (accum << 7) | (b & 0x7F);
    if (!(b & 0x80)) {
      return accum;
    }
  }
  c->bad = true;
  return 0;
}

/** 255UInt16, in any of the encodings the format allows for a value. */
static uint32_t gfnt_w2_255u16(GFNT_W2Cur * c) {
  uint32_t code = gfnt_w2_u8(c);

  if (code == 253) {
    return gfnt_w2_u16(c);
  }
  if (code == 255) {
    return 253u + gfnt_w2_u8(c);
  }
  if (code == 254) {
    return 506u + gfnt_w2_u8(c);
  }
  return code;
}

static size_t gfnt_w2_pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

static void gfnt_w2_free(GFNT_W2 * w) {
  size_t i;

  for (i = 0; i < w->entry_count; ++i) {
    w->allocator->free_fn(w->allocator->ctx, w->entries[i].owned);
    w->allocator->free_fn(w->allocator->ctx, w->entries[i].xmin);
  }
  for (i = 0; i < w->font_count; ++i) {
    w->allocator->free_fn(w->allocator->ctx, w->fonts[i].index);
  }
  w->allocator->free_fn(w->allocator->ctx, w->entries);
  w->allocator->free_fn(w->allocator->ctx, w->fonts);
  if (w->stream) {
    gcomp_buffer_free(NULL, w->stream);
  }
}

bool gfnt_woff2_looks_like(const GFNT_Reader * blob) {
  uint32_t signature = 0;

  return gfnt_reader_u32_at(blob, 0, &signature) == GFNT_OK
      && signature == GFNT_WOFF2_SIGNATURE;
}

/**
 * The header, the table directory and (for a collection) the collection
 * directory. Everything that needs no decompression.
 */
static GFNT_Result gfnt_w2_parse(GFNT_W2 * w) {
  GFNT_W2Cur c = {w->bytes, w->size, 0, false};
  uint32_t signature, length, total_compressed, meta_off, meta_len, priv_off,
      priv_len;
  uint32_t reserved;
  uint32_t count;
  uint64_t stream_total = 0;
  size_t i;

  signature = gfnt_w2_u32(&c);
  w->flavor = gfnt_w2_u32(&c);
  length = gfnt_w2_u32(&c);
  count = gfnt_w2_u16(&c);
  reserved = gfnt_w2_u16(&c);
  (void)gfnt_w2_u32(&c); // totalSfntSize: a reference value, never enforced.
  total_compressed = gfnt_w2_u32(&c);
  (void)gfnt_w2_u32(&c); // major and minor version.
  meta_off = gfnt_w2_u32(&c);
  meta_len = gfnt_w2_u32(&c);
  (void)gfnt_w2_u32(&c); // metaOrigLength.
  priv_off = gfnt_w2_u32(&c);
  priv_len = gfnt_w2_u32(&c);
  if (c.bad || signature != GFNT_WOFF2_SIGNATURE) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 0,
        "too short to hold a WOFF 2 header");
  }
  if (reserved != 0) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 14,
        "the WOFF 2 header's reserved field is not zero");
  }
  if (length > w->size) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 8,
        "the WOFF 2 header states more bytes than the file has");
  }
  w->size = length;
  c.n = length;
  if (count == 0) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 12,
        "a WOFF 2 font with no tables");
  }
  if (count > w->limits->max_tables) {
    return gfnt_w2_fail(w, GFNT_ERR_LIMIT, GFNT_TAG_WOFF2, 12,
        "more tables than the limit allows");
  }
  w->entries = w->allocator->calloc_fn(w->allocator->ctx, count,
      sizeof *w->entries);
  if (!w->entries) {
    return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0,
        "allocating the table directory");
  }
  w->entry_count = count;

  for (i = 0; i < count; ++i) {
    GFNT_W2Entry * e = &w->entries[i];
    size_t at = c.pos;
    uint32_t flags = gfnt_w2_u8(&c);
    uint32_t known = flags & 0x3F;

    e->version = (uint8_t)(flags >> 6);
    if (known == 63) {
      e->tag = gfnt_w2_u32(&c);
    }
    else {
      const char * t = gfnt_woff2_known[known];

      e->tag = GFNT_TAG(t[0], t[1], t[2], t[3]);
    }
    e->orig_length = gfnt_w2_base128(&c);
    if (e->tag == GFNT_TAG_GLYF || e->tag == GFNT_TAG_LOCA) {
      if (e->version == 0) {
        e->transformed = true;
      }
      else if (e->version != 3) {
        return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, e->tag, at,
            "a glyf or loca table with a transform version the format lacks");
      }
    }
    else if (e->tag == GFNT_TAG_HMTX) {
      if (e->version == 1) {
        e->transformed = true;
      }
      else if (e->version != 0) {
        return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, e->tag, at,
            "an hmtx table with a transform version the format lacks");
      }
    }
    else if (e->version != 0) {
      return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, e->tag, at,
          "a transform version on a table the format transforms no way");
    }
    if (e->transformed) {
      e->transform_length = gfnt_w2_base128(&c);
    }
    if (c.bad) {
      return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, at,
          "a WOFF 2 directory entry is malformed or cut short");
    }
    e->stream_length = e->transformed ? e->transform_length : e->orig_length;
    if (e->transformed && e->tag == GFNT_TAG_LOCA && e->transform_length != 0) {
      return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, e->tag, at,
          "a transformed loca states a nonzero transform length");
    }
    stream_total += e->stream_length;
    if (e->orig_length > w->limits->max_blob_bytes
        || stream_total > w->limits->max_blob_bytes) {
      return gfnt_w2_fail(w, GFNT_ERR_LIMIT, e->tag, at,
          "the font would be larger than the limit allows");
    }
  }
  w->stream_length = (size_t)stream_total;

  // The fonts: one, or the collection directory's.
  if (w->flavor == GFNT_TAG_TTCF) {
    uint32_t fonts;

    w->collection = true;
    w->ttc_version = gfnt_w2_u32(&c);
    fonts = gfnt_w2_255u16(&c);
    if (c.bad || fonts == 0) {
      return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, c.pos,
          "a WOFF 2 collection with no fonts, or a cut-short collection directory");
    }
    w->fonts = w->allocator->calloc_fn(w->allocator->ctx, fonts, sizeof *w->fonts);
    if (!w->fonts) {
      return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0,
          "allocating the collection directory");
    }
    w->font_count = fonts;
    for (i = 0; i < fonts; ++i) {
      GFNT_W2Font * f = &w->fonts[i];
      uint32_t n = gfnt_w2_255u16(&c);
      size_t k;

      f->flavor = gfnt_w2_u32(&c);
      if (c.bad || n == 0 || n > count) {
        return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, c.pos,
            "a collection font with no tables, or more than the file has");
      }
      f->index = w->allocator->calloc_fn(w->allocator->ctx, n, sizeof *f->index);
      if (!f->index) {
        return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0,
            "allocating a collection font");
      }
      f->count = n;
      for (k = 0; k < n; ++k) {
        uint32_t at = gfnt_w2_255u16(&c);

        if (c.bad || at >= count) {
          return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, c.pos,
              "a collection font names a table the directory lacks");
        }
        f->index[k] = (uint16_t)at;
      }
    }
  }
  else {
    GFNT_W2Font * f;

    w->fonts = w->allocator->calloc_fn(w->allocator->ctx, 1, sizeof *w->fonts);
    if (!w->fonts) {
      return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0, "allocating a font");
    }
    w->font_count = 1;
    f = &w->fonts[0];
    f->flavor = w->flavor;
    f->index = w->allocator->calloc_fn(w->allocator->ctx, count,
        sizeof *f->index);
    if (!f->index) {
      return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0, "allocating a font");
    }
    f->count = count;
    for (i = 0; i < count; ++i) {
      f->index[i] = (uint16_t)i;
    }
  }

  // The blocks after the directory: the compressed tables, then the optional
  // metadata and private data, each inside the file and none overlapping.
  w->compressed_offset = c.pos;
  w->compressed_length = total_compressed;
  if ((uint64_t)w->compressed_offset + total_compressed > length) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 20,
        "the compressed tables run past the end of the file");
  }
  {
    uint64_t end = (uint64_t)w->compressed_offset + total_compressed;

    if (meta_len != 0 || meta_off != 0) {
      if (meta_off < end || (uint64_t)meta_off + meta_len > length) {
        return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 28,
            "the metadata block overlaps the tables or runs past the file");
      }
      end = (uint64_t)meta_off + meta_len;
    }
    if (priv_len != 0 || priv_off != 0) {
      if (priv_off < end || (uint64_t)priv_off + priv_len > length) {
        return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2, 40,
            "the private block overlaps an earlier block or runs past the file");
      }
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_woff2_face_count(const GFNT_Blob * in, const GFNT_Limits * limits,
    size_t * out_count, GFNT_Error * error) {
  GFNT_W2 w;
  GFNT_Result result;

  if (!in || !limits || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_WOFF2, 0,
        GFNT_GLYPH_NONE, "no input, no limits, or nowhere to put the count");
  }
  memset(&w, 0, sizeof w);
  w.limits = limits;
  w.allocator = gfnt_allocator_default();
  w.error = error;
  w.bytes = gfnt_blob_data(in);
  w.size = gfnt_blob_size(in);
  result = gfnt_w2_parse(&w);
  if (result == GFNT_OK) {
    *out_count = w.font_count;
  }
  gfnt_w2_free(&w);
  return result;
}

/** Brotli-decompress the tables into one stream of exactly the stated length. */
static GFNT_Result gfnt_w2_decompress(GFNT_W2 * w) {
  gcomp_options_t * options = NULL;
  gcomp_status_t status;
  void * decoded = NULL;
  size_t length = 0;
  size_t i;
  size_t at = 0;

  if (w->stream_length == 0) {
    return GFNT_OK;
  }
  if (gcomp_options_create(&options) != GCOMP_OK) {
    return gfnt_w2_fail(w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0,
        "allocating the options for a decompress");
  }
  // The directory says how long the stream is, so that is the ceiling.
  if (gcomp_options_set_uint64(options, "limits.max_output_bytes",
          (uint64_t)w->stream_length) != GCOMP_OK) {
    gcomp_options_destroy(options);
    return gfnt_w2_fail(w, GFNT_ERR_INTERNAL, GFNT_TAG_WOFF2, 0,
        "the compression library refused an option this library sets");
  }
  status = gcomp_decode_alloc(NULL, "brotli", options,
      w->bytes + w->compressed_offset, w->compressed_length,
      &decoded, &length);
  gcomp_options_destroy(options);
  w->stream = decoded;
  if (status != GCOMP_OK) {
    return gfnt_w2_fail(w,
        status == GCOMP_ERR_MEMORY ? GFNT_ERR_OOM
            : (status == GCOMP_ERR_LIMIT || status == GCOMP_ERR_CORRUPT
                  || status == GCOMP_ERR_UNSUPPORTED) ? GFNT_ERR_CORRUPT
            : GFNT_ERR_INTERNAL,
        GFNT_TAG_WOFF2, w->compressed_offset, gcomp_status_to_string(status));
  }
  if (length != w->stream_length) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, GFNT_TAG_WOFF2,
        w->compressed_offset,
        "the tables decompressed to a different length than the directory states");
  }
  for (i = 0; i < w->entry_count; ++i) {
    w->entries[i].data = w->stream + at;
    at += w->entries[i].stream_length;
  }
  return GFNT_OK;
}

/** Sign by the low bit of the flag, as the triplet table does. */
static int32_t gfnt_w2_signed(uint32_t flag, uint32_t magnitude) {
  return (flag & 1u) ? (int32_t)magnitude : -(int32_t)magnitude;
}

/** Decode one triplet from @p glyph: the coordinate pair and the byte count. */
static void gfnt_w2_triplet(GFNT_W2Cur * glyph, uint32_t flag, int32_t * dx,
    int32_t * dy) {
  if (flag < 10) {
    uint32_t b0 = gfnt_w2_u8(glyph);

    *dx = 0;
    *dy = gfnt_w2_signed(flag, ((flag & 14u) << 7) + b0);
  }
  else if (flag < 20) {
    uint32_t b0 = gfnt_w2_u8(glyph);

    *dx = gfnt_w2_signed(flag, (((flag - 10u) & 14u) << 7) + b0);
    *dy = 0;
  }
  else if (flag < 84) {
    uint32_t base = flag - 20u;
    uint32_t b0 = gfnt_w2_u8(glyph);

    *dx = gfnt_w2_signed(flag, 1u + (base & 0x30u) + (b0 >> 4));
    *dy = gfnt_w2_signed(flag >> 1, 1u + ((base & 0x0Cu) << 2) + (b0 & 15u));
  }
  else if (flag < 120) {
    uint32_t base = flag - 84u;
    uint32_t b0 = gfnt_w2_u8(glyph);
    uint32_t b1 = gfnt_w2_u8(glyph);

    *dx = gfnt_w2_signed(flag, 1u + ((base / 12u) << 8) + b0);
    *dy = gfnt_w2_signed(flag >> 1, 1u + (((base % 12u) >> 2) << 8) + b1);
  }
  else if (flag < 124) {
    uint32_t b0 = gfnt_w2_u8(glyph);
    uint32_t b1 = gfnt_w2_u8(glyph);
    uint32_t b2 = gfnt_w2_u8(glyph);

    *dx = gfnt_w2_signed(flag, (b0 << 4) + (b1 >> 4));
    *dy = gfnt_w2_signed(flag >> 1, ((b1 & 15u) << 8) + b2);
  }
  else {
    uint32_t b0 = gfnt_w2_u8(glyph);
    uint32_t b1 = gfnt_w2_u8(glyph);
    uint32_t b2 = gfnt_w2_u8(glyph);
    uint32_t b3 = gfnt_w2_u8(glyph);

    *dx = gfnt_w2_signed(flag, (b0 << 8) + b1);
    *dy = gfnt_w2_signed(flag >> 1, (b2 << 8) + b3);
  }
}

/** Append one flag, folding a run of equal flags into the format's repeat. */
typedef struct GFNT_W2Flags {
  GFNT_WBuf * out;
  uint32_t pending;
  unsigned run;
} GFNT_W2Flags;

static void gfnt_w2_flags_flush(GFNT_W2Flags * f) {
  if (f->run == 0) {
    return;
  }
  if (f->run == 1) {
    gfnt_wbuf_u8(f->out, f->pending);
  }
  else {
    gfnt_wbuf_u8(f->out, f->pending | 0x08u);
    gfnt_wbuf_u8(f->out, f->run - 1u);
  }
  f->run = 0;
}

static void gfnt_w2_flags_add(GFNT_W2Flags * f, uint32_t flag) {
  if (f->run > 0 && f->pending == flag && f->run < 256u) {
    ++f->run;
    return;
  }
  gfnt_w2_flags_flush(f);
  f->pending = flag;
  f->run = 1;
}

static bool gfnt_w2_bit(const uint8_t * bitmap, size_t i) {
  return (bitmap[i >> 3] & (0x80u >> (i & 7u))) != 0;
}

/**
 * Undo the `glyf` transform: rebuild `glyf` and `loca`, and keep each glyph's
 * xMin for a transformed `hmtx`.
 */
static GFNT_Result gfnt_w2_glyf(GFNT_W2 * w, GFNT_W2Entry * g,
    GFNT_W2Entry * loca) {
  const GFNT_Allocator * a = w->allocator;
  GFNT_W2Cur h = {g->data, g->stream_length, 0, false};
  GFNT_W2Cur ncont, npts, flagc, glyphc, compc, bboxv, instrc;
  const uint8_t * bitmap = NULL;
  const uint8_t * overlap = NULL;
  uint32_t options, num_glyphs, index_format;
  uint32_t sizes[7];
  uint64_t pos = GFNT_WOFF2_GLYF_HEADER_BYTES;
  size_t bitmap_bytes;
  size_t i;
  uint32_t * offsets = NULL;
  int32_t * px = NULL;
  int32_t * py = NULL;
  uint8_t * on = NULL;
  GFNT_WBuf glyf, flagbuf, xbuf, ybuf;
  GFNT_W2Flags flags = {NULL, 0, 0};
  GFNT_Result result = GFNT_OK;
  GFNT_W2Cur * streams[7] = {&ncont, &npts, &flagc, &glyphc, &compc, &bboxv,
      &instrc};

  gfnt_wbuf_init(&glyf, a);
  gfnt_wbuf_init(&flagbuf, a);
  gfnt_wbuf_init(&xbuf, a);
  gfnt_wbuf_init(&ybuf, a);

  (void)gfnt_w2_u16(&h); // reserved
  options = gfnt_w2_u16(&h);
  num_glyphs = gfnt_w2_u16(&h);
  index_format = gfnt_w2_u16(&h);
  for (i = 0; i < 7; ++i) {
    sizes[i] = gfnt_w2_u32(&h);
  }
  if (h.bad) {
    result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
        "a transformed glyf too short to hold its header");
    goto done;
  }
  if (index_format > 1) {
    result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 6,
        "a transformed glyf with an index format other than 0 or 1");
    goto done;
  }
  if (num_glyphs > w->limits->max_glyphs) {
    result = gfnt_w2_fail(w, GFNT_ERR_LIMIT, g->tag, 4,
        "more glyphs than the limit allows");
    goto done;
  }
  for (i = 0; i < 7; ++i) {
    if (pos + sizes[i] > g->stream_length) {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 8 + 4 * i,
          "a transformed glyf stream runs past the table");
      goto done;
    }
    streams[i]->p = g->data + pos;
    streams[i]->n = sizes[i];
    streams[i]->pos = 0;
    streams[i]->bad = false;
    pos += sizes[i];
  }
  bitmap_bytes = 4u * ((num_glyphs + 31u) / 32u);
  if (bitmap_bytes > sizes[5]) {
    result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 28,
        "the bounding-box stream is shorter than its bitmap");
    goto done;
  }
  bitmap = bboxv.p;
  bboxv.p += bitmap_bytes;
  bboxv.n -= bitmap_bytes;
  if (options & 1u) {
    size_t need = (num_glyphs + 7u) / 8u;

    if (pos + need > g->stream_length) {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 2,
          "the overlap bitmap runs past the table");
      goto done;
    }
    overlap = g->data + pos;
  }

  offsets = a->calloc_fn(a->ctx, (size_t)num_glyphs + 1, sizeof *offsets);
  g->xmin = a->calloc_fn(a->ctx, num_glyphs ? num_glyphs : 1, sizeof *g->xmin);
  px = a->calloc_fn(a->ctx, 65537, sizeof *px);
  py = a->calloc_fn(a->ctx, 65537, sizeof *py);
  on = a->calloc_fn(a->ctx, 65537, 1);
  if (!offsets || !g->xmin || !px || !py || !on) {
    result = gfnt_w2_fail(w, GFNT_ERR_OOM, g->tag, 0, "allocating a glyph rebuild");
    goto done;
  }
  g->num_glyphs = num_glyphs;
  flags.out = &flagbuf;

  for (i = 0; i < num_glyphs; ++i) {
    int32_t nc = (int16_t)gfnt_w2_u16(&ncont);
    bool has_bbox = gfnt_w2_bit(bitmap, i);
    int32_t bbox[4] = {0, 0, 0, 0};

    offsets[i] = (uint32_t)glyf.length;
    if (ncont.bad) {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
          "the contour-count stream ends before the last glyph");
      goto done;
    }
    if (nc == 0) {
      if (has_bbox) {
        result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
            "an empty glyph states a bounding box");
        goto done;
      }
      continue;
    }
    if (nc > 0) {
      uint32_t total = 0;
      int32_t x = 0;
      int32_t y = 0;
      uint32_t k;
      uint32_t instr_len;
      const uint8_t * instr;
      bool overlap_flag = overlap && gfnt_w2_bit(overlap, i);

      if ((size_t)nc > w->limits->max_contours) {
        result = gfnt_w2_fail(w, GFNT_ERR_LIMIT, g->tag, 0,
            "more contours in a glyph than the limit allows");
        goto done;
      }
      gfnt_wbuf_u16(&glyf, (uint32_t)nc);
      gfnt_wbuf_u16(&glyf, 0); // bbox, filled in below
      gfnt_wbuf_u16(&glyf, 0);
      gfnt_wbuf_u16(&glyf, 0);
      gfnt_wbuf_u16(&glyf, 0);
      for (k = 0; k < (uint32_t)nc; ++k) {
        total += gfnt_w2_255u16(&npts);
        if (npts.bad || total > 65536u || total > w->limits->max_outline_points) {
          result = gfnt_w2_fail(w, npts.bad ? GFNT_ERR_CORRUPT : GFNT_ERR_LIMIT,
              g->tag, 0, "a glyph's point counts run out of stream or past the limit");
          goto done;
        }
        gfnt_wbuf_u16(&glyf, (total - 1u) & 0xFFFFu);
      }
      for (k = 0; k < total; ++k) {
        uint32_t f = gfnt_w2_u8(&flagc);
        int32_t dx = 0;
        int32_t dy = 0;

        on[k] = (f & 0x80u) ? 0 : 1;
        gfnt_w2_triplet(&glyphc, f & 0x7Fu, &dx, &dy);
        x += dx;
        y += dy;
        if (flagc.bad || glyphc.bad || x < -32768 || x > 32767 || y < -32768
            || y > 32767) {
          result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
              "a glyph's point streams run out, or a coordinate leaves 16 bits");
          goto done;
        }
        px[k] = x;
        py[k] = y;
      }
      instr_len = gfnt_w2_255u16(&glyphc);
      instr = gfnt_w2_take(&instrc, instr_len);
      if (glyphc.bad || instrc.bad) {
        result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
            "a glyph's instructions run past their stream");
        goto done;
      }
      gfnt_wbuf_u16(&glyf, instr_len);
      gfnt_wbuf_bytes(&glyf, instr, instr_len);
      // The points, in the format's own flag and delta coding.
      flagbuf.length = 0;
      xbuf.length = 0;
      ybuf.length = 0;
      flags.run = 0;
      {
        int32_t lx = 0;
        int32_t ly = 0;

        for (k = 0; k < total; ++k) {
          int32_t dx = px[k] - lx;
          int32_t dy = py[k] - ly;
          uint32_t f = on[k] ? 1u : 0u;

          if (k == 0 && overlap_flag) {
            f |= 0x40u;
          }
          if (dx == 0) {
            f |= 0x10u;
          }
          else if (dx >= -255 && dx <= 255) {
            f |= 0x02u | (dx > 0 ? 0x10u : 0u);
            gfnt_wbuf_u8(&xbuf, (uint32_t)(dx < 0 ? -dx : dx));
          }
          else if (dx >= -32768 && dx <= 32767) {
            gfnt_wbuf_u16(&xbuf, (uint32_t)dx & 0xFFFFu);
          }
          else {
            result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
                "a coordinate step does not fit 16 bits");
            goto done;
          }
          if (dy == 0) {
            f |= 0x20u;
          }
          else if (dy >= -255 && dy <= 255) {
            f |= 0x04u | (dy > 0 ? 0x20u : 0u);
            gfnt_wbuf_u8(&ybuf, (uint32_t)(dy < 0 ? -dy : dy));
          }
          else if (dy >= -32768 && dy <= 32767) {
            gfnt_wbuf_u16(&ybuf, (uint32_t)dy & 0xFFFFu);
          }
          else {
            result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
                "a coordinate step does not fit 16 bits");
            goto done;
          }
          gfnt_w2_flags_add(&flags, f);
          lx = px[k];
          ly = py[k];
        }
        gfnt_w2_flags_flush(&flags);
      }
      gfnt_wbuf_bytes(&glyf, flagbuf.data, flagbuf.length);
      gfnt_wbuf_bytes(&glyf, xbuf.data, xbuf.length);
      gfnt_wbuf_bytes(&glyf, ybuf.data, ybuf.length);
      if (has_bbox) {
        for (k = 0; k < 4; ++k) {
          bbox[k] = (int16_t)gfnt_w2_u16(&bboxv);
        }
        if (bboxv.bad) {
          result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
              "the bounding-box stream ends before the last stated box");
          goto done;
        }
      }
      else if (total > 0) {
        bbox[0] = bbox[2] = px[0];
        bbox[1] = bbox[3] = py[0];
        for (k = 1; k < total; ++k) {
          if (px[k] < bbox[0]) bbox[0] = px[k];
          if (px[k] > bbox[2]) bbox[2] = px[k];
          if (py[k] < bbox[1]) bbox[1] = py[k];
          if (py[k] > bbox[3]) bbox[3] = py[k];
        }
      }
    }
    else if (nc == -1) {
      size_t start = compc.pos;
      bool instructions = false;
      uint32_t cflags;
      const uint8_t * comps;
      uint32_t k;

      if (!has_bbox) {
        result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
            "a composite glyph without its bounding box");
        goto done;
      }
      do {
        cflags = gfnt_w2_u16(&compc);
        (void)gfnt_w2_u16(&compc);
        (void)gfnt_w2_take(&compc, (cflags & 0x0001u) ? 4u : 2u);
        if (cflags & 0x0008u) {
          (void)gfnt_w2_take(&compc, 2);
        }
        else if (cflags & 0x0040u) {
          (void)gfnt_w2_take(&compc, 4);
        }
        else if (cflags & 0x0080u) {
          (void)gfnt_w2_take(&compc, 8);
        }
        if (cflags & 0x0100u) {
          instructions = true;
        }
      } while (!compc.bad && (cflags & 0x0020u));
      if (compc.bad) {
        result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
            "a composite glyph runs past the composite stream");
        goto done;
      }
      comps = compc.p + start;
      for (k = 0; k < 4; ++k) {
        bbox[k] = (int16_t)gfnt_w2_u16(&bboxv);
      }
      if (bboxv.bad) {
        result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
            "the bounding-box stream ends before the last stated box");
        goto done;
      }
      gfnt_wbuf_u16(&glyf, 0xFFFFu);
      for (k = 0; k < 4; ++k) {
        gfnt_wbuf_u16(&glyf, 0);
      }
      gfnt_wbuf_bytes(&glyf, comps, compc.pos - start);
      if (instructions) {
        uint32_t instr_len = gfnt_w2_255u16(&glyphc);
        const uint8_t * instr = gfnt_w2_take(&instrc, instr_len);

        if (glyphc.bad || instrc.bad) {
          result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
              "a composite glyph's instructions run past their stream");
          goto done;
        }
        gfnt_wbuf_u16(&glyf, instr_len);
        gfnt_wbuf_bytes(&glyf, instr, instr_len);
      }
    }
    else {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, g->tag, 0,
          "a glyph with a negative contour count other than -1");
      goto done;
    }
    // The box goes into the header written above, wherever the glyph began. A
    // buffer that ran out of memory dropped bytes, so nothing is there to patch.
    if (glyf.oom || flagbuf.oom || xbuf.oom || ybuf.oom) {
      result = gfnt_w2_fail(w, GFNT_ERR_OOM, g->tag, 0, "growing a glyph buffer");
      goto done;
    }
    {
      uint8_t * head = glyf.data + offsets[i] + 2;
      unsigned k;

      for (k = 0; k < 4; ++k) {
        gfnt_write_put16(head + 2 * k, (uint32_t)bbox[k] & 0xFFFFu);
      }
      g->xmin[i] = (int16_t)bbox[0];
    }
    gfnt_wbuf_align(&glyf, 4);
    if (glyf.oom) {
      result = gfnt_w2_fail(w, GFNT_ERR_OOM, g->tag, 0, "growing the rebuilt glyf");
      goto done;
    }
    if (glyf.length > w->limits->max_blob_bytes) {
      result = gfnt_w2_fail(w, GFNT_ERR_LIMIT, g->tag, 0,
          "the font would be larger than the limit allows");
      goto done;
    }
  }
  offsets[num_glyphs] = (uint32_t)glyf.length;
  if (flagbuf.oom || xbuf.oom || ybuf.oom || glyf.oom) {
    result = gfnt_w2_fail(w, GFNT_ERR_OOM, g->tag, 0, "growing a glyph buffer");
    goto done;
  }

  {
    size_t entry_bytes = index_format ? 4u : 2u;
    size_t loca_bytes = ((size_t)num_glyphs + 1u) * entry_bytes;
    uint8_t * out;

    if (loca->orig_length != loca_bytes) {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, loca->tag, 0,
          "loca's stated length does not match the glyph count and index format");
      goto done;
    }
    if (!index_format && glyf.length > 0x1FFFEu) {
      result = gfnt_w2_fail(w, GFNT_ERR_CORRUPT, loca->tag, 0,
          "the glyf table is too long for the short loca the font states");
      goto done;
    }
    out = a->calloc_fn(a->ctx, loca_bytes ? loca_bytes : 1, 1);
    if (!out) {
      result = gfnt_w2_fail(w, GFNT_ERR_OOM, loca->tag, 0, "allocating the loca");
      goto done;
    }
    for (i = 0; i <= num_glyphs; ++i) {
      if (index_format) {
        gfnt_write_put32(out + 4 * i, offsets[i]);
      }
      else {
        gfnt_write_put16(out + 2 * i, offsets[i] / 2u);
      }
    }
    loca->owned = out;
    loca->out = out;
    loca->out_length = loca_bytes;
  }
  // The glyf buffer's bytes are handed over: copy so the buffer's allocator
  // bookkeeping stays its own.
  g->owned = a->calloc_fn(a->ctx, glyf.length ? glyf.length : 1, 1);
  if (!g->owned) {
    result = gfnt_w2_fail(w, GFNT_ERR_OOM, g->tag, 0, "allocating the glyf");
    goto done;
  }
  if (glyf.length) {
    memcpy(g->owned, glyf.data, glyf.length);
  }
  g->out = g->owned;
  g->out_length = glyf.length;

done:
  gfnt_wbuf_free(&glyf);
  gfnt_wbuf_free(&flagbuf);
  gfnt_wbuf_free(&xbuf);
  gfnt_wbuf_free(&ybuf);
  a->free_fn(a->ctx, offsets);
  a->free_fn(a->ctx, px);
  a->free_fn(a->ctx, py);
  a->free_fn(a->ctx, on);
  return result;
}

/** The entry of tag @p tag in font @p f, or NULL. */
static GFNT_W2Entry * gfnt_w2_find(GFNT_W2 * w, const GFNT_W2Font * f,
    uint32_t tag) {
  size_t k;

  for (k = 0; k < f->count; ++k) {
    if (w->entries[f->index[k]].tag == tag) {
      return &w->entries[f->index[k]];
    }
  }
  return NULL;
}

/** Undo the `hmtx` transform for the font @p f that holds it. */
static GFNT_Result gfnt_w2_hmtx(GFNT_W2 * w, GFNT_W2Entry * h,
    const GFNT_W2Font * f) {
  const GFNT_Allocator * a = w->allocator;
  const GFNT_W2Entry * hhea = gfnt_w2_find(w, f, GFNT_TAG_HHEA);
  const GFNT_W2Entry * glyf = gfnt_w2_find(w, f, GFNT_TAG_GLYF);
  GFNT_W2Cur c = {h->data, h->stream_length, 0, false};
  uint32_t flags, metrics, glyphs, i;
  uint8_t * out;
  size_t bytes;

  if (!hhea || !glyf || !glyf->transformed || !glyf->xmin || !hhea->data
      || hhea->stream_length < 36) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, h->tag, 0,
        "a transformed hmtx needs the font's hhea and its transformed glyf");
  }
  metrics = ((uint32_t)hhea->data[34] << 8) | hhea->data[35];
  glyphs = glyf->num_glyphs;
  flags = gfnt_w2_u8(&c);
  if (c.bad || (flags & 0xFCu)) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, h->tag, 0,
        "a transformed hmtx with reserved flag bits set, or no flags");
  }
  if (metrics > glyphs) {
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, h->tag, 0,
        "hhea states more metrics than the font has glyphs");
  }
  bytes = (size_t)metrics * 4u + (size_t)(glyphs - metrics) * 2u;
  out = a->calloc_fn(a->ctx, bytes ? bytes : 1, 1);
  if (!out) {
    return gfnt_w2_fail(w, GFNT_ERR_OOM, h->tag, 0, "allocating the hmtx");
  }
  for (i = 0; i < metrics; ++i) {
    uint32_t adv = gfnt_w2_u16(&c);

    gfnt_write_put16(out + 4 * (size_t)i, adv);
  }
  for (i = 0; i < metrics; ++i) {
    uint32_t lsb = (flags & 1u) ? (uint32_t)(uint16_t)glyf->xmin[i]
                                : gfnt_w2_u16(&c);

    gfnt_write_put16(out + 4 * (size_t)i + 2, lsb);
  }
  for (i = metrics; i < glyphs; ++i) {
    uint32_t lsb = (flags & 2u) ? (uint32_t)(uint16_t)glyf->xmin[i]
                                : gfnt_w2_u16(&c);

    gfnt_write_put16(out + 4 * (size_t)metrics + 2 * (size_t)(i - metrics), lsb);
  }
  if (c.bad) {
    a->free_fn(a->ctx, out);
    return gfnt_w2_fail(w, GFNT_ERR_CORRUPT, h->tag, 0,
        "a transformed hmtx runs out before its last metric");
  }
  h->owned = out;
  h->out = out;
  h->out_length = bytes;
  return GFNT_OK;
}

static int gfnt_w2_by_tag(const GFNT_W2 * w, uint16_t x, uint16_t y) {
  uint32_t a = w->entries[x].tag;
  uint32_t b = w->entries[y].tag;

  return a < b ? -1 : a > b;
}

GFNT_Result gfnt_woff2_to_sfnt(const GFNT_Blob * in, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_W2 w;
  GFNT_Result result;
  uint8_t * file = NULL;
  size_t total = 0;
  size_t i;
  size_t f_i;

  if (!in || !limits || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_WOFF2, 0,
        GFNT_GLYPH_NONE, "no input, no limits, or nowhere to put the blob");
  }
  memset(&w, 0, sizeof w);
  w.limits = limits;
  w.allocator = allocator ? allocator : gfnt_allocator_default();
  w.error = error;
  w.bytes = gfnt_blob_data(in);
  w.size = gfnt_blob_size(in);

  result = gfnt_w2_parse(&w);
  if (result == GFNT_OK) {
    result = gfnt_w2_decompress(&w);
  }
  if (result != GFNT_OK) {
    goto done;
  }

  // The transforms. glyf and loca are transformed together, per font, wherever
  // they sit in the directory (fontTools writes the directory in tag order, which
  // puts `head` between them, and browsers load those files); hmtx needs the
  // glyf's boxes and so comes after.
  for (f_i = 0; f_i < w.font_count; ++f_i) {
    GFNT_W2Entry * g = gfnt_w2_find(&w, &w.fonts[f_i], GFNT_TAG_GLYF);
    GFNT_W2Entry * l = gfnt_w2_find(&w, &w.fonts[f_i], GFNT_TAG_LOCA);

    if ((g && g->transformed) != (l && l->transformed)) {
      result = gfnt_w2_fail(&w, GFNT_ERR_CORRUPT, GFNT_TAG_GLYF, 0,
          "glyf and loca are not both transformed, or one is missing");
      goto done;
    }
    if (g && g->transformed && !g->owned) {
      if (l->paired && l->paired != g) {
        result = gfnt_w2_fail(&w, GFNT_ERR_CORRUPT, GFNT_TAG_LOCA, 0,
            "a loca shared between different glyf tables");
        goto done;
      }
      l->paired = g;
      result = gfnt_w2_glyf(&w, g, l);
      if (result != GFNT_OK) {
        goto done;
      }
    }
  }
  for (i = 0; i < w.entry_count; ++i) {
    GFNT_W2Entry * e = &w.entries[i];

    if (e->tag == GFNT_TAG_HMTX && e->transformed) {
      const GFNT_W2Font * holder = NULL;

      for (f_i = 0; f_i < w.font_count && !holder; ++f_i) {
        size_t k;

        for (k = 0; k < w.fonts[f_i].count; ++k) {
          if (w.fonts[f_i].index[k] == i) {
            holder = &w.fonts[f_i];
            break;
          }
        }
      }
      if (!holder) {
        continue;
      }
      result = gfnt_w2_hmtx(&w, e, holder);
      if (result != GFNT_OK) {
        goto done;
      }
    }
  }
  // Tables nothing transformed are the stream's bytes as they stand.
  for (i = 0; i < w.entry_count; ++i) {
    GFNT_W2Entry * e = &w.entries[i];

    if (!e->out) {
      e->out = e->data ? e->data : (const uint8_t *)"";
      e->out_length = e->stream_length;
    }
  }

  // The layout: for a collection the header and every font's directory first,
  // then each table once; for a single font its directory and then the tables.
  if (w.collection) {
    total = 12u + 4u * w.font_count + (w.ttc_version >= 0x00020000u ? 12u : 0u);
  }
  for (f_i = 0; f_i < w.font_count; ++f_i) {
    GFNT_W2Font * f = &w.fonts[f_i];
    uint16_t * order = f->index;
    size_t k;

    // Sort by tag (insertion; the count is bounded by the table limit) and look
    // for a repeat, which an sfnt directory cannot hold.
    for (k = 1; k < f->count; ++k) {
      uint16_t v = order[k];
      size_t j = k;

      while (j > 0 && gfnt_w2_by_tag(&w, order[j - 1], v) > 0) {
        order[j] = order[j - 1];
        --j;
      }
      order[j] = v;
    }
    for (k = 1; k < f->count; ++k) {
      if (w.entries[order[k]].tag == w.entries[order[k - 1]].tag) {
        result = gfnt_w2_fail(&w, GFNT_ERR_CORRUPT, w.entries[order[k]].tag, 0,
            "a font lists the same table twice");
        goto done;
      }
    }
    f->offset = total;
    total += 12u + 16u * f->count;
  }
  for (i = 0; i < w.entry_count; ++i) {
    GFNT_W2Entry * e = &w.entries[i];
    size_t padded = gfnt_w2_pad4(e->out_length);

    e->offset = total;
    if (padded > limits->max_blob_bytes - (total < limits->max_blob_bytes
                                               ? total : limits->max_blob_bytes)) {
      result = gfnt_w2_fail(&w, GFNT_ERR_LIMIT, e->tag, 0,
          "the font would be larger than the limit allows");
      goto done;
    }
    total += padded;
  }

  file = w.allocator->calloc_fn(w.allocator->ctx, total ? total : 1, 1);
  if (!file) {
    result = gfnt_w2_fail(&w, GFNT_ERR_OOM, GFNT_TAG_WOFF2, 0,
        "allocating the rebuilt font");
    goto done;
  }
  // `head`: the checkSumAdjustment is zero while the checksums are taken, and
  // is set once afterwards for a single font.
  for (i = 0; i < w.entry_count; ++i) {
    GFNT_W2Entry * e = &w.entries[i];

    if (e->out_length > 0) {
      memcpy(file + e->offset, e->out, e->out_length);
    }
    if (e->tag == GFNT_TAG_HEAD && e->out_length >= 12) {
      uint8_t stored[4];

      // The checksum of `head` is taken with the adjustment zero. A single font
      // gets its adjustment recomputed afterwards; a collection's stays as the
      // file stored it.
      memcpy(stored, file + e->offset + 8, 4);
      memset(file + e->offset + 8, 0, 4);
      e->checksum = gfnt_write_checksum(file + e->offset, e->out_length);
      if (w.collection) {
        memcpy(file + e->offset + 8, stored, 4);
      }
      continue;
    }
    e->checksum = gfnt_write_checksum(file + e->offset, e->out_length);
  }
  if (w.collection) {
    gfnt_write_put32(file, GFNT_TAG_TTCF);
    gfnt_write_put32(file + 4, w.ttc_version);
    gfnt_write_put32(file + 8, (uint32_t)w.font_count);
    for (f_i = 0; f_i < w.font_count; ++f_i) {
      gfnt_write_put32(file + 12 + 4 * f_i, (uint32_t)w.fonts[f_i].offset);
    }
  }
  for (f_i = 0; f_i < w.font_count; ++f_i) {
    const GFNT_W2Font * f = &w.fonts[f_i];
    uint8_t * p = file + f->offset;
    unsigned entry_selector = 0;
    unsigned search_range;
    size_t k;

    while ((2u << entry_selector) <= f->count) {
      ++entry_selector;
    }
    search_range = (1u << entry_selector) * 16u;
    gfnt_write_put32(p, f->flavor);
    gfnt_write_put16(p + 4, (uint32_t)f->count);
    gfnt_write_put16(p + 6, search_range);
    gfnt_write_put16(p + 8, entry_selector);
    gfnt_write_put16(p + 10, (uint32_t)f->count * 16u - search_range);
    for (k = 0; k < f->count; ++k) {
      const GFNT_W2Entry * e = &w.entries[f->index[k]];
      uint8_t * d = p + 12 + 16 * k;

      gfnt_write_put32(d, e->tag);
      gfnt_write_put32(d + 4, e->checksum);
      gfnt_write_put32(d + 8, (uint32_t)e->offset);
      gfnt_write_put32(d + 12, (uint32_t)e->out_length);
    }
  }
  if (!w.collection) {
    const GFNT_W2Entry * head = gfnt_w2_find(&w, &w.fonts[0], GFNT_TAG_HEAD);

    if (head && head->out_length >= 12) {
      gfnt_write_put32(file + head->offset + 8,
          0xB1B0AFBAu - gfnt_write_checksum(file, total));
    }
  }
  result = gfnt_blob_create_memory(file, total, GFNT_BLOB_COPY, limits,
      w.allocator, out_blob, error);

done:
  w.allocator->free_fn(w.allocator->ctx, file);
  gfnt_w2_free(&w);
  return result;
}
