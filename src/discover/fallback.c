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
 * A face for a character, from a scanned set: the part of design.md section 11 that answers
 * font fallback.
 *
 * The set is grouped by family once, up front (a face is in the group of its typographic family
 * and in that of its legacy one). A request walks the groups in a fixed order and, in each,
 * lets CSS matching choose among the faces that can map the character, opening a face only to
 * check that its `cmap` really does. A face stays open until the fallback is freed, because a
 * paragraph holds the pointers it was given.
 */

#include "match_int.h"
#include "os2_ranges.h"

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/unicode/case.h>

#include <string.h>

typedef struct Group {
  const char * name;
  size_t * members;
  size_t count;
  size_t cap;
} Group;

typedef struct Entry {
  GFNT_Blob * blob;
  GFNT_Face * face;
  bool tried;           ///< An open was attempted, and the face is open or never will be.
} Entry;

typedef struct Preference {
  GFNT_Tag script;
  GFNT_Tag language;
  size_t * groups;
  size_t count;
} Preference;

struct GFNT_FontFallback {
  const GFNT_FontSet * set;
  const GFNT_Allocator * a;
  Group * groups;
  size_t group_count;
  Entry * entries;
  Preference * prefs;
  size_t pref_count;
};

static bool same_name(const char * l, const char * r) {
  return l && r && l[0] && guni_case_folded_equal_utf8(l, strlen(l), r, strlen(r), false);
}

static size_t find_group(const GFNT_FontFallback * f, const char * name) {
  size_t i;

  for (i = 0; i < f->group_count; i++) {
    if (same_name(f->groups[i].name, name)) {
      return i;
    }
  }
  return (size_t)-1;
}

static bool add_member(GFNT_FontFallback * f, Group * g, size_t record) {
  if (g->count == g->cap) {
    size_t cap = g->cap ? g->cap * 2 : 4;
    size_t * grown = f->a->realloc_fn(f->a->ctx, g->members, cap * sizeof *grown);

    if (!grown) {
      return false;
    }
    g->members = grown;
    g->cap = cap;
  }
  g->members[g->count++] = record;
  return true;
}

/** Put record @p r in the group of @p name, making the group if the name is new. */
static bool join(GFNT_FontFallback * f, const char * name, size_t r) {
  size_t g = find_group(f, name);

  if (g == (size_t)-1) {
    Group * grown;

    grown = f->a->realloc_fn(f->a->ctx, f->groups, (f->group_count + 1) * sizeof *grown);
    if (!grown) {
      return false;
    }
    f->groups = grown;
    g = f->group_count++;
    memset(&f->groups[g], 0, sizeof f->groups[g]);
    f->groups[g].name = name;
  }
  return add_member(f, &f->groups[g], r);
}

void gfnt_fontfallback_free(GFNT_FontFallback * f) {
  size_t i;

  if (!f) {
    return;
  }
  for (i = 0; i < f->group_count; i++) {
    f->a->free_fn(f->a->ctx, f->groups[i].members);
  }
  f->a->free_fn(f->a->ctx, f->groups);
  if (f->entries) {
    for (i = 0; i < gfnt_fontset_count(f->set); i++) {
      gfnt_face_free(f->entries[i].face);
      gfnt_blob_destroy(f->entries[i].blob);
    }
  }
  f->a->free_fn(f->a->ctx, f->entries);
  for (i = 0; i < f->pref_count; i++) {
    f->a->free_fn(f->a->ctx, f->prefs[i].groups);
  }
  f->a->free_fn(f->a->ctx, f->prefs);
  f->a->free_fn(f->a->ctx, f);
}

GFNT_Result gfnt_fontfallback_create(const GFNT_FontSet * set,
    const GFNT_FontFallbackOptions * options, const GFNT_Allocator * allocator,
    GFNT_FontFallback ** out, GFNT_Error * error) {
  GFNT_FontFallback * f;
  size_t total;
  size_t i;
  size_t j;

  if (!set || !out || (options && options->preference_count && !options->preferences)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no set or nowhere to put the fallback");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  total = gfnt_fontset_count(set);
  f = allocator->calloc_fn(allocator->ctx, 1, sizeof *f);
  if (!f) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "allocating the fallback");
  }
  f->set = set;
  f->a = allocator;
  f->entries = allocator->calloc_fn(allocator->ctx, total ? total : 1, sizeof *f->entries);
  if (!f->entries) {
    gfnt_fontfallback_free(f);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "allocating the fallback");
  }
  for (i = 0; i < total; i++) {
    const GFNT_FontRecord * r = gfnt_fontset_at(set, i);

    if ((r->family[0] && !join(f, r->family, i))
        || (r->legacy_family && !join(f, r->legacy_family, i))) {
      gfnt_fontfallback_free(f);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "grouping the families");
    }
  }
  if (options && options->preference_count) {
    f->prefs = allocator->calloc_fn(allocator->ctx, options->preference_count, sizeof *f->prefs);
    if (!f->prefs) {
      gfnt_fontfallback_free(f);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "copying the preferences");
    }
    for (i = 0; i < options->preference_count; i++) {
      const GFNT_FontScriptPreference * p = &options->preferences[i];
      size_t kept = 0;

      f->pref_count = i + 1;
      f->prefs[i].script = p->script;
      f->prefs[i].language = p->language;
      f->prefs[i].groups = allocator->calloc_fn(allocator->ctx,
          p->family_count ? p->family_count : 1, sizeof(size_t));
      if (!f->prefs[i].groups) {
        gfnt_fontfallback_free(f);
        return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "copying the preferences");
      }
      for (j = 0; j < p->family_count; j++) {
        size_t g = p->families[j] ? find_group(f, p->families[j]) : (size_t)-1;

        if (g != (size_t)-1) {
          f->prefs[i].groups[kept++] = g;
        }
      }
      f->prefs[i].count = kept;
    }
  }
  *out = f;
  return GFNT_OK;
}

/** Whether a face's `OS/2` range bits claim @p cp; a face that sets none claims everything. */
static bool claims(const GFNT_FontRecord * r, uint32_t cp) {
  size_t i;

  if (!(r->unicode_range[0] | r->unicode_range[1] | r->unicode_range[2] | r->unicode_range[3])) {
    return true;
  }
  for (i = 0; i < OS2_RANGE_COUNT; i++) {
    if (cp >= os2_ranges[i].first && cp <= os2_ranges[i].last
        && (r->unicode_range[os2_ranges[i].bit / 32] >> (os2_ranges[i].bit % 32) & 1u)) {
      return true;
    }
  }
  // Bit 57 is "any character beyond the Basic Multilingual Plane".
  return cp > 0xFFFF && (r->unicode_range[1] >> 25 & 1u);
}

/** Open record @p i once; the face, or NULL when it cannot be opened. */
static const GFNT_Face * open_face(GFNT_FontFallback * f, size_t i, GFNT_Result * result) {
  Entry * e = &f->entries[i];
  const GFNT_FontRecord * r = gfnt_fontset_at(f->set, i);

  *result = GFNT_OK;
  if (e->tried) {
    return e->face;
  }
  e->tried = true;
  *result = gfnt_blob_create_file(r->path, NULL, f->a, &e->blob, NULL);
  if (*result == GFNT_OK) {
    *result = gfnt_face_load(e->blob, r->index, NULL, f->a, &e->face, NULL);
  }
  if (*result != GFNT_OK) {
    gfnt_blob_destroy(e->blob);
    e->blob = NULL;
    e->face = NULL;
    // A file that went away or is not a font is a face that cannot help; only memory is an error.
    if (*result != GFNT_ERR_OOM) {
      *result = GFNT_OK;
    }
  }
  return e->face;
}

static bool maps(const GFNT_Face * face, uint32_t cp) {
  uint32_t glyph = 0;

  return gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) == GFNT_OK && glyph != 0;
}

/**
 * Try one family: among the members that (do or do not) claim the character, the best by
 * style that really maps it.
 */
static GFNT_Result try_group(GFNT_FontFallback * f, size_t g, uint32_t cp, bool claimed,
    const GFNT_FontQuery * wanted, const GFNT_FontRecord ** recs, size_t * idx,
    size_t * out_index, bool * found) {
  const Group * group = &f->groups[g];
  size_t n = 0;
  size_t i;

  for (i = 0; i < group->count; i++) {
    const size_t m = group->members[i];

    if (claims(gfnt_fontset_at(f->set, m), cp) == claimed) {
      recs[n] = gfnt_fontset_at(f->set, m);
      idx[n] = m;
      n++;
    }
  }
  while (n > 0) {
    size_t pick = 0;
    GFNT_Result result = gfnt_match_pick(recs, n, wanted, f->a, &pick, NULL);
    const GFNT_Face * face;

    if (result != GFNT_OK) {
      return result;
    }
    face = open_face(f, idx[pick], &result);
    if (result != GFNT_OK) {
      return result;
    }
    if (face && maps(face, cp)) {
      *out_index = idx[pick];
      *found = true;
      return GFNT_OK;
    }
    memmove(&recs[pick], &recs[pick + 1], (n - pick - 1) * sizeof *recs);
    memmove(&idx[pick], &idx[pick + 1], (n - pick - 1) * sizeof *idx);
    n--;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_fontfallback_face(GFNT_FontFallback * f, uint32_t cp, GFNT_Tag script,
    GFNT_Tag language, const GFNT_FontQuery * wanted, const GFNT_Face ** out_face,
    size_t * out_index, bool * out_found, GFNT_Error * error) {
  GFNT_FontQuery plain;
  size_t * order = NULL;
  uint8_t * seen = NULL;
  const GFNT_FontRecord ** recs = NULL;
  size_t * idx = NULL;
  size_t norder = 0;
  size_t most = 0;
  size_t i;
  size_t j;
  size_t pass;
  size_t found_at = 0;
  bool found = false;
  GFNT_Result result = GFNT_OK;

  if (!f || !out_face || !out_found) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no fallback or nowhere to put the face");
  }
  *out_found = false;
  if (f->group_count == 0) {
    return GFNT_OK;
  }
  if (!wanted) {
    gfnt_fontquery_init(&plain);
    wanted = &plain;
  }
  for (i = 0; i < f->group_count; i++) {
    if (f->groups[i].count > most) {
      most = f->groups[i].count;
    }
  }
  order = f->a->calloc_fn(f->a->ctx, f->group_count, sizeof *order);
  seen = f->a->calloc_fn(f->a->ctx, f->group_count, 1);
  recs = f->a->calloc_fn(f->a->ctx, most, sizeof *recs);
  idx = f->a->calloc_fn(f->a->ctx, most, sizeof *idx);
  if (!order || !seen || !recs || !idx) {
    result = GFNT_ERR_OOM;
    goto done;
  }
  // The order: the preference of this script and language, then of the script alone, then the
  // wanted family, then everything else as the set lists it.
  for (pass = 0; pass < 2; pass++) {
    for (i = 0; i < f->pref_count; i++) {
      const Preference * p = &f->prefs[i];

      if (p->script != script || (pass == 0 ? (p->language != language || !language) : p->language != 0)) {
        continue;
      }
      for (j = 0; j < p->count; j++) {
        if (!seen[p->groups[j]]) {
          seen[p->groups[j]] = 1;
          order[norder++] = p->groups[j];
        }
      }
    }
  }
  if (wanted->family_count && wanted->families && wanted->families[0]) {
    size_t g = find_group(f, wanted->families[0]);

    if (g != (size_t)-1 && !seen[g]) {
      seen[g] = 1;
      order[norder++] = g;
    }
  }
  for (i = 0; i < f->group_count; i++) {
    if (!seen[i]) {
      seen[i] = 1;
      order[norder++] = i;
    }
  }
  // Faces that claim the character first, in every family; then, only if none does, the rest.
  for (pass = 0; pass < 2 && !found && result == GFNT_OK; pass++) {
    for (i = 0; i < norder && !found && result == GFNT_OK; i++) {
      result = try_group(f, order[i], cp, pass == 0, wanted, recs, idx, &found_at, &found);
    }
  }
  if (result == GFNT_OK && found) {
    *out_face = f->entries[found_at].face;
    if (out_index) {
      *out_index = found_at;
    }
    *out_found = true;
  }
done:
  f->a->free_fn(f->a->ctx, order);
  f->a->free_fn(f->a->ctx, seen);
  f->a->free_fn(f->a->ctx, recs);
  f->a->free_fn(f->a->ctx, idx);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE, "finding a fallback face");
  }
  return GFNT_OK;
}
