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
 * The `kern` table, as the fallback for a font whose `GPOS` has no `kern` feature.
 *
 * A font from before OpenType layout carries its pair kerning in `kern`, and so
 * do many that came after it and never moved it. The shaper uses it only when
 * `GPOS` does not already kern, because applying both is the defect design.md
 * M16 names: the pair would be tightened twice.
 *
 * Format 0 subtables, horizontal and not cross-stream, in the Microsoft header
 * (version 0) and Apple's (version 1). The class-based format 2 is read in the
 * Microsoft header; Apple's state-machine formats are not, and a subtable of a
 * format this reads is added to the kerning of the pair with every other.
 */

#include <string.h>
#include "layout.h"
#include "../sfnt/sfnt.h"

#define GFNT_TAG_kern GFNT_TAG('k', 'e', 'r', 'n')

/** One pair's kerning from one format 0 subtable, or 0. */
static int32_t gfnt_kern_format0(const GFNT_Reader * r, size_t subtable,
    size_t header, uint32_t left, uint32_t right, bool * bad) {
  uint32_t pairs = gfnt_lr_u16(r, subtable + header, bad);
  uint32_t low = 0;
  uint32_t high = pairs;
  uint32_t key = (left << 16) | right;
  size_t first = subtable + header + 8;

  while (low < high && !*bad) {
    uint32_t mid = low + (high - low) / 2;
    size_t at = first + 6 * (size_t)mid;
    uint32_t value = ((uint32_t)gfnt_lr_u16(r, at, bad) << 16)
        | gfnt_lr_u16(r, at + 2, bad);

    if (key < value) {
      high = mid;
    }
    else if (key > value) {
      low = mid + 1;
    }
    else {
      return (int16_t)gfnt_lr_u16(r, at + 4, bad);
    }
  }
  return 0;
}

/** One pair's kerning from a class-based (format 2) subtable, or 0. */
static int32_t gfnt_kern_format2(const GFNT_Reader * r, size_t subtable,
    size_t header, uint32_t left, uint32_t right, bool * bad) {
  size_t body = subtable + header;
  uint32_t row_width = gfnt_lr_u16(r, body, bad);
  size_t left_table = subtable + gfnt_lr_u16(r, body + 2, bad);
  size_t right_table = subtable + gfnt_lr_u16(r, body + 4, bad);
  size_t array = subtable + gfnt_lr_u16(r, body + 6, bad);
  uint32_t l = 0;
  uint32_t k = 0;
  uint32_t first;
  uint32_t count;

  (void)row_width;
  first = gfnt_lr_u16(r, left_table, bad);
  count = gfnt_lr_u16(r, left_table + 2, bad);
  if (left >= first && left - first < count) {
    l = gfnt_lr_u16(r, left_table + 4 + 2 * (size_t)(left - first), bad);
  }
  first = gfnt_lr_u16(r, right_table, bad);
  count = gfnt_lr_u16(r, right_table + 2, bad);
  if (right >= first && right - first < count) {
    k = gfnt_lr_u16(r, right_table + 4 + 2 * (size_t)(right - first), bad);
  }
  // The class values are byte offsets already: a row offset and a column offset.
  return (int16_t)gfnt_lr_u16(r, array + l + k, bad);
}

/** One subtable's value for a pair, or 0. */
static int32_t gfnt_kern_subtable_pair(const GFNT_Reader * r, size_t cursor,
    size_t header, uint32_t format, uint32_t left, uint32_t right, bool * bad) {
  if (format == 0) {
    return gfnt_kern_format0(r, cursor, header, left, right, bad);
  }
  if (format == 2) {
    return gfnt_kern_format2(r, cursor, header, left, right, bad);
  }
  return 0;
}

/**
 * Run every horizontal subtable over the run in turn, as HarfBuzz does: each one
 * splits its own values between the two glyphs of a pair, and a cross-stream one
 * hangs the second glyph from the first and sets that glyph's own shift, which
 * what an earlier subtable gave it yields to.
 */
static void gfnt_kern_run(const GFNT_Reader * r, GFNT_LApply * c, GFNT_LBuffer * b,
    uint32_t kern_mask, bool * bad) {
  uint32_t version = gfnt_lr_u16(r, 0, bad);
  size_t cursor;
  uint32_t tables;
  uint32_t t;
  bool apple = false;
  bool seen_cross = false;

  if (version == 0) {
    tables = gfnt_lr_u16(r, 2, bad);
    cursor = 4;
  }
  else if (version == 1 && gfnt_lr_u16(r, 2, bad) == 0) {
    apple = true;
    tables = gfnt_lr_u32(r, 4, bad);
    cursor = 8;
  }
  else {
    return;
  }
  for (t = 0; t < tables && !*bad; t++) {
    size_t length;
    uint32_t coverage;
    uint32_t format;
    size_t header;
    bool horizontal;
    bool cross;
    size_t idx = 0;

    if (apple) {
      length = gfnt_lr_u32(r, cursor, bad);
      coverage = gfnt_lr_u16(r, cursor + 4, bad);
      format = coverage & 0xFFu;
      horizontal = !(coverage & 0x8000u);
      cross = (coverage & 0x4000u) != 0;
      header = 8;
    }
    else {
      length = gfnt_lr_u16(r, cursor + 2, bad);
      coverage = gfnt_lr_u16(r, cursor + 4, bad);
      format = coverage >> 8;
      horizontal = (coverage & 1u) != 0;
      cross = (coverage & 4u) != 0;
      header = 6;
    }
    if (*bad || length < header) {
      break;
    }
    if (horizontal && (format == 0 || format == 2)) {
      GFNT_LIter it;

      if (cross && !seen_cross) {
        size_t m;

        // The kerning skips marks, which then go on with the glyph before them.
        seen_cross = true;
        for (m = 1; m < b->len; m++) {
          if (gfnt_l_is_mark(&b->info[m])) {
            b->pos[m].attach_type = GFNT_ATTACH_CURSIVE;
            b->pos[m].attach_chain = -1;
            b->has_attachment = true;
          }
        }
      }

      while (idx < b->len && !*bad) {
        size_t i = idx;
        size_t j;
        int32_t kern;

        if (!(b->info[idx].mask & kern_mask)) {
          idx++;
          continue;
        }
        gfnt_liter_init(&it, c, false);
        gfnt_liter_reset(&it, idx, 1);
        if (!gfnt_liter_next(&it)) {
          idx++;
          continue;
        }
        j = it.idx;
        kern = gfnt_kern_subtable_pair(r, cursor, header, format, b->info[i].glyph,
            b->info[j].glyph, bad);
        if (cross) {
          if (kern) {
            b->pos[j].y_offset = kern;
          }
          b->pos[j].attach_type = GFNT_ATTACH_CURSIVE;
          b->pos[j].attach_chain = (int32_t)((int64_t)i - (int64_t)j);
          b->has_attachment = true;
        }
        else if (kern) {
          // The pair's kerning is shared between the two glyphs, so that a ligature
          // caret or a break between them still has a sensible width.
          int32_t kern1 = kern >> 1;
          int32_t kern2 = kern - kern1;

          b->pos[i].x_advance += kern1;
          b->pos[j].x_advance += kern2;
          b->pos[j].x_offset += kern2;
        }
        idx = j;
      }
    }
    cursor += length;
  }
}

GFNT_Result gfnt_kern_apply(const GFNT_Face * face, GFNT_LBuffer * b,
    const GFNT_Gdef * gdef, uint32_t kern_mask, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_LApply c;
  bool bad = false;
  GFNT_Result result;

  if (!gfnt_face_has_table(face, GFNT_TAG_kern)) {
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_kern, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = NULL;
  memset(&c, 0, sizeof c);
  c.buf = b;
  c.gdef = gdef;
  c.lookup_mask = kern_mask;
  c.lookup_props = GFNT_LF_IGNORE_MARKS;
  // Positioning, so a joiner never stands between a pair.
  c.is_gpos = true;
  c.auto_zwnj = true;
  c.auto_zwj = true;
  gfnt_kern_run(&table, &c, b, kern_mask, &bad);
  if (bad) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_kern, 0,
        GFNT_GLYPH_NONE, "the kern table reads past itself");
  }
  return GFNT_OK;
}

bool gfnt_kern_has_cross_stream(const GFNT_Face * face) {
  GFNT_Reader table;
  bool bad = false;
  bool apple = false;
  uint32_t version;
  uint32_t tables;
  size_t cursor;
  uint32_t t;

  if (!gfnt_face_has_table(face, GFNT_TAG_kern)
      || gfnt_face_table_reader(face, GFNT_TAG_kern, &table, NULL) != GFNT_OK) {
    return false;
  }
  table.error = NULL;
  version = gfnt_lr_u16(&table, 0, &bad);
  if (version == 0) {
    tables = gfnt_lr_u16(&table, 2, &bad);
    cursor = 4;
  }
  else if (version == 1 && gfnt_lr_u16(&table, 2, &bad) == 0) {
    apple = true;
    tables = gfnt_lr_u32(&table, 4, &bad);
    cursor = 8;
  }
  else {
    return false;
  }
  for (t = 0; t < tables && !bad; t++) {
    size_t length = apple ? gfnt_lr_u32(&table, cursor, &bad)
        : gfnt_lr_u16(&table, cursor + 2, &bad);
    uint32_t coverage = gfnt_lr_u16(&table, cursor + 4, &bad);

    if (bad || length < (apple ? 8u : 6u)) {
      break;
    }
    if (apple ? (coverage & 0x4000u) != 0 : (coverage & 4u) != 0) {
      return true;
    }
    cursor += length;
  }
  return false;
}
