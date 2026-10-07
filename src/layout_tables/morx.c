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
 * Apple's glyph substitution: the extended glyph metamorphosis table, `morx`,
 * and its older 16-bit form, `mort`.
 *
 * Where a font carries it and no `GSUB`, it is what turns characters into the
 * forms they are drawn in. A chain is chosen by the features the caller named
 * (through the `feat` mapping HarfBuzz uses), and then each of its subtables is
 * run over the whole run in turn: a rearrangement, a contextual substitution, a
 * ligature, a plain glyph-for-glyph table or an insertion, the first four
 * driven by a state machine that reads one glyph's class at a time.
 */

#include <string.h>
#include "layout.h"
#include "../sfnt/sfnt.h"
#include "../shape/normalize.h"
#include <ghoti.io/font/shape.h>

#define GFNT_TAG_morx GFNT_TAG('m', 'o', 'r', 'x')
#define GFNT_TAG_mort GFNT_TAG('m', 'o', 'r', 't')
#define GFNT_TAG_feat GFNT_TAG('f', 'e', 'a', 't')

#define DELETED 0xFFFFu
#define CLASS_END_OF_TEXT 0u
#define CLASS_OUT_OF_BOUNDS 1u
#define CLASS_DELETED 2u
#define MAX_STACK 64u
#define MAX_WANTED 64u
#define MAX_RANGES (2 * MAX_WANTED + 1)

typedef struct Ctx {
  const GFNT_Reader * r;   ///< The `morx` (or `mort`) table.
  bool old;                ///< `mort`: 16-bit counts and offsets, byte-indexed states.
  GFNT_LBuffer * buf;
  size_t num_glyphs;
  bool bad;
  // The chain's flags over the clusters: [bounds[i], bounds[i + 1]) has rflags[i].
  size_t nranges;
  size_t bounds[MAX_RANGES + 1];
  uint32_t rflags[MAX_RANGES];
  uint32_t sub_flags;      ///< The subtable being run.
} Ctx;

static uint16_t u16(Ctx * c, size_t at) {
  return gfnt_lr_u16(c->r, at, &c->bad);
}

static uint8_t u8(Ctx * c, size_t at) {
  uint8_t value = 0;

  if (gfnt_reader_u8_at(c->r, at, &value) != GFNT_OK) {
    c->bad = true;
    return 0;
  }
  return value;
}

static uint32_t u32(Ctx * c, size_t at) {
  return gfnt_lr_u32(c->r, at, &c->bad);
}

/** An AAT lookup table's value for a glyph; false if it has none. */
static bool lookup_value(Ctx * c, size_t lt, uint32_t glyph, uint16_t * out) {
  uint32_t value = 0;

  if (!gfnt_aat_lookup(c->r, lt, c->num_glyphs, glyph, 2, &value, &c->bad)) {
    return false;
  }
  *out = (uint16_t)value;
  return true;
}

/** Whether the glyph at @p i may be touched by the subtable: its cluster's flags allow it. */
static bool actionable(const Ctx * c, size_t i) {
  uint32_t cluster = c->buf->info[i].cluster;
  size_t r;

  if (c->nranges < 2) {
    return true;
  }
  for (r = c->nranges; r > 1 && cluster < c->bounds[r - 1]; r--) {
  }
  return (c->rflags[r - 1] & c->sub_flags) != 0;
}

/** One extended state table, the part every subtable type shares. */
typedef struct Machine {
  size_t start;         ///< The start of the state table header.
  uint32_t classes;
  size_t class_table;
  size_t state_array;
  size_t entry_table;
  uint32_t first_glyph, glyph_count;   ///< `mort`: the class array's range.
} Machine;

static void machine_open(Ctx * c, size_t base, Machine * m) {
  m->start = base;
  if (c->old) {
    m->classes = u16(c, base);
    m->class_table = base + u16(c, base + 2);
    m->state_array = base + u16(c, base + 4);
    m->entry_table = base + u16(c, base + 6);
    m->first_glyph = u16(c, m->class_table);
    m->glyph_count = u16(c, m->class_table + 2);
    return;
  }
  m->classes = u32(c, base);
  m->class_table = base + u32(c, base + 4);
  m->state_array = base + u32(c, base + 8);
  m->entry_table = base + u32(c, base + 12);
}

static uint32_t machine_class(Ctx * c, const Machine * m, uint32_t glyph) {
  uint16_t value = 0;

  if (glyph == DELETED) {
    return CLASS_DELETED;
  }
  if (c->old) {
    uint32_t at;

    if (glyph < m->first_glyph || glyph - m->first_glyph >= m->glyph_count) {
      return CLASS_OUT_OF_BOUNDS;
    }
    at = glyph - m->first_glyph;
    return u8(c, m->class_table + 4 + at);
  }
  if (!lookup_value(c, m->class_table, glyph, &value)) {
    return CLASS_OUT_OF_BOUNDS;
  }
  return value;
}

/** The offset of the entry a state and a class lead to. */
static size_t machine_entry(Ctx * c, const Machine * m, uint32_t state,
    uint32_t klass, size_t entry_size) {
  uint16_t index;

  if (klass >= m->classes) {
    klass = CLASS_OUT_OF_BOUNDS;
  }
  if (c->old) {
    index = u8(c, m->state_array + (size_t)state * m->classes + klass);
    return m->entry_table + (size_t)index * entry_size;
  }
  index = u16(c, m->state_array + 2 * ((size_t)state * m->classes + klass));
  return m->entry_table + (size_t)index * entry_size;
}


typedef enum { REARRANGEMENT, CONTEXTUAL, LIGATURE, NONCONTEXTUAL = 4,
  INSERTION } SubtableType;

typedef struct Driver {
  Ctx * c;
  Machine m;
  size_t entry_size;
  bool mark_set;
  size_t mark;
  size_t first, last;       ///< Rearrangement's two marks.
  size_t match[MAX_STACK];  ///< Ligature's stack of component positions.
  size_t match_length;
  size_t subst_table;       ///< Contextual: the array of lookup offsets.
  size_t lig_action, lig_component, lig_ligature;
  size_t insert_actions;
} Driver;

static void merge(GFNT_LBuffer * b, size_t start, size_t end) {
  if (end > b->len) {
    end = b->len;
  }
  if (end - start >= 2 && start < end) {
    GFNT_LInfo * info = b->info;
    uint32_t cluster = info[start].cluster;
    size_t i;

    for (i = start + 1; i < end; i++) {
      if (info[i].cluster < cluster) {
        cluster = info[i].cluster;
      }
    }
    while (end < b->len && info[end - 1].cluster == info[end].cluster) {
      end++;
    }
    // The range is only extended back past the machine's own position, which it
    // is never before: a machine's glyphs before @c idx are not a cluster's tail.
    while (b->idx < start && start > 0
        && info[start - 1].cluster == info[start].cluster) {
      start--;
    }
    for (i = start; i < end; i++) {
      info[i].cluster = cluster;
    }
  }
}

static void rearrange(Driver * d, uint16_t flags) {
  static const unsigned char map[16] = {0x00, 0x10, 0x01, 0x11, 0x20, 0x30,
    0x02, 0x03, 0x12, 0x13, 0x21, 0x31, 0x22, 0x32, 0x23, 0x33};
  GFNT_LBuffer * b = d->c->buf;

  if (flags & 0x8000) {
    d->first = b->idx;
  }
  if (flags & 0x2000) {
    d->last = b->idx + 1 < b->len ? b->idx + 1 : b->len;
  }
  if ((flags & 0xF) && d->first < d->last) {
    unsigned m = map[flags & 0xF];
    size_t l = (m >> 4) < 2 ? (m >> 4) : 2;
    size_t r = (m & 0xF) < 2 ? (m & 0xF) : 2;
    bool reverse_l = (m >> 4) == 3;
    bool reverse_r = (m & 0xF) == 3;
    size_t start = d->first;
    size_t end = d->last;

    if (end - start >= l + r && end - start <= MAX_STACK) {
      GFNT_LInfo * info = b->info;
      GFNT_LInfo buf[4];

      merge(b, start, b->idx + 1);
      merge(b, start, end);
      memcpy(buf, info + start, l * sizeof buf[0]);
      memcpy(buf + 2, info + end - r, r * sizeof buf[0]);
      if (l != r) {
        memmove(info + start + r, info + start + l,
            (end - start - l - r) * sizeof buf[0]);
      }
      memcpy(info + start, buf + 2, r * sizeof buf[0]);
      memcpy(info + end - l, buf, l * sizeof buf[0]);
      if (reverse_l) {
        buf[0] = info[end - 1];
        info[end - 1] = info[end - 2];
        info[end - 2] = buf[0];
      }
      if (reverse_r) {
        buf[0] = info[start];
        info[start] = info[start + 1];
        info[start + 1] = buf[0];
      }
    }
  }
}

static void contextual(Driver * d, size_t entry) {
  Ctx * c = d->c;
  GFNT_LBuffer * b = c->buf;
  uint16_t flags = u16(c, entry + 2);
  uint16_t mark_index = u16(c, entry + 4);
  uint16_t current_index = u16(c, entry + 6);
  uint16_t glyph;

  // At the end of the text neither substitution is made unless a mark was set.
  if (b->idx >= b->len && !d->mark_set) {
    return;
  }
  // A machine substitutes at the mark whether or not it was set, which leaves the
  // mark at the first glyph.
  if (mark_index != 0xFFFF && d->mark < b->len) {
    if (c->old) {
      // The offset counts 16-bit words from the start of the state table, and
      // the glyph is an index from there.
      glyph = u16(c, d->m.start + 2 * ((size_t)mark_index
          + b->info[d->mark].glyph));
      if (!c->bad && glyph) {
        b->info[d->mark].glyph = glyph;
      }
    }
    else {
      uint32_t off = u32(c, d->subst_table + 4 * (size_t)mark_index);

      if (!c->bad && lookup_value(c, d->subst_table + off,
              b->info[d->mark].glyph, &glyph)) {
        b->info[d->mark].glyph = glyph;
      }
    }
  }
  if (current_index != 0xFFFF && b->len) {
    size_t at = b->idx < b->len ? b->idx : b->len - 1;

    if (c->old) {
      glyph = u16(c, d->m.start + 2 * ((size_t)current_index
          + b->info[at].glyph));
      if (!c->bad && glyph) {
        b->info[at].glyph = glyph;
      }
    }
    else {
      uint32_t off = u32(c, d->subst_table + 4 * (size_t)current_index);

      if (!c->bad && lookup_value(c, d->subst_table + off, b->info[at].glyph,
              &glyph)) {
        b->info[at].glyph = glyph;
      }
    }
  }
  if (flags & 0x8000) {
    d->mark_set = true;
    d->mark = b->idx;
  }
}

static void ligature(Driver * d, size_t entry) {
  Ctx * c = d->c;
  GFNT_LBuffer * b = c->buf;
  uint16_t flags = u16(c, entry + 2);
  uint16_t action_index = c->old ? 0 : u16(c, entry + 4);

  if (flags & 0x8000) {
    if (d->match_length == MAX_STACK) {
      memmove(d->match, d->match + 1, (MAX_STACK - 1) * sizeof d->match[0]);
      d->match_length--;
    }
    if (!d->match_length || d->match[d->match_length - 1] != b->idx) {
      d->match[d->match_length++] = b->idx;
    }
  }
  if (c->old ? (flags & 0x3FFF) != 0 : (flags & 0x2000) != 0) {
    size_t cursor = d->match_length;
    size_t action_at = d->lig_action + 4 * (size_t)action_index;
    uint32_t ligature_idx = 0;
    uint32_t action;

    if (c->old) {
      // A byte offset from the start of the state table, to a whole action.
      size_t rel = d->lig_action - d->m.start;

      if ((flags & 0x3FFFu) < rel) {
        c->bad = true;
        return;
      }
      action_at = d->lig_action + 4 * (((flags & 0x3FFFu) - rel) / 4);
    }

    if (!d->match_length || b->idx >= b->len) {
      return;
    }
    do {
      uint32_t offset;
      uint32_t comp;

      if (!cursor) {
        d->match_length = 0;
        break;
      }
      action = u32(c, action_at);
      if (c->bad) {
        // An action past the table ends the actions, not the chain.
        c->bad = false;
        break;
      }
      offset = action & 0x3FFFFFFFu;
      if (offset & 0x20000000u) {
        offset |= 0xC0000000u;
      }
      cursor--;
      comp = b->info[d->match[cursor]].glyph + offset;
      ligature_idx += c->old ? u16(c, d->m.start + 2 * (size_t)comp)
          : u16(c, d->lig_component + 2 * (size_t)comp);
      if (c->bad) {
        c->bad = false;
        break;
      }
      if (action & 0xC0000000u) {
        size_t rel = d->lig_ligature - d->m.start;
        uint16_t lig;

        if (c->old) {
          if (ligature_idx < rel) {
            c->bad = true;
            return;
          }
          ligature_idx = (uint32_t)((ligature_idx - rel) / 2);
        }
        lig = u16(c, d->lig_ligature + 2 * (size_t)ligature_idx);
        size_t lig_end = d->match[d->match_length - 1] + 1;
        size_t k;

        if (c->bad) {
          c->bad = false;
          break;
        }
        b->info[d->match[cursor]].glyph = lig;
        // The components after it are gone, though they stay until the end.
        for (k = cursor + 1; k < d->match_length; k++) {
          b->info[d->match[k]].glyph = DELETED;
        }
        d->match_length = cursor + 1;
        // A ligature's merge, unlike a rearrangement's, extends back over equal
        // clusters.
        gfnt_merge_clusters(b->info, b->len, d->match[cursor],
            lig_end < b->len ? lig_end : b->len);
        // The sum is not cleared: the next store adds to it (in `mort`, it is
        // already an index by then).
      }
      action_at += 4;
    } while (!(action & 0x80000000u));
  }
}

static bool insert_glyphs(Driver * d, size_t at, size_t src_glyph_index,
    size_t count, size_t cluster_from) {
  GFNT_LBuffer * b = d->c->buf;
  size_t i;

  // All of the glyphs or none: the array must lie in the table.
  // In `mort` the list is at twice the offset, and HarfBuzz starts one byte in.
  size_t base = d->c->old ? d->insert_actions + 2 * src_glyph_index + 1
      : d->insert_actions + 2 * src_glyph_index;

  // A run that has spent its step budget takes no more glyphs, as in HarfBuzz.
  b->max_ops -= (int64_t)count;
  if (b->max_ops <= 0) {
    return false;
  }
  (void)u16(d->c, base + 2 * (count - 1));
  if (d->c->bad) {
    return false;
  }
  for (i = 0; i < count; i++) {
    GFNT_LInfo info;
    uint16_t glyph = u16(d->c, base + 2 * i);

    if (d->c->bad) {
      return false;
    }
    info = b->info[cluster_from];
    info.glyph = glyph;
    info.props = 0;
    info.lig_props = 0;
    if (!gfnt_lbuf_insert(b, at + i, &info)) {
      b->oom = true;
      return false;
    }
    if (cluster_from >= at + i) {
      cluster_from++;
    }
  }
  return true;
}

static void insertion(Driver * d, size_t entry) {
  Ctx * c = d->c;
  GFNT_LBuffer * b = c->buf;
  uint16_t flags = u16(c, entry + 2);
  uint16_t current_index = u16(c, entry + 4);
  uint16_t marked_index = u16(c, entry + 6);
  // Where HarfBuzz's output stands when the step starts.
  size_t mark_loc = b->idx;

  if (marked_index != 0xFFFF && d->mark < b->len) {
    size_t count = flags & 0x1F;
    bool before = (flags & 0x0400) != 0;
    size_t at = before ? d->mark : d->mark + 1;

    if (count && insert_glyphs(d, at, marked_index, count, d->mark)) {
      // HarfBuzz moves to the old position plus the count whichever side of
      // the current glyph they went in on.
      b->idx += count;
    }
  }
  if (flags & 0x8000) {
    d->mark_set = true;
    d->mark = mark_loc;
  }
  if (current_index != 0xFFFF) {
    size_t count = (flags >> 5) & 0x1F;
    bool before = (flags & 0x0800) != 0;

    if (count && b->idx < b->len) {
      size_t at = before ? b->idx : b->idx + 1;
      size_t end = b->idx;

      if (insert_glyphs(d, at, current_index, count, b->idx)) {
        // A step that advances goes past the new glyphs; one that does not
        // stands where it was, on the first of them if they went in before.
        b->idx = (flags & 0x4000) ? end : end + count;
      }
    }
    else if (count) {
      // At the end of the text: appended, taking the last glyph's cluster.
      size_t old_len = b->len;

      // A step that does not advance stays on the first glyph appended, so the
      // machine runs again over it; one that advances has seen the end.
      if (b->len && insert_glyphs(d, b->len, current_index, count, b->len - 1)) {
        b->idx = (flags & 0x4000) ? old_len : b->len;
      }
    }
  }
}

/** Run one state-machine subtable over the buffer. */
static void drive(Driver * d, int type, size_t body) {
  Ctx * c = d->c;
  GFNT_LBuffer * b = c->buf;
  uint32_t state = 0;
  bool last_ok = true;
  machine_open(c, body, &d->m);
  if (c->bad) {
    return;
  }
  switch (type) {
    case REARRANGEMENT:
      d->entry_size = 4;
      break;
    case CONTEXTUAL:
      d->entry_size = 8;
      d->subst_table = c->old ? 0 : body + u32(c, body + 16);
      break;
    case LIGATURE:
      d->entry_size = c->old ? 4 : 6;
      if (c->old) {
        d->lig_action = body + u16(c, body + 8);
        d->lig_component = body + u16(c, body + 10);
        d->lig_ligature = body + u16(c, body + 12);
      }
      else {
        d->lig_action = body + u32(c, body + 16);
        d->lig_component = body + u32(c, body + 20);
        d->lig_ligature = body + u32(c, body + 24);
      }
      break;
    default:
      d->entry_size = 8;
      d->insert_actions = c->old ? body : body + u32(c, body + 16);
      break;
  }
  if (c->bad) {
    return;
  }
  d->mark_set = false;
  d->mark = 0;
  d->first = d->last = 0;
  d->match_length = 0;
  b->idx = 0;
  for (;;) {
    uint32_t klass;

    // A glyph the subtable is not enabled for is passed over, and the machine starts
    // again from the start of text after it. The end of the text is a step of its own
    // only where the range of the last glyph met enables the subtable (the glyph itself
    // may have moved by then).
    if (b->idx < b->len) {
      last_ok = actionable(c, b->idx);
      if (!last_ok) {
        state = 0;
        b->idx++;
        continue;
      }
    }
    else if (b->len && !last_ok) {
      break;
    }
    klass = b->idx < b->len
        ? machine_class(c, &d->m, b->info[b->idx].glyph)
        : CLASS_END_OF_TEXT;
    size_t entry = machine_entry(c, &d->m, state, klass, d->entry_size);
    uint16_t new_state = u16(c, entry);
    uint16_t flags = u16(c, entry + 2);

    if (c->bad || b->oom) {
      return;
    }
    switch (type) {
      case REARRANGEMENT:
        rearrange(d, flags);
        break;
      case CONTEXTUAL:
        contextual(d, entry);
        break;
      case LIGATURE:
        ligature(d, entry);
        break;
      default:
        insertion(d, entry);
        break;
    }
    if (c->bad || b->oom) {
      return;
    }
    state = new_state;
    if (c->old) {
      // A `mort` state is the byte offset of its row in the state array.
      state = (uint32_t)(((int32_t)new_state - (int32_t)(d->m.state_array
          - d->m.start)) / (int32_t)(d->m.classes ? d->m.classes : 1));
    }
    if (b->idx >= b->len) {
      break;
    }
    if (!(flags & 0x4000) || b->max_ops-- <= 0) {
      b->idx++;
    }
  }
}

static void noncontextual(Ctx * c, size_t body) {
  GFNT_LBuffer * b = c->buf;
  size_t i;

  for (i = 0; i < b->len; i++) {
    uint16_t glyph;

    if (actionable(c, i) && lookup_value(c, body, b->info[i].glyph, &glyph)) {
      b->info[i].glyph = glyph;
    }
    if (c->bad) {
      return;
    }
  }
}

/* --- the feature mapping --------------------------------------------------- */

// A disable of 0xFFFF names no setting: the feature has nothing to turn off there.
typedef struct Mapping {
  const char tag[5];
  uint16_t type, enable, disable;
} Mapping;

// HarfBuzz's table, `hb_aat_feature_mapping_t`: an OpenType feature, the AAT
// type and the selectors that turn it on and off.
static const Mapping mappings[] = {
  {"afrc", 11, 1, 0}, {"c2pc", 38, 2, 0}, {"c2sc", 38, 1, 0},
  {"calt", 36, 0, 1}, {"case", 33, 0, 1}, {"clig", 1, 18, 19},
  {"cpsp", 33, 2, 3}, {"cswh", 36, 4, 5}, {"dlig", 1, 4, 5},
  {"frac", 11, 2, 0}, {"fwid", 22, 1, 7}, {"halt", 22, 6, 7},
  {"hkna", 34, 0, 1}, {"hlig", 1, 20, 21},
  {"hngl", 23, 1, 0}, {"hwid", 22, 2, 7}, {"ital", 32, 2, 3},
  {"liga", 1, 2, 3}, {"lnum", 21, 1, 2}, {"mgrk", 15, 10, 11},
  {"nlck", 20, 13, 16}, {"onum", 21, 0, 2}, {"ordn", 10, 3, 0},
  {"palt", 22, 5, 7}, {"pcap", 37, 2, 0}, 
  {"pnum", 6, 1, 4}, {"pwid", 22, 0, 7}, {"qwid", 22, 4, 7},
  {"rlig", 1, 0, 1}, {"ruby", 28, 2, 3}, {"sinf", 10, 4, 0},
  {"smcp", 37, 1, 0}, {"smpl", 20, 1, 16}, {"ss01", 35, 2, 3},
  {"ss02", 35, 4, 5}, {"ss03", 35, 6, 7}, {"ss04", 35, 8, 9},
  {"ss05", 35, 10, 11}, {"ss06", 35, 12, 13}, {"ss07", 35, 14, 15},
  {"ss08", 35, 16, 17}, {"ss09", 35, 18, 19}, {"ss10", 35, 20, 21},
  {"ss11", 35, 22, 23}, {"ss12", 35, 24, 25}, {"ss13", 35, 26, 27},
  {"ss14", 35, 28, 29}, {"ss15", 35, 30, 31}, {"ss16", 35, 32, 33},
  {"ss17", 35, 34, 35}, {"ss18", 35, 36, 37}, {"ss19", 35, 38, 39},
  {"ss20", 35, 40, 41}, {"subs", 10, 2, 0}, {"sups", 10, 1, 0},
  {"swsh", 36, 2, 3}, {"titl", 19, 4, 0}, {"tnam", 20, 14, 16},
  {"tnum", 6, 0, 4}, {"trad", 20, 0, 16}, {"twid", 22, 3, 7},
  {"unic", 3, 14, 15},  {"vert", 4, 0, 1},
  {"expt", 20, 10, 16}, {"hist", 40, 0, 1}, {"hojo", 20, 12, 16},
  {"jp04", 20, 11, 16}, {"jp78", 20, 2, 16}, {"jp83", 20, 3, 16},
  {"jp90", 20, 4, 16}, {"pkna", 22, 0, 7}, {"valt", 22, 5, 7},
  {"vrtr", 4, 2, 3}, {"smcp", 3, 3, 0xFFFF},
  {"vhal", 22, 6, 7}, {"vkna", 34, 2, 3}, {"vpal", 22, 5, 7},
  {"vrt2", 4, 0, 1}, {"zero", 14, 4, 5},
};

typedef struct Wanted {
  uint16_t type, setting;
  bool exclusive;
  size_t start, end;   ///< The clusters the request covers, [start, end).
} Wanted;

/** Whether the `feat` table lists the feature type, and if it is exclusive. */
static bool feat_type(const GFNT_Reader * feat, uint16_t type,
    bool * exclusive) {
  int64_t lo = 0;
  int64_t hi;
  bool bad = false;

  // The entries are sorted by type and HarfBuzz searches them as such: a table
  // that is not sorted loses the types the search steps over.
  hi = (int64_t)gfnt_lr_u16(feat, 4, &bad) - 1;
  while (lo <= hi && !bad) {
    int64_t mid = (lo + hi) / 2;
    size_t at = 12 + 12 * (size_t)mid;
    uint16_t found = gfnt_lr_u16(feat, at, &bad);

    if (bad) {
      break;
    }
    if (type < found) {
      hi = mid - 1;
    }
    else if (type > found) {
      lo = mid + 1;
    }
    else {
      *exclusive = (gfnt_lr_u16(feat, at + 8, &bad) & 0x8000) != 0;
      return !bad;
    }
  }
  return false;
}

/** The features the caller named that the `feat` table can act on, in order. */
static size_t collect_wanted(const GFNT_Face * face,
    const GFNT_ShapeFeature * features, size_t count, Wanted * out) {
  GFNT_Reader feat;
  size_t n = 0;
  size_t i;

  if (!gfnt_face_has_table(face, GFNT_TAG_feat)
      || gfnt_face_table_reader(face, GFNT_TAG_feat, &feat, NULL) != GFNT_OK) {
    return 0;
  }
  for (i = 0; i < count && n < MAX_WANTED; i++) {
    char tag[5];
    size_t k = 0;
    bool aalt;

    tag[0] = (char)(features[i].tag >> 24);
    tag[1] = (char)(features[i].tag >> 16);
    tag[2] = (char)(features[i].tag >> 8);
    tag[3] = (char)features[i].tag;
    tag[4] = 0;
    aalt = !memcmp(tag, "aalt", 4);
    // A feature may be several AAT settings, one entry of the table for each.
    while (n < MAX_WANTED) {
      uint16_t type;
      uint16_t setting;
      bool exclusive = false;

      if (aalt) {
        type = 17;
        setting = (uint16_t)features[i].value;
      }
      else {
        while (k < sizeof mappings / sizeof mappings[0]
            && memcmp(mappings[k].tag, tag, 4)) {
          k++;
        }
        if (k == sizeof mappings / sizeof mappings[0]) {
          break;
        }
        type = mappings[k].type;
        setting = features[i].value ? mappings[k].enable : mappings[k].disable;
        k++;
      }
      if (feat_type(&feat, type, &exclusive)) {
        out[n].type = type;
        out[n].setting = setting;
        out[n].exclusive = exclusive;
        out[n].start = features[i].start;
        out[n].end = features[i].end;
        n++;
      }
      if (aalt) {
        break;
      }
    }
  }
  // HarfBuzz takes them by type, the order they were asked for kept within a type;
  // the flags of a chain are the same only if the requests are met in that order.
  for (i = 1; i < n; i++) {
    Wanted w = out[i];
    size_t j = i;

    while (j > 0 && out[j - 1].type > w.type) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = w;
  }
  return n;
}

/**
 * Which requests are in force at cluster @p at. HarfBuzz sweeps the starts and ends
 * of the requests in order, adding a request at its start; at an end it takes out
 * the first request of the same type and setting, which need not be the one that is
 * ending, so a global request can be lost when a ranged one for the same setting
 * stops. At one cluster ends come before starts, each in order of request.
 */
static void wanted_active(const Wanted * wanted, size_t count, size_t at,
    bool * active) {
  size_t w;
  size_t order[MAX_WANTED];
  size_t n = 0;
  size_t i;

  for (w = 0; w < count; w++) {
    active[w] = false;
  }
  // The pushes, in the order the sweep meets them.
  for (w = 0; w < count; w++) {
    if (wanted[w].start < wanted[w].end && wanted[w].start <= at) {
      order[n++] = w;
    }
  }
  // Starts come in order of cluster, then of request; the stops of those that have
  // stopped by now come in the same order, each taking out the first of its kind.
  for (i = 1; i < n; i++) {
    size_t v = order[i];
    size_t k = i;

    while (k && (wanted[order[k - 1]].start > wanted[v].start
        || (wanted[order[k - 1]].start == wanted[v].start && order[k - 1] > v))) {
      order[k] = order[k - 1];
      k--;
    }
    order[k] = v;
  }
  {
    size_t stops[MAX_WANTED];
    size_t nstops = 0;
    bool present[MAX_WANTED];
    size_t j;

    for (i = 0; i < n; i++) {
      present[i] = true;
      if (wanted[order[i]].end <= at) {
        stops[nstops++] = order[i];
      }
    }
    for (i = 1; i < nstops; i++) {
      size_t v = stops[i];
      size_t k = i;

      while (k && (wanted[stops[k - 1]].end > wanted[v].end
          || (wanted[stops[k - 1]].end == wanted[v].end && stops[k - 1] > v))) {
        stops[k] = stops[k - 1];
        k--;
      }
      stops[k] = v;
    }
    // The events interleave by cluster; a stop happens before the starts that
    // follow it, so the removals are done as the pushes pass its cluster.
    {
      size_t next_stop = 0;

      for (i = 0; i < n; i++) {
        while (next_stop < nstops
            && wanted[stops[next_stop]].end <= wanted[order[i]].start) {
          size_t f = stops[next_stop++];

          for (j = 0; j < i; j++) {
            if (present[j] && wanted[order[j]].type == wanted[f].type
                && wanted[order[j]].setting == wanted[f].setting) {
              present[j] = false;
              break;
            }
          }
        }
      }
      while (next_stop < nstops) {
        size_t f = stops[next_stop++];

        for (j = 0; j < n; j++) {
          if (present[j] && wanted[order[j]].type == wanted[f].type
              && wanted[order[j]].setting == wanted[f].setting) {
            present[j] = false;
            break;
          }
        }
      }
    }
    for (i = 0; i < n; i++) {
      active[order[i]] = present[i];
    }
  }
}

/** The flags of a chain for the clusters from @p at on: defaults, then the active requests. */
static uint32_t chain_flags(Ctx * c, size_t chain, uint32_t flags,
    uint32_t nfeat, const Wanted * wanted, size_t count, size_t at) {
  uint32_t f;
  bool active[MAX_WANTED];

  wanted_active(wanted, count, at, active);
  // The chain's own entries are taken in their order, and each that a request names
  // is applied once, as HarfBuzz does.
  for (f = 0; f < nfeat && !c->bad; f++) {
    size_t fe = chain + (c->old ? 12 : 16) + 12 * (size_t)f;
    uint16_t type = u16(c, fe);
    uint16_t setting = u16(c, fe + 2);
    size_t w;
    size_t v;
    bool named = false;

    for (w = 0; w < count && !named; w++) {
      bool shadowed = false;

      if (!active[w] || wanted[w].type != type || wanted[w].setting != setting) {
        continue;
      }
      // Of requests for the same setting, the first one made is the one that holds.
      for (v = 0; v < w && !shadowed; v++) {
        shadowed = active[v]
            && wanted[v].type == wanted[w].type
            && (wanted[w].exclusive
                || (wanted[v].setting & ~1u) == (wanted[w].setting & ~1u));
      }
      named = !shadowed;
    }
    if (named) {
      flags &= u32(c, fe + 8);
      flags |= u32(c, fe + 4);
    }
  }
  return flags;
}

bool gfnt_morx_present(const GFNT_Face * face) {
  // HarfBuzz reads `mort` only where the font has no `GSUB`: Konatu.ttf carries
  // both, and its `mort` is not used.
  return gfnt_face_has_table(face, GFNT_TAG_morx)
      || (gfnt_face_has_table(face, GFNT_TAG_mort)
          && !gfnt_face_has_table(face, GFNT_TAG('G', 'S', 'U', 'B')));
}

GFNT_Result gfnt_morx_apply(const GFNT_Face * face, GFNT_LBuffer * buf,
    bool backward, bool vertical, const GFNT_ShapeFeature * features,
    size_t feature_count, GFNT_Error * error) {
  GFNT_Reader table;
  Ctx c;
  Wanted wanted[MAX_WANTED];
  size_t wanted_count;
  size_t chains;
  size_t at;
  size_t ch;
  size_t glyphs = 0;
  size_t cuts[MAX_RANGES];
  size_t ncuts = 0;
  GFNT_Result result;

  bool old = !gfnt_face_has_table(face, GFNT_TAG_morx);

  result = gfnt_face_table_reader(face, old ? GFNT_TAG_mort : GFNT_TAG_morx,
      &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  (void)gfnt_face_num_glyphs(face, &glyphs, NULL);
  memset(&c, 0, sizeof c);
  c.old = old;
  c.r = &table;
  c.buf = buf;
  c.num_glyphs = glyphs;
  wanted_count = collect_wanted(face, features, feature_count, wanted);
  // The clusters where a request starts or stops cut the run into ranges.
  {
    size_t w;
    size_t i;

    cuts[ncuts++] = 0;
    for (w = 0; w < wanted_count; w++) {
      size_t edge[2] = {wanted[w].start, wanted[w].end};

      for (i = 0; i < 2; i++) {
        size_t k;
        bool have = false;

        if (edge[i] == 0 || edge[i] == GFNT_SHAPE_END) {
          continue;
        }
        for (k = 0; k < ncuts && !have; k++) {
          have = cuts[k] == edge[i];
        }
        if (!have && ncuts < MAX_RANGES) {
          cuts[ncuts++] = edge[i];
        }
      }
    }
    for (i = 1; i < ncuts; i++) {
      size_t v = cuts[i];
      size_t k = i;

      while (k && cuts[k - 1] > v) {
        cuts[k] = cuts[k - 1];
        k--;
      }
      cuts[k] = v;
    }
  }
  chains = u32(&c, 4);
  at = 8;
  for (ch = 0; ch < chains && !c.bad; ch++) {
    uint32_t defaults = u32(&c, at);
    uint32_t length = u32(&c, at + 4);
    uint32_t nfeat = old ? u16(&c, at + 8) : u32(&c, at + 8);
    uint32_t nsub = old ? u16(&c, at + 10) : u32(&c, at + 12);
    size_t head = old ? 12 : 16;
    size_t sub_at = at + head + 12 * (size_t)nfeat;
    uint32_t s;
    size_t r;

    // The feature entries have to fit in the chain: a count that does not is a
    // damaged table, and is not walked.
    if (c.bad || length < head || nfeat > (length - head) / 12) {
      break;
    }
    c.nranges = ncuts;
    for (r = 0; r < ncuts; r++) {
      c.bounds[r] = cuts[r];
      c.rflags[r] = chain_flags(&c, at, defaults, nfeat, wanted, wanted_count,
          cuts[r]);
    }
    for (s = 0; s < nsub && !c.bad; s++) {
      uint32_t len = old ? u16(&c, sub_at) : u32(&c, sub_at);
      uint32_t coverage;
      uint32_t sub_flags = u32(&c, sub_at + (old ? 4 : 8));
      size_t shead = old ? 8 : 12;
      int type;
      bool reverse;
      bool any = false;
      Driver d;

      if (old) {
        // The 16-bit coverage word keeps the morx flags in its top bits.
        uint32_t word = u16(&c, sub_at + 2);

        coverage = ((word & 0xF000u) << 16) | (word & 0xFFu);
      }
      else {
        coverage = u32(&c, sub_at + 4);
      }
      type = (int)(coverage & 0xFF);
      if (c.bad || len < shead) {
        break;
      }
      for (r = 0; r < ncuts; r++) {
        any = any || (c.rflags[r] & sub_flags) != 0;
      }
      if (!any) {
        goto skip;
      }
      if (!(coverage & 0x20000000u)
          && vertical != ((coverage & 0x80000000u) != 0)) {
        goto skip;
      }
      c.sub_flags = sub_flags;
      if (coverage & 0x10000000u) {
        reverse = (coverage & 0x40000000u) != 0;
      }
      else {
        reverse = ((coverage & 0x40000000u) != 0) != backward;
      }
      if (reverse) {
        gfnt_lbuf_reverse(buf);
      }
      memset(&d, 0, sizeof d);
      d.c = &c;
      switch (type) {
        case REARRANGEMENT:
        case CONTEXTUAL:
        case LIGATURE:
        case INSERTION:
          drive(&d, type, sub_at + shead);
          break;
        case NONCONTEXTUAL:
          noncontextual(&c, sub_at + shead);
          break;
        default:
          break;
      }
      if (reverse) {
        gfnt_lbuf_reverse(buf);
      }
      if (buf->limit) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_morx, 0,
            GFNT_GLYPH_NONE,
            "a morx subtable grew the run past sixty-four times its length");
      }
      if (buf->oom) {
        return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_morx, 0,
            GFNT_GLYPH_NONE, "no memory in a morx subtable");
      }
skip:
      sub_at += len;
    }
    at += length;
  }
  // A damaged table stops the machinery where it is; what has been done stands.
  return GFNT_OK;
}

void gfnt_morx_remove_deleted(GFNT_LBuffer * b) {
  size_t kept = 0;
  size_t i;
  size_t count = b->len;

  for (i = 0; i < count; i++) {
    GFNT_LInfo * info = &b->info[i];

    if (info->glyph == DELETED) {
      uint32_t cluster = info->cluster;

      if (i + 1 < count && cluster == b->info[i + 1].cluster) {
        continue;
      }
      if (kept) {
        if (cluster < b->info[kept - 1].cluster) {
          uint32_t old = b->info[kept - 1].cluster;
          size_t k;

          for (k = kept; k && b->info[k - 1].cluster == old; k--) {
            b->info[k - 1].cluster = cluster;
          }
        }
        continue;
      }
      if (i + 1 < count) {
        gfnt_merge_clusters(b->info, count, i, i + 2);
      }
      continue;
    }
    if (kept != i) {
      b->info[kept] = b->info[i];
    }
    kept++;
  }
  b->len = kept;
  b->out_info = b->info;
}
