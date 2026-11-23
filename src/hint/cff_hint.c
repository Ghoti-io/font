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
 * Hinting a charstring glyph: stems and blue zones, y direction only.
 *
 * The method, in the order the code does it (the experiments that fixed each rule are in
 * the oracle scripts under tools/oracle, `hint_cff_engine_model.py` being this file's
 * prototype):
 *
 *   1. A **hint map** is a list of stem edges in font units, each with the position it
 *      takes in pixels. One map per hint mask: the stems a mask turns on are the ones that
 *      move the points drawn after it.
 *   2. An edge in a **blue zone** of its own side (a stem's bottom edge in a bottom zone, its
 *      top edge in a top zone) is *locked* to the zone's flat position, rounded outward-biased:
 *      `round(flat * s + bias)` for a top zone and `round(flat * s - bias)` for a bottom one,
 *      where the bias fades from 0.6 pixel to nothing as the size approaches the size
 *      (`BlueScale`) at which overshoot suppression ends. At and above that size an edge on
 *      the flat position is rounded and an overshoot edge keeps its overshoot, rounded
 *      outward with a 0.2 bias. The stem's other edge follows at the stem's own width.
 *   3. A stem with no locked edge moves rigidly. The locked edges alone give it a provisional
 *      shift (interpolated at the stem's centre between the locked edges on either side, the
 *      nearest one's shift when there is one side, or interpolated against an implicit
 *      anchor at y = 0 when the locked edges are all above, or, for a stem wholly below the baseline, the
 *      nearest one's shift fading to nothing eight pixels away); then whichever of its two
 *      edges is nearer a whole pixel is rounded to it. Free stems go bottom to top, and one that
 *      would be left less than half a pixel from the edge below puts its bottom on the first whole
 *      pixel that clears it, or stays unrounded if that runs into the edge above.
 *   4. Every point is mapped through the map: linear between edges, shifted rigidly outside
 *      them. All of it is 16.16 fixed point with rounded multiplies and divisions, and the
 *      26.6 result is the 16.16 value shifted right, not rounded.
 *
 * The advance is the unhinted one rounded to a whole pixel. x is scaled and not moved.
 */

#include "cff_hint.h"

#include <string.h>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/metrics.h>
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../sfnt/sfnt.h"

/** The most edges a map holds: two per stem. */
#define GFNT_CFFH_MAX_EDGES (2 * GFNT_CS_MAX_STEMS + 8)
/** The most zones: seven pairs of BlueValues and seven of OtherBlues. */
#define GFNT_CFFH_MAX_ZONES (GFNT_CS_MAX_BLUES)

/** 0.6 and 0.2 pixel, 16.16: the outward bias at the smallest size, and the one above BlueScale. */
#define GFNT_CFFH_BOOST_NUM 6
#define GFNT_CFFH_BOOST_DEN 10
#define GFNT_CFFH_OVERSHOOT_BIAS 13107
/** The bias never exceeds half a pixel. */
#define GFNT_CFFH_BOOST_MAX 0x8000
/** The bottom edge of a stem is the one rounded only when it is strictly nearer a whole pixel; an exact tie goes to the top edge. */
#define GFNT_CFFH_TIE 0
/** A free stem is not left closer than this, half a pixel, to the edge below it. */
#define GFNT_CFFH_MIN_GAP 0x8000

struct GFNT_CffHint {
  const GFNT_Allocator * allocator;
  uint32_t ppem;
  uint16_t upem;
  int32_t scale;              ///< 16.16 pixels per font unit.
  GFNT_F2Dot14 * coords;      ///< A copy of the location, or NULL.
  GFNT_Variation variation;   ///< Over @p coords; count 0 for the default instance.
};

typedef struct GFNT_CffhEdge {
  int64_t cs;   ///< Font units, 16.16.
  int64_t ds;   ///< Pixels, 16.16.
} GFNT_CffhEdge;

typedef struct GFNT_CffhMap {
  GFNT_CffhEdge e[GFNT_CFFH_MAX_EDGES];
  size_t n;
} GFNT_CffhMap;

typedef struct GFNT_CffhZone {
  int64_t lo;
  int64_t hi;
  int64_t flat;
  bool top;
} GFNT_CffhZone;

/** A rounded 16.16 multiply, half away from zero. */
static int64_t gfnt_cffh_mul(int64_t a, int64_t b) {
  int64_t p = a * b;

  return p >= 0 ? (p + 0x8000) >> 16 : -((-p + 0x8000) >> 16);
}

/** A rounded 16.16 division, half away from zero. @p b is never zero where this is called. */
static int64_t gfnt_cffh_div(int64_t a, int64_t b) {
  bool negative = false;
  int64_t q;

  if (a < 0) {
    a = -a;
    negative = !negative;
  }
  if (b < 0) {
    b = -b;
    negative = !negative;
  }
  q = ((a << 16) + (b >> 1)) / b;
  return negative ? -q : q;
}

/** The nearest whole pixel to a 16.16 position, a half rounding up. */
static int64_t gfnt_cffh_round_px(int64_t x) {
  int64_t q = x + 0x8000;
  int64_t r = q % 0x10000;

  if (r < 0) {
    r += 0x10000;
  }
  return q - r;
}

static int64_t gfnt_cffh_abs(int64_t x) {
  return x < 0 ? -x : x;
}

/** Half of a sum, rounded toward negative infinity. */
static int64_t gfnt_cffh_half_floor(int64_t x) {
  return x >= 0 ? x / 2 : -((-x + 1) / 2);
}

/** A 16.16 value as 26.6: shifted down by ten, toward negative infinity, and clamped to 32 bits. */
static int32_t gfnt_cffh_to_26_6(int64_t ds) {
  int64_t v = ds >= 0 ? ds >> 10 : -((-ds + 1023) >> 10);

  if (v > INT32_MAX) {
    return INT32_MAX;
  }
  if (v < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)v;
}

static void gfnt_cffh_set_edge(GFNT_CffhMap * map, int64_t cs, int64_t ds) {
  for (size_t i = 0; i < map->n; ++i) {
    if (map->e[i].cs == cs) {
      map->e[i].ds = ds;
      return;
    }
  }
  if (map->n < GFNT_CFFH_MAX_EDGES) {
    map->e[map->n].cs = cs;
    map->e[map->n].ds = ds;
    map->n += 1u;
  }
}

static void gfnt_cffh_sort(GFNT_CffhMap * map) {
  for (size_t i = 1; i < map->n; ++i) {
    GFNT_CffhEdge key = map->e[i];
    size_t j = i;

    while (j > 0 && map->e[j - 1u].cs > key.cs) {
      map->e[j] = map->e[j - 1u];
      --j;
    }
    map->e[j] = key;
  }
}

/** The shift of an edge: where it went, less where plain scaling would put it. */
static int64_t gfnt_cffh_delta(const GFNT_CffhEdge * edge, int64_t scale) {
  return edge->ds - gfnt_cffh_mul(edge->cs, scale);
}

/** The shift at @p cs interpolated between two anchors, or held at the nearer end outside them. */
static int64_t gfnt_cffh_interp(const GFNT_CffhEdge * a, const GFNT_CffhEdge * b, int64_t cs,
    int64_t scale) {
  int64_t da;
  int64_t db;

  if (cs <= a->cs) {
    return gfnt_cffh_delta(a, scale);
  }
  if (cs >= b->cs) {
    return gfnt_cffh_delta(b, scale);
  }
  da = gfnt_cffh_delta(a, scale);
  db = gfnt_cffh_delta(b, scale);
  return da + gfnt_cffh_mul(db - da, gfnt_cffh_div(cs - a->cs, b->cs - a->cs));
}

static size_t gfnt_cffh_zones(const GFNT_CharstringHintParams * params, GFNT_CffhZone * out) {
  size_t n = 0;

  for (size_t i = 0; i + 1u < params->blue_value_count && n < GFNT_CFFH_MAX_ZONES; i += 2u) {
    out[n].lo = params->blue_values[i];
    out[n].hi = params->blue_values[i + 1u];
    out[n].top = i != 0;
    out[n].flat = out[n].top ? out[n].lo : out[n].hi;
    n += 1u;
  }
  for (size_t i = 0; i + 1u < params->other_blue_count && n < GFNT_CFFH_MAX_ZONES; i += 2u) {
    out[n].lo = params->other_blues[i];
    out[n].hi = params->other_blues[i + 1u];
    out[n].top = false;
    out[n].flat = out[n].hi;
    n += 1u;
  }
  return n;
}

/** Where a locked edge goes, or false when @p cs is in no zone of @p top's side. */
static bool gfnt_cffh_locked(const GFNT_CffhZone * zones, size_t zone_count, int64_t fuzz, int64_t shift, int64_t cs,
    bool top, int64_t scale, int64_t boost, int64_t * out) {
  for (size_t i = 0; i < zone_count; ++i) {
    const GFNT_CffhZone * z = &zones[i];
    int64_t v;

    if (z->top != top || cs < z->lo - fuzz || cs > z->hi + fuzz) {
      continue;
    }
    v = gfnt_cffh_mul(z->flat, scale);
    if (boost == 0 && cs != z->flat) {
      // At or above BlueScale: the overshoot is kept, rounded outward.
      int64_t overshoot = gfnt_cffh_mul(cs - z->flat, scale);
      int64_t kept = gfnt_cffh_round_px(overshoot + (z->top ? GFNT_CFFH_OVERSHOOT_BIAS : -GFNT_CFFH_OVERSHOOT_BIAS));
      int64_t units = cs > z->flat ? cs - z->flat : z->flat - cs;

      if (units < shift) {
        // An overshoot under BlueShift is flattened at any size.
        kept = 0;
      }
      else if (z->top ? kept < 0x10000 : kept > -0x10000) {
        // At or over it, the overshoot is at least a pixel.
        kept = z->top ? 0x10000 : -0x10000;
      }
      *out = gfnt_cffh_round_px(v) + kept;
    }
    else {
      *out = gfnt_cffh_round_px(z->top ? v + boost : v - boost);
    }
    return true;
  }
  return false;
}

/**
 * BlueScale as the engine applies it. A font whose widest blue zone times BlueScale exceeds one pixel
 * asks for overshoot suppression at sizes where that zone is taller than a pixel; the engine then
 * limits BlueScale to the reciprocal of that width, so suppression ends where the widest zone
 * reaches a pixel. (Measured: TeX Gyre Schola Math, a 114-unit descender zone with BlueScale 0.0455,
 * stops suppressing at 9 ppem, not at 45.)
 */
static int64_t gfnt_cffh_effective_blue_scale(const GFNT_CharstringHintParams * params) {
  int64_t widest = 0;
  int64_t scale = params->blue_scale;

  for (size_t i = 0; i + 1u < params->blue_value_count; i += 2u) {
    int64_t w = (int64_t)params->blue_values[i + 1u] - params->blue_values[i];

    widest = w > widest ? w : widest;
  }
  for (size_t i = 0; i + 1u < params->other_blue_count; i += 2u) {
    int64_t w = (int64_t)params->other_blues[i + 1u] - params->other_blues[i];

    widest = w > widest ? w : widest;
  }
  if (widest > 0 && scale > 0) {
    int64_t limit = gfnt_cffh_div(0x10000, widest);

    if (limit < scale) {
      scale = limit;
    }
  }
  return scale;
}

/** The provisional shift at @p cs from the locked edges @p blue (sorted), as the rules say. */
static int64_t gfnt_cffh_provisional(const GFNT_CffhEdge * blue, size_t blue_count, int64_t lo,
    int64_t hi, int64_t cs, int64_t scale) {
  const GFNT_CffhEdge * below = NULL;
  const GFNT_CffhEdge * above = NULL;

  for (size_t i = 0; i < blue_count; ++i) {
    if (blue[i].cs <= lo) {
      below = &blue[i];
    }
    if (!above && blue[i].cs >= hi) {
      above = &blue[i];
    }
  }
  if (below && above) {
    return gfnt_cffh_interp(below, above, cs, scale);
  }
  if (below) {
    return gfnt_cffh_delta(below, scale);
  }
  if (above) {
    GFNT_CffhEdge origin;

    if (hi <= 0) {
      // Wholly below the baseline: the nearest locked edge's shift, fading to nothing eight pixels away.
      int64_t fade = 0x10000 - (gfnt_cffh_mul(above->cs - hi, scale) >> 3);

      return gfnt_cffh_mul(gfnt_cffh_delta(above, scale), fade > 0 ? fade : 0);
    }
    origin.cs = 0;
    origin.ds = 0;
    return gfnt_cffh_interp(&origin, above, cs, scale);
  }
  return 0;
}

/** One stem still to place: no locked edge of its own. */
typedef struct GFNT_CffhFree {
  int64_t lo;
  int64_t hi;
} GFNT_CffhFree;

/** Build the hint map of the stems in @p active. */
static void gfnt_cffh_build(const GFNT_CharstringHints * hints, const bool * active,
    const GFNT_CharstringHintParams * params, int64_t scale, int64_t boost, GFNT_CffhMap * map) {
  GFNT_CffhZone zones[GFNT_CFFH_MAX_ZONES];
  size_t zone_count = gfnt_cffh_zones(params, zones);
  int64_t fuzz = params->blue_fuzz;
  int64_t shift = params->blue_shift;
  GFNT_CffhFree pending[GFNT_CS_MAX_STEMS];
  int64_t ghosts[GFNT_CS_MAX_STEMS];
  GFNT_CffhEdge blue[GFNT_CFFH_MAX_EDGES];
  size_t pending_count = 0;
  size_t ghost_count = 0;
  size_t blue_count;

  map->n = 0;
  for (size_t i = 0; i < hints->stem_count; ++i) {
    const GFNT_CharstringStem * stem = &hints->stems[i];
    int64_t edge = stem->edge;
    int64_t width = stem->width;
    int64_t lo;
    int64_t hi;
    int64_t at;

    if (!active[i] || stem->vertical) {
      continue;
    }
    if (width == -20 * GFNT_F16DOT16_ONE || width == -21 * GFNT_F16DOT16_ONE) {
      // A ghost hint is one edge: -20 the top edge at y, -21 the bottom edge at y + dy.
      bool top = width == -20 * GFNT_F16DOT16_ONE;
      int64_t cs = top ? edge : edge + width;

      if (gfnt_cffh_locked(zones, zone_count, fuzz, shift, cs, top, scale, boost, &at)) {
        gfnt_cffh_set_edge(map, cs, at);
      }
      else if (ghost_count < GFNT_CS_MAX_STEMS) {
        ghosts[ghost_count++] = cs;
      }
      continue;
    }
    lo = width >= 0 ? edge : edge + width;
    hi = width >= 0 ? edge + width : edge;
    {
      int64_t a = 0;
      int64_t b = 0;
      bool lock_lo = gfnt_cffh_locked(zones, zone_count, fuzz, shift, lo, false, scale, boost, &a);
      bool lock_hi = gfnt_cffh_locked(zones, zone_count, fuzz, shift, hi, true, scale, boost, &b);

      if (lock_lo && lock_hi) {
        gfnt_cffh_set_edge(map, lo, a);
        gfnt_cffh_set_edge(map, hi, b);
      }
      else if (lock_lo) {
        gfnt_cffh_set_edge(map, lo, a);
        gfnt_cffh_set_edge(map, hi, a + gfnt_cffh_mul(hi - lo, scale));
      }
      else if (lock_hi) {
        gfnt_cffh_set_edge(map, hi, b);
        gfnt_cffh_set_edge(map, lo, b - gfnt_cffh_mul(hi - lo, scale));
      }
      else if (pending_count < GFNT_CS_MAX_STEMS) {
        pending[pending_count].lo = lo;
        pending[pending_count].hi = hi;
        pending_count += 1u;
      }
    }
  }
  // The locked edges alone are what a free stem is placed from.
  gfnt_cffh_sort(map);
  blue_count = map->n;
  memcpy(blue, map->e, blue_count * sizeof blue[0]);
  // Free stems are placed bottom to top.
  for (size_t i = 1; i < pending_count; ++i) {
    GFNT_CffhFree key = pending[i];
    size_t j = i;

    while (j > 0 && (pending[j - 1u].lo > key.lo || (pending[j - 1u].lo == key.lo && pending[j - 1u].hi > key.hi))) {
      pending[j] = pending[j - 1u];
      --j;
    }
    pending[j] = key;
  }
  for (size_t i = 0; i < pending_count; ++i) {
    int64_t lo = pending[i].lo;
    int64_t hi = pending[i].hi;
    int64_t shift = gfnt_cffh_provisional(blue, blue_count, lo, hi, gfnt_cffh_half_floor(lo + hi), scale);
    int64_t ls = gfnt_cffh_mul(lo, scale) + shift;
    int64_t hs = gfnt_cffh_mul(hi, scale) + shift;
    int64_t r0 = gfnt_cffh_round_px(ls) - ls;
    int64_t r1 = gfnt_cffh_round_px(hs) - hs;
    int64_t move = gfnt_cffh_abs(r0) < gfnt_cffh_abs(r1) - GFNT_CFFH_TIE ? r0 : r1;
    int64_t placed = ls + move;
    int64_t width = gfnt_cffh_mul(hi, scale) - gfnt_cffh_mul(lo, scale);
    const GFNT_CffhEdge * below = NULL;
    const GFNT_CffhEdge * above = NULL;

    // A stem is not left closer than half a pixel to the edge below it: its bottom goes to the first
    // whole pixel that clears it, unless that runs into the edge above, and then it stays where
    // scaling (with the provisional shift) puts it.
    for (size_t k = 0; k < map->n; ++k) {
      if (map->e[k].cs <= lo && (!below || map->e[k].cs > below->cs || (map->e[k].cs == below->cs && map->e[k].ds > below->ds))) {
        below = &map->e[k];
      }
      if (map->e[k].cs >= hi && (!above || map->e[k].cs < above->cs || (map->e[k].cs == above->cs && map->e[k].ds < above->ds))) {
        above = &map->e[k];
      }
    }
    if (below && placed - below->ds < GFNT_CFFH_MIN_GAP) {
      int64_t candidate = gfnt_cffh_round_px(below->ds + GFNT_CFFH_MIN_GAP + 0x7FFF);

      placed = above && candidate + width > above->ds - GFNT_CFFH_MIN_GAP ? ls : candidate;
    }
    gfnt_cffh_set_edge(map, lo, placed);
    gfnt_cffh_set_edge(map, hi, placed + width);
  }
  for (size_t i = 0; i < ghost_count; ++i) {
    int64_t shift = gfnt_cffh_provisional(blue, blue_count, ghosts[i], ghosts[i], ghosts[i], scale);

    gfnt_cffh_set_edge(map, ghosts[i], gfnt_cffh_round_px(gfnt_cffh_mul(ghosts[i], scale) + shift));
  }
  gfnt_cffh_sort(map);
}

/** Map one position through a map: linear between edges, rigid outside them. */
static int64_t gfnt_cffh_map_point(const GFNT_CffhMap * map, int64_t cs, int64_t scale) {
  if (map->n == 0) {
    return gfnt_cffh_mul(cs, scale);
  }
  if (cs <= map->e[0].cs) {
    return map->e[0].ds + gfnt_cffh_mul(cs - map->e[0].cs, scale);
  }
  if (cs >= map->e[map->n - 1u].cs) {
    return map->e[map->n - 1u].ds + gfnt_cffh_mul(cs - map->e[map->n - 1u].cs, scale);
  }
  for (size_t i = 0; i + 1u < map->n; ++i) {
    const GFNT_CffhEdge * a = &map->e[i];
    const GFNT_CffhEdge * b = &map->e[i + 1u];

    if (a->cs <= cs && cs <= b->cs) {
      if (b->cs == a->cs || cs == a->cs) {
        return a->ds;
      }
      if (cs == b->cs) {
        // A point on an edge is on the edge's position exactly, not on the interval's rounding of it.
        return b->ds;
      }
      return a->ds + gfnt_cffh_mul(cs - a->cs, gfnt_cffh_div(b->ds - a->ds, b->cs - a->cs));
    }
  }
  return gfnt_cffh_mul(cs, scale);
}

bool gfnt_cffhint_applies(const GFNT_Face * face) {
  return face && (gfnt_sfnt_producer(face) == GFNT_PRODUCER_CFF || gfnt_sfnt_producer(face) == GFNT_PRODUCER_CFF2
      || gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1);
}

GFNT_Result gfnt_cffhint_create(const GFNT_Face * face, uint32_t ppem, const GFNT_Variation * variation,
    GFNT_CffHint ** out, GFNT_Error * error) {
  GFNT_CffHint * hint;
  uint16_t upem = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out || ppem == 0) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_cffhint_applies(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, 0, "only a CFF, CFF2 or Type 1 face is hinted by its stems");
  }
  if (variation && variation->count && !variation->coords) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, 0, "a variation with a count and no coordinates");
  }
  result = gfnt_face_units_per_em(face, &upem, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (upem == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, 0, "a face with no units per em cannot be scaled");
  }
  hint = face->allocator->calloc_fn(face->allocator->ctx, 1, sizeof *hint);
  if (!hint) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, 0, "no memory for the hinting context");
  }
  hint->allocator = face->allocator;
  hint->ppem = ppem;
  hint->upem = upem;
  hint->scale = gfnt_scale_for_ppem(upem, ppem);
  if (variation && variation->count) {
    hint->coords = face->allocator->calloc_fn(face->allocator->ctx, variation->count, sizeof *hint->coords);
    if (!hint->coords) {
      face->allocator->free_fn(face->allocator->ctx, hint);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, 0, "no memory for the hinting location");
    }
    memcpy(hint->coords, variation->coords, variation->count * sizeof *hint->coords);
    hint->variation = *variation;
    hint->variation.coords = hint->coords;
  }
  *out = hint;
  return GFNT_OK;
}

void gfnt_cffhint_destroy(GFNT_CffHint * hint) {
  const GFNT_Allocator * a;

  if (!hint) {
    return;
  }
  a = hint->allocator;
  if (hint->coords) {
    a->free_fn(a->ctx, hint->coords);
  }
  a->free_fn(a->ctx, hint);
}

/** A glyph's advance in whole pixels (26.6), as the unhinted advance rounded. */
static GFNT_Result gfnt_cffh_advance(const GFNT_Face * face, const GFNT_CffHint * hint, uint32_t glyph,
    int32_t * out, GFNT_Error * error) {
  int32_t units = 0;
  int64_t s26 = gfnt_cffh_div((int64_t)hint->ppem * 64, hint->upem);   // 26.6 pixels per unit, 16.16
  int64_t scaled;
  GFNT_Result result = gfnt_face_glyph_advance(face, glyph, hint->variation.count ? &hint->variation : NULL,
      &units, error);

  if (result != GFNT_OK && gfnt_sfnt_producer(face) == GFNT_PRODUCER_TYPE1) {
    GFNT_CharstringMetrics metrics;

    memset(&metrics, 0, sizeof metrics);
    result = gfnt_face_glyph_charstring_metrics(face, glyph, &metrics, error);
    units = (int32_t)(gfnt_cffh_round_px(metrics.width) >> 16);
  }
  if (result != GFNT_OK) {
    return result;
  }
  scaled = gfnt_cffh_mul(units, s26);                    // the unhinted advance, 26.6
  scaled = scaled >= 0 ? ((scaled + 32) / 64) * 64 : -(((-scaled + 31) / 64) * 64);
  *out = (int32_t)scaled;
  return GFNT_OK;
}

GFNT_Result gfnt_cffhint_glyph_outline(const GFNT_Face * face, const GFNT_CffHint * hint, uint32_t glyph,
    const GFNT_Allocator * allocator, GFNT_Outline ** out_outline, GFNT_HintStatus * out_status,
    int32_t * out_advance, GFNT_Error * error) {
  GFNT_CharstringHints hints;
  GFNT_CharstringHintParams params;
  GFNT_Outline * source = NULL;
  GFNT_Outline * result = NULL;
  const GFNT_Variation * variation;
  GFNT_Point * points = NULL;
  GFNT_PointTag * tags = NULL;
  int64_t * fine_x = NULL;
  int64_t * fine_y = NULL;
  size_t count;
  int64_t scale;
  int64_t boost = 0;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !hint || !out_outline || !out_status) {
    return GFNT_ERR_INVALID;
  }
  *out_outline = NULL;
  variation = hint->variation.count ? &hint->variation : NULL;
  scale = hint->scale;
  r = gfnt_face_glyph_outline(face, glyph, variation, NULL, &source, error);
  if (r != GFNT_OK) {
    return r;
  }
  count = gfnt_outline_point_count(source);
  memset(&hints, 0, sizeof hints);
  r = gfnt_face_glyph_hints(face, glyph, variation, &hints, &params, NULL);
  if (r != GFNT_OK || !hints.valid) {
    // A glyph whose hints cannot be read is drawn scaled and unfitted.
    r = gfnt_outline_scale(source, hint->scale, error);
    if (r == GFNT_OK) {
      result = source;
      source = NULL;
      *out_outline = result;
      *out_status = GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
      if (out_advance) {
        r = gfnt_cffh_advance(face, hint, glyph, out_advance, error);
        if (r != GFNT_OK) {
          *out_advance = 0;
          r = GFNT_OK;
        }
      }
    }
    gfnt_outline_destroy(source);
    return r;
  }
  {
    // The outward bias fades from 0.6 pixel to nothing at BlueScale.
    int64_t blue_scale = gfnt_cffh_effective_blue_scale(&params);

    if (blue_scale > 0 && scale < blue_scale) {
      int64_t remaining = 0x10000 - gfnt_cffh_div(scale, blue_scale);

      boost = (remaining * GFNT_CFFH_BOOST_NUM + GFNT_CFFH_BOOST_DEN / 2) / GFNT_CFFH_BOOST_DEN;
      if (boost > GFNT_CFFH_BOOST_MAX) {
        boost = GFNT_CFFH_BOOST_MAX;
      }
    }
  }
  points = face->allocator->calloc_fn(face->allocator->ctx, count ? count : 1u, sizeof *points);
  tags = face->allocator->calloc_fn(face->allocator->ctx, count ? count : 1u, sizeof *tags);
  fine_x = face->allocator->calloc_fn(face->allocator->ctx, count ? count : 1u, sizeof *fine_x);
  fine_y = face->allocator->calloc_fn(face->allocator->ctx, count ? count : 1u, sizeof *fine_y);
  if (!points || !tags || !fine_x || !fine_y) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, glyph, "no memory for a hinted outline");
    goto done;
  }
  {
    // The hint masks cut the outline into runs, each mapped through the stems its mask turns on.
    // Before the first mask (or when there are none) every stem declared is in force.
    struct {
      size_t start;
      size_t mask;    ///< Index of the mask, or GFNT_CS_MAX_MASKS for "all stems".
    } segments[GFNT_CS_MAX_MASKS + 1];
    size_t segment_count = 0;
    GFNT_CffhMap * map = face->allocator->calloc_fn(face->allocator->ctx, 1, sizeof *map);
    bool active[GFNT_CS_MAX_STEMS];

    if (!map) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, glyph, "no memory for a hint map");
      goto done;
    }
    if (hints.mask_count == 0 || hints.masks[0].first_point > 0) {
      segments[segment_count].start = 0;
      segments[segment_count].mask = GFNT_CS_MAX_MASKS;
      segment_count += 1u;
    }
    for (size_t k = 0; k < hints.mask_count; ++k) {
      segments[segment_count].start = hints.masks[k].first_point;
      segments[segment_count].mask = k;
      segment_count += 1u;
    }
    for (size_t seg = 0; seg < segment_count; ++seg) {
      size_t begin = segments[seg].start < count ? segments[seg].start : count;
      size_t end = seg + 1u < segment_count && segments[seg + 1u].start < count ? segments[seg + 1u].start : count;

      if (begin >= end) {
        continue;
      }
      for (size_t i = 0; i < GFNT_CS_MAX_STEMS; ++i) {
        active[i] = segments[seg].mask == GFNT_CS_MAX_MASKS
            ? true
            : i < hints.stem_count && (hints.masks[segments[seg].mask].bits[i / 8u] & (0x80u >> (i % 8u))) != 0;
      }
      gfnt_cffh_build(&hints, active, &params, scale, boost, map);
      for (size_t i = begin; i < end; ++i) {
        GFNT_Point p;
        GFNT_PointTag tag = GFNT_POINT_ON;

        if (gfnt_outline_point_at(source, i, &p, &tag) != GFNT_OK) {
          continue;
        }
        fine_y[i] = gfnt_cffh_map_point(map, (int64_t)p.y * 1024, scale);
        fine_x[i] = gfnt_cffh_mul((int64_t)p.x * 1024, scale);
        points[i].y = gfnt_cffh_to_26_6(fine_y[i]);
        points[i].x = gfnt_cffh_to_26_6(fine_x[i]);
        tags[i] = tag;
      }
    }
    face->allocator->free_fn(face->allocator->ctx, map);
  }
  r = gfnt_outline_create(allocator ? allocator : face->allocator, &result, error);
  // An accented character's accent is emitted before its base in a CFF (measured against FreeType; a Type 1
  // glyph keeps the base first), so two passes over the contours: the accent's, then the rest.
  const bool accent_first = hints.accent && gfnt_sfnt_producer(face) != GFNT_PRODUCER_TYPE1;
  const size_t contour_count = gfnt_outline_contour_count(source);

  for (size_t step = 0; r == GFNT_OK && step < (accent_first ? 2u * contour_count : contour_count); ++step) {
    size_t c = accent_first ? step % contour_count : step;
    size_t first = 0;
    size_t n = 0;

    r = gfnt_outline_contour_at(source, c, &first, &n);
    if (r != GFNT_OK) {
      break;
    }
    if (accent_first && (first >= hints.accent_first_point) != (step < contour_count)) {
      continue;
    }
    // A contour closed by a point equal to its first is not drawn twice, once the two are
    // equal *after* hinting, compared before the 26.6 truncation; before hinting, equal points can still part.
    if (n >= 2u && fine_x[first + n - 1u] == fine_x[first] && fine_y[first + n - 1u] == fine_y[first]
        && tags[first + n - 1u] == GFNT_POINT_ON && tags[first] == GFNT_POINT_ON) {
      n -= 1u;
    }
    r = gfnt_outline_begin_contour(result, error);
    for (size_t i = 0; r == GFNT_OK && i < n; ++i) {
      r = gfnt_outline_add_point(result, points[first + i], tags[first + i], error);
    }
  }
  if (r == GFNT_OK) {
    result->space = GFNT_OUTLINE_PIXELS;
    *out_outline = result;
    result = NULL;
    *out_status = GFNT_HINT_STATUS_HINTED;
    if (out_advance) {
      r = gfnt_cffh_advance(face, hint, glyph, out_advance, error);
      if (r != GFNT_OK) {
        *out_advance = 0;
        r = GFNT_OK;
      }
    }
  }
done:
  if (points) {
    face->allocator->free_fn(face->allocator->ctx, points);
  }
  if (tags) {
    face->allocator->free_fn(face->allocator->ctx, tags);
  }
  if (fine_x) {
    face->allocator->free_fn(face->allocator->ctx, fine_x);
  }
  if (fine_y) {
    face->allocator->free_fn(face->allocator->ctx, fine_y);
  }
  gfnt_outline_destroy(result);
  gfnt_outline_destroy(source);
  return r;
}
