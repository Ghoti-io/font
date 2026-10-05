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
 * `GPOS` lookup types 1 to 9: single and pair adjustment, cursive attachment, the
 * three mark attachments, the two contextual forms (apply.c) and the extension
 * wrapper; and the pass at the end that turns attachments into offsets.
 *
 * Positions are in font units and no `Device` table is read for a pixel size,
 * because this engine shapes at the font's own scale. A `VariationIndex` device
 * *is* read, against the `GDEF` item variation store, when the caller gave a
 * location: that is how a variable font moves a kern.
 */

#include "layout.h"
#include "../core/fixed.h"
#include "../var/ivs.h"

#define GFNT_VF_X_PLACEMENT 0x0001u
#define GFNT_VF_Y_PLACEMENT 0x0002u
#define GFNT_VF_X_ADVANCE 0x0004u
#define GFNT_VF_Y_ADVANCE 0x0008u
#define GFNT_VF_X_PLA_DEVICE 0x0010u
#define GFNT_VF_Y_PLA_DEVICE 0x0020u
#define GFNT_VF_X_ADV_DEVICE 0x0040u
#define GFNT_VF_Y_ADV_DEVICE 0x0080u
#define GFNT_VF_DEVICES 0x00F0u

/** A 16.16 value rounded to a whole unit, a tie going as the location says. */
static int32_t gfnt_gpos_round(const GFNT_LApply * c, int64_t value) {
  return (int32_t)gfnt_round_shift_mode(value, 16,
      c->variation ? c->variation->delta_rounding : 0);
}

static void gfnt_gpos_fault(GFNT_LApply * c, size_t offset) {
  if (!c->fault.bad) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = offset;
  }
}

/** The number of 16-bit fields a value format stores. */
static uint32_t gfnt_vf_length(uint32_t format) {
  uint32_t n = 0;

  for (; format; format &= format - 1) {
    n++;
  }
  return n;
}

/**
 * A device table's delta, as 16.16, when it is a variation index and there is a location.
 *
 * A device table for a pixel size is not read: this engine has no ppem. The same
 * table with the format 0x8000 is a pair of indices into the `GDEF` store, and
 * is what a variable font uses.
 */
static int64_t gfnt_gpos_device_delta(GFNT_LApply * c, size_t device) {
  const GFNT_Gdef * gdef = c->gdef;
  GFNT_Reader store;
  int64_t delta = 0;
  uint32_t outer;
  uint32_t inner;

  if (!device || !c->variation || !c->variation->count || !gdef->var_store) {
    return 0;
  }
  if (gfnt_lu16(c, device + 4) != 0x8000u) {
    return 0;
  }
  outer = gfnt_lu16(c, device);
  inner = gfnt_lu16(c, device + 2);
  if (gfnt_reader_sub(&gdef->table, gdef->var_store, GFNT_READER_REST, &store)
      != GFNT_OK) {
    return 0;
  }
  store.error = NULL;
  if (gfnt_ivs_delta(c->face, &store, GFNT_TAG_GDEF, c->variation->coords,
          c->variation->count, outer, inner, &delta, NULL) != GFNT_OK) {
    return 0;
  }
  // The store's sum has 24 fractional bits; positions carry 16.
  return gfnt_round_shift_mode(delta, 8, c->variation->delta_rounding);
}

/** Add one value record to a glyph's position: HarfBuzz's apply_value(). */
static bool gfnt_gpos_apply_value(GFNT_LApply * c, uint32_t format, size_t base,
    size_t values, GFNT_LPos * pos) {
  bool applied = false;

  if (!format) {
    return false;
  }
  if (format & GFNT_VF_X_PLACEMENT) {
    pos->x_offset += gfnt_ls16(c, values);
    values += 2;
    applied = true;
  }
  if (format & GFNT_VF_Y_PLACEMENT) {
    pos->y_offset += gfnt_ls16(c, values);
    values += 2;
    applied = true;
  }
  if (format & GFNT_VF_X_ADVANCE) {
    pos->x_advance += gfnt_ls16(c, values);
    values += 2;
    applied = true;
  }
  if (format & GFNT_VF_Y_ADVANCE) {
    // Only vertical text moves the pen along y; in horizontal text the value is
    // read and dropped, as HarfBuzz does. (Vertical text is not shaped here.)
    values += 2;
  }
  if (format & GFNT_VF_DEVICES) {
    if (format & GFNT_VF_X_PLA_DEVICE) {
      pos->x_offset += gfnt_gpos_round(c, gfnt_gpos_device_delta(c,
          gfnt_l_rel(base, gfnt_lu16(c, values))));
      values += 2;
    }
    if (format & GFNT_VF_Y_PLA_DEVICE) {
      pos->y_offset += gfnt_gpos_round(c, gfnt_gpos_device_delta(c,
          gfnt_l_rel(base, gfnt_lu16(c, values))));
      values += 2;
    }
    if (format & GFNT_VF_X_ADV_DEVICE) {
      pos->x_advance += gfnt_gpos_round(c, gfnt_gpos_device_delta(c,
          gfnt_l_rel(base, gfnt_lu16(c, values))));
      values += 2;
    }
    if (format & GFNT_VF_Y_ADV_DEVICE) {
      values += 2;
    }
  }
  return applied;
}

/**
 * An anchor's coordinates.
 *
 * Format 2 names a contour point of the glyph, and this reads the stated
 * coordinates instead: the point is the same place in every font that has been
 * seen, and following it would need the outline of every glyph a lookup touches.
 */
static void gfnt_gpos_anchor(GFNT_LApply * c, size_t anchor, int64_t * x,
    int64_t * y) {
  uint32_t format = gfnt_lu16(c, anchor);

  *x = (int64_t)gfnt_ls16(c, anchor + 2) * 65536;
  *y = (int64_t)gfnt_ls16(c, anchor + 4) * 65536;
  if (format == 3) {
    // Each coordinate's delta is a whole number of units before the two anchors
    // are subtracted, as in HarfBuzz: rounding the difference instead lands a
    // unit away whenever both deltas are fractional.
    *x += (int64_t)gfnt_gpos_round(c, gfnt_gpos_device_delta(c,
        gfnt_l_rel(anchor, gfnt_lu16(c, anchor + 6)))) * 65536;
    *y += (int64_t)gfnt_gpos_round(c, gfnt_gpos_device_delta(c,
        gfnt_l_rel(anchor, gfnt_lu16(c, anchor + 8)))) * 65536;
  }
  else if (format != 1 && format != 2) {
    gfnt_gpos_fault(c, anchor);
  }
}

/** The next glyph the lookup would consider, from the cursor. */
static bool gfnt_gpos_next_glyph(GFNT_LApply * c, GFNT_LIter * it) {
  gfnt_liter_init(it, c, false);
  gfnt_liter_reset(it, c->buf->idx, 1);
  return gfnt_liter_next(it);
}

static bool gfnt_gpos_single(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t format = gfnt_lu16(c, sub);
  uint32_t value_format = gfnt_lu16(c, sub + 4);
  int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
      gfnt_lbuf_cur(b)->glyph);
  size_t values;

  if (index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  if (format == 1) {
    values = sub + 6;
  }
  else if (format == 2) {
    if ((uint32_t)index >= gfnt_lu16(c, sub + 6)) {
      return false;
    }
    values = sub + 8 + 2 * (size_t)gfnt_vf_length(value_format) * (size_t)index;
  }
  else {
    gfnt_gpos_fault(c, sub);
    return false;
  }
  (void)gfnt_gpos_apply_value(c, value_format, sub, values,
      &b->pos[b->idx]);
  b->idx++;
  return true;
}

static bool gfnt_gpos_pair(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t format = gfnt_lu16(c, sub);
  uint32_t format1 = gfnt_lu16(c, sub + 4);
  uint32_t format2 = gfnt_lu16(c, sub + 6);
  uint32_t len1 = gfnt_vf_length(format1);
  uint32_t len2 = gfnt_vf_length(format2);
  int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
      gfnt_lbuf_cur(b)->glyph);
  GFNT_LIter it;
  size_t at;

  if (index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  if (!gfnt_gpos_next_glyph(c, &it)) {
    return false;
  }
  at = it.idx;
  if (format == 1) {
    uint32_t sets = gfnt_lu16(c, sub + 8);
    size_t set;
    uint32_t count;
    size_t record_size = 2 + 2 * (size_t)(len1 + len2);
    uint32_t low = 0;
    uint32_t high;
    uint32_t second = b->info[at].glyph;

    if ((uint32_t)index >= sets) {
      return false;
    }
    set = gfnt_l_rel(sub, gfnt_lu16(c, sub + 10 + 2 * (size_t)index));
    if (!set) {
      return false;
    }
    count = gfnt_lu16(c, set);
    high = count;
    while (low < high && !c->fault.bad) {
      uint32_t mid = low + (high - low) / 2;
      size_t record = set + 2 + record_size * (size_t)mid;
      uint32_t glyph = gfnt_lu16(c, record);

      if (second < glyph) {
        high = mid;
      }
      else if (second > glyph) {
        low = mid + 1;
      }
      else {
        // Device offsets of a format 1 value are from the PairSet.
        (void)gfnt_gpos_apply_value(c, format1, set, record + 2,
            &b->pos[b->idx]);
        (void)gfnt_gpos_apply_value(c, format2, set, record + 2 + 2 * (size_t)len1,
            &b->pos[at]);
        b->idx = at;
        if (len2) {
          b->idx++;
        }
        return true;
      }
    }
    return false;
  }
  if (format == 2) {
    size_t class1 = gfnt_l_rel(sub, gfnt_lu16(c, sub + 8));
    size_t class2 = gfnt_l_rel(sub, gfnt_lu16(c, sub + 10));
    uint32_t class1_count = gfnt_lu16(c, sub + 12);
    uint32_t class2_count = gfnt_lu16(c, sub + 14);
    uint32_t k1 = gfnt_l_class(c, class1, gfnt_lbuf_cur(b)->glyph);
    uint32_t k2 = gfnt_l_class(c, class2, b->info[at].glyph);
    size_t record;

    if (k1 >= class1_count || k2 >= class2_count) {
      return false;
    }
    record = sub + 16
        + 2 * (size_t)(len1 + len2) * ((size_t)k1 * class2_count + k2);
    (void)gfnt_gpos_apply_value(c, format1, sub, record, &b->pos[b->idx]);
    (void)gfnt_gpos_apply_value(c, format2, sub, record + 2 * (size_t)len1,
        &b->pos[at]);
    b->idx = at;
    if (len2) {
      b->idx++;
    }
    return true;
  }
  gfnt_gpos_fault(c, sub);
  return false;
}

/** Undo the links of a cursive chain that is about to be re-parented. */
static void gfnt_gpos_reverse_cursive(GFNT_LPos * pos, size_t i,
    size_t new_parent) {
  int32_t chain = pos[i].attach_chain;
  uint8_t type = pos[i].attach_type;
  size_t j;

  if (!chain || !(type & GFNT_ATTACH_CURSIVE)) {
    return;
  }
  pos[i].attach_chain = 0;
  j = (size_t)((int64_t)i + chain);
  // Stop when the new parent is met on the old chain.
  if (j == new_parent) {
    return;
  }
  gfnt_gpos_reverse_cursive(pos, j, new_parent);
  pos[j].y_offset = -pos[i].y_offset;
  pos[j].attach_chain = -chain;
  pos[j].attach_type = type;
}

static bool gfnt_gpos_cursive(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  size_t coverage = gfnt_l_rel(sub, gfnt_lu16(c, sub + 2));
  uint32_t count = gfnt_lu16(c, sub + 4);
  int32_t this_index = gfnt_l_coverage(c, coverage, gfnt_lbuf_cur(b)->glyph);
  size_t this_record;
  size_t entry;
  size_t prev_record;
  size_t exit_anchor;
  int32_t prev_index;
  GFNT_LIter it;
  size_t i;
  size_t j;
  int64_t entry_x;
  int64_t entry_y;
  int64_t exit_x;
  int64_t exit_y;
  int32_t d;
  size_t child;
  size_t parent;
  int64_t x_offset;
  int64_t y_offset;

  if (gfnt_lu16(c, sub) != 1) {
    gfnt_gpos_fault(c, sub);
    return false;
  }
  if (this_index == GFNT_LAYOUT_NOT_COVERED || (uint32_t)this_index >= count) {
    return false;
  }
  this_record = sub + 6 + 4 * (size_t)this_index;
  entry = gfnt_l_rel(sub, gfnt_lu16(c, this_record));
  if (!entry) {
    return false;
  }
  gfnt_liter_init(&it, c, false);
  gfnt_liter_reset(&it, b->idx, 1);
  if (!gfnt_liter_prev(&it)) {
    return false;
  }
  prev_index = gfnt_l_coverage(c, coverage, b->info[it.idx].glyph);
  if (prev_index == GFNT_LAYOUT_NOT_COVERED || (uint32_t)prev_index >= count) {
    return false;
  }
  prev_record = sub + 6 + 4 * (size_t)prev_index;
  exit_anchor = gfnt_l_rel(sub, gfnt_lu16(c, prev_record + 2));
  if (!exit_anchor) {
    return false;
  }
  i = it.idx;
  j = b->idx;
  gfnt_gpos_anchor(c, exit_anchor, &exit_x, &exit_y);
  gfnt_gpos_anchor(c, entry, &entry_x, &entry_y);
  if (!c->rtl) {
    b->pos[i].x_advance = gfnt_gpos_round(c, exit_x) + b->pos[i].x_offset;
    d = gfnt_gpos_round(c, entry_x) + b->pos[j].x_offset;
    b->pos[j].x_advance -= d;
    b->pos[j].x_offset -= d;
  }
  else {
    d = gfnt_gpos_round(c, exit_x) + b->pos[i].x_offset;
    b->pos[i].x_advance -= d;
    b->pos[i].x_offset -= d;
    b->pos[j].x_advance = gfnt_gpos_round(c, entry_x) + b->pos[j].x_offset;
  }
  // The cross-direction adjustment: attach the child to the parent. Which of the
  // two glyphs is which depends on the lookup's right-to-left flag, because the
  // flag says which one stays on the baseline.
  child = i;
  parent = j;
  x_offset = entry_x - exit_x;
  y_offset = entry_y - exit_y;
  if (!(c->lookup_props & GFNT_LF_RIGHT_TO_LEFT)) {
    size_t k = child;

    child = parent;
    parent = k;
    x_offset = -x_offset;
    y_offset = -y_offset;
  }
  (void)x_offset;
  // If the child was already attached, its old chain is reversed so that the
  // whole tree it was part of now hangs from the new parent.
  gfnt_gpos_reverse_cursive(b->pos, child, parent);
  b->pos[child].attach_type = GFNT_ATTACH_CURSIVE;
  b->pos[child].attach_chain = (int32_t)((int64_t)parent - (int64_t)child);
  b->has_attachment = true;
  b->pos[child].y_offset = gfnt_gpos_round(c, y_offset);
  // If the parent was attached to the child, separate them.
  if (b->pos[parent].attach_chain == -b->pos[child].attach_chain) {
    b->pos[parent].attach_chain = 0;
    b->pos[parent].y_offset = 0;
  }
  b->idx++;
  return true;
}

/**
 * Attach the current mark to an anchor of the glyph at @p glyph_pos.
 *
 * @p mark_array is the MarkArray. The anchor is one of a row of `class_count`
 * anchor offsets at @p row, whose offsets are relative to @p anchor_table: the
 * BaseArray, a LigatureAttach, or a Mark2Array.
 */
static bool gfnt_gpos_mark_attach(GFNT_LApply * c, size_t mark_array,
    uint32_t mark_index, size_t anchor_table, size_t row, uint32_t class_count,
    size_t glyph_pos) {
  GFNT_LBuffer * b = c->buf;
  size_t record = mark_array + 2 + 4 * (size_t)mark_index;
  uint32_t mark_class;
  size_t mark_anchor;
  size_t glyph_anchor;
  int64_t mark_x;
  int64_t mark_y;
  int64_t base_x;
  int64_t base_y;
  GFNT_LPos * o;

  if (mark_index >= gfnt_lu16(c, mark_array)) {
    return false;
  }
  mark_class = gfnt_lu16(c, record);
  mark_anchor = gfnt_l_rel(mark_array, gfnt_lu16(c, record + 2));
  if (mark_class >= class_count) {
    return false;
  }
  // No anchor for this class on this base: let a later subtable try.
  glyph_anchor = gfnt_l_rel(anchor_table,
      gfnt_lu16(c, row + 2 * (size_t)mark_class));
  if (!glyph_anchor || !mark_anchor) {
    return false;
  }
  gfnt_gpos_anchor(c, mark_anchor, &mark_x, &mark_y);
  gfnt_gpos_anchor(c, glyph_anchor, &base_x, &base_y);
  o = &b->pos[b->idx];
  o->x_offset = gfnt_gpos_round(c, base_x - mark_x);
  o->y_offset = gfnt_gpos_round(c, base_y - mark_y);
  o->attach_type = GFNT_ATTACH_MARK;
  o->attach_chain = (int32_t)((int64_t)glyph_pos - (int64_t)b->idx);
  b->has_attachment = true;
  b->idx++;
  return true;
}

static bool gfnt_gpos_mark_base(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  int32_t mark_index = gfnt_l_coverage(c,
      gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), gfnt_lbuf_cur(b)->glyph);
  uint32_t class_count = gfnt_lu16(c, sub + 6);
  size_t mark_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 8));
  size_t base_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 10));
  GFNT_LIter it;
  int32_t base_index;

  if (gfnt_lu16(c, sub) != 1) {
    gfnt_gpos_fault(c, sub);
    return false;
  }
  if (mark_index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  // The nearest glyph before that is not a mark, whatever the lookup's flags.
  gfnt_liter_init(&it, c, false);
  it.lookup_props = GFNT_LF_IGNORE_MARKS;
  gfnt_liter_reset(&it, b->idx, 1);
  for (;;) {
    const GFNT_LInfo * found;

    if (!gfnt_liter_prev(&it)) {
      return false;
    }
    // A mark attaches to the first glyph of a multiple substitution's output, not
    // to the others: those are skipped, and the search goes on behind them.
    found = &b->info[it.idx];
    if (!(found->props & GFNT_PROP_MULTIPLIED) || gfnt_l_lig_comp(found) == 0
        || it.idx == 0
        || gfnt_l_lig_id(found) != gfnt_l_lig_id(&b->info[it.idx - 1])
        || gfnt_l_lig_comp(found)
            != gfnt_l_lig_comp(&b->info[it.idx - 1]) + 1) {
      break;
    }
    it.num_items++;
  }
  base_index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 4)),
      b->info[it.idx].glyph);
  if (base_index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)base_index >= gfnt_lu16(c, base_array)) {
    return false;
  }
  return gfnt_gpos_mark_attach(c, mark_array, (uint32_t)mark_index, base_array,
      base_array + 2 + 2 * (size_t)class_count * (size_t)base_index,
      class_count, it.idx);
}

static bool gfnt_gpos_mark_ligature(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  int32_t mark_index = gfnt_l_coverage(c,
      gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), gfnt_lbuf_cur(b)->glyph);
  uint32_t class_count = gfnt_lu16(c, sub + 6);
  size_t mark_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 8));
  size_t lig_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 10));
  GFNT_LIter it;
  int32_t lig_index;
  size_t attach;
  uint32_t comp_count;
  uint32_t comp_index;
  uint32_t lig_id;
  uint32_t mark_id;
  uint32_t mark_comp;

  if (gfnt_lu16(c, sub) != 1) {
    gfnt_gpos_fault(c, sub);
    return false;
  }
  if (mark_index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  gfnt_liter_init(&it, c, false);
  it.lookup_props = GFNT_LF_IGNORE_MARKS;
  gfnt_liter_reset(&it, b->idx, 1);
  if (!gfnt_liter_prev(&it)) {
    return false;
  }
  lig_index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 4)),
      b->info[it.idx].glyph);
  if (lig_index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)lig_index >= gfnt_lu16(c, lig_array)) {
    return false;
  }
  attach = gfnt_l_rel(lig_array, gfnt_lu16(c, lig_array + 2 + 2 * (size_t)lig_index));
  if (!attach) {
    return false;
  }
  comp_count = gfnt_lu16(c, attach);
  if (!comp_count) {
    return false;
  }
  // If the mark belongs to this ligature's own component, attach to that one;
  // otherwise to the last component.
  lig_id = gfnt_l_lig_id(&b->info[it.idx]);
  mark_id = gfnt_l_lig_id(gfnt_lbuf_cur(b));
  mark_comp = gfnt_l_lig_comp(gfnt_lbuf_cur(b));
  if (lig_id && lig_id == mark_id && mark_comp > 0) {
    comp_index = (comp_count < mark_comp ? comp_count : mark_comp) - 1;
  }
  else {
    comp_index = comp_count - 1;
  }
  return gfnt_gpos_mark_attach(c, mark_array, (uint32_t)mark_index, attach,
      attach + 2 + 2 * (size_t)class_count * (size_t)comp_index, class_count,
      it.idx);
}

static bool gfnt_gpos_mark_mark(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  int32_t mark1_index = gfnt_l_coverage(c,
      gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), gfnt_lbuf_cur(b)->glyph);
  uint32_t class_count = gfnt_lu16(c, sub + 6);
  size_t mark1_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 8));
  size_t mark2_array = gfnt_l_rel(sub, gfnt_lu16(c, sub + 10));
  GFNT_LIter it;
  size_t j;
  uint32_t id1;
  uint32_t id2;
  uint32_t comp1;
  uint32_t comp2;
  bool good = false;
  int32_t mark2_index;

  if (gfnt_lu16(c, sub) != 1) {
    gfnt_gpos_fault(c, sub);
    return false;
  }
  if (mark1_index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  // The nearest glyph before, under the lookup's own flags minus the ignore ones.
  gfnt_liter_init(&it, c, false);
  it.lookup_props = c->lookup_props & ~(uint32_t)GFNT_LF_IGNORE_FLAGS;
  gfnt_liter_reset(&it, b->idx, 1);
  if (!gfnt_liter_prev(&it)) {
    return false;
  }
  if (!gfnt_l_is_mark(&b->info[it.idx])) {
    return false;
  }
  j = it.idx;
  id1 = gfnt_l_lig_id(gfnt_lbuf_cur(b));
  id2 = gfnt_l_lig_id(&b->info[j]);
  comp1 = gfnt_l_lig_comp(gfnt_lbuf_cur(b));
  comp2 = gfnt_l_lig_comp(&b->info[j]);
  if (id1 == id2) {
    // Marks of one base, or of one component of one ligature.
    good = id1 == 0 || comp1 == comp2;
  }
  else {
    // Different ligatures: it still matches if one of the marks is itself a
    // ligature.
    good = (id1 > 0 && !comp1) || (id2 > 0 && !comp2);
  }
  if (!good) {
    return false;
  }
  mark2_index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 4)),
      b->info[j].glyph);
  if (mark2_index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)mark2_index >= gfnt_lu16(c, mark2_array)) {
    return false;
  }
  return gfnt_gpos_mark_attach(c, mark1_array, (uint32_t)mark1_index,
      mark2_array,
      mark2_array + 2 + 2 * (size_t)class_count * (size_t)mark2_index,
      class_count, j);
}

bool gfnt_gpos_apply_subtable(GFNT_LApply * c, uint16_t type, size_t sub) {
  if (type == 9) {
    uint32_t offset;

    if (gfnt_lu16(c, sub) != 1) {
      gfnt_gpos_fault(c, sub);
      return false;
    }
    type = gfnt_lu16(c, sub + 2);
    offset = gfnt_lu32(c, sub + 4);
    sub += offset;
    if (type == 9 || c->fault.bad) {
      return false;
    }
  }
  switch (type) {
    case 1:
      return gfnt_gpos_single(c, sub);
    case 2:
      return gfnt_gpos_pair(c, sub);
    case 3:
      return gfnt_gpos_cursive(c, sub);
    case 4:
      return gfnt_gpos_mark_base(c, sub);
    case 5:
      return gfnt_gpos_mark_ligature(c, sub);
    case 6:
      return gfnt_gpos_mark_mark(c, sub);
    case 7:
      return gfnt_l_context_apply(c, sub);
    case 8:
      return gfnt_l_chain_context_apply(c, sub);
    default:
      gfnt_gpos_fault(c, sub);
      return false;
  }
}

void gfnt_gpos_position_start(GFNT_LBuffer * b) {
  size_t i;

  for (i = 0; i < b->len; i++) {
    b->pos[i].attach_chain = 0;
    b->pos[i].attach_type = 0;
  }
  b->has_attachment = false;
}

/**
 * Make an attachment's offset absolute: add the offset of the glyph it hangs
 * from, and take away the advances between the two, since the offset was
 * measured from the base's origin and the pen has moved on.
 */
static void gfnt_gpos_propagate(GFNT_LPos * pos, size_t len, size_t i,
    bool rtl) {
  int32_t chain = pos[i].attach_chain;
  uint8_t type = pos[i].attach_type;
  size_t j;
  size_t k;

  if (!chain) {
    return;
  }
  pos[i].attach_chain = 0;
  if (chain < 0 && (size_t)(-(int64_t)chain) > i) {
    return;
  }
  j = (size_t)((int64_t)i + chain);
  if (j >= len) {
    return;
  }
  gfnt_gpos_propagate(pos, len, j, rtl);
  if (type & GFNT_ATTACH_CURSIVE) {
    pos[i].y_offset += pos[j].y_offset;
  }
  else {
    pos[i].x_offset += pos[j].x_offset;
    pos[i].y_offset += pos[j].y_offset;
    if (j < i && !rtl) {
      for (k = j; k < i; k++) {
        pos[i].x_offset -= pos[k].x_advance;
        pos[i].y_offset -= pos[k].y_advance;
      }
    }
    else if (j < i) {
      for (k = j + 1; k < i + 1; k++) {
        pos[i].x_offset += pos[k].x_advance;
        pos[i].y_offset += pos[k].y_advance;
      }
    }
  }
}

void gfnt_gpos_position_finish_offsets(GFNT_LBuffer * b, bool rtl) {
  size_t i;

  if (!b->has_attachment) {
    return;
  }
  for (i = 0; i < b->len; i++) {
    gfnt_gpos_propagate(b->pos, b->len, i, rtl);
  }
}
