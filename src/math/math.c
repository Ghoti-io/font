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
 * The `MATH` reader. See `math.h` in the public headers. Each call reads the header and the
 * record it was asked for, straight from the table.
 */

#include <ghoti.io/font/math.h>
#include "../core/fixed.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../var/gvar.h"
#include "../var/ivs.h"

#define GFNT_TAG_MATH GFNT_TAG('M', 'A', 'T', 'H')
#define GFNT_TAG_GDEF_ GFNT_TAG('G', 'D', 'E', 'F')

typedef struct Math {
  const GFNT_Face * face;
  GFNT_Reader table;
  const GFNT_Variation * variation;
  size_t constants;
  size_t glyph_info;
  size_t variants;
} Math;

static GFNT_Result bad(GFNT_Error * error, const char * what) {
  return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE, what);
}

/** A big-endian 16-bit value at @p at of the table, or false when it is not inside it. */
static bool u16(const GFNT_Reader * t, size_t at, uint32_t * out) {
  GFNT_Reader r = *t;
  uint16_t v;

  r.error = NULL;
  if (gfnt_reader_seek(&r, at) != GFNT_OK || gfnt_read_u16(&r, &v) != GFNT_OK) {
    return false;
  }
  *out = v;
  return true;
}

static bool s16(const GFNT_Reader * t, size_t at, int32_t * out) {
  uint32_t v;

  if (!u16(t, at, &v)) {
    return false;
  }
  *out = (int16_t)(uint16_t)v;
  return true;
}

static GFNT_Result open_math(const GFNT_Face * face, const GFNT_Variation * variation,
    Math * m, GFNT_Error * error) {
  GFNT_Result result;
  uint32_t major;
  uint32_t v;

  if (!face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no face");
  }
  m->face = face;
  m->variation = variation;
  result = gfnt_face_table_reader(face, GFNT_TAG_MATH, &m->table, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!u16(&m->table, 0, &major) || !u16(&m->table, 4, &v)) {
    return bad(error, "a MATH header cut short");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE,
        "a MATH version this library does not read");
  }
  m->constants = v;
  if (!u16(&m->table, 6, &v)) {
    return bad(error, "a MATH header cut short");
  }
  m->glyph_info = v;
  if (!u16(&m->table, 8, &v)) {
    return bad(error, "a MATH header cut short");
  }
  m->variants = v;
  return GFNT_OK;
}

/** The glyph's index in a coverage table at @p cov, or -1. */
static int64_t coverage_index(const GFNT_Reader * t, size_t cov, uint32_t glyph, bool * ok) {
  uint32_t format;
  uint32_t count;
  size_t lo;
  size_t hi;

  *ok = true;
  if (!u16(t, cov, &format) || !u16(t, cov + 2, &count)) {
    *ok = false;
    return -1;
  }
  if (format == 1) {
    lo = 0;
    hi = count;
    if (t->length - cov - 4 < (size_t)count * 2) {
      *ok = false;
      return -1;
    }
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      uint32_t g = 0;

      (void)u16(t, cov + 4 + 2 * mid, &g);
      if (g == glyph) {
        return (int64_t)mid;
      }
      if (g < glyph) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return -1;
  }
  if (format == 2) {
    lo = 0;
    hi = count;
    if (t->length - cov - 4 < (size_t)count * 6) {
      *ok = false;
      return -1;
    }
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      uint32_t first = 0;
      uint32_t last = 0;
      uint32_t start = 0;

      (void)u16(t, cov + 4 + 6 * mid, &first);
      (void)u16(t, cov + 6 + 6 * mid, &last);
      (void)u16(t, cov + 8 + 6 * mid, &start);
      if (glyph < first) {
        hi = mid;
      } else if (glyph > last) {
        lo = mid + 1;
      } else {
        return (int64_t)start + (int64_t)(glyph - first);
      }
    }
    return -1;
  }
  *ok = false;
  return -1;
}

/** The `GDEF` item variation store as a reader that begins at it; false when there is none. */
static bool gdef_store(const Math * m, GFNT_Reader * store) {
  GFNT_Reader g;
  uint32_t minor;
  uint32_t major;
  uint32_t off_hi;
  uint32_t off_lo;
  size_t off;

  if (gfnt_face_table_reader(m->face, GFNT_TAG_GDEF_, &g, NULL) != GFNT_OK) {
    return false;
  }
  if (!u16(&g, 0, &major) || !u16(&g, 2, &minor) || major != 1 || minor < 3
      || !u16(&g, 14, &off_hi) || !u16(&g, 16, &off_lo)) {
    return false;
  }
  off = (size_t)off_hi << 16 | off_lo;
  if (!off || off >= g.length) {
    return false;
  }
  return gfnt_reader_sub(&g, off, g.length - off, store) == GFNT_OK;
}

/**
 * A MathValueRecord at @p at: its value, plus a variation index's delta at the location.
 * Offsets in the record are relative to @p base, the table the record is in.
 */
static GFNT_Result value_record(const Math * m, size_t at, size_t base, int32_t * out,
    GFNT_Error * error) {
  int32_t v;
  uint32_t dev;

  if (!s16(&m->table, at, &v) || !u16(&m->table, at + 2, &dev)) {
    return bad(error, "a MathValueRecord past the end of MATH");
  }
  if (dev && m->variation && m->variation->count && m->variation->coords) {
    size_t d = base + dev;
    uint32_t outer;
    uint32_t inner;
    uint32_t format;

    if (!u16(&m->table, d, &outer) || !u16(&m->table, d + 2, &inner)
        || !u16(&m->table, d + 4, &format)) {
      return bad(error, "a Device table past the end of MATH");
    }
    if (format == 0x8000u) {
      GFNT_Reader store;
      int64_t delta = 0;
      GFNT_Result result;

      if (gdef_store(m, &store)) {
        result = gfnt_ivs_delta(m->face, &store, GFNT_TAG_MATH, m->variation->coords,
            m->variation->count, outer, inner, &delta, error);
        if (result != GFNT_OK) {
          return result;
        }
        v += (int32_t)gfnt_round_shift_mode(delta, GFNT_GVAR_FRACTION_BITS,
            (int)m->variation->delta_rounding);
      }
    }
  }
  *out = v;
  return GFNT_OK;
}

bool gfnt_face_has_math(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_MATH);
}

GFNT_Result gfnt_face_math_constant(const GFNT_Face * face, const GFNT_Variation * variation,
    GFNT_MathConstant constant, int32_t * out_value, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t at;
  int32_t v;

  if (!out_value || (unsigned)constant >= GFNT_MATH_CONSTANT_COUNT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no output, or a MATH constant that does not exist");
  }
  result = open_math(face, variation, &m, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!m.constants) {
    return bad(error, "a MATH with no MathConstants");
  }
  if (constant < 2) {
    if (!s16(&m.table, m.constants + 2 * (size_t)constant, &v)) {
      return bad(error, "MathConstants cut short");
    }
  } else if (constant < 4) {
    uint32_t u;

    if (!u16(&m.table, m.constants + 2 * (size_t)constant, &u)) {
      return bad(error, "MathConstants cut short");
    }
    v = (int32_t)u;
  } else if (constant < GFNT_MATH_CONSTANT_COUNT - 1) {
    at = m.constants + 8 + 4 * ((size_t)constant - 4);
    result = value_record(&m, at, m.constants, &v, error);
    if (result != GFNT_OK) {
      return result;
    }
  } else {
    if (!s16(&m.table, m.constants + 8 + 4 * 51, &v)) {
      return bad(error, "MathConstants cut short");
    }
  }
  *out_value = v;
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_glyph_value(const GFNT_Face * face, const GFNT_Variation * variation,
    uint32_t glyph, GFNT_MathGlyphValue which, bool * out_present, int32_t * out_value,
    GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  uint32_t off;
  uint32_t count;
  uint32_t cov;
  size_t info;
  bool ok;
  int64_t index;

  if (!out_present || !out_value || (unsigned)which > GFNT_MATH_TOP_ACCENT_ATTACHMENT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "a bad argument");
  }
  result = open_math(face, variation, &m, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_present = false;
  *out_value = 0;
  if (!m.glyph_info) {
    return GFNT_OK;
  }
  if (!u16(&m.table, m.glyph_info + 2 * (size_t)which, &off)) {
    return bad(error, "MathGlyphInfo cut short");
  }
  if (!off) {
    return GFNT_OK;
  }
  info = m.glyph_info + off;
  if (!u16(&m.table, info, &cov) || !u16(&m.table, info + 2, &count)) {
    return bad(error, "a glyph value table cut short");
  }
  if (!cov) {
    return bad(error, "a glyph value table with no coverage");
  }
  index = coverage_index(&m.table, info + cov, glyph, &ok);
  if (!ok) {
    return bad(error, "a bad coverage table");
  }
  if (index < 0 || (uint64_t)index >= count) {
    return GFNT_OK;
  }
  result = value_record(&m, info + 4 + 4 * (size_t)index, info, out_value, error);
  if (result == GFNT_OK) {
    *out_present = true;
  }
  return result;
}

GFNT_Result gfnt_face_math_is_extended_shape(const GFNT_Face * face, uint32_t glyph,
    bool * out_extended, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  uint32_t off;
  bool ok;

  if (!out_extended) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_extended = false;
  if (!m.glyph_info) {
    return GFNT_OK;
  }
  if (!u16(&m.table, m.glyph_info + 4, &off)) {
    return bad(error, "MathGlyphInfo cut short");
  }
  if (!off) {
    return GFNT_OK;
  }
  *out_extended = coverage_index(&m.table, m.glyph_info + off, glyph, &ok) >= 0;
  return ok ? GFNT_OK : bad(error, "a bad coverage table");
}

/** The MathKern table of a glyph's corner: 0 if absent. */
static GFNT_Result kern_table(const Math * m, uint32_t glyph, GFNT_MathKernCorner corner,
    size_t * out, GFNT_Error * error) {
  uint32_t off;
  uint32_t cov;
  uint32_t count;
  size_t info;
  int64_t index;
  bool ok;

  *out = 0;
  if ((unsigned)corner > GFNT_MATH_KERN_BOTTOM_LEFT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "a bad corner");
  }
  if (!m->glyph_info) {
    return GFNT_OK;
  }
  if (!u16(&m->table, m->glyph_info + 6, &off)) {
    return bad(error, "MathGlyphInfo cut short");
  }
  if (!off) {
    return GFNT_OK;
  }
  info = m->glyph_info + off;
  if (!u16(&m->table, info, &cov) || !u16(&m->table, info + 2, &count)) {
    return bad(error, "MathKernInfo cut short");
  }
  index = coverage_index(&m->table, info + cov, glyph, &ok);
  if (!ok) {
    return bad(error, "a bad coverage table");
  }
  if (index < 0 || (uint64_t)index >= count) {
    return GFNT_OK;
  }
  if (!u16(&m->table, info + 4 + 8 * (size_t)index + 2 * (size_t)corner, &off)) {
    return bad(error, "MathKernInfo cut short");
  }
  *out = off ? info + off : 0;
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_kern_count(const GFNT_Face * face, uint32_t glyph,
    GFNT_MathKernCorner corner, bool * out_present, size_t * out_heights, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t kern;
  uint32_t n;

  if (!out_present || !out_heights) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result == GFNT_OK) {
    result = kern_table(&m, glyph, corner, &kern, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  *out_present = false;
  *out_heights = 0;
  if (!kern) {
    return GFNT_OK;
  }
  if (!u16(&m.table, kern, &n)) {
    return bad(error, "a MathKern cut short");
  }
  *out_present = true;
  *out_heights = n;
  return GFNT_OK;
}

static GFNT_Result kern_entry(const GFNT_Face * face, const GFNT_Variation * variation,
    uint32_t glyph, GFNT_MathKernCorner corner, size_t index, bool height, int32_t * out,
    GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t kern;
  uint32_t n;

  if (!out) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, variation, &m, error);
  if (result == GFNT_OK) {
    result = kern_table(&m, glyph, corner, &kern, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (!kern || !u16(&m.table, kern, &n) || index > n || (height && index == n)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE,
        "no such kern entry");
  }
  return value_record(&m, kern + 2 + 4 * (height ? index : (size_t)n + index), kern, out, error);
}

GFNT_Result gfnt_face_math_kern_height_at(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, GFNT_MathKernCorner corner, size_t index,
    int32_t * out_height, GFNT_Error * error) {
  return kern_entry(face, variation, glyph, corner, index, true, out_height, error);
}

GFNT_Result gfnt_face_math_kern_value_at(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, GFNT_MathKernCorner corner, size_t index,
    int32_t * out_kern, GFNT_Error * error) {
  return kern_entry(face, variation, glyph, corner, index, false, out_kern, error);
}

GFNT_Result gfnt_face_math_min_connector_overlap(const GFNT_Face * face, uint32_t * out_overlap,
    GFNT_Error * error) {
  Math m;
  GFNT_Result result;

  if (!out_overlap) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!m.variants) {
    *out_overlap = 0;
    return GFNT_OK;
  }
  return u16(&m.table, m.variants, out_overlap) ? GFNT_OK : bad(error, "MathVariants cut short");
}

/** The glyph's MathGlyphConstruction table, 0 if the glyph has none in that direction. */
static GFNT_Result construction(const Math * m, uint32_t glyph, bool vertical, size_t * out,
    GFNT_Error * error) {
  uint32_t cov;
  uint32_t count;
  uint32_t off;
  int64_t index;
  bool ok;
  size_t v = m->variants;

  *out = 0;
  if (!v) {
    return GFNT_OK;
  }
  if (!u16(&m->table, v + (vertical ? 2 : 4), &cov) || !u16(&m->table, v + 6, &count)) {
    return bad(error, "MathVariants cut short");
  }
  if (!vertical) {
    uint32_t vcount = count;

    if (!u16(&m->table, v + 8, &count)) {
      return bad(error, "MathVariants cut short");
    }
    off = 10 + 2 * vcount;
  } else {
    off = 10;
  }
  if (!cov) {
    return GFNT_OK;
  }
  index = coverage_index(&m->table, v + cov, glyph, &ok);
  if (!ok) {
    return bad(error, "a bad coverage table");
  }
  if (index < 0 || (uint64_t)index >= count) {
    return GFNT_OK;
  }
  {
    uint32_t c;

    if (!u16(&m->table, v + off + 2 * (size_t)index, &c)) {
      return bad(error, "MathVariants cut short");
    }
    *out = c ? v + c : 0;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_variant_count(const GFNT_Face * face, uint32_t glyph, bool vertical,
    size_t * out_count, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t c;
  uint32_t n;

  if (!out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result == GFNT_OK) {
    result = construction(&m, glyph, vertical, &c, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = 0;
  if (c) {
    if (!u16(&m.table, c + 2, &n)) {
      return bad(error, "a construction cut short");
    }
    *out_count = n;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_variant_at(const GFNT_Face * face, uint32_t glyph, bool vertical,
    size_t index, GFNT_MathVariant * out_variant, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t c;
  uint32_t n;
  uint32_t g;
  uint32_t a;

  if (!out_variant) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result == GFNT_OK) {
    result = construction(&m, glyph, vertical, &c, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (!c || !u16(&m.table, c + 2, &n) || index >= n) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE,
        "no such variant");
  }
  if (!u16(&m.table, c + 4 + 4 * index, &g) || !u16(&m.table, c + 6 + 4 * index, &a)) {
    return bad(error, "a variant past the end of MATH");
  }
  out_variant->glyph = g;
  out_variant->advance = a;
  return GFNT_OK;
}

/** The glyph assembly's table, 0 if none. */
static GFNT_Result assembly(const Math * m, uint32_t glyph, bool vertical, size_t * out,
    GFNT_Error * error) {
  size_t c;
  uint32_t off;
  GFNT_Result result = construction(m, glyph, vertical, &c, error);

  *out = 0;
  if (result != GFNT_OK || !c) {
    return result;
  }
  if (!u16(&m->table, c, &off)) {
    return bad(error, "a construction cut short");
  }
  *out = off ? c + off : 0;
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_assembly_count(const GFNT_Face * face, uint32_t glyph, bool vertical,
    size_t * out_count, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t a;
  uint32_t n;

  if (!out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result == GFNT_OK) {
    result = assembly(&m, glyph, vertical, &a, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = 0;
  if (a) {
    if (!u16(&m.table, a + 4, &n)) {
      return bad(error, "an assembly cut short");
    }
    *out_count = n;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_math_assembly_italics_correction(const GFNT_Face * face,
    const GFNT_Variation * variation, uint32_t glyph, bool vertical, int32_t * out_value,
    GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t a;

  if (!out_value) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, variation, &m, error);
  if (result == GFNT_OK) {
    result = assembly(&m, glyph, vertical, &a, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (!a) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE,
        "the glyph has no assembly");
  }
  return value_record(&m, a, a, out_value, error);
}

GFNT_Result gfnt_face_math_assembly_part_at(const GFNT_Face * face, uint32_t glyph, bool vertical,
    size_t index, GFNT_MathPart * out_part, GFNT_Error * error) {
  Math m;
  GFNT_Result result;
  size_t a;
  uint32_t n;
  uint32_t f[5];
  size_t i;

  if (!out_part) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no output");
  }
  result = open_math(face, NULL, &m, error);
  if (result == GFNT_OK) {
    result = assembly(&m, glyph, vertical, &a, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (!a || !u16(&m.table, a + 4, &n) || index >= n) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_MATH, 0, GFNT_GLYPH_NONE,
        "no such assembly part");
  }
  for (i = 0; i < 5; i++) {
    if (!u16(&m.table, a + 6 + 10 * index + 2 * i, &f[i])) {
      return bad(error, "a part past the end of MATH");
    }
  }
  out_part->glyph = f[0];
  out_part->start_connector_length = f[1];
  out_part->end_connector_length = f[2];
  out_part->full_advance = f[3];
  out_part->flags = (uint16_t)f[4];
  return GFNT_OK;
}
