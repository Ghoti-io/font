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
 * The automatic hinter, from the shapes of the face's own letters: the light target (y only) and, for
 * ::GFNT_HINT_MODE_AUTO_XY, the normal one (both axes, stem widths snapped).
 *
 * A face with no hint data of its own is fitted from its outlines. Recovered by experiment against
 * FreeType's light automatic hinter (tools/oracle/autohint_*.py, `autohint_edges_model.py` being this
 * file's prototype), in the order the code does it:
 *
 *   1. **Blue zones**, once per face: for the six zones (capital top and bottom, small f top, small
 *      top, small bottom, small descender) the extreme of each letter the zone is named for, flat or
 *      round, a reference from the flat ones and an overshoot from the round (the element at half
 *      the count). The **standard width** is the thinnest horizontal stem of an `o`.
 *   2. **Scale**, once per size: the x-height, the overshoot of the small-top zone, is rounded up from
 *      three eighths of a pixel and the whole outline's y scale is corrected to put it there (unless
 *      that would move the tallest zone by two pixels). A zone more than three quarters of a pixel
 *      tall is inactive; its overshoot is kept in steps of half a pixel.
 *   3. **Segments**: runs of points whose outgoing vectors are horizontal, linked across a stem to the
 *      opposite segment above or below that overlaps it, and **edges**: segments of one direction
 *      within a fifth of the standard width, with the link or serif of their segments.
 *   4. **Fitting**: an edge near a zone is aligned to its fitted position and its stem partner follows
 *      at the scaled width; the first remaining stem is the anchor and stems narrower than a pixel and
 *      a half are centred on a pixel's middle (or a little off it), the rest placed from the anchor;
 *      serifs follow their base, and what is left is interpolated or snapped to half pixels.
 *   5. **Points**: those on an edge take its position, the others between edges by their font-unit
 *      distance, and off-curve points and those in a straight run along their contour between the
 *      touched points.
 *
 * Everything is 26.6 and 16.16 integers as FreeType's, so that the rounding agrees. In the light target x is
 * scaled and not moved. Fitting both axes (FreeType's normal target), x is fitted by the same code on the outline
 * turned a quarter, with no zones, the uncorrected scale and the thinnest vertical stem of the standard letter; in
 * both axes a stem's width is snapped (af_snap_width) instead of kept, an edge with a fitted neighbour on one side
 * only moves with it in x, and the origin and advance follow the outermost x edges. Latin, Greek and Cyrillic have zones of their own, found from the face's letters of each;
 * a glyph of any other script, or of one the face has no letters of, is fitted with the Latin ones.
 */

#include "autofit.h"

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../sfnt/sfnt.h"
#include "autofit_scripts.h"

#define AF_ZONES 6
#define AF_DIR_UP 1
#define AF_DIR_DOWN (-1)
#define AF_DIR_LEFT (-2)
#define AF_DIR_RIGHT 2

typedef struct AfZone {
  int32_t ref;      ///< Font units.
  int32_t shoot;
  bool top;
  bool present;
  int32_t ref_fit;  ///< 26.6 at the size.
  int32_t shoot_fit;
  bool active;
} AfZone;

/** What is read from a face's letters of one script: its zones, standard stem width and corrected scale. */
typedef struct AfScript {
  bool present;       ///< The face has letters of the script to take the zones from.
  int32_t yscale;     ///< The scale with the x-height correction.
  int32_t stdw;       ///< The standard horizontal stem width, font units.
  int32_t xstdw;      ///< The standard vertical stem width, font units (for ::GFNT_HINT_MODE_AUTO_XY).
  AfZone zones[AF_ZONES];
} AfScript;

enum {
  AF_LATIN, AF_GREEK, AF_CYRILLIC, AF_HEBREW, AF_ARABIC, AF_GEORGIAN, AF_ARMENIAN,
#define AF_X_ENUM(name, standard, t0, t1, t2, t3, t4, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9) AF_GEN_##name,
  AF_GENERATED_SCRIPTS(AF_X_ENUM)
#undef AF_X_ENUM
  AF_SCRIPTS
};

struct GFNT_Autofit {
  const GFNT_Allocator * allocator;
  uint32_t ppem;
  uint16_t upem;
  GFNT_F2Dot14 * coords;
  GFNT_Variation variation;
  int32_t scale;      ///< 26.6 pixels per font unit, 16.16, uncorrected.
  bool xy;            ///< Fit x as well, and snap stem widths (::GFNT_HINT_MODE_AUTO_XY).
  AfScript scripts[AF_SCRIPTS];
  uint8_t * script_of;  ///< The script of each glyph; absent for a face with no cmap letters of another script.
  uint32_t glyph_count;
  uint8_t * nonbase;    ///< 1 for a glyph FreeType does not fit to the blue zones (a mark or an accent); absent when none.
};

/** The letters each zone is named for, per script, and the letter the standard stem width comes from. */
static const struct {
  const uint32_t * letters[AF_ZONES];
  uint32_t standard;
} af_blue_strings[AF_SCRIPTS] = {
  {{U"THEZOCQS", U"HEZLOCUS", U"fijkdbh", U"xzroesc", U"xzroesc", U"pqgjy"}, U'o'},
  {{U"ΓΒΕΖΘΟΩ", U"ΒΔΖΞΘΟ", U"βθδζλξ", U"οε", U"αεοσ", U"βγημρφχψ"}, U'ο'},
  {{U"БВЕПЗОСЭ", U"БВЕШЗОСЭ", U"", U"хпншезос", U"хпншезос", U"руф"}, U'о'},
  {{U"אבדהחכםרתמסצ", U"אבדהחכםרתמסצ", U"", U"", U"", U"ךןףץק"}, U'ו'},
  {{U"اكلطظ", U"ادذ", U"", U"", U"", U"جحخرزصعغ"}, U'ا'},
  {{U"", U"ბზთმნოპსშჩძწხჰ", U"ბზმნპსშჩძწხჰ", U"ვეთოფღ", U"", U"გდეკლფღყც"}, U'ო'},
  {{U"ԲԴԸԼԿՂՈՊՌՐՒԱԵԺՍՏՕ", U"ԲԴԸԼԿՂՈՊՌՐՒԱԵԺՍՏՕ", U"եէժիխկձհճմնվփ", U"ապոռսւօտ", U"ապոռսւօտ", U"բգդզըթլղշչպջրցքիխկվփ"}, U'ս'},
#define AF_X_ZONES(name, standard, t0, t1, t2, t3, t4, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9) \
  {{t0, t1, t2, t3, U"", t4}, standard},
  AF_GENERATED_SCRIPTS(AF_X_ZONES)
#undef AF_X_ZONES
};
static const bool af_zone_top[AF_ZONES] = {true, false, true, true, false, false};
enum { AF_CAP_TOP, AF_CAP_BOT, AF_F_TOP, AF_SMALL_TOP, AF_SMALL_BOT, AF_DESC };

/** FreeType's rounded multiply of an integer by a 16.16 value. */
static int64_t af_mulfix(int64_t a, int64_t b) {
  int64_t p = a * b;

  return p >= 0 ? (p + 0x8000) >> 16 : -((-p + 0x8000) >> 16);
}

/** a * b / c, rounded to nearest, half away from zero. */
static int64_t af_muldiv(int64_t a, int64_t b, int64_t c) {
  int64_t p = a * b;
  int64_t half = c / 2;

  return p >= 0 ? (p + half) / c : -((-p + half) / c);
}

/** FreeType's rounded 16.16 quotient, half away from zero. */
static int64_t af_divfix(int64_t a, int64_t b) {
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

static int64_t af_pixround(int64_t v) {
  return (v + 32) & ~(int64_t)63;
}

/** Half, rounded toward negative infinity. */
static int64_t af_half(int64_t v) {
  return v >= 0 ? v / 2 : -((-v + 1) / 2);
}

static int af_direction(int64_t dx, int64_t dy) {
  int64_t ax = dx < 0 ? -dx : dx;
  int64_t ay = dy < 0 ? -dy : dy;

  if (ax == 0 && ay == 0) {
    return 0;
  }
  if (ax > 14 * ay) {
    return dx > 0 ? AF_DIR_RIGHT : AF_DIR_LEFT;
  }
  if (ay > 14 * ax) {
    return dy > 0 ? AF_DIR_UP : AF_DIR_DOWN;
  }
  return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* One glyph's points, segments and edges                                                           */
/* ------------------------------------------------------------------------------------------------ */

typedef struct AfSeg {
  int dir;
  size_t first;      ///< Offset into the shared point list.
  size_t count;
  int64_t min_x;
  int64_t max_x;
  int64_t pos;
  bool round;
  bool top;
  ptrdiff_t link;
  ptrdiff_t serif;
  ptrdiff_t edge;
} AfSeg;

typedef struct AfEdge {
  int64_t fpos;
  int64_t opos;
  int64_t pos;
  bool top;
  bool round;
  bool done;
  bool has_blue;
  int64_t blue;
  ptrdiff_t link;
  ptrdiff_t serif;
} AfEdge;

typedef struct AfGlyph {
  size_t n;
  size_t contours;
  int64_t * x;
  int64_t * y;
  bool * on;
  int8_t * in_dir;
  int8_t * out_dir;
  size_t * far_prev;  ///< The nearest earlier point on the contour that is not near.
  size_t * far_next;  ///< The nearest later point that is not near.
  size_t * prev;
  size_t * next;
  size_t * first;      ///< First point of each contour.
  size_t * count;      ///< Points in it.
  bool clockwise;
  bool x_axis;         ///< The outline is turned a quarter and its "y" is x (::GFNT_HINT_MODE_AUTO_XY).
  /** With x_axis, each point's y as the light target fits it: an x segment shorter than a pixel by that measure is
   *  no segment, so the ends of a serif slab under a pixel tall move with the stem they flank and a small dot keeps
   *  its scaled place. Measured with a serif I of every slab height at 12, 16 and 24 px: the slab's ends are a stem
   *  exactly when its top is fitted 64/64 or more above its bottom (63 is not), whatever its scaled height; at the
   *  snapped (normal) height, or the scaled one, no single threshold fits all three sizes. */
  const int64_t * cross_fit;
  AfSeg * segs;
  size_t seg_count;
  size_t * seg_points;
  size_t seg_point_count;
  AfEdge * edges;
  size_t edge_count;
  int64_t * out_y;     ///< The hinted y, 26.6.
  bool * touched;
  void * block;
} AfGlyph;

static void af_glyph_free(const GFNT_Allocator * a, AfGlyph * g) {
  if (g && g->block) {
    a->free_fn(a->ctx, g->block);
    g->block = NULL;
  }
}

/** Read an outline into a glyph's point arrays, in font units. */
static GFNT_Result af_glyph_load(const GFNT_Allocator * a, const GFNT_Outline * outline, bool rotate, AfGlyph * g) {
  size_t n = gfnt_outline_point_count(outline);
  size_t contours = gfnt_outline_contour_count(outline);
  size_t cap = n ? n : 1u;
  size_t bytes;
  uint8_t * block;
  int64_t area = 0;

  memset(g, 0, sizeof *g);
  g->n = n;
  g->contours = contours;
  bytes = cap * (sizeof(int64_t) * 3u + sizeof(bool) * 2u + sizeof(int8_t) * 2u + sizeof(size_t) * 4u)
      + (contours ? contours : 1u) * sizeof(size_t) * 2u
      + cap * (sizeof(AfSeg) + sizeof(AfEdge)) + (cap * 2u + 4u) * sizeof(size_t) + 64u;
  block = a->calloc_fn(a->ctx, 1, bytes);
  if (!block) {
    return GFNT_ERR_OOM;
  }
  g->block = block;
#define AF_TAKE(field, type, count) \
  do { g->field = (type *)(void *)block; block += (count) * sizeof(type); } while (0)
  /* Largest alignment first. */
  AF_TAKE(segs, AfSeg, cap);
  AF_TAKE(edges, AfEdge, cap);
  AF_TAKE(x, int64_t, cap);
  AF_TAKE(y, int64_t, cap);
  AF_TAKE(out_y, int64_t, cap);
  AF_TAKE(prev, size_t, cap);
  AF_TAKE(far_prev, size_t, cap);
  AF_TAKE(far_next, size_t, cap);
  AF_TAKE(next, size_t, cap);
  AF_TAKE(first, size_t, contours ? contours : 1u);
  AF_TAKE(count, size_t, contours ? contours : 1u);
  AF_TAKE(seg_points, size_t, cap * 2u + 4u);
  AF_TAKE(on, bool, cap);
  AF_TAKE(touched, bool, cap);
  AF_TAKE(in_dir, int8_t, cap);
  AF_TAKE(out_dir, int8_t, cap);
#undef AF_TAKE
  for (size_t c = 0; c < contours; ++c) {
    size_t f = 0;
    size_t len = 0;

    gfnt_outline_contour_at(outline, c, &f, &len);
    g->first[c] = f;
    g->count[c] = len;
    for (size_t i = f; i < f + len; ++i) {
      g->next[i] = i == f + len - 1u ? f : i + 1u;
      g->prev[i] = i == f ? f + len - 1u : i - 1u;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    GFNT_Point p;
    GFNT_PointTag tag = GFNT_POINT_ON;

    gfnt_outline_point_at(outline, i, &p, &tag);
    // The outline is 26.6 of a font unit; the hinter works in whole units.
    g->x[i] = ((int64_t)p.x + 32) >> 6;
    g->y[i] = ((int64_t)p.y + 32) >> 6;
    if (rotate) {
      // A quarter turn, which keeps the contours' orientation: the y code then fits what was x.
      int64_t t = g->x[i];

      g->x[i] = -g->y[i];
      g->y[i] = t;
    }
    g->on[i] = tag == GFNT_POINT_ON;
  }
  for (size_t i = 0; i < n; ++i) {
    size_t j = g->next[i];
    size_t k = g->prev[i];

    g->out_dir[i] = (int8_t)af_direction(g->x[j] - g->x[i], g->y[j] - g->y[i]);
    g->in_dir[i] = (int8_t)af_direction(g->x[i] - g->x[k], g->y[i] - g->y[k]);
    area += g->x[i] * g->y[j] - g->x[j] * g->y[i];
  }
  g->clockwise = area < 0;
  return GFNT_OK;
}

static bool af_horizontal(int d) {
  return d == AF_DIR_LEFT || d == AF_DIR_RIGHT;
}

/** A flat or round run of points shorter than this fraction of an em (1/AF_MIN_SEGMENT_DIV) is not a segment: it is a tip or a hook,
 *  and the pixel-grid fitting of its neighbours is what places it. Measured against FreeType's answers: every set improves up to
 *  about 1/100 of an em and stays level to 1/120. */
#define AF_MIN_SEGMENT_DIV 100

/** Horizontal segments: maximal runs of consecutive points whose outgoing vector has one horizontal direction.
 *  Runs shorter than a hundredth of an em, `up` units per em, are left out. */
/* Two points are near each other when neither coordinate differs by more than this many 2048ths of an em. The
 * direction of a vector that ends on a near point is taken to the point after it, and a flat run goes across them. */
#define AF_NEAR_NUM 16
#define AF_NEAR_DEN 2048

/** Whether a vector is shorter than the near limit along both axes. */
static bool af_near(int64_t dx, int64_t dy, int64_t up) {
  int64_t ax = dx < 0 ? -dx : dx;
  int64_t ay = dy < 0 ? -dy : dy;

  return (ax > ay ? ax : ay) * AF_NEAR_DEN < up * AF_NEAR_NUM;
}

/** The first point after p, along its contour, that is not near it; the next one when every other point is. */
static size_t af_reach(const AfGlyph * g, size_t p, int64_t up) {
  size_t j = g->next[p];
  size_t guard = 0;

  while (j != p && guard++ < g->n) {
    int64_t dx = g->x[j] - g->x[p];
    int64_t dy = g->y[j] - g->y[p];

    if (!af_near(dx, dy, up)) {
      return j;
    }
    j = g->next[j];
  }
  return g->next[p];
}

/** Directions seen past points that sit very close to each other: a point's vectors reach the nearest point that is not near. */
static void af_near_directions(AfGlyph * g, int64_t up) {
  for (size_t i = 0; i < g->n; ++i) {
    size_t j = af_reach(g, i, up);
    size_t k = g->prev[i];
    size_t guard = 0;

    while (k != i && guard++ < g->n) {
      if (!af_near(g->x[i] - g->x[k], g->y[i] - g->y[k], up)) {
        break;
      }
      k = g->prev[k];
    }
    g->far_next[i] = j;
    g->far_prev[i] = k;
    g->out_dir[i] = (int8_t)af_direction(g->x[j] - g->x[i], g->y[j] - g->y[i]);
    g->in_dir[i] = (int8_t)af_direction(g->x[i] - g->x[k], g->y[i] - g->y[k]);
  }
}

/** The extent of a segment's points along the other axis, as AfGlyph::cross_fit measures them. */
static int64_t af_cross_length(const AfGlyph * g, const AfSeg * s) {
  int64_t lo = INT64_MAX;
  int64_t hi = INT64_MIN;

  for (size_t k = 0; k < s->count; ++k) {
    int64_t v = g->cross_fit[g->seg_points[s->first + k]];

    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
  }
  return hi - lo;
}

static void af_segments(AfGlyph * g, int64_t up) {
  af_near_directions(g, up);
  g->seg_count = 0;
  g->seg_point_count = 0;
  for (size_t c = 0; c < g->contours; ++c) {
    size_t start = g->first[c];
    size_t n = g->count[c];
    size_t begin = n;
    size_t count = 0;

    for (size_t k = 0; k < n; ++k) {
      size_t i = start + k;

      if (af_horizontal(g->out_dir[i]) && g->out_dir[g->prev[i]] != g->out_dir[i]) {
        begin = k;
        break;
      }
    }
    if (n == 0 || begin == n) {
      continue;
    }
    while (count < n) {
      size_t i = start + (begin + count) % n;

      if (af_horizontal(g->out_dir[i])) {
        int d = g->out_dir[i];
        AfSeg * s = &g->segs[g->seg_count];
        size_t m = count + 1u;
        int64_t min_y;
        int64_t max_y;
        int64_t min_x;
        int64_t max_x;

        size_t at = i;

        s->first = g->seg_point_count;
        g->seg_points[g->seg_point_count++] = i;
        for (;;) {
          size_t reach = af_reach(g, at, up);

          while (at != reach && m < n) {
            at = g->next[at];
            g->seg_points[g->seg_point_count++] = at;
            ++m;
          }
          if (m >= n || g->out_dir[at] != d) {
            break;
          }
        }
        s->count = g->seg_point_count - s->first;
        min_y = max_y = g->y[g->seg_points[s->first]];
        min_x = max_x = g->x[g->seg_points[s->first]];
        for (size_t k = 1; k < s->count; ++k) {
          size_t p = g->seg_points[s->first + k];

          min_y = g->y[p] < min_y ? g->y[p] : min_y;
          max_y = g->y[p] > max_y ? g->y[p] : max_y;
          min_x = g->x[p] < min_x ? g->x[p] : min_x;
          max_x = g->x[p] > max_x ? g->x[p] : max_x;
        }
        if ((max_x - min_x) * AF_MIN_SEGMENT_DIV < up || (g->cross_fit && af_cross_length(g, s) < 64)) {
          g->seg_point_count = s->first;
          count = m;
          continue;
        }
        s->dir = d;
        s->pos = af_half(min_y + max_y);
        s->min_x = min_x;
        s->max_x = max_x;
        s->round = !g->on[g->seg_points[s->first]] || !g->on[g->seg_points[s->first + s->count - 1u]];
        s->top = (d == AF_DIR_RIGHT) == g->clockwise;
        s->link = -1;
        s->serif = -1;
        s->edge = -1;
        g->seg_count += 1u;
        count = m;
      }
      else {
        count += 1u;
      }
    }
  }
}

/** The score of a pair of segments that could be a stem: lower is better, in 16.16. */
static int64_t af_link_score(int64_t dist, int64_t overlap, int64_t up) {
  return ((dist * 3000) << 16) / up + ((6000 * up) << 16) / (overlap * 2048);
}

static void af_link_segments(AfGlyph * g, int64_t up) {
  for (size_t i = 0; i < g->seg_count; ++i) {
    g->segs[i].link = -1;
    g->segs[i].serif = -1;
  }
  for (int pass = 0; pass < 2; ++pass) {
    // Pass 0: from each bottom edge up to a top edge; pass 1: from each top edge down to a bottom one.
    for (size_t i = 0; i < g->seg_count; ++i) {
      AfSeg * a = &g->segs[i];
      ptrdiff_t best = -1;
      int64_t best_score = 0;

      if ((pass == 0) == a->top) {
        continue;
      }
      for (size_t j = 0; j < g->seg_count; ++j) {
        const AfSeg * b = &g->segs[j];
        int64_t lo;
        int64_t hi;
        int64_t overlap;
        int64_t dist;
        int64_t score;

        if (pass == 0 ? (!b->top || b->pos <= a->pos) : (b->top || b->pos >= a->pos)) {
          continue;
        }
        lo = a->min_x > b->min_x ? a->min_x : b->min_x;
        hi = a->max_x < b->max_x ? a->max_x : b->max_x;
        overlap = hi - lo;
        if (overlap * 2048 < 12 * up) {
          continue;
        }
        dist = pass == 0 ? b->pos - a->pos : a->pos - b->pos;
        score = af_link_score(dist, overlap, up);
        if (best < 0 || score < best_score) {
          best = (ptrdiff_t)j;
          best_score = score;
        }
      }
      a->link = best;
    }
  }
  for (size_t i = 0; i < g->seg_count; ++i) {
    AfSeg * s = &g->segs[i];

    if (s->link >= 0 && g->segs[s->link].link != (ptrdiff_t)i) {
      s->serif = s->link;
      s->link = -1;
    }
  }
}

/** Group segments into edges. @return the number of edges. */
static void af_edges(AfGlyph * g, int64_t yscale, int64_t stdw) {
  int64_t threshold = af_mulfix(stdw / 5, yscale);

  if (threshold > 16) {
    threshold = 16;
  }
  g->edge_count = 0;
  for (size_t i = 0; i < g->seg_count; ++i) {
    AfSeg * s = &g->segs[i];
    ptrdiff_t found = -1;

    for (size_t e = 0; e < g->edge_count; ++e) {
      int64_t d = af_mulfix(g->edges[e].fpos - s->pos, yscale);

      if (g->edges[e].top == s->top && (d < 0 ? -d : d) < threshold) {
        found = (ptrdiff_t)e;
        break;
      }
    }
    if (found >= 0) {
      s->edge = found;
      continue;
    }
    {
      // Insert in order of position, after any edge at the same one.
      size_t at = g->edge_count;

      while (at > 0 && g->edges[at - 1u].fpos > s->pos) {
        --at;
      }
      memmove(&g->edges[at + 1u], &g->edges[at], (g->edge_count - at) * sizeof g->edges[0]);
      for (size_t k = 0; k < i; ++k) {
        if (g->segs[k].edge >= (ptrdiff_t)at) {
          g->segs[k].edge += 1;
        }
      }
      memset(&g->edges[at], 0, sizeof g->edges[0]);
      g->edges[at].fpos = s->pos;
      g->edges[at].top = s->top;
      g->edges[at].link = -1;
      g->edges[at].serif = -1;
      s->edge = (ptrdiff_t)at;
      g->edge_count += 1u;
    }
  }
  for (size_t e = 0; e < g->edge_count; ++e) {
    size_t rounds = 0;
    size_t total = 0;
    AfEdge * ed = &g->edges[e];

    for (size_t i = 0; i < g->seg_count; ++i) {
      if (g->segs[i].edge == (ptrdiff_t)e) {
        total += 1u;
        rounds += g->segs[i].round ? 1u : 0u;
      }
    }
    ed->round = rounds > 0 && rounds >= total - rounds;
    for (size_t i = 0; i < g->seg_count; ++i) {
      const AfSeg * s = &g->segs[i];
      bool serif;
      const AfSeg * other;
      ptrdiff_t edge2;

      if (s->edge != (ptrdiff_t)e) {
        continue;
      }
      serif = s->serif >= 0 && g->segs[s->serif].edge >= 0 && g->segs[s->serif].edge != (ptrdiff_t)e;
      if (!((s->link >= 0 && g->segs[s->link].edge >= 0) || serif)) {
        continue;
      }
      other = &g->segs[serif ? s->serif : s->link];
      edge2 = serif ? ed->serif : ed->link;
      if (edge2 >= 0) {
        int64_t sd = s->pos - other->pos;
        int64_t ed2 = ed->fpos - g->edges[edge2].fpos;

        if ((sd < 0 ? -sd : sd) < (ed2 < 0 ? -ed2 : ed2)) {
          edge2 = other->edge;
        }
      }
      else {
        edge2 = other->edge;
      }
      if (serif) {
        ed->serif = edge2;
      }
      else {
        ed->link = edge2;
      }
    }
    if (ed->serif >= 0 && ed->link >= 0) {
      ed->serif = -1;
    }
  }
}

/** The length of a vector, an integer square root. */
static int64_t af_hypot(int64_t x, int64_t y) {
  uint64_t v = (uint64_t)(x * x + y * y);
  uint64_t r = 0;
  uint64_t bit = (uint64_t)1 << 62;

  while (bit > v) {
    bit >>= 2;
  }
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    }
    else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (int64_t)r;
}

/** Off the curve, or on it with the same direction in and out. */
static bool af_weak(const AfGlyph * g, size_t i) {
  if (!g->on[i]) {
    return true;
  }
  if (g->in_dir[i] == g->out_dir[i] && g->out_dir[i] != 0) {
    return true;
  }
  if (g->in_dir[i] == 0 && g->out_dir[i] == 0) {
    // The first point of a contour has no incoming vector for FreeType's flat test, so a corner there is
    // always flat: a weak point, however sharp it is (the apex of the counter of an A that starts a contour).
    for (size_t c = 0; c < g->contours; ++c) {
      if (g->first[c] == i && g->count[c] > 0) {
        return true;
      }
    }
    // Two diagonals: a corner too slight to matter is weak, FreeType's "flat" test.
    size_t p = g->far_prev[i];
    size_t n = g->far_next[i];
    int64_t ix = g->x[i] - g->x[p];
    int64_t iy = g->y[i] - g->y[p];
    int64_t ox = g->x[n] - g->x[i];
    int64_t oy = g->y[n] - g->y[i];
    int64_t sx = ix + ox;
    int64_t sy = iy + oy;
    int64_t li;
    int64_t lo;
    int64_t lt;

    li = af_hypot(ix, iy);
    lo = af_hypot(ox, oy);
    lt = af_hypot(sx, sy);
    return li + lo - lt < (lt >> 4);
  }
  return false;
}

/* ------------------------------------------------------------------------------------------------ */
/* The face's zones and the scale                                                                   */
/* ------------------------------------------------------------------------------------------------ */

static int af_compare_int(const void * a, const void * b) {
  const int32_t * pa = a;
  const int32_t * pb = b;
  int32_t x = pa[0];
  int32_t y = pb[0];

  return x < y ? -1 : x > y ? 1 : 0;
}

/** The extreme of a letter, in font units, and whether it is round. false when the face has no such glyph. */
static bool af_letter_extreme(const GFNT_Face * face, const GFNT_Variation * variation, uint32_t cp, bool top,
    int32_t * out_y, bool * out_round) {
  uint32_t glyph = 0;
  GFNT_Outline * outline = NULL;
  AfGlyph g;
  const GFNT_Allocator * a = face->allocator;
  bool found = false;

  if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK || glyph == 0) {
    return false;
  }
  if (gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline, NULL) != GFNT_OK) {
    return false;
  }
  if (af_glyph_load(a, outline, false, &g) == GFNT_OK && g.n > 0) {
    size_t best = 0;

    for (size_t i = 1; i < g.n; ++i) {
      if (top ? g.y[i] > g.y[best] : g.y[i] < g.y[best]) {
        best = i;
      }
    }
    *out_y = (int32_t)g.y[best];
    *out_round = !g.on[g.prev[best]] || !g.on[g.next[best]];
    found = true;
  }
  af_glyph_free(a, &g);
  gfnt_outline_destroy(outline);
  return found;
}

/** The thinnest horizontal stem (or, with @p rotate, vertical stem) of a script's standard letter (`o` in Latin), font
 *  units; 0 when the face has none. */
static int32_t af_standard_width(const GFNT_Face * face, const GFNT_Variation * variation, int64_t up, uint32_t cp,
    bool rotate) {
  uint32_t glyph = 0;
  GFNT_Outline * outline = NULL;
  AfGlyph g;
  int32_t best = 0;

  if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK || glyph == 0
      || gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline, NULL) != GFNT_OK) {
    return 0;
  }
  if (af_glyph_load(face->allocator, outline, rotate, &g) == GFNT_OK) {
    af_segments(&g, up);
    af_link_segments(&g, up);
    for (size_t i = 0; i < g.seg_count; ++i) {
      if (g.segs[i].link >= 0) {
        int64_t d = g.segs[i].pos - g.segs[g.segs[i].link].pos;

        d = d < 0 ? -d : d;
        if (best == 0 || d < best) {
          best = (int32_t)d;
        }
      }
    }
  }
  af_glyph_free(face->allocator, &g);
  gfnt_outline_destroy(outline);
  return best;
}

/** A zone's fits at a scale: the reference on a pixel and the overshoot in steps of half a pixel. */
static void af_zone_fit(AfZone * z, int64_t yscale) {
  int64_t dist = af_mulfix((int64_t)z->ref - z->shoot, yscale);
  int64_t delta2;

  z->active = dist <= 48 && dist >= -48;
  if (!z->active) {
    return;
  }
  delta2 = af_mulfix((int64_t)z->shoot - z->ref, yscale);
  delta2 = delta2 < 0 ? -delta2 : delta2;
  if (delta2 < 32) {
    delta2 = 0;
  }
  else if (delta2 < 64) {
    delta2 = 32 + (((delta2 - 32) + 16) & ~(int64_t)31);
  }
  else {
    delta2 = af_pixround(delta2);
  }
  z->ref_fit = (int32_t)af_pixround(af_mulfix(z->ref, yscale));
  z->shoot_fit = (int32_t)(z->shoot > z->ref ? z->ref_fit + delta2 : z->ref_fit - delta2);
}

bool gfnt_autofit_applies(const GFNT_Face * face) {
  int p = face ? (int)gfnt_sfnt_producer(face) : (int)GFNT_PRODUCER_NONE;

  return p == GFNT_PRODUCER_GLYF || p == GFNT_PRODUCER_CFF || p == GFNT_PRODUCER_CFF2 || p == GFNT_PRODUCER_TYPE1;
}

/** One script's zones, standard stem width and corrected scale, from the face's letters of it. */
static void af_script_build(const GFNT_Face * face, GFNT_Autofit * af, int sc, int64_t scale) {
  AfScript * script = &af->scripts[sc];
  const GFNT_Variation * variation = af->variation.count ? &af->variation : NULL;
  int64_t scaled;
  int64_t fitted;

  script->yscale = (int32_t)scale;
  for (int z = 0; z < AF_ZONES; ++z) {
    int32_t flats[64];
    int32_t rounds[64];
    size_t nf = 0;
    size_t nr = 0;

    for (const uint32_t * c = af_blue_strings[sc].letters[z]; *c; ++c) {
      int32_t y = 0;
      bool is_round = false;

      if (!af_letter_extreme(face, variation, *c, af_zone_top[z], &y, &is_round)) {
        continue;
      }
      if (is_round && nr < 64u) {
        rounds[nr++] = y;
      }
      else if (!is_round && nf < 64u) {
        flats[nf++] = y;
      }
    }
    script->zones[z].top = af_zone_top[z];
    if (nf == 0 && nr == 0) {
      continue;
    }
    qsort(flats, nf, sizeof flats[0], af_compare_int);
    qsort(rounds, nr, sizeof rounds[0], af_compare_int);
    script->zones[z].present = true;
    script->present = true;
    script->zones[z].ref = nf ? flats[nf / 2u] : rounds[nr / 2u];
    script->zones[z].shoot = nr ? rounds[nr / 2u] : flats[nf / 2u];
    // An overshoot on the wrong side of its flat (a round top below the flat ones) is no overshoot: both
    // go to the middle.
    if (script->zones[z].shoot != script->zones[z].ref
        && ((script->zones[z].shoot > script->zones[z].ref) != af_zone_top[z])) {
      script->zones[z].ref = script->zones[z].shoot = (int32_t)(((int64_t)script->zones[z].ref + script->zones[z].shoot) / 2);
    }
  }
  script->stdw = af_standard_width(face, variation, af->upem, af_blue_strings[sc].standard, false);
  if (script->stdw == 0) {
    script->stdw = 50 * af->upem / 2048;
  }
  script->xstdw = af_standard_width(face, variation, af->upem, af_blue_strings[sc].standard, true);
  if (script->xstdw == 0) {
    script->xstdw = 50 * af->upem / 2048;
  }
  if (script->zones[AF_SMALL_TOP].present) {
    int64_t tallest = 0;

    scaled = af_mulfix(script->zones[AF_SMALL_TOP].shoot, scale);
    fitted = (scaled + 40) & ~(int64_t)63;
    if (scaled != 0 && scaled != fitted) {
      int64_t corrected = af_muldiv(scale, fitted, scaled);

      for (int z = 0; z < AF_ZONES; ++z) {
        int64_t r = script->zones[z].ref < 0 ? -(int64_t)script->zones[z].ref : script->zones[z].ref;
        int64_t s = script->zones[z].shoot < 0 ? -(int64_t)script->zones[z].shoot : script->zones[z].shoot;

        if (script->zones[z].present) {
          tallest = r > tallest ? r : tallest;
          tallest = s > tallest ? s : tallest;
        }
      }
      {
        int64_t moved = af_mulfix(tallest, corrected) - af_mulfix(tallest, scale);

        if ((moved < 0 ? -moved : moved) < 128) {
          script->yscale = (int32_t)corrected;
        }
      }
    }
  }
  for (int z = 0; z < AF_ZONES; ++z) {
    if (script->zones[z].present) {
      af_zone_fit(&script->zones[z], script->yscale);
    }
  }
}

/** The code points a script's letters are looked up from, as inclusive pairs; Latin is the default. */
static const uint32_t af_ranges[AF_SCRIPTS][10] = {
  {0x20, 0x7E, 0xA0, 0x24F, 0x1E00, 0x1EFF, 0x2C60, 0x2C7F, 0xA720, 0xA7FF},
  {0x370, 0x3FF, 0x1F00, 0x1FFF, 0, 0, 0, 0, 0, 0},
  {0x400, 0x52F, 0x1C80, 0x1C8F, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0, 0},
  {0x590, 0x5FF, 0xFB1D, 0xFB4F, 0, 0, 0, 0, 0, 0},
  {0x600, 0x6FF, 0x750, 0x77F, 0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0, 0},
  {0x10D0, 0x10FF, 0x1C90, 0x1CBF, 0x2D00, 0x2D2F, 0, 0, 0, 0},
  {0x530, 0x58F, 0xFB13, 0xFB17, 0, 0, 0, 0, 0, 0},
#define AF_X_RANGES(name, standard, t0, t1, t2, t3, t4, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9) \
  {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9},
  AF_GENERATED_SCRIPTS(AF_X_RANGES)
#undef AF_X_RANGES
};

/** Which glyphs belong to which script: the first script whose range reaches the glyph through the cmap. */
static GFNT_Result af_script_map(const GFNT_Face * face, GFNT_Autofit * af) {
  size_t count = 0;
  const GFNT_Allocator * a = face->allocator;

  bool any = false;

  for (int sc = AF_GREEK; sc < AF_SCRIPTS; ++sc) {
    any = any || af->scripts[sc].present;
  }
  if (!any) {
    return GFNT_OK;
  }
  if (gfnt_face_num_glyphs(face, &count, NULL) != GFNT_OK || count == 0) {
    return GFNT_OK;
  }
  af->script_of = a->calloc_fn(a->ctx, count, 1);
  if (!af->script_of) {
    return GFNT_ERR_OOM;
  }
  memset(af->script_of, 0xFF, count);
  af->glyph_count = (uint32_t)count;
  for (int sc = 0; sc < AF_SCRIPTS; ++sc) {
    for (size_t r = 0; r + 1 < 10u; r += 2) {
      for (uint32_t cp = af_ranges[sc][r]; af_ranges[sc][r] && cp <= af_ranges[sc][r + 1]; ++cp) {
        uint32_t glyph = 0;

        if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) == GFNT_OK && glyph != 0 && glyph < count
            && af->script_of[glyph] == 0xFF) {
          af->script_of[glyph] = (uint8_t)sc;
        }
      }
    }
  }
  return GFNT_OK;
}

/**
 * The code points of glyphs that are marks or accents and not letters. FreeType fits them without the blue zones:
 * a macron, a dieresis, a tilde or a cedilla sits where its own stem rule puts it, and not on the ascender or the
 * descender the nearest zone would give it. Found by measurement, over the real faces, with the zones taken off
 * each range in turn: the spacing and combining marks of the Latin script, the ASCII circumflex, low line and grave,
 * and the Latin-1 marks that look like them, the Greek tonos and dialytika tonos and ypogegrammeni, and the
 * combining marks of Cyrillic. The superscript digits and the ordinal indicators are letters.
 */
static const uint32_t af_nonbase_ranges[][2] = {
  {0x5E, 0x60}, {0x7E, 0x7E}, {0xA8, 0xA9}, {0xAE, 0xB0}, {0xB4, 0xB4}, {0xB7, 0xB9}, {0xBC, 0xBE},
  {0x2B9, 0x2DF}, {0x2E5, 0x2FF}, {0x300, 0x36F}, {0x37A, 0x37A}, {0x384, 0x385}, {0x483, 0x489}, {0x1AB0, 0x1AFF},
  {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE20, 0xFE2F},
};

/** Which glyphs of the face are marks, through its cmap. */
static GFNT_Result af_nonbase_map(const GFNT_Face * face, GFNT_Autofit * af) {
  size_t count = 0;
  const GFNT_Allocator * a = face->allocator;

  if (gfnt_face_num_glyphs(face, &count, NULL) != GFNT_OK || count == 0) {
    return GFNT_OK;
  }
  af->nonbase = a->calloc_fn(a->ctx, count, 1);
  if (!af->nonbase) {
    return GFNT_ERR_OOM;
  }
  af->glyph_count = (uint32_t)count;
  for (size_t r = 0; r < sizeof af_nonbase_ranges / sizeof af_nonbase_ranges[0]; ++r) {
    for (uint32_t cp = af_nonbase_ranges[r][0]; cp <= af_nonbase_ranges[r][1]; ++cp) {
      uint32_t glyph = 0;

      if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) == GFNT_OK && glyph != 0 && glyph < count) {
        af->nonbase[glyph] = 1;
      }
    }
  }
  return GFNT_OK;
}

/** The script a glyph is fitted with: its own when the face has that script's letters, else Latin. */
static const AfScript * af_script_of(const GFNT_Autofit * af, uint32_t glyph) {
  if (af->script_of && glyph < af->glyph_count && af->script_of[glyph] < AF_SCRIPTS
      && af->scripts[af->script_of[glyph]].present) {
    return &af->scripts[af->script_of[glyph]];
  }
  return &af->scripts[AF_LATIN];
}

GFNT_Result gfnt_autofit_create(const GFNT_Face * face, uint32_t ppem, const GFNT_Variation * variation, bool xy,
    GFNT_Autofit ** out, GFNT_Error * error) {
  GFNT_Autofit * af;
  const GFNT_Allocator * a;
  uint16_t upem = 0;
  int64_t scale;

  gfnt_error_clear(error);
  if (!face || !out) {
    return GFNT_ERR_INVALID;
  }
  *out = NULL;
  if (!gfnt_autofit_applies(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, 0, "the automatic hinter fits outline faces");
  }
  if (gfnt_face_units_per_em(face, &upem, error) != GFNT_OK || upem == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, 0, "a face with no em");
  }
  a = face->allocator;
  af = a->calloc_fn(a->ctx, 1, sizeof *af);
  if (!af) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, 0, "no memory for the automatic hinter");
  }
  af->allocator = a;
  af->ppem = ppem;
  af->upem = upem;
  af->xy = xy;
  if (variation && variation->count) {
    af->coords = a->calloc_fn(a->ctx, variation->count, sizeof *af->coords);
    if (!af->coords) {
      a->free_fn(a->ctx, af);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, 0, "no memory for the automatic hinter");
    }
    memcpy(af->coords, variation->coords, variation->count * sizeof *af->coords);
    af->variation = *variation;
    af->variation.coords = af->coords;
  }
  scale = (((int64_t)ppem * 64 << 16) + upem / 2) / upem;
  af->scale = (int32_t)scale;
  for (int sc = 0; sc < AF_SCRIPTS; ++sc) {
    af_script_build(face, af, sc, scale);
  }
  if (af_script_map(face, af) != GFNT_OK || af_nonbase_map(face, af) != GFNT_OK) {
    gfnt_autofit_destroy(af);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, 0, "no memory for the automatic hinter");
  }
  *out = af;
  return GFNT_OK;
}

void gfnt_autofit_destroy(GFNT_Autofit * af) {
  if (!af) {
    return;
  }
  if (af->coords) {
    af->allocator->free_fn(af->allocator->ctx, af->coords);
  }
  if (af->script_of) {
    af->allocator->free_fn(af->allocator->ctx, af->script_of);
  }
  if (af->nonbase) {
    af->allocator->free_fn(af->allocator->ctx, af->nonbase);
  }
  af->allocator->free_fn(af->allocator->ctx, af);
}

/* ------------------------------------------------------------------------------------------------ */
/* Fitting one glyph                                                                                */
/* ------------------------------------------------------------------------------------------------ */

/** The centre of a stem under a pixel and a half wide: a pixel's middle, or a little off it. */
static int64_t af_stem_center(int64_t org_center, int64_t cur_len) {
  int64_t u = cur_len <= 64 ? 32 : 38;
  int64_t d = cur_len <= 64 ? 32 : 26;
  int64_t p1 = af_pixround(org_center);
  int64_t e1 = org_center - (p1 - u);
  int64_t e2 = org_center - (p1 + d);

  e1 = e1 < 0 ? -e1 : e1;
  e2 = e2 < 0 ? -e2 : e2;
  return e1 < e2 ? p1 - u : p1 + d;
}

/**
 * A stem's fitted width, 26.6, in ::GFNT_HINT_MODE_AUTO_XY (the light target keeps the scaled width): at least 56;
 * the standard width when within 40 of it (and then at least 48); under three pixels, the fraction of a pixel kept
 * below 10, made 10 below a half, 54 below 54 and kept above; from three pixels, a whole number of pixels.
 * Measured with one bar against a range of standard widths and sizes (`autohint_probe.py`, `xy_width`).
 */
static int64_t af_snap_width(int64_t dist, int64_t standard) {
  int64_t delta;
  int64_t frac;

  if (dist < 56) {
    dist = 56;
  }
  delta = dist - standard;
  if ((delta < 0 ? -delta : delta) < 40) {
    return standard < 48 ? 48 : standard;
  }
  if (dist >= 3 * 64) {
    return af_pixround(dist);
  }
  frac = dist & 63;
  dist &= ~(int64_t)63;
  if (frac < 10) {
    return dist + frac;
  }
  if (frac < 32) {
    return dist + 10;
  }
  if (frac < 54) {
    return dist + 54;
  }
  return dist + frac;
}

static void af_clamp(AfGlyph * g, size_t k) {
  AfEdge * e = &g->edges[k];

  if (k > 0 && g->edges[k - 1u].done && e->pos < g->edges[k - 1u].pos) {
    e->pos = g->edges[k - 1u].pos;
  }
}

/**
 * Whether a stem placed from @p base keeps its scaled width in y when widths are snapped: when an edge above the base
 * links one way to it (a serif lands on it from above, as the stem top of an L on its bar or the apex of an A on its
 * crossbar). A serif from below onto the base (a Γ), or from above onto the other edge (a top bar with a stem rising
 * from it), leaves the snap in place, and in x no serif does (either sense loses glyphs). Found with
 * `autohint_probe.py` shapes; the width is then kept under three pixels and rounded above.
 */
static bool af_keeps_width(const AfGlyph * g, const AfEdge * base) {
  ptrdiff_t k = base - g->edges;

  if (g->x_axis) {
    return false;
  }
  for (size_t f = 0; f < g->edge_count; ++f) {
    if (g->edges[f].serif == k && g->edges[f].fpos > base->fpos) {
      return true;
    }
  }
  return false;
}

static int64_t af_fitted_width(const AfGlyph * g, const AfEdge * base, int64_t dist, int64_t standard) {
  if (af_keeps_width(g, base)) {
    return dist < 3 * 64 ? dist : af_pixround(dist);
  }
  return af_snap_width(dist, standard);
}

/** Place @p stem from its linked @p base: at the scaled distance, or with @p snap at its fitted width. */
static void af_align_linked(const AfGlyph * g, AfEdge * base, AfEdge * stem, bool snap, int64_t standard) {
  int64_t dist = stem->opos - base->opos;

  if (snap) {
    dist = dist < 0 ? -af_fitted_width(g, base, -dist, standard) : af_fitted_width(g, base, dist, standard);
  }
  stem->pos = base->pos + dist;
  stem->done = true;
}

static void af_fit_edges(AfGlyph * g, const GFNT_Autofit * af, const AfScript * script, bool nonbase, bool snap,
    int64_t standard, bool x_axis) {
  int64_t yscale = script->yscale;

  // A face whose standard width is under 40/64 of a pixel at the size is fitted with its stems' own widths, as the
  // light target fits them: measured over ten faces at six sizes, where the scaled standard width of 39 (Liberation
  // Sans at 9 px) snaps nothing and 40 (C059 at 14 px) snaps.
  if (standard < 40) {
    snap = false;
  }
  int64_t best_limit = af_mulfix(af->upem / 40, yscale);
  ptrdiff_t anchor = -1;
  bool have_anchor = false;

  if (best_limit > 32) {
    best_limit = 32;
  }
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];

    e->opos = af_mulfix(e->fpos, yscale);
    e->pos = e->opos;
    e->done = false;
    e->has_blue = false;
  }
  // Blue zones: the nearest zone of the edge's side within a fortieth of an em.
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];
    int64_t bd = best_limit;

    for (int z = 0; z < AF_ZONES; ++z) {
      const AfZone * zone = &script->zones[z];
      int64_t d;

      if (nonbase || !zone->present || !zone->active || zone->top != e->top) {
        continue;
      }
      d = af_mulfix(e->fpos - zone->ref, yscale);
      d = d < 0 ? -d : d;
      if (d < bd) {
        bd = d;
        e->has_blue = true;
        e->blue = zone->ref_fit;
      }
      if (e->round && d != 0) {
        bool under = e->fpos < zone->ref;

        if (zone->top != under) {
          d = af_mulfix(e->fpos - zone->shoot, yscale);
          d = d < 0 ? -d : d;
          if (d < bd) {
            bd = d;
            e->has_blue = true;
            e->blue = zone->shoot_fit;
          }
        }
      }
    }
  }
  // Edges in a zone first, with their stem partner at its scaled width.
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];
    AfEdge * edge1 = NULL;
    AfEdge * edge2 = e->link >= 0 ? &g->edges[e->link] : NULL;
    int64_t blue = 0;

    if (e->has_blue) {
      blue = e->blue;
      edge1 = e;
    }
    else if (edge2 && edge2->has_blue) {
      blue = edge2->blue;
      edge1 = edge2;
      edge2 = e;
    }
    if (!edge1) {
      continue;
    }
    edge1->pos = blue;
    edge1->done = true;
    if (edge2 && !edge2->has_blue) {
      af_align_linked(g, edge1, edge2, snap, standard);
    }
    if (!have_anchor) {
      anchor = (ptrdiff_t)k;
      have_anchor = true;
    }
  }
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];

    // The first edge in order whose partner is paired with another edge is the odd one out: with no stem
    // ahead of it it is rounded to a pixel and becomes the anchor, and the stems are placed from it.
    if (have_anchor || e->done || e->link < 0) {
      continue;
    }
    if (g->edges[e->link].link == (ptrdiff_t)k) {
      break;
    }
    e->link = -1;
    e->pos = af_pixround(e->opos);
    e->done = true;
    anchor = (ptrdiff_t)k;
    have_anchor = true;
  }
  // The stems: the first is the anchor, the rest are placed from it.
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];
    AfEdge * l;
    int64_t org_len;
    int64_t cur_len;

    if (e->done || e->link < 0) {
      continue;
    }
    l = &g->edges[e->link];
    if (l->has_blue) {
      af_align_linked(g, l, e, snap, standard);
      continue;
    }
    org_len = l->opos - e->opos;
    cur_len = snap ? af_fitted_width(g, e, org_len, standard) : org_len;
    if (!have_anchor) {
      // Centred on the original edge plus half the fitted width (the same as half the original in the light
      // target, whose widths are not changed): DejaVu Sans h at 14 px, centre 123 and not 121, goes to 154.
      int64_t org_center = e->opos + (cur_len >> 1);

      if (cur_len < 96) {
        e->pos = af_stem_center(org_center, cur_len) - (cur_len >> 1);
      }
      else {
        e->pos = af_pixround(e->opos);
        // A positive side bearing is not rounded away: a first wide stem from 16/64 to 31/64 of a pixel right of
        // the origin goes to one pixel, not to the origin (a stem swept across a probe face; DejaVu Sans Bold o).
        if (x_axis && e->pos <= 0 && e->opos > 0) {
          e->pos = 64;
        }
      }
      l->pos = e->pos + cur_len;
      anchor = (ptrdiff_t)k;
      have_anchor = true;
    }
    else {
      int64_t org_pos = g->edges[anchor].pos + (e->opos - g->edges[anchor].opos);
      int64_t org_center = org_pos + (org_len >> 1);

      if (l->done) {
        e->pos = l->pos - cur_len;
      }
      else if (cur_len < 96) {
        int64_t center = af_stem_center(org_center, cur_len);

        e->pos = center - (cur_len >> 1);
        l->pos = center + (cur_len >> 1);
      }
      else {
        int64_t p1 = af_pixround(org_pos);
        int64_t d1 = p1 + (cur_len >> 1) - org_center;
        int64_t p2 = af_pixround(org_pos + org_len) - cur_len;
        int64_t d2 = p2 + (cur_len >> 1) - org_center;

        d1 = d1 < 0 ? -d1 : d1;
        d2 = d2 < 0 ? -d2 : d2;
        e->pos = d1 < d2 ? p1 : p2;
        l->pos = e->pos + cur_len;
      }
    }
    e->done = true;
    l->done = true;
    af_clamp(g, k);
  }
  // Serifs, and what is left.
  for (size_t k = 0; k < g->edge_count; ++k) {
    AfEdge * e = &g->edges[k];
    int64_t delta = 1000;

    if (e->done) {
      continue;
    }
    if (e->serif >= 0) {
      delta = g->edges[e->serif].opos - e->opos;
      delta = delta < 0 ? -delta : delta;
    }
    if (delta < 64 + 48) {
      e->pos = g->edges[e->serif].pos + (e->opos - g->edges[e->serif].opos);
    }
    else if (!have_anchor) {
      e->pos = af_pixround(e->opos);
      anchor = (ptrdiff_t)k;
      have_anchor = true;
    }
    else {
      const AfEdge * before = NULL;
      const AfEdge * after = NULL;

      for (size_t m = k; m-- > 0;) {
        if (g->edges[m].done) {
          before = &g->edges[m];
          break;
        }
      }
      for (size_t m = k + 1u; m < g->edge_count; ++m) {
        if (g->edges[m].done) {
          after = &g->edges[m];
          break;
        }
      }
      if (before && after) {
        e->pos = after->opos == before->opos
            ? before->pos
            : before->pos + af_muldiv(e->opos - before->opos, after->pos - before->pos, after->opos - before->opos);
      }
      else if (x_axis && (before || after)) {
        const AfEdge * near = before ? before : after;

        e->pos = e->opos + (near->pos - near->opos);
      }
      else {
        e->pos = g->edges[anchor].pos + ((e->opos - g->edges[anchor].opos + 16) & ~(int64_t)31);
      }
    }
    e->done = true;
    af_clamp(g, k);
  }
}

/** The hinted y of every point. */
static void af_fit_points(AfGlyph * g, const AfScript * script) {
  int64_t yscale = script->yscale;

  for (size_t i = 0; i < g->n; ++i) {
    g->touched[i] = false;
    g->out_y[i] = 0;
  }
  for (size_t s = 0; s < g->seg_count; ++s) {
    const AfSeg * seg = &g->segs[s];

    if (seg->edge < 0) {
      continue;
    }
    for (size_t k = 0; k < seg->count; ++k) {
      size_t p = g->seg_points[seg->first + k];

      g->out_y[p] = g->edges[seg->edge].pos;
      g->touched[p] = true;
    }
  }
  // Strong points: between edges by their distance in font units.
  for (size_t i = 0; i < g->n; ++i) {
    int64_t org;
    int64_t oy;

    if (g->touched[i] || af_weak(g, i)) {
      continue;
    }
    org = g->y[i];
    oy = af_mulfix(org, yscale);
    g->touched[i] = true;
    if (g->edge_count == 0) {
      g->out_y[i] = oy;
    }
    else if (org <= g->edges[0].fpos) {
      g->out_y[i] = oy + (g->edges[0].pos - g->edges[0].opos);
    }
    else if (org >= g->edges[g->edge_count - 1u].fpos) {
      const AfEdge * e = &g->edges[g->edge_count - 1u];

      g->out_y[i] = oy + (e->pos - e->opos);
    }
    else {
      for (size_t k = 0; k + 1u < g->edge_count; ++k) {
        const AfEdge * a = &g->edges[k];
        const AfEdge * b = &g->edges[k + 1u];

        if (a->fpos <= org && org <= b->fpos) {
          g->out_y[i] = b->fpos != a->fpos
              ? a->pos + af_mulfix(org - a->fpos, af_divfix(b->pos - a->pos, b->fpos - a->fpos))
              : a->pos;
          break;
        }
      }
    }
  }
  // Weak points: along their contour, between the touched ones.
  for (size_t c = 0; c < g->contours; ++c) {
    size_t start = g->first[c];
    size_t n = g->count[c];
    size_t touched_count = 0;
    size_t first_touched = n;

    for (size_t k = 0; k < n; ++k) {
      if (g->touched[start + k]) {
        touched_count += 1u;
        if (first_touched == n) {
          first_touched = k;
        }
      }
    }
    if (n == 0) {
      continue;
    }
    if (touched_count == 0) {
      for (size_t k = 0; k < n; ++k) {
        g->out_y[start + k] = af_mulfix(g->y[start + k], yscale);
      }
      continue;
    }
    if (touched_count == 1u) {
      int64_t d = g->out_y[start + first_touched] - af_mulfix(g->y[start + first_touched], yscale);

      for (size_t k = 0; k < n; ++k) {
        if (!g->touched[start + k]) {
          g->out_y[start + k] = af_mulfix(g->y[start + k], yscale) + d;
        }
      }
      continue;
    }
    {
      size_t a = first_touched;

      for (size_t step = 0; step < touched_count; ++step) {
        size_t b = (a + 1u) % n;
        int64_t u1;
        int64_t u2;
        int64_t c1;
        int64_t c2;
        int64_t d1;
        int64_t d2;

        while (!g->touched[start + b]) {
          b = (b + 1u) % n;
        }
        u1 = af_mulfix(g->y[start + a], yscale);
        u2 = af_mulfix(g->y[start + b], yscale);
        c1 = g->out_y[start + a];
        c2 = g->out_y[start + b];
        if (u1 > u2) {
          int64_t t = u1;

          u1 = u2;
          u2 = t;
          t = c1;
          c1 = c2;
          c2 = t;
        }
        d1 = c1 - u1;
        d2 = c2 - u2;
        for (size_t m = (a + 1u) % n; m != b; m = (m + 1u) % n) {
          int64_t u = af_mulfix(g->y[start + m], yscale);

          if (u <= u1) {
            g->out_y[start + m] = u + d1;
          }
          else if (u >= u2) {
            g->out_y[start + m] = u + d2;
          }
          else {
            g->out_y[start + m] = u2 != u1 ? c1 + af_mulfix(u - u1, af_divfix(c2 - c1, u2 - u1)) : c1;
          }
        }
        a = b;
      }
    }
  }
}

GFNT_Result gfnt_autofit_glyph_outline(const GFNT_Face * face, const GFNT_Autofit * af, uint32_t glyph,
    const GFNT_Allocator * allocator, GFNT_Outline ** out_outline, GFNT_HintStatus * out_status,
    int32_t * out_advance, GFNT_Error * error) {
  GFNT_Outline * source = NULL;
  GFNT_Outline * result = NULL;
  AfGlyph g;
  AfGlyph gx;
  AfScript xaxis;
  GFNT_Result r;
  const GFNT_Variation * variation;
  const AfScript * script;
  int64_t advance = -1;
  int64_t * light_y = NULL;
  bool nonbase;

  gfnt_error_clear(error);
  if (!face || !af || !out_outline || !out_status) {
    return GFNT_ERR_INVALID;
  }
  *out_outline = NULL;
  memset(&gx, 0, sizeof gx);
  variation = af->variation.count ? &af->variation : NULL;
  r = gfnt_face_glyph_outline(face, glyph, variation, NULL, &source, error);
  if (r != GFNT_OK) {
    return r;
  }
  r = af_glyph_load(face->allocator, source, false, &g);
  if (r == GFNT_OK && af->xy) {
    r = af_glyph_load(face->allocator, source, true, &gx);
  }
  if (r != GFNT_OK) {
    af_glyph_free(face->allocator, &g);
    gfnt_outline_destroy(source);
    return gfnt_error_set(error, r, 0, 0, glyph, "no memory for a hinted outline");
  }
  af_segments(&g, af->upem);
  af_link_segments(&g, af->upem);
  script = af_script_of(af, glyph);
  nonbase = af->nonbase && glyph < af->glyph_count && af->nonbase[glyph];
  af_edges(&g, script->yscale, script->stdw);
  if (af->xy) {
    // The light target's y, kept for the x segments' lengths, then the snapped one.
    af_fit_edges(&g, af, script, nonbase, false, af_mulfix(script->stdw, script->yscale), false);
    af_fit_points(&g, script);
    light_y = face->allocator->calloc_fn(face->allocator->ctx, g.n ? g.n : 1u, sizeof *light_y);
    if (!light_y) {
      af_glyph_free(face->allocator, &g);
      af_glyph_free(face->allocator, &gx);
      gfnt_outline_destroy(source);
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, glyph, "no memory for a hinted outline");
    }
    memcpy(light_y, g.out_y, g.n * sizeof *light_y);
    af_edges(&g, script->yscale, script->stdw);
  }
  af_fit_edges(&g, af, script, nonbase, af->xy, af_mulfix(script->stdw, script->yscale), false);
  af_fit_points(&g, script);
  if (af->xy) {
    // x is fitted by the same code on the outline turned a quarter: no blue zones, the uncorrected scale and the
    // vertical standard width.
    memset(&xaxis, 0, sizeof xaxis);
    xaxis.present = true;
    xaxis.yscale = af->scale;
    xaxis.stdw = script->xstdw;
    gx.x_axis = true;
    gx.cross_fit = light_y;
    af_segments(&gx, af->upem);
    af_link_segments(&gx, af->upem);
    af_edges(&gx, xaxis.yscale, xaxis.stdw);
    af_fit_edges(&gx, af, &xaxis, true, true, af_mulfix(xaxis.stdw, xaxis.yscale), true);
    af_fit_points(&gx, &xaxis);
  }
  if (af->xy && gx.edge_count > 0) {
    // The outline is not moved; the advance ends at the rightmost edge's fitted position plus its scaled bearing,
    // rounded, and a glyph with no advance (a mark) keeps none. Over 5,401 glyphs of three faces whose x all agree,
    // this gives FreeType's advance for 5,122; moving the origin by the left edge's rounded change, or adding that
    // change, gives fewer.
    int32_t units = 0;

    if (gfnt_face_glyph_advance(face, glyph, variation, &units, NULL) == GFNT_OK) {
      const AfEdge * e2 = &gx.edges[gx.edge_count - 1u];
      int64_t old_advance = af_mulfix(units, af->scale);

      advance = units == 0 ? 0 : af_pixround(e2->pos + (old_advance - e2->opos));
    }
  }
  r = gfnt_outline_create(allocator ? allocator : face->allocator, &result, error);
  for (size_t c = 0; r == GFNT_OK && c < g.contours; ++c) {
    r = gfnt_outline_begin_contour(result, error);
    for (size_t i = g.first[c]; r == GFNT_OK && i < g.first[c] + g.count[c]; ++i) {
      GFNT_Point p;
      GFNT_PointTag tag = GFNT_POINT_ON;
      GFNT_Point original;

      gfnt_outline_point_at(source, i, &original, &tag);
      p.x = af->xy ? (int32_t)gx.out_y[i] : (int32_t)af_mulfix(g.x[i], af->scale);
      p.y = (int32_t)g.out_y[i];
      r = gfnt_outline_add_point(result, p, tag, error);
    }
  }
  if (r == GFNT_OK) {
    result->space = GFNT_OUTLINE_PIXELS;
    *out_outline = result;
    result = NULL;
    *out_status = GFNT_HINT_STATUS_HINTED;
    if (out_advance) {
      int32_t units = 0;

      *out_advance = 0;
      if (advance >= 0) {
        *out_advance = (int32_t)advance;
      }
      else if (gfnt_face_glyph_advance(face, glyph, variation, &units, NULL) == GFNT_OK) {
        *out_advance = (int32_t)(af_pixround(af_mulfix(units, af->scale)));
      }
    }
  }
  af_glyph_free(face->allocator, &g);
  af_glyph_free(face->allocator, &gx);
  if (light_y) {
    face->allocator->free_fn(face->allocator->ctx, light_y);
  }
  gfnt_outline_destroy(result);
  gfnt_outline_destroy(source);
  return r;
}
