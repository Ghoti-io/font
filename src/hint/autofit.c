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
 * The automatic hinter, light target: y only, from the shapes of the face's own letters.
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
 * Everything is 26.6 and 16.16 integers as FreeType's, so that the rounding agrees. x is scaled and
 * not moved. Latin, Greek and Cyrillic have zones of their own, found from the face's letters of each;
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
  AfZone zones[AF_ZONES];
} AfScript;

enum { AF_LATIN, AF_GREEK, AF_CYRILLIC, AF_HEBREW, AF_ARABIC, AF_GEORGIAN, AF_ARMENIAN, AF_SCRIPTS };

struct GFNT_Autofit {
  const GFNT_Allocator * allocator;
  uint32_t ppem;
  uint16_t upem;
  GFNT_F2Dot14 * coords;
  GFNT_Variation variation;
  int32_t scale;      ///< 26.6 pixels per font unit, 16.16, uncorrected.
  AfScript scripts[AF_SCRIPTS];
  uint8_t * script_of;  ///< The script of each glyph; absent for a face with no cmap letters of another script.
  uint32_t glyph_count;
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
  {{U"ԲԴԸԼԿՂՈՊՌՐՒԱԵԺՍՏՕ", U"ԲԴԸԼԿՂՈՊՌՐՒԱԵԺՍՏՕ", U"եէժիխկձհճմնվփ", U"ապոռսւօտ", U"ապոռսւօտ", U"բգդզըթլղշչպջրցքիխկվփ"}, U'ո'},
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
  if (ax >= 14 * ay) {
    return dx > 0 ? AF_DIR_RIGHT : AF_DIR_LEFT;
  }
  if (ay >= 14 * ax) {
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
  size_t * prev;
  size_t * next;
  size_t * first;      ///< First point of each contour.
  size_t * count;      ///< Points in it.
  bool clockwise;
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
static GFNT_Result af_glyph_load(const GFNT_Allocator * a, const GFNT_Outline * outline, AfGlyph * g) {
  size_t n = gfnt_outline_point_count(outline);
  size_t contours = gfnt_outline_contour_count(outline);
  size_t cap = n ? n : 1u;
  size_t bytes;
  uint8_t * block;
  int64_t area = 0;

  memset(g, 0, sizeof *g);
  g->n = n;
  g->contours = contours;
  bytes = cap * (sizeof(int64_t) * 3u + sizeof(bool) * 2u + sizeof(int8_t) * 2u + sizeof(size_t) * 2u)
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

/** Horizontal segments: maximal runs of consecutive points whose outgoing vector has one horizontal direction. */
static void af_segments(AfGlyph * g) {
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

        s->first = g->seg_point_count;
        g->seg_points[g->seg_point_count++] = i;
        while (m < n && g->out_dir[start + (begin + m) % n] == d) {
          g->seg_points[g->seg_point_count++] = start + (begin + m) % n;
          ++m;
        }
        g->seg_points[g->seg_point_count++] = start + (begin + m) % n;   // where the last horizontal vector arrives
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
        if (overlap * 2048 < 8 * up) {
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
    // Two diagonals: a corner too slight to matter is weak, FreeType's "flat" test.
    size_t p = g->prev[i];
    size_t n = g->next[i];
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
  if (af_glyph_load(a, outline, &g) == GFNT_OK && g.n > 0) {
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

/** The thinnest horizontal stem of a script's standard letter (`o` in Latin), font units; 0 when the face has none. */
static int32_t af_standard_width(const GFNT_Face * face, const GFNT_Variation * variation, int64_t up, uint32_t cp) {
  uint32_t glyph = 0;
  GFNT_Outline * outline = NULL;
  AfGlyph g;
  int32_t best = 0;

  if (gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK || glyph == 0
      || gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline, NULL) != GFNT_OK) {
    return 0;
  }
  if (af_glyph_load(face->allocator, outline, &g) == GFNT_OK) {
    af_segments(&g);
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
    int32_t flats[16];
    int32_t rounds[16];
    size_t nf = 0;
    size_t nr = 0;

    for (const uint32_t * c = af_blue_strings[sc].letters[z]; *c; ++c) {
      int32_t y = 0;
      bool is_round = false;

      if (!af_letter_extreme(face, variation, *c, af_zone_top[z], &y, &is_round)) {
        continue;
      }
      if (is_round && nr < 16u) {
        rounds[nr++] = y;
      }
      else if (!is_round && nf < 16u) {
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
  }
  script->stdw = af_standard_width(face, variation, af->upem, af_blue_strings[sc].standard);
  if (script->stdw == 0) {
    script->stdw = 50 * af->upem / 2048;
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
};

/** Which glyphs belong to which script: the first script whose range reaches the glyph through the cmap. */
static GFNT_Result af_script_map(const GFNT_Face * face, GFNT_Autofit * af) {
  size_t count = 0;
  const GFNT_Allocator * a = face->allocator;

  if (!af->scripts[AF_GREEK].present && !af->scripts[AF_CYRILLIC].present && !af->scripts[AF_HEBREW].present
      && !af->scripts[AF_ARABIC].present && !af->scripts[AF_GEORGIAN].present
      && !af->scripts[AF_ARMENIAN].present) {
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

/** The script a glyph is fitted with: its own when the face has that script's letters, else Latin. */
static const AfScript * af_script_of(const GFNT_Autofit * af, uint32_t glyph) {
  if (af->script_of && glyph < af->glyph_count && af->script_of[glyph] < AF_SCRIPTS
      && af->scripts[af->script_of[glyph]].present) {
    return &af->scripts[af->script_of[glyph]];
  }
  return &af->scripts[AF_LATIN];
}

GFNT_Result gfnt_autofit_create(const GFNT_Face * face, uint32_t ppem, const GFNT_Variation * variation,
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
  if (af_script_map(face, af) != GFNT_OK) {
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

static void af_clamp(AfGlyph * g, size_t k) {
  AfEdge * e = &g->edges[k];

  if (k > 0 && e->pos < g->edges[k - 1u].pos) {
    e->pos = g->edges[k - 1u].pos;
  }
  if (k + 1u < g->edge_count && g->edges[k + 1u].done && e->pos > g->edges[k + 1u].pos) {
    e->pos = g->edges[k + 1u].pos;
  }
}

static void af_align_linked(AfEdge * base, AfEdge * stem) {
  stem->pos = base->pos + (stem->opos - base->opos);
  stem->done = true;
}

static void af_fit_edges(AfGlyph * g, const GFNT_Autofit * af, const AfScript * script) {
  int64_t yscale = script->yscale;
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

      if (!zone->present || !zone->active || zone->top != e->top) {
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
      af_align_linked(edge1, edge2);
    }
    if (!have_anchor) {
      anchor = (ptrdiff_t)k;
      have_anchor = true;
    }
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
      af_align_linked(l, e);
      continue;
    }
    org_len = l->opos - e->opos;
    cur_len = org_len;
    if (!have_anchor) {
      int64_t org_center = e->opos + (org_len >> 1);

      if (cur_len < 96) {
        e->pos = af_stem_center(org_center, cur_len) - (cur_len >> 1);
      }
      else {
        e->pos = af_pixround(e->opos);
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
    af_clamp(g, (size_t)e->link);
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
    if (delta < 64 + 16) {
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
  GFNT_Result r;
  const GFNT_Variation * variation;
  const AfScript * script;

  gfnt_error_clear(error);
  if (!face || !af || !out_outline || !out_status) {
    return GFNT_ERR_INVALID;
  }
  *out_outline = NULL;
  variation = af->variation.count ? &af->variation : NULL;
  r = gfnt_face_glyph_outline(face, glyph, variation, NULL, &source, error);
  if (r != GFNT_OK) {
    return r;
  }
  r = af_glyph_load(face->allocator, source, &g);
  if (r != GFNT_OK) {
    gfnt_outline_destroy(source);
    return gfnt_error_set(error, r, 0, 0, glyph, "no memory for a hinted outline");
  }
  af_segments(&g);
  af_link_segments(&g, af->upem);
  script = af_script_of(af, glyph);
  af_edges(&g, script->yscale, script->stdw);
  af_fit_edges(&g, af, script);
  af_fit_points(&g, script);
  r = gfnt_outline_create(allocator ? allocator : face->allocator, &result, error);
  for (size_t c = 0; r == GFNT_OK && c < g.contours; ++c) {
    r = gfnt_outline_begin_contour(result, error);
    for (size_t i = g.first[c]; r == GFNT_OK && i < g.first[c] + g.count[c]; ++i) {
      GFNT_Point p;
      GFNT_PointTag tag = GFNT_POINT_ON;
      GFNT_Point original;

      gfnt_outline_point_at(source, i, &original, &tag);
      p.x = (int32_t)af_mulfix(g.x[i], af->scale);
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
      if (gfnt_face_glyph_advance(face, glyph, variation, &units, NULL) == GFNT_OK) {
        *out_advance = (int32_t)(af_pixround(af_mulfix(units, af->scale)));
      }
    }
  }
  af_glyph_free(face->allocator, &g);
  gfnt_outline_destroy(result);
  gfnt_outline_destroy(source);
  return r;
}
