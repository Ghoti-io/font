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
 * `name` for a subset that is renamed.
 *
 * documentation/design.md section 12.3. A font whose licence reserves its name (the SIL Open
 * Font License's Reserved Font Name) may not be modified and distributed under it, and a subset
 * is a modified font. Renaming is one option of the subsetter and it is the whole of the change:
 * every `name` record that states the family - the family (1), the unique identifier (3), the full
 * name (4), the PostScript name (6), the typographic family (16), the compatible full name (18),
 * the CID findfont name (20), the WWS family (21) and the variations PostScript prefix (25) - is
 * removed in every language and platform, and the first six are written again for the new family
 * in Windows English. Everything else, the copyright and licence and the style names included, is
 * carried as it was. A `CFF ` is named to match by the caller.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "write.h"

typedef struct Rec {
  uint16_t platform;
  uint16_t encoding;
  uint16_t language;
  uint16_t id;
  const uint8_t * text;
  size_t length;
  uint8_t * owned;       // when the text is made here
} Rec;

static uint32_t be16(const uint8_t * p) {
  return ((uint32_t)p[0] << 8) | p[1];
}

/** The IDs that state the family, in any form. */
static bool states_family(uint16_t id) {
  return id == 1 || id == 3 || id == 4 || id == 6 || id == 16 || id == 18 || id == 20
      || id == 21 || id == 25;
}

static int by_key(const void * l, const void * r) {
  const Rec * a = l;
  const Rec * b = r;

  if (a->platform != b->platform) {
    return a->platform < b->platform ? -1 : 1;
  }
  if (a->encoding != b->encoding) {
    return a->encoding < b->encoding ? -1 : 1;
  }
  if (a->language != b->language) {
    return a->language < b->language ? -1 : 1;
  }
  if (a->id != b->id) {
    return a->id < b->id ? -1 : 1;
  }
  return 0;
}

/** UTF-8 to UTF-16BE: 0, 1 for ill-formed UTF-8 or 2 for no memory. The result is allocated. */
static int utf16_of(const GFNT_Allocator * a, const char * s, uint8_t ** out, size_t * out_len) {
  const uint8_t * p = (const uint8_t *)s;
  size_t n = strlen(s);
  uint8_t * buf = a->calloc_fn(a->ctx, n * 4 + 2, 1);
  size_t len = 0;
  size_t i = 0;

  if (!buf) {
    return 2;
  }
  while (i < n) {
    uint32_t cp;
    size_t extra;

    if (p[i] < 0x80) {
      cp = p[i];
      extra = 0;
    }
    else if (p[i] >= 0xC2 && p[i] < 0xE0) {
      cp = p[i] & 0x1F;
      extra = 1;
    }
    else if (p[i] >= 0xE0 && p[i] < 0xF0) {
      cp = p[i] & 0x0F;
      extra = 2;
    }
    else if (p[i] >= 0xF0 && p[i] < 0xF5) {
      cp = p[i] & 0x07;
      extra = 3;
    }
    else {
      a->free_fn(a->ctx, buf);
      return 1;
    }
    for (size_t k = 1; k <= extra; ++k) {
      if (i + k >= n || (p[i + k] & 0xC0) != 0x80) {
        a->free_fn(a->ctx, buf);
        return 1;
      }
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF))
        || (cp >= 0xD800 && cp < 0xE000)) {
      a->free_fn(a->ctx, buf);
      return 1;
    }
    i += extra + 1;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      buf[len++] = (uint8_t)(0xD8 + (cp >> 18));
      buf[len++] = (uint8_t)(cp >> 10);
      buf[len++] = (uint8_t)(0xDC + ((cp >> 8) & 3));
      buf[len++] = (uint8_t)cp;
    }
    else {
      buf[len++] = (uint8_t)(cp >> 8);
      buf[len++] = (uint8_t)cp;
    }
  }
  *out = buf;
  *out_len = len;
  return 0;
}

/** UTF-16BE to UTF-8 into @p out (room for 3 bytes per unit); the length. Unpaired units become '?'. */
static size_t utf8_of(const uint8_t * p, size_t n, char * out) {
  size_t o = 0;
  size_t i;

  for (i = 0; i + 1 < n; i += 2) {
    uint32_t cp = be16(p + i);

    if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n && be16(p + i + 2) >= 0xDC00
        && be16(p + i + 2) < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (be16(p + i + 2) - 0xDC00);
      i += 2;
    }
    else if (cp >= 0xD800 && cp < 0xE000) {
      cp = '?';
    }
    if (cp < 0x80) {
      out[o++] = (char)cp;
    }
    else if (cp < 0x800) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    else {
      out[o++] = (char)(0xF0 | (cp >> 18));
      out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[o] = '\0';
  return o;
}

/** The source's style name: ID 17 else 2, Windows English first, then any Windows, then Macintosh. */
static void style_of(const Rec * recs, size_t n, char * out, size_t cap) {
  static const uint16_t ids[2] = {17, 2};
  size_t k;
  size_t i;

  for (k = 0; k < 2; ++k) {
    const Rec * best = NULL;

    for (i = 0; i < n; ++i) {
      if (recs[i].id != ids[k] || !recs[i].length) {
        continue;
      }
      if (recs[i].platform == 3 && recs[i].language == 0x409) {
        best = &recs[i];
        break;
      }
      if (!best && recs[i].platform == 3) {
        best = &recs[i];
      }
    }
    if (best && best->length * 3 + 1 <= cap) {
      utf8_of(best->text, best->length, out);
      return;
    }
    for (i = 0; i < n && !best; ++i) {
      if (recs[i].id == ids[k] && recs[i].platform == 1 && recs[i].length && recs[i].length < cap) {
        memcpy(out, recs[i].text, recs[i].length);
        out[recs[i].length] = '\0';
        return;
      }
    }
  }
  strcpy(out, "Regular");
}

/** A PostScript name: printable ASCII but for spaces and "[](){}<>/%", at most 63 characters. */
static void ps_part(const char * in, char * out, size_t cap) {
  size_t o = 0;

  for (; *in && o + 1 < cap; ++in) {
    const unsigned char c = (unsigned char)*in;

    if (c > 32 && c < 127 && !strchr("[](){}<>/%", c)) {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
}

GFNT_Result gfnt_subset_name(const GFNT_Face * face, const char * family,
    const GFNT_Allocator * a, char ** out_ps_name, GFNT_WBuf * out, GFNT_Error * error) {
  const GFNT_Tag tag = GFNT_TAG('n', 'a', 'm', 'e');
  GFNT_Reader reader;
  const uint8_t * t = NULL;
  size_t n_bytes;
  uint32_t format = 0;
  uint32_t count = 0;
  size_t storage;
  size_t langtags = 0;
  Rec * recs = NULL;
  size_t nrecs = 0;
  size_t cap;
  size_t i;
  bool had16 = false;
  bool had21 = false;
  bool had_mac = false;
  bool ascii = true;
  char style[256];
  char ps_family[128];
  char ps_style[128];
  char ps[300];
  char full[600];
  char unique[700];
  size_t ntexts = 0;
  GFNT_WBuf strings;
  GFNT_Result result = GFNT_OK;

  if (!family || !family[0] || strlen(family) > 200) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
        "a family name that is empty or longer than 200 bytes");
  }
  gfnt_wbuf_init(&strings, a);
  if (!gfnt_face_has_table(face, tag) || gfnt_face_table_reader(face, tag, &reader, NULL) != GFNT_OK
      || gfnt_read_bytes(&reader, reader.length, &t) != GFNT_OK) {
    t = NULL;
    n_bytes = 0;
  }
  else {
    n_bytes = reader.length;
  }
  if (t && n_bytes >= 6) {
    format = be16(t);
    count = be16(t + 2);
    storage = be16(t + 4);
    if ((format != 0 && format != 1) || 6 + (size_t)count * 12 > n_bytes || storage > n_bytes) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
          "a name table that cannot be read");
    }
  }
  else {
    t = NULL;
    count = 0;
    storage = 0;
  }
  for (i = 0; family[i]; ++i) {
    ascii = ascii && (unsigned char)family[i] < 0x80;
  }
  cap = (size_t)count + 16;
  recs = a->calloc_fn(a->ctx, cap, sizeof *recs);
  if (!recs) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "renaming");
  }
  for (i = 0; i < count; ++i) {
    const uint8_t * r = t + 6 + 12 * i;
    size_t len = be16(r + 8);
    size_t off = storage + be16(r + 10);

    if (off + len > n_bytes) {
      continue;        // a record that points outside the table says nothing
    }
    if (be16(r + 6) == 16) {
      had16 = true;
    }
    if (be16(r + 6) == 21) {
      had21 = true;
    }
    if (be16(r) == 1 && states_family((uint16_t)be16(r + 6))) {
      had_mac = true;
    }
    if (states_family((uint16_t)be16(r + 6))) {
      continue;
    }
    recs[nrecs].platform = (uint16_t)be16(r);
    recs[nrecs].encoding = (uint16_t)be16(r + 2);
    recs[nrecs].language = (uint16_t)be16(r + 4);
    recs[nrecs].id = (uint16_t)be16(r + 6);
    recs[nrecs].text = t + off;
    recs[nrecs].length = len;
    ++nrecs;
  }
  // The style comes from the source, whatever is dropped from it.
  {
    Rec * all = a->calloc_fn(a->ctx, (size_t)count + 1, sizeof *all);
    size_t nall = 0;

    if (!all) {
      a->free_fn(a->ctx, recs);
      return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "renaming");
    }
    for (i = 0; i < count; ++i) {
      const uint8_t * r = t + 6 + 12 * i;
      size_t len = be16(r + 8);
      size_t off = storage + be16(r + 10);

      if (off + len > n_bytes || (be16(r + 6) != 2 && be16(r + 6) != 17)) {
        continue;
      }
      all[nall].platform = (uint16_t)be16(r);
      all[nall].encoding = (uint16_t)be16(r + 2);
      all[nall].language = (uint16_t)be16(r + 4);
      all[nall].id = (uint16_t)be16(r + 6);
      all[nall].text = t + off;
      all[nall].length = len;
      ++nall;
    }
    style_of(all, nall, style, sizeof style);
    a->free_fn(a->ctx, all);
  }
  ps_part(family, ps_family, sizeof ps_family);
  ps_part(style, ps_style, sizeof ps_style);
  if (!ps_family[0]) {
    strcpy(ps_family, "Subset");
  }
  if (!ps_style[0]) {
    strcpy(ps_style, "Regular");
  }
  snprintf(ps, sizeof ps, "%s-%s", ps_family, ps_style);
  ps[63] = '\0';
  if (strcmp(style, "Regular") == 0) {
    snprintf(full, sizeof full, "%s", family);
  }
  else {
    snprintf(full, sizeof full, "%s %s", family, style);
  }
  snprintf(unique, sizeof unique, "%s;%s;subset", family, style);
  // The records written for the new family: the six that always are, and the typographic and WWS
  // families only where the source had them.
  {
    struct { uint16_t id; const char * text; } fresh[6];

    fresh[ntexts].id = 1; fresh[ntexts++].text = family;
    fresh[ntexts].id = 3; fresh[ntexts++].text = unique;
    fresh[ntexts].id = 4; fresh[ntexts++].text = full;
    fresh[ntexts].id = 6; fresh[ntexts++].text = ps;
    if (had16) {
      fresh[ntexts].id = 16; fresh[ntexts++].text = family;
    }
    if (had21) {
      fresh[ntexts].id = 21; fresh[ntexts++].text = family;
    }
    for (i = 0; i < ntexts; ++i) {
      uint8_t * u16 = NULL;
      size_t u16len = 0;

      const int made = utf16_of(a, fresh[i].text, &u16, &u16len);

      if (made) {
        result = gfnt_error_set(error, made == 2 ? GFNT_ERR_OOM : GFNT_ERR_INVALID, tag, 0,
            GFNT_GLYPH_NONE, made == 2 ? "renaming" : "a family name that is not valid UTF-8");
        goto done;
      }
      recs[nrecs].platform = 3;
      recs[nrecs].encoding = 1;
      recs[nrecs].language = 0x409;
      recs[nrecs].id = fresh[i].id;
      recs[nrecs].text = u16;
      recs[nrecs].length = u16len;
      recs[nrecs].owned = u16;
      ++nrecs;
      // Macintosh Roman agrees with ASCII, so a font that had Macintosh names keeps them.
      if (had_mac && ascii) {
        const size_t len = strlen(fresh[i].text);
        uint8_t * mac = a->calloc_fn(a->ctx, len + 1, 1);

        if (!mac) {
          result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "renaming");
          goto done;
        }
        memcpy(mac, fresh[i].text, len);
        recs[nrecs].platform = 1;
        recs[nrecs].encoding = 0;
        recs[nrecs].language = 0;
        recs[nrecs].id = fresh[i].id;
        recs[nrecs].text = mac;
        recs[nrecs].length = len;
        recs[nrecs].owned = mac;
        ++nrecs;
      }
    }
  }
  qsort(recs, nrecs, sizeof *recs, by_key);
  if (format == 1 && t && storage > 6 + (size_t)count * 12) {
    langtags = be16(t + 6 + (size_t)count * 12);
  }
  // Header, records, (format 1: the language tag records), then the strings.
  {
    size_t header = 6 + nrecs * 12 + (format == 1 ? 2 + langtags * 4 : 0);
    size_t base = out->length;

    gfnt_wbuf_u16(out, format);
    gfnt_wbuf_u16(out, (uint32_t)nrecs);
    gfnt_wbuf_u16(out, (uint32_t)header);
    for (i = 0; i < nrecs; ++i) {
      if (strings.length + recs[i].length > 0xFFFFu) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, tag, 0, GFNT_GLYPH_NONE,
            "a name table whose strings do not fit 16-bit offsets");
        goto done;
      }
      gfnt_wbuf_u16(out, recs[i].platform);
      gfnt_wbuf_u16(out, recs[i].encoding);
      gfnt_wbuf_u16(out, recs[i].language);
      gfnt_wbuf_u16(out, recs[i].id);
      gfnt_wbuf_u16(out, (uint32_t)recs[i].length);
      gfnt_wbuf_u16(out, (uint32_t)strings.length);
      gfnt_wbuf_bytes(&strings, recs[i].text, recs[i].length);
    }
    if (format == 1) {
      gfnt_wbuf_u16(out, (uint32_t)langtags);
      for (i = 0; i < langtags; ++i) {
        const uint8_t * lt = t + 6 + (size_t)count * 12 + 2 + 4 * i;
        size_t len = be16(lt);
        size_t off = storage + be16(lt + 2);

        if (off + len > n_bytes || strings.length + len > 0xFFFFu) {
          gfnt_wbuf_u16(out, 0);
          gfnt_wbuf_u16(out, 0);
          continue;
        }
        gfnt_wbuf_u16(out, (uint32_t)len);
        gfnt_wbuf_u16(out, (uint32_t)strings.length);
        gfnt_wbuf_bytes(&strings, t + off, len);
      }
    }
    gfnt_wbuf_bytes(out, strings.data, strings.length);
    (void)base;
  }
  if (out->oom || strings.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "renaming");
    goto done;
  }
  *out_ps_name = a->calloc_fn(a->ctx, strlen(ps) + 1, 1);
  if (!*out_ps_name) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "renaming");
    goto done;
  }
  memcpy(*out_ps_name, ps, strlen(ps));
done:
  for (i = 0; i < nrecs; ++i) {
    a->free_fn(a->ctx, recs[i].owned);
  }
  a->free_fn(a->ctx, recs);
  gfnt_wbuf_free(&strings);
  return result;
}
