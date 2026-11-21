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
 * The map from characters to glyphs that a face's glyph names give (namecmap.h).
 */

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <stdlib.h>
#include <string.h>
#include "namecmap.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"

typedef struct NamePair {
  uint32_t codepoint;
  uint32_t glyph;
  uint32_t variant; ///< 1 when the name has a suffix after a period.
} NamePair;

static int pair_cmp(const void * a, const void * b) {
  const NamePair * x = a;
  const NamePair * y = b;

  if (x->codepoint != y->codepoint) {
    return x->codepoint < y->codepoint ? -1 : 1;
  }
  if (x->variant != y->variant) {
    return x->variant < y->variant ? -1 : 1;
  }
  return x->glyph < y->glyph ? -1 : x->glyph > y->glyph;
}

void gfnt_namecmap_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_NameCmap * map = table;

  if (!map) {
    return;
  }
  allocator->free_fn(allocator->ctx, map->codepoints);
  allocator->free_fn(allocator->ctx, map->glyphs);
  map->codepoints = NULL;
  map->glyphs = NULL;
  map->count = 0;
}

GFNT_Result gfnt_namecmap_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  GFNT_NameCmap * map = out;
  const GFNT_Allocator * allocator = face->allocator;
  NamePair * pairs;
  size_t glyphs = 0;
  size_t n = 0;
  size_t i;
  size_t unique = 0;
  GFNT_Result result;

  (void)context;
  *map = (GFNT_NameCmap) {0};
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  pairs = allocator->malloc_fn(allocator->ctx, (glyphs ? glyphs : 1) * sizeof *pairs);
  if (!pairs) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "listing the characters of the glyph names");
  }
  for (i = 1; i < glyphs; i++) {
    char * name = NULL;
    uint32_t cps[2];
    size_t count = 0;

    result = gfnt_face_glyph_name(face, (uint32_t)i, allocator, &name, NULL, error);
    if (result == GFNT_ERR_UNSUPPORTED) {
      allocator->free_fn(allocator->ctx, pairs);
      return result;
    }
    if (result == GFNT_ERR_OOM) {
      allocator->free_fn(allocator->ctx, pairs);
      return result;
    }
    if (result != GFNT_OK) {
      continue;   /* a glyph this face does not name is a glyph no character reaches */
    }
    /* count 2 is "more than one": the function reports the full count either way. */
    result = gfnt_glyph_name_to_unicode(name, false, cps, 2, &count, NULL);
    if (count == 1 && result == GFNT_OK) {
      pairs[n].codepoint = cps[0];
      pairs[n].glyph = (uint32_t)i;
      pairs[n].variant = strchr(name, '.') != NULL;
      n++;
    }
    gfnt_glyph_name_free(allocator, name);
  }
  qsort(pairs, n, sizeof *pairs, pair_cmp);
  for (i = 0; i < n; i++) {
    if (i == 0 || pairs[i].codepoint != pairs[i - 1].codepoint) {
      unique++;
    }
  }
  map->codepoints = allocator->malloc_fn(allocator->ctx, (unique ? unique : 1) * sizeof(uint32_t));
  map->glyphs = allocator->malloc_fn(allocator->ctx, (unique ? unique : 1) * sizeof(uint32_t));
  if (!map->codepoints || !map->glyphs) {
    gfnt_namecmap_release(allocator, map);
    allocator->free_fn(allocator->ctx, pairs);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "building the characters of the glyph names");
  }
  for (i = 0; i < n; i++) {
    if (i == 0 || pairs[i].codepoint != pairs[i - 1].codepoint) {
      map->codepoints[map->count] = pairs[i].codepoint;
      map->glyphs[map->count] = pairs[i].glyph;
      map->count++;
    }
  }
  allocator->free_fn(allocator->ctx, pairs);
  return GFNT_OK;
}

GFNT_Result gfnt_namecmap_lookup(const GFNT_Face * face, uint32_t codepoint,
    uint32_t * out_glyph, GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_NameCmap scratch;
  size_t low = 0;
  size_t high;
  GFNT_Result result;

  result = gfnt_table_cached(face, &owner->name_cmap_state, &owner->name_cmap, &scratch,
      sizeof scratch, gfnt_namecmap_parse, NULL, gfnt_namecmap_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  high = owner->name_cmap.count;
  *out_glyph = 0;
  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (owner->name_cmap.codepoints[mid] == codepoint) {
      *out_glyph = owner->name_cmap.glyphs[mid];
      return GFNT_OK;
    }
    if (owner->name_cmap.codepoints[mid] < codepoint) {
      low = mid + 1;
    }
    else {
      high = mid;
    }
  }
  return GFNT_OK;
}
