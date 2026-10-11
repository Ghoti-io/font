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
 * (version 0) and Apple's (version 1). The class-based format 2 and the compact format 3 (a
 * matrix of byte classes) are read in both; Apple's state-machine format 1 is not. A subtable of a format this reads is added to the kerning of the
 * pair with every other.
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

/**
 * One pair's kerning from a class-based (format 2) subtable, or 0.
 *
 * The class values are byte offsets from the start of the *subtable*, not of the
 * value array: a left value is the offset of its row (the array's own offset
 * included) and a right value the offset of its column inside the row. A pair
 * kerns only if both glyphs are in their class tables and the cell they name lies
 * in the array. This is how HarfBuzz reads them, found by experiment on tables
 * built to tell the readings apart: counting left values from the array kerns
 * nothing there, and a glyph outside the right table is never kerned.
 */
static int32_t gfnt_kern_format2(const GFNT_Reader * r, size_t subtable,
    size_t length, size_t header, uint32_t left, uint32_t right, bool * bad) {
  size_t body = subtable + header;
  size_t left_table = subtable + gfnt_lr_u16(r, body + 2, bad);
  size_t right_table = subtable + gfnt_lr_u16(r, body + 4, bad);
  uint32_t array = gfnt_lr_u16(r, body + 6, bad);
  uint32_t l;
  uint32_t k;
  uint32_t first;
  uint32_t count;

  first = gfnt_lr_u16(r, left_table, bad);
  count = gfnt_lr_u16(r, left_table + 2, bad);
  if (*bad || left < first || left - first >= count) {
    return 0;
  }
  l = gfnt_lr_u16(r, left_table + 4 + 2 * (size_t)(left - first), bad);
  first = gfnt_lr_u16(r, right_table, bad);
  count = gfnt_lr_u16(r, right_table + 2, bad);
  if (*bad || right < first || right - first >= count) {
    return 0;
  }
  k = gfnt_lr_u16(r, right_table + 4 + 2 * (size_t)(right - first), bad);
  if (*bad || (size_t)l + k < array || (size_t)l + k + 2 > length) {
    return 0;
  }
  return (int16_t)gfnt_lr_u16(r, subtable + l + k, bad);
}

static uint32_t gfnt_lr_u8(const GFNT_Reader * r, size_t offset, bool * bad) {
  uint8_t v = 0;

  if (gfnt_reader_u8_at(r, offset, &v) != GFNT_OK) {
    *bad = true;
  }
  return v;
}

/**
 * One pair's kerning from a compact format 3 subtable, or 0. HarfBuzz reads it behind either
 * header, and so does this.
 *
 * Each glyph has a left class and a right class (a byte each, indexed by glyph id), a
 * pair picks a cell of the class matrix, the cell holds an index, and the index picks a
 * value. A glyph past the class arrays is in class 0, which is a class like another: its
 * cell is read. A class past the matrix, or an index past the values, kerns nothing.
 */
static int32_t gfnt_kern_format3(const GFNT_Reader * r, size_t subtable, size_t header,
    uint32_t left, uint32_t right, bool * bad) {
  size_t body = subtable + header;
  uint32_t glyphs = gfnt_lr_u16(r, body, bad);
  uint32_t values = gfnt_lr_u8(r, body + 2, bad);
  uint32_t left_classes = gfnt_lr_u8(r, body + 3, bad);
  uint32_t right_classes = gfnt_lr_u8(r, body + 4, bad);
  size_t value_at = body + 6;
  size_t left_at = value_at + 2 * (size_t)values;
  size_t right_at = left_at + glyphs;
  size_t index_at = right_at + glyphs;
  uint32_t l = 0;
  uint32_t k = 0;
  uint32_t cell;
  uint32_t index;

  if (*bad) {
    return 0;
  }
  if (left < glyphs) {
    l = gfnt_lr_u8(r, left_at + left, bad);
  }
  if (right < glyphs) {
    k = gfnt_lr_u8(r, right_at + right, bad);
  }
  if (*bad || l >= left_classes || k >= right_classes) {
    return 0;
  }
  cell = l * right_classes + k;
  index = gfnt_lr_u8(r, index_at + cell, bad);
  if (*bad || index >= values) {
    return 0;
  }
  return (int16_t)gfnt_lr_u16(r, value_at + 2 * (size_t)index, bad);
}

/** One subtable's value for a pair, or 0. */
static int32_t gfnt_kern_subtable_pair(const GFNT_Reader * r, size_t cursor,
    size_t length, size_t header, uint32_t format, uint32_t left, uint32_t right,
    bool * bad) {
  if (format == 3) {
    return gfnt_kern_format3(r, cursor, header, left, right, bad);
  }
  if (format == 0) {
    return gfnt_kern_format0(r, cursor, header, left, right, bad);
  }
  if (format == 2) {
    return gfnt_kern_format2(r, cursor, length, header, left, right, bad);
  }
  return 0;
}

/**
 * A state-machine (format 1) subtable: contextual kerning, in the older 16-bit form of an AAT
 * state table. A glyph's class comes from a byte array, the state array holds entry numbers, and
 * a transition names its next state by the *byte offset* of that state's row. The actions are the
 * same as `kerx`'s format 1: glyphs a transition pushes are kept on a stack of eight, and a
 * transition that names values applies them to the stack from the top until one has its low bit
 * set. HarfBuzz applies the value to the glyph's advance and its offset alike.
 */
#define KERN_STACK 8u
#define KERN_DETACHED INT32_MIN

static bool gfnt_kern_format1(const GFNT_Reader * r, size_t machine, size_t end, bool cross,
    bool vertical, GFNT_LBuffer * b, uint32_t kern_mask, bool * bad) {
  uint32_t classes = gfnt_lr_u16(r, machine, bad);
  size_t class_table = machine + gfnt_lr_u16(r, machine + 2, bad);
  size_t state_array = machine + gfnt_lr_u16(r, machine + 4, bad);
  size_t entry_table = machine + gfnt_lr_u16(r, machine + 6, bad);
  size_t values = machine + gfnt_lr_u16(r, machine + 8, bad);
  uint32_t first_glyph = gfnt_lr_u16(r, class_table, bad);
  uint32_t glyph_count = gfnt_lr_u16(r, class_table + 2, bad);
  size_t stack[KERN_STACK];
  size_t depth = 0;
  size_t row = state_array;       /* the byte offset of the current state's row */
  bool wrote = false;             /* a cross-stream value landed on some glyph */

  // Found by experiment on tables built to tell the readings apart, and not understood: HarfBuzz
  // does not perform the end-of-text transition of a run in which every glyph is out of bounds
  // (outside the class table or in class 1), no transition has yet named an action, and the
  // machine is back in the start state. That fits every table tried but a sliver of the random
  // ones (about 1 string in 1,200), which the differential reports.
  bool every_glyph_out = true;
  bool acted = false;
  {
    size_t i;

    for (i = 0; i < b->len && every_glyph_out; i++) {
      uint32_t glyph = b->info[i].glyph;

      if (glyph >= first_glyph && glyph - first_glyph < glyph_count
          && gfnt_lr_u8(r, class_table + 4 + (glyph - first_glyph), bad) != 1) {
        every_glyph_out = false;
      }
    }
  }
  b->idx = 0;
  while (!*bad && classes) {
    uint32_t klass = 0;
    uint32_t index;
    size_t entry;
    uint32_t new_state;
    uint32_t flags;

    if (b->idx < b->len) {
      uint32_t glyph = b->info[b->idx].glyph;

      if (glyph == 0xFFFFu) {
        klass = 2;
      }
      else if (glyph >= first_glyph && glyph - first_glyph < glyph_count) {
        klass = gfnt_lr_u8(r, class_table + 4 + (glyph - first_glyph), bad);
      }
      else {
        klass = 1;
      }
    }
    if (klass >= classes) {
      klass = 1;
    }
    index = gfnt_lr_u8(r, row + klass, bad);
    entry = entry_table + 4 * (size_t)index;
    new_state = gfnt_lr_u16(r, entry, bad);
    flags = gfnt_lr_u16(r, entry + 2, bad);
    if (*bad) {
      return wrote;
    }
    if (b->idx >= b->len && every_glyph_out && !acted && row == state_array) {
      break;
    }
    if (flags & 0x2000u) {
      depth = 0;
    }
    if (flags & 0x8000u) {
      if (depth < KERN_STACK) {
        stack[depth++] = b->idx;
      }
      else {
        depth = 0;
      }
    }
    if (flags & 0x3FFFu) {
      acted = true;
    }
    if ((flags & 0x3FFFu) && depth) {
      size_t at = machine + (flags & 0x3FFFu);
      bool last = false;

      // HarfBuzz checks that a value is there for every glyph on the stack, whether or not an
      // end mark comes sooner, and when the list is cut short by the subtable's end it applies
      // nothing and empties the stack.
      if (at + 2 * depth > end) {
        depth = 0;
      }
      while (!last && depth && !*bad) {
        size_t g = stack[--depth];
        int32_t v = (int16_t)gfnt_lr_u16(r, at, bad);

        at += 2;
        if (g >= b->len) {
          continue;
        }
        last = (v & 1) != 0;
        v &= ~1;
        if (cross) {
          // Across the line of text: y in a horizontal run, x in a vertical one.
          int32_t * across = vertical ? &b->pos[g].x_offset : &b->pos[g].y_offset;

          wrote = true;
          if (v == -0x8000) {
            *across = KERN_DETACHED;
          }
          else if (*across != KERN_DETACHED) {
            *across += v;
          }
        }
        else if (b->info[g].mask & kern_mask) {
          b->pos[g].x_advance += v;
          b->pos[g].x_offset += v;
        }
      }
      (void)values;
    }
    if (new_state < (uint32_t)(state_array - machine)) {
      return wrote;
    }
    row = machine + new_state;
    if (b->idx >= b->len) {
      break;
    }
    if (!(flags & 0x4000u) || b->max_ops-- <= 0) {
      b->idx++;
    }
  }
  return wrote;
}

/**
 * After a cross-stream state machine: a glyph it detached lands at zero, and every other glyph
 * hangs from the one before it, so that the offsets of a chain add up (as `kerx` does).
 */
static void gfnt_kern_hang_shifts(GFNT_LBuffer * b, bool vertical) {
  size_t i;

  for (i = 0; i < b->len; i++) {
    int32_t * across = vertical ? &b->pos[i].x_offset : &b->pos[i].y_offset;

    if (*across == KERN_DETACHED) {
      *across = 0;
      b->pos[i].attach_type = 0;
      b->pos[i].attach_chain = 0;
    }
    else if (i) {
      b->pos[i].attach_type = GFNT_ATTACH_CURSIVE;
      b->pos[i].attach_chain = -1;
      b->has_attachment = true;
    }
  }
}

/**
 * Run every horizontal subtable over the run in turn, as HarfBuzz does: each one
 * splits its own values between the two glyphs of a pair, and a cross-stream one
 * hangs the second glyph from the first and sets that glyph's own shift, which
 * what an earlier subtable gave it yields to.
 */
static void gfnt_kern_run(const GFNT_Reader * r, GFNT_LApply * c, GFNT_LBuffer * b,
    uint32_t kern_mask, bool vertical, bool * bad) {
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
    bool vertical_cross;
    bool cross;
    size_t idx = 0;

    if (apple) {
      length = gfnt_lr_u32(r, cursor, bad);
      coverage = gfnt_lr_u16(r, cursor + 4, bad);
      format = coverage & 0xFFu;
      // A subtable that varies with the design space (0x2000) is one HarfBuzz does not apply.
      horizontal = !(coverage & 0x8000u) && !(coverage & 0x2000u);
      vertical_cross = (coverage & 0x8000u) && !(coverage & 0x2000u);
      cross = (coverage & 0x4000u) != 0;
      header = 8;
    }
    else {
      length = gfnt_lr_u16(r, cursor + 2, bad);
      coverage = gfnt_lr_u16(r, cursor + 4, bad);
      format = coverage >> 8;
      horizontal = (coverage & 1u) != 0;
      vertical_cross = !horizontal;
      cross = (coverage & 4u) != 0;
      header = 6;
    }
    if (*bad || length < header) {
      break;
    }
    if (vertical) {
      // HarfBuzz leaves the pair tables alone in a vertical run, and of the state machines runs only
      // the cross-stream ones that are marked vertical, which then move glyphs sideways.
      if (format == 1 && cross && vertical_cross) {
        if (gfnt_kern_format1(r, cursor + header, cursor + length, true, true, b, kern_mask, bad)) {
          gfnt_kern_hang_shifts(b, true);
        }
      }
    }
    else if (horizontal && format == 1) {
      if (gfnt_kern_format1(r, cursor + header, cursor + length, cross, false, b, kern_mask, bad)) {
        gfnt_kern_hang_shifts(b, false);
      }
    }
    else if (horizontal && (format == 0 || format == 2 || format == 3)) {
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
        kern = gfnt_kern_subtable_pair(r, cursor, length, header, format, b->info[i].glyph,
            b->info[j].glyph, bad);
        if (cross) {
          if (kern) {
            b->pos[j].y_offset = kern;
          }
          // A state machine's hanging of every glyph from the one before it stands.
          if (!b->pos[j].attach_type) {
            b->pos[j].attach_type = GFNT_ATTACH_CURSIVE;
            b->pos[j].attach_chain = (int32_t)((int64_t)i - (int64_t)j);
          }
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
    const GFNT_Gdef * gdef, uint32_t kern_mask, bool vertical, GFNT_Error * error) {
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
  c.vertical = vertical;
  gfnt_kern_run(&table, &c, b, kern_mask, vertical, &bad);
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

bool gfnt_kern_has_state_machine(const GFNT_Face * face) {
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
    if (apple ? (coverage & 0xFFu) == 1 : (coverage >> 8) == 1) {
      return true;
    }
    cursor += length;
  }
  return false;
}
