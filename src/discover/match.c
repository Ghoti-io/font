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
 * CSS Fonts Level 4 font matching over a ::GFNT_FontSet.
 *
 * documentation/design.md section 11. Every step of section 5.2's algorithm is a short search
 * over the faces that are still in the running: *does any face's range contain the value
 * asked for*, else *the nearest value above (or below) it, in the order the specification
 * gives*, and then *drop the faces that do not include that value*. A static face is a
 * range of one value and a variable face is its axis's range, which is how the
 * specification treats a variable face ("as if each supported combination of values is a
 * unique font in the set"). All the arithmetic is in 16.16.
 *
 * Italic and oblique are two properties of a face, as in the specification's model: a
 * roman face is italic 0 and oblique 0 degrees, an italic face is italic 1 with no oblique
 * angle, an oblique face has its angle and no italic value, a face with a `slnt` axis
 * covers the oblique angles that axis spans (the axis's negation) and one with `ital`
 * covers that axis's italic values.
 */

#include "match_int.h"

#include <ghoti.io/font/variation.h>
#include <ghoti.io/unicode/case.h>

#include <stdint.h>
#include <string.h>

#define ONE ((int64_t)65536)

typedef enum Dim { DIM_WEIGHT, DIM_WIDTH, DIM_ITALIC, DIM_OBLIQUE } Dim;

typedef struct Cand {
  const GFNT_FontRecord * rec;
  bool alive;
} Cand;

/** The CSS percentage of an OS/2 width class (the table in the property's definition). */
static int64_t width_of_class(uint16_t class_) {
  static const int32_t percent_x8[10] = {800, 400, 500, 600, 700, 800, 900, 1000, 1200, 1600};

  // 50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200 percent: eighths.
  if (class_ < 1 || class_ > 9) {
    return 100 * ONE;
  }
  return (int64_t)percent_x8[class_] * ONE / 8;
}

/**
 * The range of values @p r covers in @p dim, and whether it has the dimension at all (an
 * italic face has no oblique angle and an oblique one no italic value).
 */
static bool range_of(const GFNT_FontRecord * r, Dim dim, bool mirror, int64_t * lo,
    int64_t * hi) {
  switch (dim) {
    case DIM_WEIGHT:
      if (r->wght_axis.present) {
        *lo = r->wght_axis.min;
        *hi = r->wght_axis.max;
      } else {
        *lo = *hi = (r->weight ? (int64_t)r->weight : 400) * ONE;
      }
      return true;
    case DIM_WIDTH:
      if (r->wdth_axis.present) {
        *lo = r->wdth_axis.min;
        *hi = r->wdth_axis.max;
      } else {
        *lo = *hi = width_of_class(r->width);
      }
      return true;
    case DIM_ITALIC:
      if (r->ital_axis.present) {
        *lo = r->ital_axis.min;
        *hi = r->ital_axis.max;
        return true;
      }
      if (r->slant == GFNT_SLANT_OBLIQUE && !r->slnt_axis.present) {
        return false;
      }
      *lo = *hi = r->slant == GFNT_SLANT_ITALIC ? ONE : 0;
      return true;
    case DIM_OBLIQUE:
      if (r->slnt_axis.present) {
        // `slnt` is negative for a face that leans right; an oblique angle is positive.
        *lo = -(int64_t)r->slnt_axis.max;
        *hi = -(int64_t)r->slnt_axis.min;
      } else if (r->slant == GFNT_SLANT_ITALIC) {
        return false;
      } else if (r->slant == GFNT_SLANT_OBLIQUE) {
        *lo = *hi = r->oblique_angle ? (int64_t)r->oblique_angle : GFNT_OBLIQUE_DEFAULT_ANGLE;
      } else {
        *lo = *hi = 0;
      }
      if (mirror) {
        int64_t swap = *lo;

        *lo = -*hi;
        *hi = -swap;
      }
      return true;
  }
  return false;
}

static bool contains(const Cand * c, size_t n, Dim dim, bool mirror, int64_t v) {
  size_t i;

  for (i = 0; i < n; i++) {
    int64_t lo;
    int64_t hi;

    if (c[i].alive && range_of(c[i].rec, dim, mirror, &lo, &hi) && lo <= v && v <= hi) {
      return true;
    }
  }
  return false;
}

/** Keep only the faces that include @p v. */
static void narrow(Cand * c, size_t n, Dim dim, bool mirror, int64_t v) {
  size_t i;

  for (i = 0; i < n; i++) {
    int64_t lo;
    int64_t hi;

    if (c[i].alive && !(range_of(c[i].rec, dim, mirror, &lo, &hi) && lo <= v && v <= hi)) {
      c[i].alive = false;
    }
  }
}

/**
 * The least value at or above @p from, and at most @p cap, that some face covers; false
 * when there is none. With @p positive only values above zero count.
 */
static bool find_up(const Cand * c, size_t n, Dim dim, bool mirror, int64_t from, int64_t cap,
    bool positive, int64_t * out) {
  bool found = false;
  size_t i;

  for (i = 0; i < n; i++) {
    int64_t lo;
    int64_t hi;
    int64_t v;

    if (!c[i].alive || !range_of(c[i].rec, dim, mirror, &lo, &hi)) {
      continue;
    }
    v = lo > from ? lo : from;
    if (v > hi || v > cap || (positive && v <= 0)) {
      continue;
    }
    if (!found || v < *out) {
      *out = v;
      found = true;
    }
  }
  return found;
}

/** The greatest value at or below @p from, and at least @p floor, that some face covers. */
static bool find_down(const Cand * c, size_t n, Dim dim, bool mirror, int64_t from,
    int64_t floor, bool positive, int64_t * out) {
  bool found = false;
  size_t i;

  for (i = 0; i < n; i++) {
    int64_t lo;
    int64_t hi;
    int64_t v;

    if (!c[i].alive || !range_of(c[i].rec, dim, mirror, &lo, &hi)) {
      continue;
    }
    v = hi < from ? hi : from;
    if (v < lo || v < floor || (positive && v <= 0)) {
      continue;
    }
    if (!found || v > *out) {
      *out = v;
      found = true;
    }
  }
  return found;
}

#define NO_CAP INT64_MAX
#define NO_FLOOR INT64_MIN

/** `font-width`: the value itself if a face has it, else the nearest in the order given. */
static void match_width(Cand * c, size_t n, int64_t want) {
  int64_t v;

  if (contains(c, n, DIM_WIDTH, false, want)) {
    v = want;
  } else if (want <= 100 * ONE) {
    if (!find_down(c, n, DIM_WIDTH, false, want, NO_FLOOR, false, &v)) {
      find_up(c, n, DIM_WIDTH, false, want, NO_CAP, false, &v);
    }
  } else {
    if (!find_up(c, n, DIM_WIDTH, false, want, NO_CAP, false, &v)) {
      find_down(c, n, DIM_WIDTH, false, want, NO_FLOOR, false, &v);
    }
  }
  narrow(c, n, DIM_WIDTH, false, v);
}

/** `font-weight`, with the 400 to 500 rule. */
static void match_weight(Cand * c, size_t n, int64_t want) {
  int64_t v;

  if (contains(c, n, DIM_WEIGHT, false, want)) {
    v = want;
  } else if (want >= 400 * ONE && want <= 500 * ONE) {
    // At or above, up to and including 500; then below, descending; then above 500.
    if (!find_up(c, n, DIM_WEIGHT, false, want, 500 * ONE, false, &v)
        && !find_down(c, n, DIM_WEIGHT, false, want, NO_FLOOR, false, &v)) {
      find_up(c, n, DIM_WEIGHT, false, 500 * ONE, NO_CAP, false, &v);
    }
  } else if (want < 400 * ONE) {
    if (!find_down(c, n, DIM_WEIGHT, false, want, NO_FLOOR, false, &v)) {
      find_up(c, n, DIM_WEIGHT, false, want, NO_CAP, false, &v);
    }
  } else {
    if (!find_up(c, n, DIM_WEIGHT, false, want, NO_CAP, false, &v)) {
      find_down(c, n, DIM_WEIGHT, false, want, NO_FLOOR, false, &v);
    }
  }
  narrow(c, n, DIM_WEIGHT, false, v);
}

/** The values the style step settled on. */
typedef struct Style {
  int64_t italic;
  int64_t oblique;
} Style;

/** Narrow to the faces that include @p v, and note it as the italic value or oblique angle. */
static void commit(Cand * c, size_t n, Dim dim, bool mirror, int64_t v, Style * s) {
  narrow(c, n, dim, mirror, v);
  if (dim == DIM_ITALIC) {
    s->italic = v;
  } else {
    s->oblique = mirror ? -v : v;
  }
}

/**
 * One search of the style step: the least value at or above @p from (at most @p cap), or the
 * greatest at or below it (at least @p cap), that a face covers in @p dim. On success the
 * faces are narrowed to it and true is returned, which ends the step.
 */
static bool up(Cand * c, size_t n, Dim dim, bool mirror, int64_t from, bool positive, Style * s) {
  int64_t v = 0;

  if (!find_up(c, n, dim, mirror, from, NO_CAP, positive, &v)) {
    return false;
  }
  commit(c, n, dim, mirror, v, s);
  return true;
}

static bool down(Cand * c, size_t n, Dim dim, bool mirror, int64_t from, bool positive, Style * s) {
  int64_t v = 0;

  if (!find_down(c, n, dim, mirror, from, NO_FLOOR, positive, &v)) {
    return false;
  }
  commit(c, n, dim, mirror, v, s);
  return true;
}

/** `font-style`, after the specification's steps for `normal`, `italic` and `oblique`. */
static void match_style(Cand * c, size_t n, GFNT_Slant slant, int64_t angle, Style * s) {
  s->italic = 0;
  s->oblique = 0;
  if (slant == GFNT_SLANT_ITALIC) {
    if (contains(c, n, DIM_ITALIC, false, ONE)) {
      commit(c, n, DIM_ITALIC, false, ONE, s);
      return;
    }
    // Italic values above the request ascending, then below it; then oblique angles at or above
    // 11 degrees ascending, then those below descending; then italic, then oblique, at or below 0.
    (void)(up(c, n, DIM_ITALIC, false, ONE, true, s) || down(c, n, DIM_ITALIC, false, ONE, true, s)
        || up(c, n, DIM_OBLIQUE, false, 11 * ONE, true, s)
        || down(c, n, DIM_OBLIQUE, false, 11 * ONE, true, s)
        || down(c, n, DIM_ITALIC, false, 0, false, s) || down(c, n, DIM_OBLIQUE, false, 0, false, s));
    return;
  }
  if (slant == GFNT_SLANT_OBLIQUE) {
    const bool mirror = angle < 0;
    const int64_t t = mirror ? -angle : angle;

    if (contains(c, n, DIM_OBLIQUE, mirror, t)) {
      commit(c, n, DIM_OBLIQUE, mirror, t, s);
      return;
    }
    if (t >= 11 * ONE) {
      // Oblique angles above ascending, then below descending; then italic at or above 1
      // ascending, then below it descending.
      if (up(c, n, DIM_OBLIQUE, mirror, t, true, s) || down(c, n, DIM_OBLIQUE, mirror, t, true, s)
          || up(c, n, DIM_ITALIC, false, ONE, true, s) || down(c, n, DIM_ITALIC, false, ONE, true, s)) {
        return;
      }
    } else {
      // Oblique angles below descending, then above ascending; then italic up to 1 descending,
      // then above 1 ascending. ("Less than 1" is read as "up to 1": an italic face is 1.)
      if (down(c, n, DIM_OBLIQUE, mirror, t, true, s) || up(c, n, DIM_OBLIQUE, mirror, t, true, s)
          || down(c, n, DIM_ITALIC, false, ONE, true, s) || up(c, n, DIM_ITALIC, false, ONE, true, s)) {
        return;
      }
    }
    // Last, oblique then italic at or below zero, descending.
    (void)(down(c, n, DIM_OBLIQUE, false, 0, false, s) || down(c, n, DIM_ITALIC, false, 0, false, s));
    return;
  }
  // normal: oblique at or above zero ascending, then italic at or above zero ascending, then
  // oblique below zero, then italic below zero (both descending).
  (void)(up(c, n, DIM_OBLIQUE, false, 0, false, s) || up(c, n, DIM_ITALIC, false, 0, false, s)
      || down(c, n, DIM_OBLIQUE, false, 0, false, s) || down(c, n, DIM_ITALIC, false, 0, false, s));
}

static bool name_is(const char * have, const char * want) {
  return have && have[0] && guni_case_folded_equal_utf8(have, strlen(have), want, strlen(want),
      false);
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/** The faces of one family name, narrowed to one; @p found is false when the family has none. */
static GFNT_Result match_name(const GFNT_FontSet * set, const char * name,
    const GFNT_FontQuery * q, const GFNT_Allocator * allocator,
    const GFNT_FontRecord ** recs, GFNT_FontMatch * out, bool * found) {
  const size_t total = gfnt_fontset_count(set);
  size_t n = 0;
  size_t i;
  size_t pick = 0;
  GFNT_Result result;

  *found = false;
  for (i = 0; i < total; i++) {
    const GFNT_FontRecord * r = gfnt_fontset_at(set, i);

    if (name_is(r->family, name) || name_is(r->legacy_family, name)) {
      recs[n++] = r;
    }
  }
  if (n == 0) {
    return GFNT_OK;
  }
  result = gfnt_match_pick(recs, n, q, allocator, &pick, out);
  if (result != GFNT_OK) {
    return result;
  }
  out->index = (size_t)(recs[pick] - gfnt_fontset_at(set, 0));
  *found = true;
  return GFNT_OK;
}

GFNT_Result gfnt_match_pick(const GFNT_FontRecord * const * recs, size_t n,
    const GFNT_FontQuery * q, const GFNT_Allocator * allocator, size_t * out_pick,
    GFNT_FontMatch * out_values) {
  Cand * cands;
  size_t i;
  size_t first = 0;
  int64_t want_weight = q->weight ? q->weight : 400 * ONE;
  int64_t want_width = q->width ? q->width : 100 * ONE;
  int64_t angle = q->oblique_angle_set ? q->oblique_angle : GFNT_OBLIQUE_DEFAULT_ANGLE;
  int64_t lo;
  int64_t hi;
  Style style;

  if (n == 0) {
    return GFNT_ERR_INVALID;
  }
  cands = allocator->calloc_fn(allocator->ctx, n, sizeof *cands);
  if (!cands) {
    return GFNT_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    cands[i].rec = recs[i];
    cands[i].alive = true;
  }
  match_width(cands, n, want_width);
  match_style(cands, n, q->slant, q->slant == GFNT_SLANT_OBLIQUE ? angle : 0, &style);
  match_weight(cands, n, want_weight);
  while (first < n && !cands[first].alive) {
    first++;
  }
  if (first == n) {
    allocator->free_fn(allocator->ctx, cands);
    return GFNT_ERR_INTERNAL;
  }
  *out_pick = first;
  if (out_values) {
    range_of(cands[first].rec, DIM_WEIGHT, false, &lo, &hi);
    out_values->weight = (GFNT_F16Dot16)clamp64(want_weight, lo, hi);
    range_of(cands[first].rec, DIM_WIDTH, false, &lo, &hi);
    out_values->width = (GFNT_F16Dot16)clamp64(want_width, lo, hi);
    out_values->italic = (GFNT_F16Dot16)style.italic;
    out_values->oblique = (GFNT_F16Dot16)style.oblique;
  }
  allocator->free_fn(allocator->ctx, cands);
  return GFNT_OK;
}

void gfnt_fontquery_init(GFNT_FontQuery * query) {
  if (query) {
    memset(query, 0, sizeof *query);
  }
}

static GFNT_Result match_one(const GFNT_FontSet * set, const char * family,
    const GFNT_FontQuery * q, const GFNT_Allocator * allocator, GFNT_FontMatch * out,
    bool * found, GFNT_Error * error) {
  const size_t total = gfnt_fontset_count(set);
  const GFNT_FontRecord ** recs;
  GFNT_Result result;
  const char * alias = NULL;

  *found = false;
  if (total == 0) {
    return GFNT_OK;
  }
  recs = allocator->calloc_fn(allocator->ctx, total, sizeof *recs);
  if (!recs) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the faces of a family");
  }
  result = match_name(set, family, q, allocator, recs, out, found);
  if (result == GFNT_OK && !*found
      && gfnt_fontset_resolve_alias(set, family, &alias, NULL) == GFNT_OK
      && alias && strcmp(alias, family) != 0) {
    result = match_name(set, alias, q, allocator, recs, out, found);
  }
  allocator->free_fn(allocator->ctx, recs);
  if (result != GFNT_OK) {
    return gfnt_error_set(error, result, 0, 0, GFNT_GLYPH_NONE, "matching a family");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_fontset_match_family(const GFNT_FontSet * set, const char * family,
    const GFNT_FontQuery * query, const GFNT_Allocator * allocator, GFNT_FontMatch * out_match,
    bool * out_found, GFNT_Error * error) {
  GFNT_Result result;

  if (!set || !family || !query || !out_match || !out_found) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no set, family, query or place for the answer");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = match_one(set, family, query, allocator, out_match, out_found, error);
  out_match->family = 0;
  return result;
}

GFNT_Result gfnt_fontset_match(const GFNT_FontSet * set, const GFNT_FontQuery * query,
    const GFNT_Allocator * allocator, GFNT_FontMatch * out_match, bool * out_found,
    GFNT_Error * error) {
  size_t i;

  if (!set || !query || !out_match || !out_found || (query->family_count && !query->families)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no set, query or place for the answer");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  *out_found = false;
  for (i = 0; i < query->family_count; i++) {
    GFNT_Result result;

    if (!query->families[i]) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a NULL family name");
    }
    result = match_one(set, query->families[i], query, allocator, out_match, out_found, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (*out_found) {
      out_match->family = i;
      return GFNT_OK;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_fontmatch_location(const GFNT_Face * face, const GFNT_FontMatch * match,
    GFNT_F2Dot14 * out_coordinates, size_t capacity, size_t * out_count, GFNT_Error * error) {
  size_t axes = 0;
  size_t i;
  GFNT_F16Dot16 user[64];
  GFNT_Result result;

  if (!face || !match || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, match or place for the count");
  }
  result = gfnt_face_axis_count(face, &axes, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (axes == 0) {
    *out_count = 0;
    return GFNT_OK;
  }
  if (axes > capacity || axes > sizeof user / sizeof user[0] || !out_coordinates) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "more axes than the coordinates have room for");
  }
  for (i = 0; i < axes; i++) {
    GFNT_Axis axis;
    int64_t v;

    result = gfnt_face_axis_at(face, i, &axis, error);
    if (result != GFNT_OK) {
      return result;
    }
    v = axis.def;
    if (axis.tag == GFNT_TAG('w', 'g', 'h', 't')) {
      v = match->weight;
    } else if (axis.tag == GFNT_TAG('w', 'd', 't', 'h')) {
      v = match->width;
    } else if (axis.tag == GFNT_TAG('s', 'l', 'n', 't')) {
      v = -(int64_t)match->oblique;
    } else if (axis.tag == GFNT_TAG('i', 't', 'a', 'l')) {
      v = match->italic;
    }
    user[i] = (GFNT_F16Dot16)clamp64(v, axis.min, axis.max);
  }
  result = gfnt_face_normalize(face, user, axes, out_coordinates, capacity, error);
  if (result == GFNT_OK) {
    *out_count = axes;
  }
  return result;
}
