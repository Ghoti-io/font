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
 * Subsetting a `CFF ` table.
 *
 * documentation/design.md section 12.2 and 12.3. A subset CFF is written, not
 * patched: the charstrings of the kept glyphs with their subroutine calls
 * **flattened** (so no subroutine is shared, and none is left that no kept glyph
 * calls), a charset naming only those glyphs, a String INDEX holding only the
 * strings still named, and Private DICTs with their `Subrs` offset removed.
 *
 * Flattening is done by a small evaluator, not by the drawing interpreter: it has
 * to leave the program a program. Operands are held back until the operator that
 * takes them is reached, so that the number a `callsubr` consumed can be taken
 * back out instead of emitted. A subroutine number that arithmetic computed
 * cannot be, and is refused by name.
 */

#include <stdlib.h>
#include <string.h>
#include "../cff/cff.h"
#include "../cff/cff2.h"
#include "../cff/cff_strings.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "write.h"

#define NO_GLYPH 0xFFFFFFFFu
#define STACK_MAX GFNT_CFF2_DICT_MAX_OPERANDS

/** The bytes of the `CFF ` table. */
typedef struct Tab {
  const uint8_t * p;
  size_t n;
} Tab;

static bool load_tab(const GFNT_Face * face, GFNT_Tag tag, Tab * tab) {
  GFNT_Reader reader;

  if (!gfnt_face_has_table(face, tag)
      || gfnt_face_table_reader(face, tag, &reader, NULL) != GFNT_OK
      || gfnt_read_bytes(&reader, reader.length, &tab->p) != GFNT_OK) {
    return false;
  }
  tab->n = reader.length;
  return true;
}

/** Offset @p i of an INDEX, as the table states it. */
static bool index_offset(const Tab * t, const GFNT_CffIndex * ix, uint32_t i,
    size_t * out) {
  size_t at = ix->offsets + (size_t)i * ix->off_size;
  size_t value = 0;
  size_t k;

  if (at + ix->off_size > t->n) {
    return false;
  }
  for (k = 0; k < ix->off_size; ++k) {
    value = (value << 8) | t->p[at + k];
  }
  *out = value;
  return true;
}

/** Element @p i of an INDEX, borrowed. */
static bool index_elem(const Tab * t, const GFNT_CffIndex * ix, uint32_t i,
    const uint8_t ** out, size_t * out_length) {
  size_t a;
  size_t b;

  if (i >= ix->count || !index_offset(t, ix, i, &a) || !index_offset(t, ix, i + 1, &b)
      || a < 1 || b < a || ix->data_base + b > t->n) {
    return false;
  }
  *out = t->p + ix->data_base + a;
  *out_length = b - a;
  return true;
}

/* ---- DICT tokens --------------------------------------------------------- */

typedef struct DictEnt {
  uint16_t op;
  size_t a;        // first operand byte
  size_t b;        // the operator's first byte
  size_t end;      // one past the operator
  size_t n;        // operands
  int32_t v[3];    // the first three, when integers
} DictEnt;

/** Next DICT entry: 1 got one, 0 at the end, -1 malformed. */
static int dict_next(const uint8_t * d, size_t n, size_t * pos, DictEnt * e) {
  size_t p = *pos;

  if (p >= n) {
    return 0;
  }
  memset(e, 0, sizeof *e);
  e->a = p;
  for (;;) {
    uint8_t c;
    int32_t val = 0;

    if (p >= n) {
      return -1;
    }
    c = d[p];
    if (c <= 24) {
      e->b = p;
      if (c == 12) {
        if (p + 1 >= n) {
          return -1;
        }
        e->op = (uint16_t)(0x0c00 | d[p + 1]);
        p += 2;
      }
      else {
        e->op = c;
        p += 1;
      }
      e->end = p;
      *pos = p;
      return 1;
    }
    if (c == 28) {
      if (p + 3 > n) {
        return -1;
      }
      val = (int16_t)(((uint32_t)d[p + 1] << 8) | d[p + 2]);
      p += 3;
    }
    else if (c == 29) {
      if (p + 5 > n) {
        return -1;
      }
      val = (int32_t)(((uint32_t)d[p + 1] << 24) | ((uint32_t)d[p + 2] << 16)
          | ((uint32_t)d[p + 3] << 8) | d[p + 4]);
      p += 5;
    }
    else if (c == 30) {
      ++p;
      for (;;) {
        uint8_t x;

        if (p >= n) {
          return -1;
        }
        x = d[p++];
        if ((x & 0x0f) == 0x0f || (x >> 4) == 0x0f) {
          break;
        }
      }
    }
    else if (c >= 32 && c <= 246) {
      val = (int32_t)c - 139;
      ++p;
    }
    else if (c >= 247 && c <= 250) {
      if (p + 2 > n) {
        return -1;
      }
      val = ((int32_t)c - 247) * 256 + d[p + 1] + 108;
      p += 2;
    }
    else if (c >= 251 && c <= 254) {
      if (p + 2 > n) {
        return -1;
      }
      val = -((int32_t)c - 251) * 256 - d[p + 1] - 108;
      p += 2;
    }
    else {
      return -1;
    }
    if (e->n < 3) {
      e->v[e->n] = val;
    }
    ++e->n;
  }
}

/** An integer operand, compactly. */
static void dput_int(GFNT_WBuf * b, int32_t v) {
  if (v >= -107 && v <= 107) {
    gfnt_wbuf_u8(b, (uint32_t)(v + 139));
  }
  else if (v >= 108 && v <= 1131) {
    uint32_t w = (uint32_t)(v - 108);

    gfnt_wbuf_u8(b, 247 + (w >> 8));
    gfnt_wbuf_u8(b, w & 0xFF);
  }
  else if (v >= -1131 && v <= -108) {
    uint32_t w = (uint32_t)(-v - 108);

    gfnt_wbuf_u8(b, 251 + (w >> 8));
    gfnt_wbuf_u8(b, w & 0xFF);
  }
  else if (v >= -32768 && v <= 32767) {
    gfnt_wbuf_u8(b, 28);
    gfnt_wbuf_u16(b, (uint32_t)v & 0xFFFF);
  }
  else {
    gfnt_wbuf_u8(b, 29);
    gfnt_wbuf_u32(b, (uint32_t)v);
  }
}

/** An integer operand in five bytes, so that its value never changes a length. */
static void dput_fixed(GFNT_WBuf * b, uint32_t v) {
  gfnt_wbuf_u8(b, 29);
  gfnt_wbuf_u32(b, v);
}

static void dput_op(GFNT_WBuf * b, uint16_t op) {
  if (op >= 0x0c00) {
    gfnt_wbuf_u8(b, 12);
    gfnt_wbuf_u8(b, op & 0xFF);
  }
  else {
    gfnt_wbuf_u8(b, op);
  }
}

/* ---- INDEX output ------------------------------------------------------- */

typedef struct Item {
  const uint8_t * p;
  size_t n;
} Item;

/** Write an INDEX of @p count items. */
static void write_index_ex(GFNT_WBuf * out, const Item * items, size_t count, bool wide) {
  size_t total = 0;
  size_t off_size = 1;
  size_t i;
  size_t at = 1;

  if (wide) {
    gfnt_wbuf_u32(out, (uint32_t)count);
  }
  else {
    gfnt_wbuf_u16(out, (uint32_t)count);
  }
  if (count == 0) {
    return;
  }
  for (i = 0; i < count; ++i) {
    total += items[i].n;
  }
  while (off_size < 4 && total + 1 >= ((size_t)1 << (8 * off_size))) {
    ++off_size;
  }
  gfnt_wbuf_u8(out, (uint32_t)off_size);
  for (i = 0; i <= count; ++i) {
    size_t k;

    for (k = off_size; k > 0; --k) {
      gfnt_wbuf_u8(out, (uint32_t)(at >> (8 * (k - 1))) & 0xFF);
    }
    if (i < count) {
      at += items[i].n;
    }
  }
  for (i = 0; i < count; ++i) {
    gfnt_wbuf_bytes(out, items[i].p, items[i].n);
  }
}

static void write_index(GFNT_WBuf * out, const Item * items, size_t count) {
  write_index_ex(out, items, count, false);
}

/* ---- The flattener ------------------------------------------------------ */

typedef struct Ent {
  int64_t v;        // 16.16
  uint8_t raw[5];
  uint8_t len;
  uint8_t pending;  // not yet written
} Ent;

typedef struct Flat {
  const Tab * tab;
  const GFNT_CffIndex * lsubrs;
  const GFNT_CffIndex * gsubrs;
  const GFNT_Limits * lim;
  GFNT_Error * error;
  uint32_t glyph;
  GFNT_WBuf out;
  GFNT_WBuf masks;          // the length of each hint mask written, in order
  Ent st[STACK_MAX];
  size_t depth;
  size_t max_stack;
  size_t nstems;
  size_t ops;
  bool width_checked;
  bool width_kept;
  bool ended;
  bool seac;
  bool drop_hint;
  bool cff2;
  const uint8_t * vstore;   // the item variation store, for the region counts blend needs
  size_t vstore_n;
  uint32_t vsindex;
  uint8_t seac_codes[2];
} Flat;

static GFNT_Result flat_fail(Flat * f, GFNT_Result code, const char * why) {
  return gfnt_error_set(f->error, code, GFNT_TAG_CFF, 0, f->glyph, why);
}

static void flush(Flat * f) {
  size_t i;

  for (i = 0; i < f->depth; ++i) {
    if (f->st[i].pending) {
      gfnt_wbuf_bytes(&f->out, f->st[i].raw, f->st[i].len);
      f->st[i].pending = 0;
    }
  }
}

static GFNT_Result push_num(Flat * f, int64_t v, const uint8_t * raw, size_t len) {
  Ent * e;

  if (f->depth >= f->max_stack) {
    return flat_fail(f, GFNT_ERR_CORRUPT, "a charstring stack deeper than the format allows");
  }
  e = &f->st[f->depth++];
  e->v = v;
  memcpy(e->raw, raw, len);
  e->len = (uint8_t)len;
  e->pending = 1;
  return GFNT_OK;
}

static void mark_unknown(Flat * f, size_t count) {
  size_t i;

  for (i = 0; i < count; ++i) {
    Ent * e = &f->st[f->depth - 1 - i];

    e->pending = 0;
    e->len = 0;
    e->v = 0;
  }
}

/** The stems a hint operator declares, and whether the bottom operand is the width. */
static GFNT_Result stem_args(Flat * f, size_t * pairs, bool * has_width) {
  size_t d = f->depth;
  bool w = f->width_kept;

  if (!f->cff2 && !f->width_checked && (d & 1)) {
    w = true;
  }
  if (d < (w ? 1u : 0u)) {
    return flat_fail(f, GFNT_ERR_CORRUPT, "a hint operator with no operands for its width");
  }
  *has_width = w;
  *pairs = (d - (w ? 1u : 0u)) / 2;
  return GFNT_OK;
}

/** Whether every operand above the width is still held back. */
static bool droppable(const Flat * f, bool has_width) {
  size_t i;

  for (i = has_width ? 1u : 0u; i < f->depth; ++i) {
    if (!f->st[i].pending) {
      return false;
    }
  }
  return true;
}

/** The regions one ItemVariationData names, or -1 when the store cannot say. */
static int64_t region_count(const uint8_t * store, size_t n, uint32_t vsindex) {
  size_t count;
  size_t at;

  if (!store || n < 8) {
    return -1;
  }
  count = ((size_t)store[6] << 8) | store[7];
  if (vsindex >= count || 8 + 4 * (size_t)vsindex + 4 > n) {
    return -1;
  }
  at = ((size_t)store[8 + 4 * vsindex] << 24) | ((size_t)store[9 + 4 * vsindex] << 16)
      | ((size_t)store[10 + 4 * vsindex] << 8) | store[11 + 4 * vsindex];
  if (at + 6 > n) {
    return -1;
  }
  return (int64_t)(((size_t)store[at + 4] << 8) | store[at + 5]);
}

static GFNT_Result flat_run(Flat * f, const uint8_t * b, size_t n, size_t level);

static GFNT_Result flat_call(Flat * f, bool global, size_t level) {
  const GFNT_CffIndex * ix = global ? f->gsubrs : f->lsubrs;
  const uint8_t * bytes;
  size_t length;
  int64_t number;
  size_t bias;

  if (f->depth == 0) {
    return flat_fail(f, GFNT_ERR_CORRUPT, "a subroutine call with nothing to call");
  }
  if (!f->st[f->depth - 1].pending) {
    return flat_fail(f, GFNT_ERR_UNSUPPORTED,
        "a subroutine number that arithmetic computed, which cannot be flattened");
  }
  number = f->st[f->depth - 1].v >> 16;
  --f->depth;
  bias = ix->count < 1240 ? 107 : ix->count < 33900 ? 1131 : 32768;
  number += (int64_t)bias;
  if (number < 0 || (uint64_t)number >= ix->count
      || !index_elem(f->tab, ix, (uint32_t)number, &bytes, &length)) {
    return flat_fail(f, GFNT_ERR_CORRUPT, "a call to a subroutine the font does not have");
  }
  if (level + 1 > f->lim->max_charstring_depth) {
    return flat_fail(f, GFNT_ERR_LIMIT, "subroutines nested deeper than the limit");
  }
  return flat_run(f, bytes, length, level + 1);
}

/* Arity of the arithmetic and stack operators: pops, pushes. */
static bool arithmetic(uint8_t c2, size_t * pops, size_t * pushes) {
  switch (c2) {
    case 3: case 4: case 10: case 11: case 12: case 15: case 24:
      *pops = 2; *pushes = 1; return true;
    case 5: case 9: case 14: case 21: case 26: case 29:
      *pops = 1; *pushes = 1; return true;
    case 18: *pops = 1; *pushes = 0; return true;
    case 20: case 30: *pops = 2; *pushes = 0; return true;
    case 22: *pops = 4; *pushes = 1; return true;
    case 23: *pops = 0; *pushes = 1; return true;
    case 27: *pops = 1; *pushes = 2; return true;
    case 28: *pops = 2; *pushes = 2; return true;
    default: return false;
  }
}

static GFNT_Result flat_run(Flat * f, const uint8_t * b, size_t n, size_t level) {
  size_t i = 0;
  GFNT_Result result;

  while (i < n && !f->ended) {
    uint8_t c = b[i];

    if (c >= 32 || c == 28) {
      int64_t v;
      size_t len;

      if (c == 28) {
        if (i + 3 > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a number cut short");
        }
        v = (int64_t)(int16_t)(((uint32_t)b[i + 1] << 8) | b[i + 2]) * 65536;
        len = 3;
      }
      else if (c <= 246) {
        v = ((int64_t)c - 139) * 65536;
        len = 1;
      }
      else if (c <= 250) {
        if (i + 2 > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a number cut short");
        }
        v = (((int64_t)c - 247) * 256 + b[i + 1] + 108) * 65536;
        len = 2;
      }
      else if (c <= 254) {
        if (i + 2 > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a number cut short");
        }
        v = (-((int64_t)c - 251) * 256 - b[i + 1] - 108) * 65536;
        len = 2;
      }
      else {
        if (i + 5 > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a number cut short");
        }
        v = (int32_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16)
            | ((uint32_t)b[i + 3] << 8) | b[i + 4]);
        len = 5;
      }
      result = push_num(f, v, b + i, len);
      if (result != GFNT_OK) {
        return result;
      }
      i += len;
      continue;
    }
    if (++f->ops > f->lim->max_charstring_ops) {
      return flat_fail(f, GFNT_ERR_LIMIT, "a charstring that runs more operators than the limit");
    }
    switch (c) {
      case 1: case 3: case 18: case 23: {
        size_t pairs;
        bool w;

        result = stem_args(f, &pairs, &w);
        if (result != GFNT_OK) {
          return result;
        }
        f->nstems += pairs;
        f->width_checked = true;
        if (f->drop_hint && droppable(f, w)) {
          f->depth = w ? 1 : 0;
          f->width_kept = w;
        }
        else {
          flush(f);
          gfnt_wbuf_u8(&f->out, c);
          f->depth = 0;
          f->width_kept = false;
        }
        ++i;
        break;
      }
      case 19: case 20: {
        size_t masklen;
        bool w = f->width_kept;

        if (f->depth > 0) {
          size_t pairs;

          result = stem_args(f, &pairs, &w);
          if (result != GFNT_OK) {
            return result;
          }
          f->nstems += pairs;
        }
        f->width_checked = true;
        masklen = (f->nstems + 7) / 8;
        if (i + 1 + masklen > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a hint mask that runs off the charstring");
        }
        if (f->drop_hint && droppable(f, w)) {
          f->depth = w ? 1 : 0;
          f->width_kept = w;
        }
        else {
          flush(f);
          gfnt_wbuf_u8(&f->out, c);
          gfnt_wbuf_bytes(&f->out, b + i + 1, masklen);
          gfnt_wbuf_u8(&f->masks, (uint32_t)masklen);
          f->depth = 0;
          f->width_kept = false;
        }
        i += 1 + masklen;
        break;
      }
      case 10:
      case 29:
        result = flat_call(f, c == 29, level);
        if (result != GFNT_OK) {
          return result;
        }
        ++i;
        break;
      case 11:
        return GFNT_OK;
      case 14: {
        size_t d = f->depth;

        if (f->cff2) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "endchar, which a CFF2 charstring does not have");
        }

        if (d == 4 || d == 5) {
          if (!f->st[d - 2].pending || !f->st[d - 1].pending) {
            return flat_fail(f, GFNT_ERR_UNSUPPORTED,
                "an accented character whose component codes arithmetic computed");
          }
          f->seac = true;
          f->seac_codes[0] = (uint8_t)(f->st[d - 2].v >> 16);
          f->seac_codes[1] = (uint8_t)(f->st[d - 1].v >> 16);
        }
        flush(f);
        gfnt_wbuf_u8(&f->out, 14);
        f->depth = 0;
        f->ended = true;
        ++i;
        break;
      }
      case 15: {
        if (!f->cff2) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "vsindex, which only a CFF2 charstring has");
        }
        if (f->depth == 0 || !f->st[f->depth - 1].pending) {
          return flat_fail(f, f->depth ? GFNT_ERR_UNSUPPORTED : GFNT_ERR_CORRUPT,
              "a vsindex with no literal operand");
        }
        if (f->st[f->depth - 1].v < 0) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a negative vsindex");
        }
        f->vsindex = (uint32_t)(f->st[f->depth - 1].v >> 16);
        flush(f);
        gfnt_wbuf_u8(&f->out, 15);
        --f->depth;
        ++i;
        break;
      }
      case 16: {
        int64_t n;
        int64_t k;
        size_t need;

        if (!f->cff2) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "blend, which only a CFF2 charstring has");
        }
        if (f->depth == 0 || !f->st[f->depth - 1].pending) {
          return flat_fail(f, f->depth ? GFNT_ERR_UNSUPPORTED : GFNT_ERR_CORRUPT,
              "a blend whose operand count is not a literal");
        }
        n = f->st[f->depth - 1].v >> 16;
        k = region_count(f->vstore, f->vstore_n, f->vsindex);
        if (k < 0 || n < 0 || n > 513) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a blend the variation store cannot size");
        }
        need = 1 + (size_t)n * (size_t)(k + 1);
        if (f->depth < need) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "a blend with fewer operands than it counts");
        }
        flush(f);
        gfnt_wbuf_u8(&f->out, 16);
        f->depth = f->depth - need + (size_t)n;
        mark_unknown(f, (size_t)n);
        ++i;
        break;
      }
      case 4: case 5: case 6: case 7: case 8: case 21: case 22: case 24: case 25:
      case 26: case 27: case 30: case 31:
        flush(f);
        gfnt_wbuf_u8(&f->out, c);
        f->depth = 0;
        f->width_checked = true;
        f->width_kept = false;
        ++i;
        break;
      case 12: {
        uint8_t c2;
        size_t pops;
        size_t pushes;

        if (i + 2 > n) {
          return flat_fail(f, GFNT_ERR_CORRUPT, "an escape operator cut short");
        }
        c2 = b[i + 1];
        if (c2 >= 34 && c2 <= 37) {
          flush(f);
          gfnt_wbuf_u8(&f->out, 12);
          gfnt_wbuf_u8(&f->out, c2);
          f->depth = 0;
          f->width_checked = true;
          f->width_kept = false;
        }
        else if (arithmetic(c2, &pops, &pushes)) {
          if (f->depth < pops) {
            return flat_fail(f, GFNT_ERR_CORRUPT, "an operator with too few operands");
          }
          if (f->depth - pops + pushes > f->max_stack) {
            return flat_fail(f, GFNT_ERR_CORRUPT,
                "a charstring stack deeper than the format allows");
          }
          flush(f);
          gfnt_wbuf_u8(&f->out, 12);
          gfnt_wbuf_u8(&f->out, c2);
          f->depth = f->depth - pops + pushes;
          mark_unknown(f, pushes);
        }
        else {
          return flat_fail(f, GFNT_ERR_CORRUPT, "an operator Type 2 does not have");
        }
        i += 2;
        break;
      }
      default:
        return flat_fail(f, GFNT_ERR_CORRUPT, "an operator Type 2 does not have");
    }
  }
  return GFNT_OK;
}

/* ---- Source structure --------------------------------------------------- */

typedef struct Src {
  const GFNT_Face * face;
  const GFNT_Cff * cff;        // NULL for a CFF2
  const GFNT_Cff2 * c2;        // NULL for a CFF
  bool cff2;
  bool fds;                    // Font DICTs: a CID-keyed CFF, or any CFF2
  bool rename;                 // the font is being renamed: its names are not carried
  Tab tab;
  const uint8_t * top;
  size_t top_n;
  const GFNT_CffIndex * charstrings;
  const GFNT_CffIndex * gsubrs;
  const GFNT_CffIndex * fdarray;
  size_t fdselect;
  const uint8_t * vstore;      // the item variation store (CFF2), without its length
  size_t vstore_n;
} Src;

static GFNT_Result src_open(const GFNT_Face * face, Src * s, GFNT_Error * error) {
  GFNT_Result result;

  memset(s, 0, sizeof *s);
  s->face = face;
  if (gfnt_face_has_table(face, GFNT_TAG_CFF2)) {
    size_t head;
    size_t top_n;

    s->cff2 = true;
    s->fds = true;
    result = gfnt_face_cff2(face, &s->c2, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (!load_tab(face, GFNT_TAG_CFF2, &s->tab) || s->tab.n < 5) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0, GFNT_GLYPH_NONE,
          "a CFF2 with no header");
    }
    head = s->tab.p[2];
    top_n = ((size_t)s->tab.p[3] << 8) | s->tab.p[4];
    if (head < 5 || head + top_n > s->tab.n) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF2, 0, GFNT_GLYPH_NONE,
          "a CFF2 Top DICT that runs off the table");
    }
    s->top = s->tab.p + head;
    s->top_n = top_n;
    s->charstrings = &s->c2->charstrings;
    s->gsubrs = &s->c2->gsubrs;
    s->fdarray = &s->c2->fdarray;
    s->fdselect = s->c2->fdselect;
    if (s->c2->vstore && s->c2->vstore + s->c2->vstore_length <= s->tab.n) {
      s->vstore = s->tab.p + s->c2->vstore;
      s->vstore_n = s->c2->vstore_length;
    }
    return GFNT_OK;
  }
  result = gfnt_face_cff(face, &s->cff, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (s->cff->charstring_type != 2) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a CFF whose charstrings are Type 1 cannot be subset");
  }
  s->fds = s->cff->is_cid;
  s->charstrings = &s->cff->charstrings;
  s->gsubrs = &s->cff->gsubrs;
  s->fdarray = &s->cff->fdarray;
  s->fdselect = s->cff->fdselect;
  if (!load_tab(face, GFNT_TAG_CFF, &s->tab)
      || !index_elem(&s->tab, &s->cff->top_dicts, 0, &s->top, &s->top_n)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a CFF with no Top DICT");
  }
  return GFNT_OK;
}

/** Run one glyph's charstring through the flattener. */
static GFNT_Result flatten_glyph(const Src * s, uint32_t glyph, bool drop_hint,
    const GFNT_Limits * limits, Flat * f, GFNT_Error * error) {
  const uint8_t * bytes;
  size_t length;
  GFNT_CffIndex subrs;
  GFNT_Result result;

  if (!index_elem(&s->tab, s->charstrings, glyph, &bytes, &length)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, glyph,
        "a glyph the CharStrings INDEX does not have");
  }
  f->vsindex = 0;
  if (s->cff2) {
    GFNT_Cff2Font font;

    result = gfnt_cff2_font_for_glyph(s->face, s->c2, glyph, &font, error);
    subrs = font.subrs;
    f->vsindex = font.vsindex;
  }
  else {
    GFNT_CffPrivate priv;

    result = gfnt_cff_private_for_glyph(s->face, s->cff, glyph, &priv, error);
    subrs = priv.subrs;
  }
  if (result != GFNT_OK) {
    return result;
  }
  f->out.length = 0;
  f->masks.length = 0;
  f->tab = &s->tab;
  f->lsubrs = &subrs;
  f->gsubrs = s->gsubrs;
  f->cff2 = s->cff2;
  f->vstore = s->vstore;
  f->vstore_n = s->vstore_n;
  f->lim = limits;
  f->error = error;
  f->glyph = glyph;
  f->depth = 0;
  f->max_stack = s->cff2 ? GFNT_CFF2_DICT_MAX_OPERANDS : GFNT_CFF_DICT_MAX_OPERANDS;
  f->nstems = 0;
  f->ops = 0;
  f->width_checked = false;
  f->width_kept = false;
  f->ended = false;
  f->seac = false;
  f->drop_hint = drop_hint;
  result = flat_run(f, bytes, length, 0);
  if (result != GFNT_OK) {
    return result;
  }
  flush(f);
  if (f->out.oom) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, glyph, "flattening a charstring");
  }
  return GFNT_OK;
}

static GFNT_Result seac_glyph(const Src * s, uint8_t code, uint32_t host,
    uint32_t * out, GFNT_Error * error) {
  uint32_t sid = gfnt_cff_standard_encoding[code];

  if (sid == 0 || gfnt_cff_glyph_for_sid(s->face, s->cff, sid, out, NULL) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, host,
        "an accented character built from a glyph the font does not have");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_subset_cff_closure(const GFNT_Face * face, uint8_t * keep, size_t count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_Error * error) {
  Src s;
  Flat * f;
  uint32_t * work;
  size_t sp = 0;
  uint32_t g;
  GFNT_Result result = src_open(face, &s, error);

  if (result != GFNT_OK || s.cff2) {
    return result;      // a CFF2 charstring has no accented characters
  }
  f = allocator->calloc_fn(allocator->ctx, 1, sizeof *f);
  work = allocator->calloc_fn(allocator->ctx, count, sizeof *work);
  if (!f || !work) {
    allocator->free_fn(allocator->ctx, f);
    allocator->free_fn(allocator->ctx, work);
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "allocating the accent closure");
  }
  gfnt_wbuf_init(&f->out, allocator);
  gfnt_wbuf_init(&f->masks, allocator);
  for (g = 0; g < count; ++g) {
    if (keep[g]) {
      work[sp++] = g;
    }
  }
  while (sp && result == GFNT_OK) {
    uint32_t top = work[--sp];

    result = flatten_glyph(&s, top, false, limits, f, error);
    if (result == GFNT_OK && f->seac) {
      int k;

      for (k = 0; k < 2 && result == GFNT_OK; ++k) {
        uint32_t part = 0;

        result = seac_glyph(&s, f->seac_codes[k], top, &part, error);
        if (result == GFNT_OK && part < count && !keep[part]) {
          keep[part] = 1;
          work[sp++] = part;
        }
      }
    }
  }
  gfnt_wbuf_free(&f->out);
  gfnt_wbuf_free(&f->masks);
  allocator->free_fn(allocator->ctx, f);
  allocator->free_fn(allocator->ctx, work);
  return result;
}

/* ---- The table ---------------------------------------------------------- */

/** Which Font DICT a glyph belongs to. */
static size_t fd_of(const Src * s, uint32_t glyph) {
  const Tab * t = &s->tab;
  size_t at = s->fdselect;

  if (!s->fds || at == 0 || at >= t->n) {
    return 0;
  }
  if (t->p[at] == 0) {
    return at + 1 + glyph < t->n ? t->p[at + 1 + glyph] : 0;
  }
  if (t->p[at] == 3 && at + 3 <= t->n) {
    size_t ranges = ((size_t)t->p[at + 1] << 8) | t->p[at + 2];
    size_t r;

    for (r = 0; r < ranges; ++r) {
      size_t p = at + 3 + r * 3;
      size_t first;
      size_t next;

      if (p + 5 > t->n) {
        break;
      }
      first = ((size_t)t->p[p] << 8) | t->p[p + 1];
      next = ((size_t)t->p[p + 3] << 8) | t->p[p + 4];
      if (glyph >= first && glyph < next) {
        return t->p[p + 2];
      }
    }
  }
  if (t->p[at] == 4 && at + 5 <= t->n) {
    size_t ranges = ((size_t)t->p[at + 1] << 24) | ((size_t)t->p[at + 2] << 16)
        | ((size_t)t->p[at + 3] << 8) | t->p[at + 4];
    size_t r;

    for (r = 0; r < ranges; ++r) {
      size_t p = at + 5 + r * 6;
      size_t first;
      size_t next;

      if (p + 10 > t->n) {
        break;
      }
      first = ((size_t)t->p[p] << 24) | ((size_t)t->p[p + 1] << 16)
          | ((size_t)t->p[p + 2] << 8) | t->p[p + 3];
      next = ((size_t)t->p[p + 6] << 24) | ((size_t)t->p[p + 7] << 16)
          | ((size_t)t->p[p + 8] << 8) | t->p[p + 9];
      if (glyph >= first && glyph < next) {
        return ((size_t)t->p[p + 4] << 8) | t->p[p + 5];
      }
    }
  }
  return 0;
}

static bool is_sid_op(uint16_t op) {
  return op <= 4 || op == 0x0c00 || op == 0x0c15 || op == 0x0c16 || op == 0x0c26;
}

/** The strings still named, sorted, and what each old SID became. */
typedef struct SidMap {
  uint32_t * old;
  size_t count;
} SidMap;

static int cmp_u32(const void * a, const void * b) {
  const uint32_t * x = a;
  const uint32_t * y = b;

  return *x < *y ? -1 : *x > *y;
}

static uint32_t map_sid(const SidMap * m, uint32_t sid) {
  size_t lo = 0;
  size_t hi = m->count;

  if (sid < GFNT_CFF_STANDARD_STRING_COUNT) {
    return sid;
  }
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;

    if (m->old[mid] < sid) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  return GFNT_CFF_STANDARD_STRING_COUNT + (uint32_t)lo;
}

/** Whether an SID names a string this font has. */
static bool sid_valid(const Src * s, uint32_t sid) {
  return sid < GFNT_CFF_STANDARD_STRING_COUNT
      || sid - GFNT_CFF_STANDARD_STRING_COUNT < s->cff->strings.count;
}

typedef struct Plan {
  size_t nfd;               // Font DICTs written (1 for a name-keyed font)
  size_t nsrc;              // Font DICTs the source has
  size_t * fd_src;          // new FD -> source FD
  uint32_t * fd_new;        // source FD -> new FD
  uint8_t * fd_used;
} Plan;

/** Write one DICT's SID-carrying and plain operators, skipping @p skip. */
/** The Top and Font DICT entries that name the font: FullName, FamilyName and a Font DICT's FontName. */
static bool names_the_font(uint16_t op) {
  return op == 2 || op == 3 || op == 0x0c26;
}

typedef bool (*SkipFn)(uint16_t op, bool drop_hint);

static bool skip_top(uint16_t op, bool drop_hint) {
  (void)drop_hint;
  return op == 15 || op == 16 || op == 17 || op == 18 || op == 24 || op == 0x0c24
      || op == 0x0c25;
}

static bool skip_font(uint16_t op, bool drop_hint) {
  (void)drop_hint;
  return op == 18;
}

static bool skip_private(uint16_t op, bool drop_hint) {
  if (op == 19) {
    return true;
  }
  if (!drop_hint) {
    return false;
  }
  switch (op) {
    case 6: case 7: case 8: case 9: case 10: case 11:
    case 0x0c09: case 0x0c0a: case 0x0c0b: case 0x0c0c: case 0x0c0d: case 0x0c0e:
    case 0x0c11: case 0x0c12: case 0x0c13:
      return true;
    default:
      return false;
  }
}

static GFNT_Result copy_dict(const Src * s, const uint8_t * d, size_t n, SkipFn skip,
    bool drop_hint, const SidMap * map, GFNT_WBuf * out, GFNT_Error * error) {
  size_t pos = 0;
  DictEnt e;
  int got;
  bool held = false;      // a CFF2 blend, whose results the next operator takes
  size_t held_a = 0;
  size_t held_end = 0;

  while ((got = dict_next(d, n, &pos, &e)) == 1) {
    if (s->cff2 && e.op == 23) {
      held = true;
      held_a = e.a;
      held_end = e.end;
      continue;
    }
    if (skip(e.op, drop_hint)) {
      held = false;
      continue;
    }
    if (held) {
      gfnt_wbuf_bytes(out, d + held_a, held_end - held_a);
      held = false;
    }
    if (s->rename && names_the_font(e.op)) {
      held = false;
      continue;
    }
    if (!s->cff2 && is_sid_op(e.op) && e.n >= 1) {
      if (!sid_valid(s, (uint32_t)e.v[0]) || e.v[0] < 0) {
        continue;       // a name the font does not have: left out, as the reader leaves it
      }
      dput_int(out, (int32_t)map_sid(map, (uint32_t)e.v[0]));
      dput_op(out, e.op);
      continue;
    }
    if (!s->cff2 && e.op == 0x0c1e && e.n == 3) {
      if (e.v[0] < 0 || e.v[1] < 0 || !sid_valid(s, (uint32_t)e.v[0])
          || !sid_valid(s, (uint32_t)e.v[1])) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
            "a ROS naming a string the font does not have");
      }
      dput_int(out, (int32_t)map_sid(map, (uint32_t)e.v[0]));
      dput_int(out, (int32_t)map_sid(map, (uint32_t)e.v[1]));
      dput_int(out, e.v[2]);
      dput_op(out, e.op);
      continue;
    }
    gfnt_wbuf_bytes(out, d + e.a, e.end - e.a);
  }
  if (got < 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a malformed DICT");
  }
  return GFNT_OK;
}

/** Collect the custom SIDs one DICT names. */
static GFNT_Result collect_sids(const Src * s, const uint8_t * d, size_t n,
    uint32_t * list, size_t * used, size_t cap, GFNT_Error * error) {
  size_t pos = 0;
  DictEnt e;
  int got;

  while ((got = dict_next(d, n, &pos, &e)) == 1) {
    size_t k;
    size_t take = 0;

    if (s->rename && names_the_font(e.op)) {
      continue;
    }
    if (is_sid_op(e.op) && e.n >= 1) {
      take = 1;
    }
    else if (e.op == 0x0c1e && e.n == 3) {
      take = 2;
    }
    for (k = 0; k < take; ++k) {
      if (e.v[k] >= GFNT_CFF_STANDARD_STRING_COUNT && sid_valid(s, (uint32_t)e.v[k])
          && *used < cap) {
        list[(*used)++] = (uint32_t)e.v[k];
      }
    }
  }
  if (got < 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a malformed DICT");
  }
  return GFNT_OK;
}

/** The Private DICT a Top or Font DICT names: its size and offset. */
static bool private_of(const uint8_t * d, size_t n, size_t * size, size_t * offset) {
  size_t pos = 0;
  DictEnt e;
  int got;

  while ((got = dict_next(d, n, &pos, &e)) == 1) {
    if (e.op == 18 && e.n >= 2 && e.v[0] >= 0 && e.v[1] >= 0) {
      *size = (size_t)e.v[0];
      *offset = (size_t)e.v[1];
      return true;
    }
  }
  return false;
}

/** The Font DICT and the bytes of a source Private DICT, for FD @p k. */
static GFNT_Result source_private(const Src * s, size_t k, const uint8_t ** font,
    size_t * font_n, const uint8_t ** priv, size_t * priv_n, GFNT_Error * error) {
  size_t size = 0;
  size_t offset = 0;
  const uint8_t * d;
  size_t dn;

  if (s->fds) {
    if (!index_elem(&s->tab, s->fdarray, (uint32_t)k, font, font_n)) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
          "a glyph in a Font DICT the FDArray does not have");
    }
    d = *font;
    dn = *font_n;
  }
  else {
    *font = NULL;
    *font_n = 0;
    d = s->top;
    dn = s->top_n;
  }
  if (!private_of(d, dn, &size, &offset)) {
    *priv = NULL;
    *priv_n = 0;
    return GFNT_OK;
  }
  if (offset > s->tab.n || size > s->tab.n - offset) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a Private DICT that runs off the table");
  }
  *priv = s->tab.p + offset;
  *priv_n = size;
  return GFNT_OK;
}

typedef struct Charset {
  int format;            // -1 predefined ISOAdobe, else 0, 1 or 2
  GFNT_WBuf bytes;
} Charset;

static void build_charset(Charset * c, const uint32_t * v, size_t n, bool cid) {
  size_t j;
  size_t ranges = 0;
  size_t longest = 0;
  size_t f0;
  size_t f1;
  size_t f2;
  bool iso = !cid && n - 1 <= 228;

  for (j = 1; j < n && iso; ++j) {
    iso = v[j] == j;
  }
  if (iso) {
    c->format = -1;
    return;
  }
  for (j = 1; j < n;) {
    size_t k = j;

    while (k + 1 < n && v[k + 1] == v[k] + 1 && k + 1 - j < 65535) {
      ++k;
    }
    ++ranges;
    if (k - j > longest) {
      longest = k - j;
    }
    j = k + 1;
  }
  f0 = 1 + 2 * (n - 1);
  f1 = longest <= 255 ? 1 + 3 * ranges : (size_t)-1;
  f2 = 1 + 4 * ranges;
  if (f0 <= f1 && f0 <= f2) {
    c->format = 0;
    gfnt_wbuf_u8(&c->bytes, 0);
    for (j = 1; j < n; ++j) {
      gfnt_wbuf_u16(&c->bytes, v[j]);
    }
    return;
  }
  c->format = f1 <= f2 ? 1 : 2;
  gfnt_wbuf_u8(&c->bytes, (uint32_t)c->format);
  for (j = 1; j < n;) {
    size_t k = j;

    while (k + 1 < n && v[k + 1] == v[k] + 1 && k + 1 - j < 65535) {
      ++k;
    }
    gfnt_wbuf_u16(&c->bytes, v[j]);
    if (c->format == 1) {
      gfnt_wbuf_u8(&c->bytes, (uint32_t)(k - j));
    }
    else {
      gfnt_wbuf_u16(&c->bytes, (uint32_t)(k - j));
    }
    j = k + 1;
  }
}


typedef struct Build {
  const Src * s;
  const SidMap * map;
  const Plan * plan;
  bool explicit_charset;
  bool has_fdselect;
  size_t charset_off, fdselect_off, charstrings_off, fdarray_off, vstore_off;
  size_t * private_off;
  size_t * private_size;
} Build;

static GFNT_Result write_top(const Build * b, GFNT_WBuf * out, GFNT_Error * error) {
  GFNT_Result result = copy_dict(b->s, b->s->top, b->s->top_n, skip_top, false, b->map,
      out, error);

  if (result != GFNT_OK) {
    return result;
  }
  if (b->explicit_charset) {
    dput_fixed(out, (uint32_t)b->charset_off);
    dput_op(out, 15);
  }
  dput_fixed(out, (uint32_t)b->charstrings_off);
  dput_op(out, 17);
  if (b->s->fds) {
    dput_fixed(out, (uint32_t)b->fdarray_off);
    dput_op(out, 0x0c24);
    if (b->has_fdselect) {
      dput_fixed(out, (uint32_t)b->fdselect_off);
      dput_op(out, 0x0c25);
    }
  }
  else {
    dput_fixed(out, (uint32_t)b->private_size[0]);
    dput_fixed(out, (uint32_t)b->private_off[0]);
    dput_op(out, 18);
  }
  if (b->s->cff2 && b->s->vstore) {
    dput_fixed(out, (uint32_t)b->vstore_off);
    dput_op(out, 24);
  }
  return GFNT_OK;
}

static GFNT_Result write_font_dict(const Build * b, size_t k, GFNT_WBuf * out,
    GFNT_Error * error) {
  const uint8_t * font;
  size_t font_n;
  const uint8_t * priv;
  size_t priv_n;
  GFNT_Result result = source_private(b->s, b->plan->fd_src[k], &font, &font_n, &priv,
      &priv_n, error);

  if (result != GFNT_OK) {
    return result;
  }
  result = copy_dict(b->s, font, font_n, skip_font, false, b->map, out, error);
  if (result != GFNT_OK) {
    return result;
  }
  dput_fixed(out, (uint32_t)b->private_size[k]);
  dput_fixed(out, (uint32_t)b->private_off[k]);
  dput_op(out, 18);
  return GFNT_OK;
}

/** The Font DICT INDEX of a font that has them, for the offsets as they currently stand. */
static GFNT_Result build_fd_index(const Build * b, Item * items, GFNT_WBuf * out,
    const GFNT_Allocator * a, GFNT_Error * error) {
  GFNT_WBuf dicts;
  size_t k;
  size_t consumed = 0;
  GFNT_Result result = GFNT_OK;

  gfnt_wbuf_init(&dicts, a);
  for (k = 0; k < b->plan->nfd && result == GFNT_OK; ++k) {
    size_t before = dicts.length;

    result = write_font_dict(b, k, &dicts, error);
    items[k].n = dicts.length - before;
  }
  if (result == GFNT_OK && !dicts.oom) {
    for (k = 0; k < b->plan->nfd; ++k) {
      items[k].p = dicts.data + consumed;
      consumed += items[k].n;
    }
    write_index_ex(out, items, b->plan->nfd, b->s->cff2);
  }
  else if (result == GFNT_OK) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "building the Font DICTs");
  }
  gfnt_wbuf_free(&dicts);
  return result;
}

/** FDSelect for the kept glyphs, in whichever format is smallest and can say it. */
static void build_fdselect(const Src * s, const Plan * plan, const uint16_t * fd_of_new,
    size_t new_count, GFNT_WBuf * out) {
  size_t ranges = 0;
  size_t j;
  size_t f0 = 1 + new_count;
  size_t f3;

  for (j = 0; j < new_count; ++j) {
    if (j == 0 || fd_of_new[j] != fd_of_new[j - 1]) {
      ++ranges;
    }
  }
  f3 = 5 + 3 * ranges;
  if (plan->nfd > 256) {
    gfnt_wbuf_u8(out, 4);
    gfnt_wbuf_u32(out, (uint32_t)ranges);
    for (j = 0; j < new_count; ++j) {
      if (j == 0 || fd_of_new[j] != fd_of_new[j - 1]) {
        gfnt_wbuf_u32(out, (uint32_t)j);
        gfnt_wbuf_u16(out, plan->fd_new[fd_of_new[j]]);
      }
    }
    gfnt_wbuf_u32(out, (uint32_t)new_count);
    return;
  }
  (void)s;
  if (f0 <= f3) {
    gfnt_wbuf_u8(out, 0);
    for (j = 0; j < new_count; ++j) {
      gfnt_wbuf_u8(out, plan->fd_new[fd_of_new[j]]);
    }
    return;
  }
  gfnt_wbuf_u8(out, 3);
  gfnt_wbuf_u16(out, (uint32_t)ranges);
  for (j = 0; j < new_count; ++j) {
    if (j == 0 || fd_of_new[j] != fd_of_new[j - 1]) {
      gfnt_wbuf_u16(out, (uint32_t)j);
      gfnt_wbuf_u8(out, plan->fd_new[fd_of_new[j]]);
    }
  }
  gfnt_wbuf_u16(out, (uint32_t)new_count);
}

GFNT_Result gfnt_subset_cff(const GFNT_Face * face, const uint32_t * old_of_new,
    size_t new_count, size_t count, bool drop_hinting, bool subroutines, const char * ps_name,
    const GFNT_Limits * limits, const GFNT_Allocator * a, GFNT_WBuf * out, GFNT_Error * error) {
  Src s;
  Flat * f = NULL;
  GFNT_WBuf cs;
  GFNT_WBuf fdsel, top, top_ix, str_ix, gsub_ix, cs_ix, fd_ix, name_ix, vstore_buf;
  GFNT_WBuf * privates = NULL;
  GFNT_WBuf maskbuf;
  GFNT_WBuf sub_cs, sub_bodies;
  size_t * sub_at = NULL;
  size_t nsub = 0;
  size_t * mask_at = NULL;
  size_t * new_at = NULL;
  Item * sub_items = NULL;
  size_t * cs_at = NULL;
  uint32_t * values = NULL;
  uint16_t * fd_of_new = NULL;
  uint32_t * sids = NULL;
  size_t nsids = 0;
  size_t sid_cap = 0;
  SidMap map = {NULL, 0};
  Plan plan;
  Build b;
  Charset charset;
  Item * items = NULL;
  size_t j;
  size_t k;
  GFNT_Result result;

  memset(&plan, 0, sizeof plan);
  memset(&b, 0, sizeof b);
  gfnt_wbuf_init(&cs, a);
  gfnt_wbuf_init(&maskbuf, a);
  gfnt_wbuf_init(&sub_cs, a);
  gfnt_wbuf_init(&sub_bodies, a);
  gfnt_wbuf_init(&fdsel, a);
  gfnt_wbuf_init(&top, a);
  gfnt_wbuf_init(&name_ix, a);
  gfnt_wbuf_init(&top_ix, a);
  gfnt_wbuf_init(&str_ix, a);
  gfnt_wbuf_init(&gsub_ix, a);
  gfnt_wbuf_init(&cs_ix, a);
  gfnt_wbuf_init(&fd_ix, a);
  gfnt_wbuf_init(&vstore_buf, a);
  gfnt_wbuf_init(&charset.bytes, a);
  charset.format = -1;

  result = src_open(face, &s, error);
  if (result != GFNT_OK) {
    goto done;
  }
  s.rename = ps_name != NULL;
  plan.nsrc = s.fds ? s.fdarray->count : 1;
  if (plan.nsrc == 0 || plan.nsrc > 65535 || (!s.cff2 && plan.nsrc > 256)) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "a font with no Font DICT, or more than its format allows");
    goto done;
  }
  f = a->calloc_fn(a->ctx, 1, sizeof *f);
  cs_at = a->calloc_fn(a->ctx, new_count + 1, sizeof *cs_at);
  mask_at = a->calloc_fn(a->ctx, new_count + 1, sizeof *mask_at);
  new_at = a->calloc_fn(a->ctx, new_count + 1, sizeof *new_at);
  values = a->calloc_fn(a->ctx, new_count + 1, sizeof *values);
  fd_of_new = a->calloc_fn(a->ctx, new_count + 1, sizeof *fd_of_new);
  sid_cap = new_count + 600;
  sids = a->calloc_fn(a->ctx, sid_cap, sizeof *sids);
  plan.fd_src = a->calloc_fn(a->ctx, plan.nsrc, sizeof *plan.fd_src);
  plan.fd_new = a->calloc_fn(a->ctx, plan.nsrc, sizeof *plan.fd_new);
  plan.fd_used = a->calloc_fn(a->ctx, plan.nsrc, 1);
  privates = a->calloc_fn(a->ctx, plan.nsrc, sizeof *privates);
  b.private_off = a->calloc_fn(a->ctx, plan.nsrc, sizeof *b.private_off);
  b.private_size = a->calloc_fn(a->ctx, plan.nsrc, sizeof *b.private_size);
  if (!f || !cs_at || !mask_at || !new_at || !values || !fd_of_new || !sids || !plan.fd_src || !plan.fd_new
      || !plan.fd_used || !privates || !b.private_off || !b.private_size) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "allocating the CFF subset");
    goto done;
  }
  for (k = 0; k < plan.nsrc; ++k) {
    gfnt_wbuf_init(&privates[k], a);
  }
  gfnt_wbuf_init(&f->out, a);
  gfnt_wbuf_init(&f->masks, a);
  b.s = &s;
  b.map = &map;
  b.plan = &plan;

  // Charstrings, the names (or CIDs) they carry, and the Font DICT each belongs to.
  for (j = 0; j < new_count; ++j) {
    uint32_t g = old_of_new[j];
    uint32_t named = g != NO_GLYPH ? g : (uint32_t)j;
    uint32_t sid = 0;
    size_t fd;

    cs_at[j] = cs.length;
    mask_at[j] = maskbuf.length;
    if (g == NO_GLYPH) {
      if (!s.cff2) {
        gfnt_wbuf_u8(&cs, 14);
      }
      fd = fd_of(&s, 0);
    }
    else {
      result = flatten_glyph(&s, g, drop_hinting, limits, f, error);
      if (result != GFNT_OK) {
        goto done;
      }
      gfnt_wbuf_bytes(&cs, f->out.data, f->out.length);
      gfnt_wbuf_bytes(&maskbuf, f->masks.data, f->masks.length);
      fd = fd_of(&s, g);
    }
    if (named >= count) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, named,
          "a glyph past the face's glyph count");
      goto done;
    }
    if (j > 0 && !s.cff2) {
      result = gfnt_cff_glyph_sid(face, s.cff, named, &sid, error);
      if (result != GFNT_OK) {
        goto done;
      }
    }
    values[j] = sid;
    if (fd >= plan.nsrc) {
      fd = 0;
    }
    fd_of_new[j] = (uint16_t)fd;
    plan.fd_used[fd] = 1;
    if (cs.oom || cs.length > limits->max_blob_bytes) {
      result = gfnt_error_set(error, cs.oom ? GFNT_ERR_OOM : GFNT_ERR_LIMIT, GFNT_TAG_CFF,
          0, GFNT_GLYPH_NONE, "building the CharStrings");
      goto done;
    }
  }
  cs_at[new_count] = cs.length;
  mask_at[new_count] = maskbuf.length;
  if (maskbuf.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "building the CharStrings");
    goto done;
  }

  // Sequences that several charstrings repeat become global subroutines. The ceiling on how
  // deeply they call each other leaves the reader's own limit room for the charstring's call.
  if (subroutines && limits->max_charstring_depth > 2) {
    const size_t ceiling = limits->max_charstring_depth - 2 < 8 ? limits->max_charstring_depth - 2 : 8;

    result = gfnt_cff_subroutinize(a, cs.data, cs_at, maskbuf.data, mask_at, new_count,
        (uint8_t)ceiling, &sub_cs, new_at, &sub_bodies, &sub_at, &nsub, error);
    if (result != GFNT_OK) {
      goto done;
    }
    if (nsub) {
      sub_items = a->calloc_fn(a->ctx, nsub, sizeof *sub_items);
      if (!sub_items) {
        result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
            "building the subroutines");
        goto done;
      }
      for (j = 0; j < nsub; ++j) {
        sub_items[j].p = sub_bodies.data + sub_at[j];
        sub_items[j].n = sub_at[j + 1] - sub_at[j];
      }
    }
  }

  // Font DICTs: the ones still used, in their order.
  if (s.fds) {
    for (k = 0; k < plan.nsrc; ++k) {
      if (plan.fd_used[k]) {
        plan.fd_new[k] = (uint32_t)plan.nfd;
        plan.fd_src[plan.nfd++] = k;
      }
    }
  }
  else {
    plan.nfd = 1;
    plan.fd_src[0] = 0;
  }
  items = a->calloc_fn(a->ctx, plan.nfd + new_count + sid_cap + 4, sizeof *items);
  if (!items) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "allocating the CFF subset");
    goto done;
  }

  if (!s.cff2) {
    // Strings still named.
    result = collect_sids(&s, s.top, s.top_n, sids, &nsids, sid_cap, error);
    for (k = 0; k < plan.nfd && result == GFNT_OK && s.fds; ++k) {
      const uint8_t * font;
      size_t font_n;
      const uint8_t * priv;
      size_t priv_n;

      result = source_private(&s, plan.fd_src[k], &font, &font_n, &priv, &priv_n, error);
      if (result == GFNT_OK) {
        result = collect_sids(&s, font, font_n, sids, &nsids, sid_cap, error);
      }
    }
    if (result != GFNT_OK) {
      goto done;
    }
    if (!s.fds) {
      for (j = 1; j < new_count; ++j) {
        if (values[j] >= GFNT_CFF_STANDARD_STRING_COUNT) {
          if (!sid_valid(&s, values[j])) {
            result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, j,
                "a charset naming a string the font does not have");
            goto done;
          }
          if (nsids < sid_cap) {
            sids[nsids++] = values[j];
          }
        }
      }
    }
    qsort(sids, nsids, sizeof *sids, cmp_u32);
    {
      size_t u = 0;

      for (j = 0; j < nsids; ++j) {
        if (u == 0 || sids[u - 1] != sids[j]) {
          sids[u++] = sids[j];
        }
      }
      map.old = sids;
      map.count = u;
    }
    for (j = 0; j < map.count; ++j) {
      if (!index_elem(&s.tab, &s.cff->strings, map.old[j] - GFNT_CFF_STANDARD_STRING_COUNT,
              &items[j].p, &items[j].n)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
            "a String INDEX element out of range");
        goto done;
      }
    }
    write_index(&str_ix, items, map.count);

    // Charset.
    if (!s.fds) {
      for (j = 1; j < new_count; ++j) {
        values[j] = map_sid(&map, values[j]);
      }
    }
    build_charset(&charset, values, new_count, s.fds);
    b.explicit_charset = charset.format >= 0;
  }

  // FDSelect: a CFF2 with one Font DICT states none.
  if (s.fds && !(s.cff2 && plan.nfd == 1)) {
    build_fdselect(&s, &plan, fd_of_new, new_count, &fdsel);
    b.has_fdselect = true;
  }

  // Private DICTs.
  for (k = 0; k < plan.nfd; ++k) {
    const uint8_t * font;
    size_t font_n;
    const uint8_t * priv;
    size_t priv_n;

    result = source_private(&s, plan.fd_src[k], &font, &font_n, &priv, &priv_n, error);
    if (result == GFNT_OK && priv) {
      result = copy_dict(&s, priv, priv_n, skip_private, drop_hinting, &map, &privates[k],
          error);
    }
    if (result != GFNT_OK) {
      goto done;
    }
    b.private_size[k] = privates[k].length;
  }

  // The fixed parts, to measure, and then the offsets.
  {
    Item one;
    size_t at;

    if (!s.cff2) {
      const uint8_t * name;
      size_t name_n;

      if (ps_name) {
        name = (const uint8_t *)ps_name;
        name_n = strlen(ps_name);
      }
      else if (!index_elem(&s.tab, &s.cff->names, 0, &name, &name_n)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
            "a CFF with no name");
        goto done;
      }
      one.p = name;
      one.n = name_n;
      write_index(&name_ix, &one, 1);
    }
    write_index_ex(&gsub_ix, sub_items, nsub, s.cff2);
    for (j = 0; j < new_count; ++j) {
      if (nsub) {
        items[j].p = sub_cs.data + new_at[j];
        items[j].n = new_at[j + 1] - new_at[j];
      }
      else {
        items[j].p = cs.data + cs_at[j];
        items[j].n = cs_at[j + 1] - cs_at[j];
      }
    }
    write_index_ex(&cs_ix, items, new_count, s.cff2);
    if (s.cff2 && s.vstore) {
      gfnt_wbuf_u16(&vstore_buf, (uint32_t)s.vstore_n);
      gfnt_wbuf_bytes(&vstore_buf, s.vstore, s.vstore_n);
    }

    // The Top DICT's length is fixed by its five-byte integers, so a first pass with
    // offsets of zero measures it.
    result = write_top(&b, &top, error);
    if (result != GFNT_OK) {
      goto done;
    }
    if (s.cff2) {
      at = 5 + top.length;
    }
    else {
      one.p = top.data;
      one.n = top.length;
      write_index(&top_ix, &one, 1);
      at = 4 + name_ix.length + top_ix.length + str_ix.length;
    }
    at += gsub_ix.length;
    b.charset_off = at;
    at += charset.bytes.length;
    if (s.cff2) {
      b.charstrings_off = at;
      at += cs_ix.length;
      b.fdselect_off = at;
      at += fdsel.length;
    }
    else {
      b.fdselect_off = at;
      at += fdsel.length;
      b.charstrings_off = at;
      at += cs_ix.length;
    }
    b.fdarray_off = at;
    if (s.fds) {
      result = build_fd_index(&b, items, &fd_ix, a, error);
      if (result != GFNT_OK) {
        goto done;
      }
      at += fd_ix.length;
    }
    b.vstore_off = at;
    at += vstore_buf.length;
    for (k = 0; k < plan.nfd; ++k) {
      b.private_off[k] = at;
      at += privates[k].length;
    }

    // Again, with the true offsets (the same length).
    top.length = 0;
    result = write_top(&b, &top, error);
    if (result != GFNT_OK) {
      goto done;
    }
    if (!s.cff2) {
      one.p = top.data;
      one.n = top.length;
      top_ix.length = 0;
      write_index(&top_ix, &one, 1);
    }
    if (s.fds) {
      fd_ix.length = 0;
      result = build_fd_index(&b, items, &fd_ix, a, error);
      if (result != GFNT_OK) {
        goto done;
      }
    }
  }

  // Assemble.
  if (s.cff2) {
    gfnt_wbuf_u8(out, 2);
    gfnt_wbuf_u8(out, 0);
    gfnt_wbuf_u8(out, 5);
    gfnt_wbuf_u16(out, (uint32_t)top.length);
    gfnt_wbuf_bytes(out, top.data, top.length);
    gfnt_wbuf_bytes(out, gsub_ix.data, gsub_ix.length);
    gfnt_wbuf_bytes(out, cs_ix.data, cs_ix.length);
    gfnt_wbuf_bytes(out, fdsel.data, fdsel.length);
    gfnt_wbuf_bytes(out, fd_ix.data, fd_ix.length);
    gfnt_wbuf_bytes(out, vstore_buf.data, vstore_buf.length);
  }
  else {
    gfnt_wbuf_u8(out, 1);
    gfnt_wbuf_u8(out, 0);
    gfnt_wbuf_u8(out, 4);
    gfnt_wbuf_u8(out, 4);
    gfnt_wbuf_bytes(out, name_ix.data, name_ix.length);
    gfnt_wbuf_bytes(out, top_ix.data, top_ix.length);
    gfnt_wbuf_bytes(out, str_ix.data, str_ix.length);
    gfnt_wbuf_bytes(out, gsub_ix.data, gsub_ix.length);
    gfnt_wbuf_bytes(out, charset.bytes.data, charset.bytes.length);
    gfnt_wbuf_bytes(out, fdsel.data, fdsel.length);
    gfnt_wbuf_bytes(out, cs_ix.data, cs_ix.length);
    gfnt_wbuf_bytes(out, fd_ix.data, fd_ix.length);
  }
  for (k = 0; k < plan.nfd; ++k) {
    gfnt_wbuf_bytes(out, privates[k].data, privates[k].length);
  }
  if (out->oom || charset.bytes.oom || top.oom || top_ix.oom || str_ix.oom || fd_ix.oom
      || fdsel.oom || sub_cs.oom || sub_bodies.oom || cs_ix.oom || name_ix.oom || gsub_ix.oom || vstore_buf.oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CFF, 0, GFNT_GLYPH_NONE,
        "assembling the CFF subset");
  }

done:
  if (f) {
    gfnt_wbuf_free(&f->out);
    gfnt_wbuf_free(&f->masks);
  }
  a->free_fn(a->ctx, mask_at);
  a->free_fn(a->ctx, new_at);
  a->free_fn(a->ctx, sub_at);
  a->free_fn(a->ctx, sub_items);
  gfnt_wbuf_free(&maskbuf);
  gfnt_wbuf_free(&sub_cs);
  gfnt_wbuf_free(&sub_bodies);
  a->free_fn(a->ctx, f);
  a->free_fn(a->ctx, cs_at);
  a->free_fn(a->ctx, values);
  a->free_fn(a->ctx, fd_of_new);
  a->free_fn(a->ctx, sids);
  a->free_fn(a->ctx, items);
  a->free_fn(a->ctx, plan.fd_src);
  a->free_fn(a->ctx, plan.fd_new);
  a->free_fn(a->ctx, plan.fd_used);
  a->free_fn(a->ctx, b.private_off);
  a->free_fn(a->ctx, b.private_size);
  if (privates) {
    for (k = 0; k < plan.nsrc; ++k) {
      gfnt_wbuf_free(&privates[k]);
    }
  }
  a->free_fn(a->ctx, privates);
  gfnt_wbuf_free(&cs);
  gfnt_wbuf_free(&charset.bytes);
  gfnt_wbuf_free(&fdsel);
  gfnt_wbuf_free(&top);
  gfnt_wbuf_free(&name_ix);
  gfnt_wbuf_free(&top_ix);
  gfnt_wbuf_free(&str_ix);
  gfnt_wbuf_free(&gsub_ix);
  gfnt_wbuf_free(&cs_ix);
  gfnt_wbuf_free(&fd_ix);
  gfnt_wbuf_free(&vstore_buf);
  return result;
}
