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

#include "kashida.h"
#include <ghoti.io/unicode/char.h>

#define TATWEEL 0x0640u

typedef struct {
  size_t at;                 // index into the word
  GUNI_JoiningType type;
  GUNI_JoiningGroup group;
} Letter;

static bool joins_forward(const Letter * l) {
  return l->type == GUNI_JT_DUAL_JOINING || l->type == GUNI_JT_LEFT_JOINING
      || l->type == GUNI_JT_JOIN_CAUSING;
}

static bool joins_backward(const Letter * l) {
  return l->type == GUNI_JT_DUAL_JOINING || l->type == GUNI_JT_RIGHT_JOINING
      || l->type == GUNI_JT_JOIN_CAUSING;
}

static bool is_heh(GUNI_JoiningGroup g) {
  return g == GUNI_JG_HEH || g == GUNI_JG_HEH_GOAL || g == GUNI_JG_KNOTTED_HEH
      || g == GUNI_JG_TEH_MARBUTA || g == GUNI_JG_TEH_MARBUTA_GOAL;
}

static bool is_yeh(GUNI_JoiningGroup g) {
  return g == GUNI_JG_YEH || g == GUNI_JG_FARSI_YEH || g == GUNI_JG_YEH_BARREE
      || g == GUNI_JG_YEH_WITH_TAIL || g == GUNI_JG_THIN_YEH
      || g == GUNI_JG_KASHMIRI_YEH || g == GUNI_JG_BURUSHASKI_YEH_BARREE
      || g == GUNI_JG_ROHINGYA_YEH;
}

/** The class of a word-final letter that has been joined to. */
static GFNT_KashidaPriority final_class(GUNI_JoiningGroup g) {
  switch (g) {
    case GUNI_JG_DAL:
      return GFNT_KASHIDA_BEFORE_HEH;
    case GUNI_JG_ALEF:
    case GUNI_JG_TAH:
    case GUNI_JG_LAM:
    case GUNI_JG_KAF:
    case GUNI_JG_SWASH_KAF:
    case GUNI_JG_GAF:
      return GFNT_KASHIDA_BEFORE_ALEF;
    case GUNI_JG_WAW:
    case GUNI_JG_AIN:
    case GUNI_JG_QAF:
    case GUNI_JG_FEH:
    case GUNI_JG_AFRICAN_QAF:
    case GUNI_JG_AFRICAN_FEH:
      return GFNT_KASHIDA_BEFORE_WAW;
    default:
      return is_heh(g) ? GFNT_KASHIDA_BEFORE_HEH : GFNT_KASHIDA_BEFORE_FINAL;
  }
}

bool gfnt_kashida_point(const uint32_t * cp, size_t n, size_t * before,
    GFNT_KashidaPriority * priority, GFNT_KashidaAllowed allowed, void * ctx) {
  // The letters that take part in joining; marks and other transparent
  // characters are skipped, as the shaper skips them.
  Letter letters[256];
  size_t count = 0;
  size_t i;
  size_t best_at = 0;
  GFNT_KashidaPriority best = (GFNT_KashidaPriority)99;

  for (i = 0; i < n; i++) {
    GUNI_JoiningType t = guni_joining_type(cp[i]);
    if (t == GUNI_JT_TRANSPARENT) {
      continue;
    }
    if (t == GUNI_JT_NON_JOINING && cp[i] >= 0x80) {
      GUNI_GeneralCategory gc = guni_general_category(cp[i]);
      if ((gc == GUNI_GC_NONSPACING_MARK || gc == GUNI_GC_ENCLOSING_MARK
              || gc == GUNI_GC_FORMAT) && cp[i] != 0x200C && cp[i] != 0x200D) {
        continue;
      }
    }
    if (count == sizeof letters / sizeof letters[0]) {
      return false;          // not a word anyone justifies
    }
    letters[count].at = i;
    letters[count].type = cp[i] == 0x200D ? GUNI_JT_JOIN_CAUSING : t;
    letters[count].group = guni_joining_group(cp[i]);
    count++;
  }
  // An author's own tatweel wins outright; several in a row are one stroke, and
  // the stretch goes in front of it. The last such stroke is the one nearest the
  // end of the word.
  for (i = 0; i < count; i++) {
    bool stroke = cp[letters[i].at] == TATWEEL;
    bool first = i == 0 || cp[letters[i - 1].at] != TATWEEL;
    if (stroke && first && i > 0 && joins_forward(&letters[i - 1])
        && (!allowed || allowed(ctx, letters[i].at))) {
      best = GFNT_KASHIDA_AUTHOR;
      best_at = letters[i].at;
    }
  }
  for (i = 1; i < count; i++) {
    const Letter * p = &letters[i - 1];
    const Letter * k = &letters[i];
    const Letter * next = i + 1 < count ? &letters[i + 1] : NULL;
    bool k_forward;
    GFNT_KashidaPriority rank;

    if (!joins_forward(p) || !joins_backward(k)) {
      continue;
    }
    // A pair a font must draw as one glyph is never pulled apart: lam-alef, and
    // the lam-lam-heh of Allah (its join to the heh too).
    if (p->group == GUNI_JG_LAM && k->group == GUNI_JG_ALEF) {
      continue;
    }
    if (p->group == GUNI_JG_LAM && k->group == GUNI_JG_LAM && next
        && is_heh(next->group)) {
      continue;
    }
    if (i >= 2 && letters[i - 2].group == GUNI_JG_LAM && p->group == GUNI_JG_LAM
        && is_heh(k->group) && joins_forward(&letters[i - 2])) {
      continue;
    }
    k_forward = next && joins_forward(k) && joins_backward(next);
    if (cp[p->at] == TATWEEL || cp[k->at] == TATWEEL) {
      continue;              // the author's stroke, taken above
    }
    if (p->group == GUNI_JG_SEEN || p->group == GUNI_JG_SAD) {
      rank = GFNT_KASHIDA_AFTER_SEEN;
    } else if (!k_forward) {
      rank = final_class(k->group);
    } else if (k->group == GUNI_JG_BEH && next
        && (next->group == GUNI_JG_REH || is_yeh(next->group))) {
      rank = GFNT_KASHIDA_BEFORE_BEH_RA;
    } else {
      rank = GFNT_KASHIDA_MEDIAL;
    }
    if (rank <= best && (!allowed || allowed(ctx, k->at))) {
      // The later of equals: nearest the end of the word.
      best = rank;
      best_at = k->at;
    }
  }
  if (best == 99) {
    return false;
  }
  *before = best_at;
  if (priority) {
    *priority = best;
  }
  return true;
}
