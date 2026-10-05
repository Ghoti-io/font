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
 * Apple's glyph substitution: the extended glyph metamorphosis table, `morx`.
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
#define GFNT_TAG_feat GFNT_TAG('f', 'e', 'a', 't')

#define DELETED 0xFFFFu
#define CLASS_END_OF_TEXT 0u
#define CLASS_OUT_OF_BOUNDS 1u
#define CLASS_DELETED 2u
#define MAX_STACK 64u
#define MAX_WANTED 64u
#define MAX_RANGES (2 * MAX_WANTED + 1)

typedef struct Ctx {
  const GFNT_Reader * r;   ///< The `morx` table.
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
} Machine;

static void machine_open(Ctx * c, size_t base, Machine * m) {
  m->start = base;
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
  if (start < end) {
    gfnt_merge_clusters(b->info, b->len, start, end);
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

  if (d->mark_set && mark_index != 0xFFFF && d->mark < b->len) {
    uint32_t off = u32(c, d->subst_table + 4 * (size_t)mark_index);

    if (!c->bad && lookup_value(c, d->subst_table + off,
            b->info[d->mark].glyph, &glyph)) {
      b->info[d->mark].glyph = glyph;
    }
  }
  if (current_index != 0xFFFF && b->len) {
    size_t at = b->idx < b->len ? b->idx : b->len - 1;
    uint32_t off = u32(c, d->subst_table + 4 * (size_t)current_index);

    if (!c->bad && lookup_value(c, d->subst_table + off, b->info[at].glyph,
            &glyph)) {
      b->info[at].glyph = glyph;
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
  uint16_t action_index = u16(c, entry + 4);

  if (flags & 0x8000) {
    if (d->match_length == MAX_STACK) {
      memmove(d->match, d->match + 1, (MAX_STACK - 1) * sizeof d->match[0]);
      d->match_length--;
    }
    d->match[d->match_length++] = b->idx;
  }
  if (flags & 0x2000) {
    size_t cursor = d->match_length;
    size_t action_at = d->lig_action + 4 * (size_t)action_index;
    uint32_t ligature_idx = 0;
    uint32_t action;

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
        return;
      }
      offset = action & 0x3FFFFFFFu;
      if (offset & 0x20000000u) {
        offset |= 0xC0000000u;
      }
      cursor--;
      comp = b->info[d->match[cursor]].glyph + offset;
      ligature_idx += u16(c, d->lig_component + 2 * (size_t)comp);
      if (action & 0xC0000000u) {
        uint16_t lig = u16(c, d->lig_ligature + 2 * (size_t)ligature_idx);
        size_t lig_end = d->match[d->match_length - 1] + 1;
        size_t k;

        if (c->bad) {
          return;
        }
        b->info[d->match[cursor]].glyph = lig;
        // The components after it are gone, though they stay until the end.
        for (k = cursor + 1; k < d->match_length; k++) {
          b->info[d->match[k]].glyph = DELETED;
        }
        d->match_length = cursor + 1;
        merge(b, d->match[cursor], lig_end);
        ligature_idx = 0;
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
  (void)u16(d->c, d->insert_actions + 2 * (src_glyph_index + count - 1));
  if (d->c->bad) {
    return false;
  }
  for (i = 0; i < count; i++) {
    GFNT_LInfo info;
    uint16_t glyph = u16(d->c, d->insert_actions + 2 * (src_glyph_index + i));

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
        b->idx = end + count + (before ? 0 : 0);
        if (before) {
          b->idx = end + count;
        }
        else {
          b->idx = end + count;
        }
      }
    }
    else if (count) {
      // At the end of the text: appended, taking the last glyph's cluster.
      if (b->len && insert_glyphs(d, b->len, current_index, count, b->len - 1)) {
        b->idx = b->len;
      }
    }
  }
}

/** Run one state-machine subtable over the buffer. */
static void drive(Driver * d, int type, size_t body) {
  Ctx * c = d->c;
  GFNT_LBuffer * b = c->buf;
  uint32_t state = 0;
  int64_t ops = (int64_t)b->len * 64;

  if (ops < 16384) {
    ops = 16384;
  }
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
      d->subst_table = body + u32(c, body + 16);
      break;
    case LIGATURE:
      d->entry_size = 6;
      d->lig_action = body + u32(c, body + 16);
      d->lig_component = body + u32(c, body + 20);
      d->lig_ligature = body + u32(c, body + 24);
      break;
    default:
      d->entry_size = 8;
      d->insert_actions = body + u32(c, body + 16);
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
    uint32_t klass = b->idx < b->len
        ? (actionable(c, b->idx)
              ? machine_class(c, &d->m, b->info[b->idx].glyph)
              : CLASS_OUT_OF_BOUNDS)
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
    if (b->idx >= b->len) {
      break;
    }
    if (!(flags & 0x4000) || ops-- <= 0) {
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
    size_t k;
    uint16_t type;
    uint16_t setting;
    bool exclusive = false;
    bool found = false;

    tag[0] = (char)(features[i].tag >> 24);
    tag[1] = (char)(features[i].tag >> 16);
    tag[2] = (char)(features[i].tag >> 8);
    tag[3] = (char)features[i].tag;
    tag[4] = 0;
    if (!memcmp(tag, "aalt", 4)) {
      type = 17;
      setting = (uint16_t)features[i].value;
    }
    else {
      for (k = 0; k < sizeof mappings / sizeof mappings[0]; k++) {
        if (!memcmp(mappings[k].tag, tag, 4)) {
          found = true;
          break;
        }
      }
      if (!found) {
        continue;
      }
      type = mappings[k].type;
      setting = features[i].value ? mappings[k].enable : mappings[k].disable;
    }
    if (!feat_type(&feat, type, &exclusive)) {
      continue;
    }
    out[n].type = type;
    out[n].setting = setting;
    out[n].exclusive = exclusive;
    out[n].start = features[i].start;
    out[n].end = features[i].end;
    n++;
  }
  return n;
}

/** The flags of a chain for the clusters from @p at on: defaults, then the active requests. */
static uint32_t chain_flags(Ctx * c, size_t chain, uint32_t flags,
    uint32_t nfeat, const Wanted * wanted, size_t count, size_t at) {
  size_t w;
  size_t v;

  for (w = 0; w < count; w++) {
    uint32_t f;
    bool shadowed = false;

    if (at < wanted[w].start || at >= wanted[w].end) {
      continue;
    }
    // Of requests for the same setting, the first one made is the one that holds.
    for (v = 0; v < w && !shadowed; v++) {
      shadowed = at >= wanted[v].start && at < wanted[v].end
          && wanted[v].type == wanted[w].type
          && (wanted[w].exclusive
              || (wanted[v].setting & ~1u) == (wanted[w].setting & ~1u));
    }
    if (shadowed) {
      continue;
    }
    for (f = 0; f < nfeat && !c->bad; f++) {
      size_t fe = chain + 16 + 12 * (size_t)f;

      if (u16(c, fe) == wanted[w].type && u16(c, fe + 2) == wanted[w].setting) {
        flags &= u32(c, fe + 8);
        flags |= u32(c, fe + 4);
      }
    }
  }
  return flags;
}

bool gfnt_morx_present(const GFNT_Face * face) {
  return gfnt_face_has_table(face, GFNT_TAG_morx);
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

  result = gfnt_face_table_reader(face, GFNT_TAG_morx, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  (void)gfnt_face_num_glyphs(face, &glyphs, NULL);
  memset(&c, 0, sizeof c);
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
    uint32_t nfeat = u32(&c, at + 8);
    uint32_t nsub = u32(&c, at + 12);
    size_t sub_at = at + 16 + 12 * (size_t)nfeat;
    uint32_t s;
    size_t r;

    // The feature entries have to fit in the chain: a count that does not is a
    // damaged table, and is not walked.
    if (c.bad || length < 16 || nfeat > (length - 16) / 12) {
      break;
    }
    c.nranges = ncuts;
    for (r = 0; r < ncuts; r++) {
      c.bounds[r] = cuts[r];
      c.rflags[r] = chain_flags(&c, at, defaults, nfeat, wanted, wanted_count,
          cuts[r]);
    }
    for (s = 0; s < nsub && !c.bad; s++) {
      uint32_t len = u32(&c, sub_at);
      uint32_t coverage = u32(&c, sub_at + 4);
      uint32_t sub_flags = u32(&c, sub_at + 8);
      int type = (int)(coverage & 0xFF);
      bool reverse;
      bool any = false;
      Driver d;

      if (c.bad || len < 12) {
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
          drive(&d, type, sub_at + 12);
          break;
        case NONCONTEXTUAL:
          noncontextual(&c, sub_at + 12);
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
