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
 * `GSUB` lookup types 1 to 8: single, multiple, alternate and ligature
 * substitution, the two contextual forms (shared with `GPOS`, in apply.c), the
 * extension wrapper, and reverse chaining single substitution.
 */

#include "layout.h"

bool gfnt_gsub_is_reverse(uint16_t type) {
  return type == 8;
}

/** HarfBuzz's _set_glyph_class: the properties a replaced glyph then has. */
static void gfnt_gsub_set_class(GFNT_LApply * c, GFNT_LInfo * info,
    uint32_t glyph, uint16_t guess, bool ligature, bool component) {
  uint16_t props = info->props;

  props |= GFNT_PROP_SUBSTITUTED;
  if (ligature) {
    props |= GFNT_PROP_LIGATED;
    props &= (uint16_t)~GFNT_PROP_MULTIPLIED;
  }
  if (component) {
    props |= GFNT_PROP_MULTIPLIED;
  }
  if (c->gdef->has_glyph_classes) {
    props &= GFNT_PROP_PRESERVE;
    props |= gfnt_gdef_props(c, glyph);
  }
  else if (guess) {
    props &= GFNT_PROP_PRESERVE;
    props |= guess;
  }
  info->props = props;
}

static bool gfnt_gsub_replace(GFNT_LApply * c, uint32_t glyph) {
  gfnt_gsub_set_class(c, gfnt_lbuf_cur(c->buf), glyph, 0, false, false);
  return gfnt_lbuf_replace_glyph(c->buf, glyph);
}

static bool gfnt_gsub_single(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t format = gfnt_lu16(c, sub);
  uint32_t glyph = gfnt_lbuf_cur(b)->glyph;
  int32_t index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)),
      glyph);

  if (index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  if (format == 1) {
    return gfnt_gsub_replace(c,
        (glyph + (uint32_t)(int32_t)gfnt_ls16(c, sub + 4)) & 0xFFFFu);
  }
  if (format == 2) {
    if ((uint32_t)index >= gfnt_lu16(c, sub + 4)) {
      return false;
    }
    return gfnt_gsub_replace(c, gfnt_lu16(c, sub + 6 + 2 * (size_t)index));
  }
  c->fault.bad = true;
  c->fault.table = c->lt->tag;
  c->fault.offset = sub;
  return false;
}

static bool gfnt_gsub_multiple(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t glyph = gfnt_lbuf_cur(b)->glyph;
  int32_t index;
  size_t seq;
  uint32_t count;
  uint16_t klass;
  uint32_t lig_id;
  uint32_t i;

  if (gfnt_lu16(c, sub) != 1) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = sub;
    return false;
  }
  index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), glyph);
  if (index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
    return false;
  }
  seq = gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index));
  if (!seq) {
    return false;
  }
  count = gfnt_lu16(c, seq);
  // One glyph is an in-place replacement, and not a "multiplied" one.
  if (count == 1) {
    return gfnt_gsub_replace(c, gfnt_lu16(c, seq + 2));
  }
  // The specification forbids an empty sequence, and Uniscribe, and so every font
  // that needed one, deletes the glyph.
  if (count == 0) {
    return gfnt_lbuf_delete_glyph(b);
  }
  klass = gfnt_l_is_ligature(gfnt_lbuf_cur(b)) ? GFNT_PROP_BASE : 0;
  lig_id = gfnt_l_lig_id(gfnt_lbuf_cur(b));
  for (i = 0; i < count && !c->fault.bad; i++) {
    uint32_t out = gfnt_lu16(c, seq + 2 + 2 * (size_t)i);
    GFNT_LInfo * cur = gfnt_lbuf_cur(b);

    // Attached to a ligature already: leave that alone. Otherwise each output
    // glyph is a component of the one that was here.
    if (!lig_id) {
      cur->lig_props = (uint8_t)(i & 0x0Fu);
    }
    gfnt_gsub_set_class(c, cur, out, klass, false, true);
    if (!gfnt_lbuf_output_glyph(b, out)) {
      return false;
    }
  }
  return gfnt_lbuf_skip_glyph(b);
}

static bool gfnt_gsub_alternate(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t glyph = gfnt_lbuf_cur(b)->glyph;
  int32_t index;
  size_t set;
  uint32_t count;
  uint32_t shift = 0;
  uint32_t alt_index;
  uint32_t lookup_mask;
  uint32_t glyph_mask;

  if (gfnt_lu16(c, sub) != 1) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = sub;
    return false;
  }
  index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), glyph);
  if (index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
    return false;
  }
  set = gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index));
  if (!set) {
    return false;
  }
  count = gfnt_lu16(c, set);
  if (!count) {
    return false;
  }
  // Which alternate is in the feature's value, in the bits the plan gave it. If
  // two features enabled this lookup together the answer is a mix of the two,
  // which is HarfBuzz's behaviour and is noted there as a known limit. HarfBuzz
  // keeps the bit that every global feature shares above the others, so a lookup
  // a global feature shares with another reads that bit as a very large index
  // and takes no alternate; the bit here is the lowest, and is moved to the top.
  lookup_mask = (c->lookup_mask & ~1u) | ((c->lookup_mask & 1u) << 31);
  glyph_mask = (gfnt_lbuf_cur(b)->mask & ~1u) | ((gfnt_lbuf_cur(b)->mask & 1u) << 31);
  while (shift < 32 && !((lookup_mask >> shift) & 1u)) {
    shift++;
  }
  alt_index = (lookup_mask & glyph_mask) >> shift;
  if (alt_index > count || alt_index == 0) {
    return false;
  }
  return gfnt_gsub_replace(c, gfnt_lu16(c, set + 2 + 2 * (size_t)(alt_index - 1)));
}

/** Replace the glyphs a ligature matched, and keep the ligature bookkeeping. */
static void gfnt_gsub_ligate(GFNT_LApply * c, uint32_t count,
    const size_t * match_positions, size_t match_end, uint32_t lig_glyph,
    uint32_t total_component_count) {
  GFNT_LBuffer * b = c->buf;
  bool is_base_ligature = gfnt_l_is_base(&b->info[match_positions[0]]);
  bool is_mark_ligature = gfnt_l_is_mark(&b->info[match_positions[0]]);
  bool is_ligature;
  uint16_t klass;
  uint32_t lig_id;
  uint32_t last_lig_id;
  uint32_t last_num_comps;
  uint32_t comps_so_far;
  uint32_t i;

  gfnt_lbuf_merge_clusters(b, b->idx, match_end);
  // A base glyph with marks that ligate is still a base, so that the marks that
  // follow can attach to it; and a ligature of marks only is a mark ligature,
  // which keeps its old ligature id so that it can attach to a base ligature.
  for (i = 1; i < count; i++) {
    if (!gfnt_l_is_mark(&b->info[match_positions[i]])) {
      is_base_ligature = false;
      is_mark_ligature = false;
      break;
    }
  }
  is_ligature = !is_base_ligature && !is_mark_ligature;
  klass = is_ligature ? GFNT_PROP_LIGATURE : 0;
  lig_id = is_ligature ? gfnt_lbuf_allocate_lig_id(b) : 0;
  last_lig_id = gfnt_l_lig_id(gfnt_lbuf_cur(b));
  last_num_comps = gfnt_l_lig_num_comps(gfnt_lbuf_cur(b));
  comps_so_far = last_num_comps;
  if (is_ligature) {
    gfnt_lbuf_cur(b)->lig_props = (uint8_t)((lig_id << 5) | 0x10u
        | (total_component_count & 0x0Fu));
  }
  gfnt_gsub_set_class(c, gfnt_lbuf_cur(b), lig_glyph, klass, true, false);
  (void)gfnt_lbuf_replace_glyph(b, lig_glyph);
  for (i = 1; i < count; i++) {
    while (b->idx < match_positions[i] && !b->oom) {
      if (is_ligature) {
        uint32_t this_comp = gfnt_l_lig_comp(gfnt_lbuf_cur(b));
        uint32_t new_comp;

        if (this_comp == 0) {
          this_comp = last_num_comps;
        }
        new_comp = comps_so_far - last_num_comps
            + (this_comp < last_num_comps ? this_comp : last_num_comps);
        gfnt_lbuf_cur(b)->lig_props =
            (uint8_t)((lig_id << 5) | (new_comp & 0x0Fu));
      }
      (void)gfnt_lbuf_next_glyph(b);
    }
    last_lig_id = gfnt_l_lig_id(gfnt_lbuf_cur(b));
    last_num_comps = gfnt_l_lig_num_comps(gfnt_lbuf_cur(b));
    comps_so_far += last_num_comps;
    // The glyph itself is consumed by the ligature.
    b->idx++;
  }
  if (!is_mark_ligature && last_lig_id) {
    size_t k;

    // Marks that followed the last component now follow the ligature's last.
    for (k = b->idx; k < b->len; k++) {
      uint32_t this_comp;
      uint32_t new_comp;

      if (last_lig_id != gfnt_l_lig_id(&b->info[k])) {
        break;
      }
      this_comp = gfnt_l_lig_comp(&b->info[k]);
      if (!this_comp) {
        break;
      }
      new_comp = comps_so_far - last_num_comps
          + (this_comp < last_num_comps ? this_comp : last_num_comps);
      b->info[k].lig_props = (uint8_t)((lig_id << 5) | (new_comp & 0x0Fu));
    }
  }
}

static bool gfnt_gsub_ligature(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t glyph = gfnt_lbuf_cur(b)->glyph;
  int32_t index;
  size_t set;
  uint32_t ligatures;
  uint32_t i;

  if (gfnt_lu16(c, sub) != 1) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = sub;
    return false;
  }
  index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), glyph);
  if (index == GFNT_LAYOUT_NOT_COVERED
      || (uint32_t)index >= gfnt_lu16(c, sub + 4)) {
    return false;
  }
  set = gfnt_l_rel(sub, gfnt_lu16(c, sub + 6 + 2 * (size_t)index));
  if (!set) {
    return false;
  }
  ligatures = gfnt_lu16(c, set);
  for (i = 0; i < ligatures && !c->fault.bad; i++) {
    size_t lig = gfnt_l_rel(set, gfnt_lu16(c, set + 2 + 2 * (size_t)i));
    uint32_t lig_glyph;
    uint32_t count;
    size_t positions[GFNT_LAYOUT_MAX_CONTEXT];
    size_t match_end = 0;
    uint32_t total = 0;
    GFNT_Matcher matcher = {GFNT_MATCH_GLYPH, 0};

    if (!lig) {
      continue;
    }
    lig_glyph = gfnt_lu16(c, lig);
    count = gfnt_lu16(c, lig + 2);
    if (!count) {
      continue;
    }
    // A ligature of one component is a plain replacement and is not counted as a
    // ligature.
    if (count == 1) {
      return gfnt_gsub_replace(c, lig_glyph);
    }
    if (!gfnt_l_match_input(c, count, matcher, lig + 4, &match_end, positions,
            &total)) {
      continue;
    }
    gfnt_gsub_ligate(c, count, positions, match_end, lig_glyph, total);
    return true;
  }
  return false;
}

static bool gfnt_gsub_reverse_chain(GFNT_LApply * c, size_t sub) {
  GFNT_LBuffer * b = c->buf;
  uint32_t glyph = gfnt_lbuf_cur(b)->glyph;
  int32_t index;
  uint32_t backtrack_count;
  size_t backtrack;
  size_t lookahead_at;
  uint32_t lookahead_count;
  size_t lookahead;
  size_t substitutes_at;
  size_t start_index = 0;
  size_t end_index = 0;
  GFNT_Matcher cover = {GFNT_MATCH_COVERAGE, (uint32_t)sub};

  // A reverse lookup cannot be called from another lookup.
  if (c->nesting_left != GFNT_LAYOUT_MAX_NESTING) {
    return false;
  }
  if (gfnt_lu16(c, sub) != 1) {
    c->fault.bad = true;
    c->fault.table = c->lt->tag;
    c->fault.offset = sub;
    return false;
  }
  index = gfnt_l_coverage(c, gfnt_l_rel(sub, gfnt_lu16(c, sub + 2)), glyph);
  if (index == GFNT_LAYOUT_NOT_COVERED) {
    return false;
  }
  backtrack_count = gfnt_lu16(c, sub + 4);
  backtrack = sub + 6;
  lookahead_at = backtrack + 2 * (size_t)backtrack_count;
  lookahead_count = gfnt_lu16(c, lookahead_at);
  lookahead = lookahead_at + 2;
  substitutes_at = lookahead + 2 * (size_t)lookahead_count;
  if ((uint32_t)index >= gfnt_lu16(c, substitutes_at)) {
    return false;
  }
  {
    GFNT_LIter it;
    uint32_t i;

    gfnt_liter_init(&it, c, true);
    gfnt_liter_reset(&it, gfnt_lbuf_backtrack_len(b), backtrack_count);
    gfnt_liter_set_match(&it, cover.kind, cover.origin, backtrack);
    for (i = 0; i < backtrack_count; i++) {
      if (!gfnt_liter_prev(&it)) {
        return false;
      }
    }
    start_index = it.idx;
    gfnt_liter_init(&it, c, true);
    gfnt_liter_reset(&it, b->idx, lookahead_count);
    gfnt_liter_set_match(&it, cover.kind, cover.origin, lookahead);
    for (i = 0; i < lookahead_count; i++) {
      if (!gfnt_liter_next(&it)) {
        return false;
      }
    }
    end_index = it.idx + 1;
  }
  (void)start_index;
  (void)end_index;
  {
    GFNT_LInfo * cur = gfnt_lbuf_cur(b);
    uint32_t out = gfnt_lu16(c, substitutes_at + 2 + 2 * (size_t)index);

    gfnt_gsub_set_class(c, cur, out, 0, false, false);
    // In place, and the cursor is not moved: the loop that called does that.
    cur->glyph = out;
  }
  return true;
}

bool gfnt_gsub_apply_subtable(GFNT_LApply * c, uint16_t type, size_t sub) {
  if (type == 7) {
    uint32_t offset;

    if (gfnt_lu16(c, sub) != 1) {
      c->fault.bad = true;
      c->fault.table = c->lt->tag;
      c->fault.offset = sub;
      return false;
    }
    type = gfnt_lu16(c, sub + 2);
    offset = gfnt_lu32(c, sub + 4);
    sub += offset;
    if (type == 7 || c->fault.bad) {
      return false;
    }
  }
  switch (type) {
    case 1:
      return gfnt_gsub_single(c, sub);
    case 2:
      return gfnt_gsub_multiple(c, sub);
    case 3:
      return gfnt_gsub_alternate(c, sub);
    case 4:
      return gfnt_gsub_ligature(c, sub);
    case 5:
      return gfnt_l_context_apply(c, sub);
    case 6:
      return gfnt_l_chain_context_apply(c, sub);
    case 8:
      return gfnt_gsub_reverse_chain(c, sub);
    default:
      c->fault.bad = true;
      c->fault.table = c->lt->tag;
      c->fault.offset = sub;
      return false;
  }
}
