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
 * Apple's `kerx` table: kerning and attachment, in HarfBuzz's way.
 *
 * A font with a version 2 `kerx` table is kerned by it and by nothing else: `GPOS` is not
 * run, and the `kern` table is not read, even when `kerx` has no subtable that
 * applies. Every subtable is run over the whole run in turn and what they give
 * adds up. A vertical subtable is not run on horizontal text, and vertical text
 * is not kerned by `kerx` at all. A cross-stream subtable moves the glyphs
 * across the line, and the shift carries on to every glyph after it. A subtable
 * that processes descending is not run: HarfBuzz kerns nothing by one, in either
 * direction, whatever the pairs.
 */

#include <string.h>
#include "layout.h"
#include "../sfnt/sfnt.h"

#define GFNT_TAG_kerx GFNT_TAG('k', 'e', 'r', 'x')
#define GFNT_TAG_ankr GFNT_TAG('a', 'n', 'k', 'r')

#define KERX_VERTICAL   0x80000000u
#define KERX_CROSS      0x40000000u
#define KERX_VARIATION  0x20000000u
#define KERX_DESCENDING 0x10000000u

bool gfnt_kerx_present(const GFNT_Face * face) {
  GFNT_Reader table;
  bool bad = false;

  // HarfBuzz acts on version 2 only: a table of any other version is as good as
  // none, and `GPOS` kerns as it would.
  if (!gfnt_face_has_table(face, GFNT_TAG_kerx)
      || gfnt_face_table_reader(face, GFNT_TAG_kerx, &table, NULL) != GFNT_OK) {
    return false;
  }
  table.error = NULL;
  return gfnt_lr_u16(&table, 0, &bad) == 2 && !bad;
}

/** One pair's value from an ordered-list (format 0) subtable, or 0. */
static int32_t kerx_format0(const GFNT_Reader * r, size_t subtable,
    uint32_t left, uint32_t right, bool * bad) {
  uint32_t pairs = gfnt_lr_u32(r, subtable + 12, bad);
  uint32_t low = 0;
  uint32_t high = pairs;
  uint32_t key = (left << 16) | right;
  size_t first = subtable + 28;

  if (pairs > (UINT32_MAX - 28) / 6) {
    *bad = true;
    return 0;
  }
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

/**
 * One pair's value from a class-array (format 2) subtable, or 0. The class values
 * are indices into the array of values, as HarfBuzz reads them, and a pair past
 * the end of the array has no kerning.
 */
static int32_t kerx_format2(const GFNT_Reader * r, size_t subtable,
    size_t num_glyphs, uint32_t left, uint32_t right, bool * bad) {
  size_t left_table = subtable + gfnt_lr_u32(r, subtable + 16, bad);
  size_t right_table = subtable + gfnt_lr_u32(r, subtable + 20, bad);
  size_t array = subtable + gfnt_lr_u32(r, subtable + 24, bad);
  uint32_t l = 0;
  uint32_t k = 0;
  bool outside = false;
  int32_t value;

  if (*bad) {
    return 0;
  }
  // A glyph that has no class, on either side, has no kerning with anything.
  if (!gfnt_aat_lookup(r, left_table, num_glyphs, left, 2, &l, bad)
      || !gfnt_aat_lookup(r, right_table, num_glyphs, right, 2, &k, bad)) {
    return 0;
  }
  value = (int16_t)gfnt_lr_u16(r, array + 2 * ((size_t)l + k), &outside);
  return outside ? 0 : value;
}

/**
 * One pair's value from an index-array (format 6) subtable, or 0. As in format 2,
 * the two lookup values add up to an index into the array, which holds the values
 * themselves, 16 or 32 bits each; the separate kerning vector is not read.
 */
static int32_t kerx_format6(const GFNT_Reader * r, size_t subtable,
    size_t num_glyphs, uint32_t left, uint32_t right, bool * bad) {
  uint32_t flags = gfnt_lr_u32(r, subtable + 12, bad);
  size_t row_table = subtable + gfnt_lr_u32(r, subtable + 20, bad);
  size_t column_table = subtable + gfnt_lr_u32(r, subtable + 24, bad);
  size_t array = subtable + gfnt_lr_u32(r, subtable + 28, bad);
  unsigned size = (flags & 1u) ? 4 : 2;
  uint32_t l = 0;
  uint32_t k = 0;
  bool outside = false;
  int32_t value;

  if (*bad) {
    return 0;
  }
  if (!gfnt_aat_lookup(r, row_table, num_glyphs, left, size, &l, bad)
      || !gfnt_aat_lookup(r, column_table, num_glyphs, right, size, &k, bad)) {
    return 0;
  }
  if (size == 4) {
    value = (int32_t)gfnt_lr_u32(r, array + 4 * ((size_t)l + k), &outside);
  }
  else {
    value = (int16_t)gfnt_lr_u16(r, array + 2 * ((size_t)l + k), &outside);
  }
  return outside ? 0 : value;
}

/**
 * A subtable with tuples (variation data) holds in place of each value the
 * offset, from the start of the subtable, of a list of values; the first is the
 * one used, as no variation is applied. A value that points outside is nothing.
 */
static int32_t kerx_tuple(const GFNT_Reader * r, size_t subtable, int32_t value) {
  bool outside = false;
  int32_t first = (int16_t)gfnt_lr_u16(r, subtable + (size_t)(uint32_t)value,
      &outside);

  return outside ? 0 : first;
}

static int32_t kerx_pair(const GFNT_Reader * r, size_t subtable,
    size_t num_glyphs, uint32_t format, uint32_t left, uint32_t right,
    bool tuples, bool * bad) {
  int32_t value;

  switch (format) {
    case 0:
      value = kerx_format0(r, subtable, left, right, bad);
      break;
    case 2:
      value = kerx_format2(r, subtable, num_glyphs, left, right, bad);
      break;
    case 6:
      value = kerx_format6(r, subtable, num_glyphs, left, right, bad);
      break;
    default:
      return 0;
  }
  return tuples && value ? kerx_tuple(r, subtable, (int32_t)(uint16_t)value)
      : value;
}

/**
 * A state-machine (format 1) subtable: contextual kerning. Glyphs a transition
 * pushes are kept on a stack of eight; a transition that names values applies
 * them to the stack from the top, one glyph to each value, until a value with its
 * low bit set ends the list; the glyphs not reached stay on the stack. A ninth push empties it.
 */
#define KERX_STACK 8u
#define KERX_DETACHED INT32_MIN

static void kerx_format1(const GFNT_Reader * r, size_t subtable, size_t num_glyphs,
    bool cross, GFNT_LBuffer * b, uint32_t kern_mask, bool * bad) {
  size_t base = subtable + 12;
  uint32_t classes = gfnt_lr_u32(r, base, bad);
  size_t class_table = base + gfnt_lr_u32(r, base + 4, bad);
  size_t state_array = base + gfnt_lr_u32(r, base + 8, bad);
  size_t entry_table = base + gfnt_lr_u32(r, base + 12, bad);
  size_t values = base + gfnt_lr_u32(r, base + 16, bad);
  size_t stack[KERX_STACK];
  size_t depth = 0;
  uint32_t state = 0;
  b->idx = 0;
  while (!*bad) {
    uint32_t klass = 0;
    size_t entry;
    uint32_t index;
    uint16_t new_state;
    uint16_t flags;
    uint16_t action;

    if (b->idx < b->len) {
      uint32_t glyph = b->info[b->idx].glyph;
      uint32_t value = 0;

      if (glyph == 0xFFFFu) {
        klass = 2;
      }
      else if (!gfnt_aat_lookup(r, class_table, num_glyphs, glyph, 2, &value,
                   bad)) {
        klass = 1;
      }
      else {
        klass = value;
      }
    }
    if (klass >= classes) {
      klass = 1;
    }
    index = gfnt_lr_u16(r, state_array + 2 * ((size_t)state * classes + klass),
        bad);
    entry = entry_table + 6 * (size_t)index;
    new_state = (uint16_t)gfnt_lr_u16(r, entry, bad);
    flags = (uint16_t)gfnt_lr_u16(r, entry + 2, bad);
    action = (uint16_t)gfnt_lr_u16(r, entry + 4, bad);
    if (*bad) {
      return;
    }
    if (flags & 0x2000u) {
      depth = 0;
    }
    if (flags & 0x8000u) {
      // At the end of the text the position past the last glyph is pushed too, and
      // takes the first of the values to nothing.
      if (depth < KERX_STACK) {
        stack[depth++] = b->idx;
      }
      else {
        depth = 0;
      }
    }
    if (action != 0xFFFFu && depth) {
      size_t at = values + (size_t)(action & ~1u);
      bool last = false;

      while (!last && depth && !*bad) {
        size_t g = stack[--depth];
        int32_t v = (int16_t)gfnt_lr_u16(r, at, bad);

        at += 2;
        if (g >= b->len) {
          // The position past the end: its value is read and its end mark is not.
          continue;
        }
        last = (v & 1) != 0;
        v &= ~1;
        if (cross) {
          // What each glyph is shifted by is its own; the run is summed after.
          if (v == -0x8000) {
            b->pos[g].y_offset = KERX_DETACHED;
          }
          else if (b->pos[g].y_offset != KERX_DETACHED) {
            b->pos[g].y_offset += v;
          }
        }
        else if (b->info[g].mask & kern_mask) {
          b->pos[g].x_advance += v;
          b->pos[g].x_offset += v;
        }
      }
    }
    state = new_state;
    if (b->idx >= b->len) {
      break;
    }
    if (!(flags & 0x4000u) || b->max_ops-- <= 0) {
      b->idx++;
    }
  }
}

/**
 * Anchor point @p index of @p glyph from the `ankr` table, or (0, 0) where the
 * table, the glyph or the point is not there, as HarfBuzz has it.
 */
static void kerx_anchor(const GFNT_Reader * ankr, bool have, size_t num_glyphs,
    uint32_t glyph, uint32_t index, int32_t * x, int32_t * y) {
  bool bad = false;
  uint32_t offset = 0;

  *x = 0;
  *y = 0;
  if (!have) {
    return;
  }
  if (!gfnt_aat_lookup(ankr, gfnt_lr_u32(ankr, 4, &bad), num_glyphs, glyph, 2,
          &offset, &bad) || bad) {
    return;
  }
  {
    size_t data = (size_t)gfnt_lr_u32(ankr, 8, &bad) + offset;
    uint32_t count = gfnt_lr_u32(ankr, data, &bad);

    if (bad || index >= count) {
      return;
    }
    *x = (int16_t)gfnt_lr_u16(ankr, data + 4 + 4 * (size_t)index, &bad);
    *y = (int16_t)gfnt_lr_u16(ankr, data + 6 + 4 * (size_t)index, &bad);
    if (bad) {
      *x = 0;
      *y = 0;
    }
  }
}

/**
 * An attachment (format 4) subtable: a state machine that marks a glyph and then
 * hangs a later one from it, putting a point of the one on a point of the other.
 * The points are named as numbers in the table (coordinates) or as anchors of the
 * `ankr` table; points of the glyphs' outlines are not read.
 */
static void kerx_format4(const GFNT_Reader * r, size_t subtable, size_t num_glyphs,
    const GFNT_Reader * ankr, bool have_ankr, GFNT_LBuffer * b, bool * bad) {
  size_t base = subtable + 12;
  uint32_t classes = gfnt_lr_u32(r, base, bad);
  size_t class_table = base + gfnt_lr_u32(r, base + 4, bad);
  size_t state_array = base + gfnt_lr_u32(r, base + 8, bad);
  size_t entry_table = base + gfnt_lr_u32(r, base + 12, bad);
  uint32_t control = gfnt_lr_u32(r, base + 16, bad);
  uint32_t kind = control >> 30;
  size_t actions = base + (control & 0x00FFFFFFu);
  uint32_t state = 0;
  bool mark_set = false;
  size_t mark = 0;
  b->idx = 0;
  while (!*bad) {
    uint32_t klass = 0;
    size_t entry;
    uint32_t index;
    uint16_t new_state;
    uint16_t flags;
    uint16_t action;

    if (b->idx < b->len) {
      uint32_t glyph = b->info[b->idx].glyph;
      uint32_t value = 0;

      if (glyph == 0xFFFFu) {
        klass = 2;
      }
      else if (!gfnt_aat_lookup(r, class_table, num_glyphs, glyph, 2, &value,
                   bad)) {
        klass = 1;
      }
      else {
        klass = value;
      }
    }
    if (klass >= classes) {
      klass = 1;
    }
    index = gfnt_lr_u16(r, state_array + 2 * ((size_t)state * classes + klass),
        bad);
    entry = entry_table + 6 * (size_t)index;
    new_state = (uint16_t)gfnt_lr_u16(r, entry, bad);
    flags = (uint16_t)gfnt_lr_u16(r, entry + 2, bad);
    action = (uint16_t)gfnt_lr_u16(r, entry + 4, bad);
    if (*bad) {
      return;
    }
    if (mark_set && action != 0xFFFFu && b->idx < b->len && kind != 0) {
      size_t at = actions + (kind == 2 ? 8 : 4) * (size_t)action;
      int32_t mark_x;
      int32_t mark_y;
      int32_t curr_x;
      int32_t curr_y;

      if (kind == 2) {
        mark_x = (int16_t)gfnt_lr_u16(r, at, bad);
        mark_y = (int16_t)gfnt_lr_u16(r, at + 2, bad);
        curr_x = (int16_t)gfnt_lr_u16(r, at + 4, bad);
        curr_y = (int16_t)gfnt_lr_u16(r, at + 6, bad);
      }
      else {
        uint32_t mark_point = gfnt_lr_u16(r, at, bad);
        uint32_t curr_point = gfnt_lr_u16(r, at + 2, bad);

        kerx_anchor(ankr, have_ankr, num_glyphs, b->info[mark].glyph, mark_point,
            &mark_x, &mark_y);
        kerx_anchor(ankr, have_ankr, num_glyphs, b->info[b->idx].glyph,
            curr_point, &curr_x, &curr_y);
      }
      if (!*bad) {
        GFNT_LPos * o = &b->pos[b->idx];

        o->x_offset = mark_x - curr_x;
        o->y_offset = mark_y - curr_y;
        o->attach_type = GFNT_ATTACH_MARK;
        o->attach_chain = (int32_t)((int64_t)mark - (int64_t)b->idx);
        b->has_attachment = true;
      }
    }
    if (flags & 0x8000u) {
      mark_set = true;
      mark = b->idx;
    }
    state = new_state;
    if (b->idx >= b->len) {
      break;
    }
    if (!(flags & 0x4000u) || b->max_ops-- <= 0) {
      b->idx++;
    }
  }
}

/**
 * Every glyph rides on the one before it, so a shift of one carries on to all the
 * glyphs after it, except that a glyph set apart starts again from nothing. The
 * shifts of all the subtables are in the glyphs by now, each its own; hanging the
 * glyphs from one another is left to the end, where the offsets of a chain are
 * added up once whatever the number of subtables.
 */
static void kerx_hang_shifts(GFNT_LBuffer * b) {
  size_t i;

  for (i = 0; i < b->len; i++) {
    if (b->pos[i].y_offset == KERX_DETACHED) {
      b->pos[i].y_offset = 0;
      b->pos[i].attach_type = 0;
      b->pos[i].attach_chain = 0;
    }
    else if (!b->pos[i].attach_type && i) {
      b->pos[i].attach_type = GFNT_ATTACH_CURSIVE;
      b->pos[i].attach_chain = -1;
      b->has_attachment = true;
    }
  }
}

/** Run one pair-kerning subtable over the run. */
static void kerx_run(const GFNT_Reader * r, size_t subtable, size_t num_glyphs,
    uint32_t format, bool cross, bool tuples, GFNT_LApply * c, GFNT_LBuffer * b, uint32_t kern_mask,
    bool * bad) {
  GFNT_LIter it;
  size_t idx = 0;

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
    kern = kerx_pair(r, subtable, num_glyphs, format, b->info[i].glyph,
        b->info[j].glyph, tuples, bad);
    if (cross) {
      // The glyph hangs from the one before it, and what it is shifted by is its own
      // and what that one is shifted by. A pair with a value sets the glyph's own
      // shift, replacing what an earlier subtable gave it; one without leaves it.
      if (kern) {
        b->pos[j].y_offset = kern;
      }
      if (!b->pos[j].attach_type) {
        b->pos[j].attach_type = GFNT_ATTACH_CURSIVE;
        b->pos[j].attach_chain = (int32_t)((int64_t)i - (int64_t)j);
        b->has_attachment = true;
      }
    }
    else if (kern) {
      int32_t kern1 = kern >> 1;
      int32_t kern2 = kern - kern1;

      b->pos[i].x_advance += kern1;
      b->pos[j].x_advance += kern2;
      b->pos[j].x_offset += kern2;
    }
    idx = j;
  }
}

GFNT_Result gfnt_kerx_apply(const GFNT_Face * face, GFNT_LBuffer * b,
    const GFNT_Gdef * gdef, uint32_t kern_mask, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader ankr;
  bool have_ankr = false;
  GFNT_LApply c;
  GFNT_Result result;
  size_t cursor = 8;
  uint32_t tables;
  uint32_t t;
  size_t glyphs = 0;
  bool bad = false;
  bool shifts = false;
  bool seen_cross = false;

  result = gfnt_face_table_reader(face, GFNT_TAG_kerx, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = NULL;
  if (gfnt_face_has_table(face, GFNT_TAG_ankr)
      && gfnt_face_table_reader(face, GFNT_TAG_ankr, &ankr, NULL) == GFNT_OK) {
    ankr.error = NULL;
    have_ankr = true;
  }
  (void)gfnt_face_num_glyphs(face, &glyphs, NULL);
  tables = gfnt_lr_u32(&table, 4, &bad);
  memset(&c, 0, sizeof c);
  c.buf = b;
  c.gdef = gdef;
  c.lookup_mask = kern_mask;
  c.lookup_props = GFNT_LF_IGNORE_MARKS;
  c.is_gpos = true;
  c.auto_zwnj = true;
  c.auto_zwj = true;
  for (t = 0; t < tables && !bad; t++) {
    uint32_t length = gfnt_lr_u32(&table, cursor, &bad);
    uint32_t coverage = gfnt_lr_u32(&table, cursor + 4, &bad);

    if (bad || length < 12) {
      break;
    }
    if (!(coverage & KERX_VERTICAL)) {
      uint32_t format = coverage & 0xFFu;

      if ((coverage & KERX_CROSS) && !seen_cross) {
        size_t i;

        // The first cross-stream subtable hangs every glyph on the one before it,
        // over whatever an earlier subtable attached them to.
        seen_cross = true;
        for (i = 1; i < b->len; i++) {
          if (b->pos[i].attach_type) {
            b->pos[i].attach_type = GFNT_ATTACH_CURSIVE;
            b->pos[i].attach_chain = -1;
          }
        }
      }
      if (format == 1) {
        bool flip = (coverage & KERX_DESCENDING) != 0;

        if (flip) {
          gfnt_lbuf_reverse(b);
        }
        kerx_format1(&table, cursor, glyphs, (coverage & KERX_CROSS) != 0, b,
            kern_mask, &bad);
        if (flip) {
          gfnt_lbuf_reverse(b);
        }
        if (coverage & KERX_CROSS) {
          shifts = true;
        }
      }
      else if (format == 4) {
        // The descending flag is not read here: the glyphs are met in order.
        kerx_format4(&table, cursor, glyphs, &ankr, have_ankr, b, &bad);
      }
      else if (!(coverage & KERX_DESCENDING)) {
        kerx_run(&table, cursor, glyphs, format, (coverage & KERX_CROSS) != 0,
            gfnt_lr_u32(&table, cursor + 8, &bad) != 0, &c, b, kern_mask, &bad);
      }
    }
    cursor += length;
  }
  if (shifts) {
    kerx_hang_shifts(b);
  }
  if (bad) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_kerx, 0,
        GFNT_GLYPH_NONE, "the kerx table reads past itself");
  }
  return GFNT_OK;
}
