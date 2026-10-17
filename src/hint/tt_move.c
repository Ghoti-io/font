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
 * The instructions that measure and move points: the projection and freedom
 * vectors, the zones, and the reference points.
 *
 * Written from the instruction-set chapter of the OpenType specification. The
 * chapter fixes what each instruction does and not the rounding of every
 * intermediate value; those are marked and are settled by the comparison with
 * FreeType, point by point.
 */

#include "vm_internal.h"

#include <string.h>

#define FAIL(code) do { failure = (code); goto fault; } while (0)
/* An instruction that finds fewer operands than it pops: a failure, or for a tolerant run
   zeros in place of what is missing (they replace what is there, as the reference's do). */
#define NEED(n) do { \
    if (run->depth < (size_t)(n)) { \
      if (!run->ctx->params.tolerant) FAIL(GFNT_HINT_FAIL_STACK_UNDERFLOW); \
      gfnt_hint_soft(run, GFNT_HINT_FAIL_STACK_UNDERFLOW); \
      memset(run->stack, 0, (size_t)(n) * sizeof run->stack[0]); \
      run->depth = (size_t)(n); \
    } \
  } while (0)
#define POP() (run->stack[--run->depth])
#define PUSH(v) do { int32_t pushed_ = (v); run->stack[run->depth++] = pushed_; } while (0)

/* -- zones ---------------------------------------------------------------- */

GFNT_Result gfnt_hint_zone_alloc(const GFNT_Allocator * a, GFNT_HintZone * zone,
    size_t capacity, size_t contours) {
  memset(zone, 0, sizeof *zone);
  zone->capacity = capacity;
  zone->cur = a->calloc_fn(a->ctx, capacity ? capacity : 1u, sizeof zone->cur[0]);
  zone->org = a->calloc_fn(a->ctx, capacity ? capacity : 1u, sizeof zone->org[0]);
  zone->orus = a->calloc_fn(a->ctx, capacity ? capacity : 1u, sizeof zone->orus[0]);
  zone->flags = a->calloc_fn(a->ctx, capacity ? capacity : 1u, 1);
  zone->ends = a->calloc_fn(a->ctx, contours ? contours : 1u, sizeof zone->ends[0]);
  if (!zone->cur || !zone->org || !zone->orus || !zone->flags || !zone->ends) {
    gfnt_hint_zone_free(a, zone);
    return GFNT_ERR_OOM;
  }
  return GFNT_OK;
}

void gfnt_hint_zone_free(const GFNT_Allocator * a, GFNT_HintZone * zone) {
  if (zone->cur) a->free_fn(a->ctx, zone->cur);
  if (zone->org) a->free_fn(a->ctx, zone->org);
  if (zone->orus) a->free_fn(a->ctx, zone->orus);
  if (zone->flags) a->free_fn(a->ctx, zone->flags);
  if (zone->ends) a->free_fn(a->ctx, zone->ends);
  memset(zone, 0, sizeof *zone);
}

void gfnt_hint_zone_copy(GFNT_HintZone * to, const GFNT_HintZone * from) {
  to->count = from->count;
  to->contour_count = from->contour_count;
  memcpy(to->cur, from->cur, from->count * sizeof from->cur[0]);
  memcpy(to->org, from->org, from->count * sizeof from->org[0]);
  memcpy(to->orus, from->orus, from->count * sizeof from->orus[0]);
  memcpy(to->flags, from->flags, from->count);
  memcpy(to->ends, from->ends, from->contour_count * sizeof from->ends[0]);
}

/* -- vector arithmetic ------------------------------------------------------ */

/* (value + 2^13) >> 14, symmetric about zero. */
static int32_t gfnt_hint_shift14(int64_t value) {
  int64_t magnitude = value < 0 ? -value : value;

  magnitude = (magnitude + 0x2000) >> 14;
  return (int32_t)(uint32_t)(uint64_t)(value < 0 ? -magnitude : magnitude);
}

/* a * b / 2^14 */
static int32_t gfnt_hint_mul14(int32_t a, int32_t b) {
  return gfnt_hint_shift14((int64_t)a * b);
}

static int32_t gfnt_hint_dot14(int32_t x, int32_t y, const int16_t v[2]) {
  return gfnt_hint_shift14((int64_t)x * v[0] + (int64_t)y * v[1]);
}

static int32_t gfnt_hint_project(const GFNT_HintRun * run, int32_t dx, int32_t dy) {
  return gfnt_hint_dot14(dx, dy, run->gs.pv);
}

static int32_t gfnt_hint_dual_project(const GFNT_HintRun * run, int32_t dx, int32_t dy) {
  return gfnt_hint_dot14(dx, dy, run->gs.dpv);
}

/* The cosine between the freedom and projection vectors, 2.14. */
static int32_t gfnt_hint_freedom_dot(const GFNT_HintRun * run) {
  int64_t sum = (int64_t)run->gs.fv[0] * run->gs.pv[0]
      + (int64_t)run->gs.fv[1] * run->gs.pv[1];

  return (int32_t)(sum >> 14);
}

static uint64_t gfnt_hint_isqrt(uint64_t n) {
  uint64_t root = 0;
  uint64_t bit = (uint64_t)1 << 62;

  while (bit > n) {
    bit >>= 2;
  }
  while (bit) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    }
    else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

/*
 * The unit vector, 2.14, along (dx, dy): each component truncated toward zero
 * from its exact value. A zero vector is the x axis.
 *
 * |dx| * 2^14 / sqrt(dx^2 + dy^2) is the square root of dx^2 * 2^28 / (dx^2 +
 * dy^2), and the floor of a square root is the square root of the floor, so an
 * integer square root of an integer quotient gives the exact truncation. Inputs
 * past 2^17 are scaled down first, which loses nothing that 2.14 holds.
 */
static void gfnt_hint_unit(int64_t dx, int64_t dy, int16_t out[2]) {
  uint64_t ax = (uint64_t)(dx < 0 ? -dx : dx);
  uint64_t ay = (uint64_t)(dy < 0 ? -dy : dy);
  uint64_t sum;
  uint64_t x;
  uint64_t y;

  while (ax > ((uint64_t)1 << 17) || ay > ((uint64_t)1 << 17)) {
    ax >>= 1;
    ay >>= 1;
  }
  sum = ax * ax + ay * ay;
  if (sum == 0) {
    out[0] = 0x4000;
    out[1] = 0;
    return;
  }
  x = gfnt_hint_isqrt((ax * ax << 28) / sum);
  y = gfnt_hint_isqrt((ay * ay << 28) / sum);
  out[0] = (int16_t)(dx < 0 ? -(int64_t)x : (int64_t)x);
  out[1] = (int16_t)(dy < 0 ? -(int64_t)y : (int64_t)y);
}

/* Whether the version 40 compatibility behaviour is on: it is for that
   interpreter until the font asks for native ClearType (INSTCTRL 3 with 4). */
static bool gfnt_hint_compat(const GFNT_HintRun * run) {
  return run->ctx->params.version >= 40 && !(run->instruct_control & 4u);
}

/* In that behaviour a program that has interpolated along both axes has finished
   with the outline: nothing it does afterwards moves a point. */
static bool gfnt_hint_frozen(const GFNT_HintRun * run) {
  return gfnt_hint_compat(run) && run->iup_called[0] && run->iup_called[1];
}

/* Move a point by @p distance along the projection vector, by way of the freedom vector. */
static void gfnt_hint_move(GFNT_HintRun * run, GFNT_HintZone * z, size_t i,
    int32_t distance) {
  const int16_t * fv = run->gs.fv;
  int32_t f = gfnt_hint_freedom_dot(run);

  /* A freedom vector within about 3.6 degrees of perpendicular to the projection
     vector cannot move a point by a measurable amount along it without moving it
     enormously; FreeType moves nothing and marks nothing, and so does this. */
  if (f > -0x400 && f < 0x400) {
    f = 0;
  }
  if (f == 0 || gfnt_hint_frozen(run)) {
    return;
  }
  if (fv[0] != 0) {
    /* The compatibility behaviour leaves x alone and still counts the point as
       touched there. */
    if (!gfnt_hint_compat(run)) {
      z->cur[i].x = gfnt_hint_add(z->cur[i].x, gfnt_hint_muldiv(distance, fv[0], f));
    }
    z->flags[i] |= GFNT_HINT_TOUCH_X;
  }
  if (fv[1] != 0) {
    z->cur[i].y = gfnt_hint_add(z->cur[i].y, gfnt_hint_muldiv(distance, fv[1], f));
    z->flags[i] |= GFNT_HINT_TOUCH_Y;
  }
}

/* Move a point by an explicit displacement, touching the axes the freedom vector uses. */
static void gfnt_hint_displace(const GFNT_HintRun * run, GFNT_HintZone * z, size_t i,
    int32_t dx, int32_t dy) {
  if (gfnt_hint_frozen(run)) {
    return;
  }
  if (run->gs.fv[0] != 0) {
    if (!gfnt_hint_compat(run)) {
      z->cur[i].x = gfnt_hint_add(z->cur[i].x, dx);
    }
    z->flags[i] |= GFNT_HINT_TOUCH_X;
  }
  if (run->gs.fv[1] != 0) {
    z->cur[i].y = gfnt_hint_add(z->cur[i].y, dy);
    z->flags[i] |= GFNT_HINT_TOUCH_Y;
  }
}

/* The original distance from @p b to @p a in two zones, along the dual vector. */
static int32_t gfnt_hint_original_distance(const GFNT_HintRun * run,
    const GFNT_HintZone * za, size_t a, const GFNT_HintZone * zb, size_t b) {
  if (za == &run->zone[0] || zb == &run->zone[0]) {
    return gfnt_hint_dual_project(run, gfnt_hint_sub(za->org[a].x, zb->org[b].x),
        gfnt_hint_sub(za->org[a].y, zb->org[b].y));
  }
  /* In the glyph zone the font-unit coordinates are exact; measure there and
     scale once, so the distance carries no rounding of its own points. */
  return gfnt_hint_mul_fix(gfnt_hint_dual_project(run,
      gfnt_hint_sub(za->orus[a].x, zb->orus[b].x),
      gfnt_hint_sub(za->orus[a].y, zb->orus[b].y)), run->scale);
}

static int32_t gfnt_hint_scaled_original_distance(const GFNT_HintRun * run,
    const GFNT_HintZone * za, size_t a, const GFNT_HintZone * zb, size_t b) {
  return gfnt_hint_dual_project(run, gfnt_hint_sub(za->org[a].x, zb->org[b].x),
      gfnt_hint_sub(za->org[a].y, zb->org[b].y));
}

static int32_t gfnt_hint_current_distance(const GFNT_HintRun * run,
    const GFNT_HintZone * za, size_t a, const GFNT_HintZone * zb, size_t b) {
  return gfnt_hint_project(run, gfnt_hint_sub(za->cur[a].x, zb->cur[b].x),
      gfnt_hint_sub(za->cur[a].y, zb->cur[b].y));
}

bool gfnt_hint_is_point_op(uint8_t op) {
  switch (op) {
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0F: case 0x27: case 0x29: case 0x2E: case 0x2F: case 0x30: case 0x31:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A:
    case 0x5D: case 0x71: case 0x72:
    case 0x80: case 0x81: case 0x82: case 0x86: case 0x87:
      return true;
    default:
      return op >= 0xC0;
  }
}

/* -- IUP ---------------------------------------------------------------------- */

static void gfnt_hint_iup_run(GFNT_HintZone * z, size_t first, size_t last,
    size_t t1, size_t t2, bool x_axis) {
  /* Interpolate the untouched points strictly between touched points t1 and t2,
     walking the contour from t1 to t2 (cyclically within [first, last]). */
  size_t i = t1;
  int32_t o1;
  int32_t o2;
  int32_t c1;
  int32_t c2;
  int32_t u1;
  int32_t u2;
  int32_t scale = 0;
  bool degenerate;

#define COORD(arr, k) (x_axis ? (arr)[k].x : (arr)[k].y)
  o1 = COORD(z->org, t1);
  o2 = COORD(z->org, t2);
  c1 = COORD(z->cur, t1);
  c2 = COORD(z->cur, t2);
  u1 = COORD(z->orus, t1);
  u2 = COORD(z->orus, t2);
  if (u1 > u2) {
    int32_t t;
    t = o1; o1 = o2; o2 = t;
    t = c1; c1 = c2; c2 = t;
    t = u1; u1 = u2; u2 = t;
  }
  degenerate = u1 == u2;
  if (!degenerate) {
    scale = gfnt_hint_div_fix(gfnt_hint_sub(c2, c1), gfnt_hint_sub(u2, u1));
  }
  for (;;) {
    int32_t org;
    int32_t units;
    int32_t value;

    i = i == last ? first : i + 1u;
    if (i == t2) {
      break;
    }
    /* Outside the touched pair a point moves as the nearer one did, from where
       it began. (The lone touched point below is different: the shift is added to
       where the points are, so two interpolations in one axis shift twice, as
       FreeType's do.) */
    org = COORD(z->org, i);
    units = COORD(z->orus, i);
    /* Which side of the pair a point is on is decided on the scaled original
       coordinates, which can tie where the font units do not; the proportion
       between them is taken in font units. */
    if (org <= o1) {
      value = gfnt_hint_add(org, gfnt_hint_sub(c1, o1));
    }
    else if (org >= o2) {
      value = gfnt_hint_add(org, gfnt_hint_sub(c2, o2));
    }
    else if (degenerate) {
      value = gfnt_hint_add(org, gfnt_hint_sub(c1, o1));
    }
    else {
      value = gfnt_hint_add(c1, gfnt_hint_mul_fix(gfnt_hint_sub(units, u1), scale));
    }
    if (x_axis) {
      z->cur[i].x = value;
    }
    else {
      z->cur[i].y = value;
    }
  }
#undef COORD
}

static void gfnt_hint_iup(GFNT_HintZone * z, bool x_axis) {
  uint8_t touch = x_axis ? GFNT_HINT_TOUCH_X : GFNT_HINT_TOUCH_Y;
  size_t start = 0;

  for (size_t c = 0; c < z->contour_count; ++c) {
    size_t end = z->ends[c];
    size_t first_touched = 0;
    size_t previous = 0;
    size_t touched = 0;
    bool any = false;

    if (end >= z->count || end < start) {
      return;
    }
    for (size_t i = start; i <= end; ++i) {
      if (!(z->flags[i] & touch)) {
        continue;
      }
      if (!any) {
        any = true;
        first_touched = i;
      }
      else {
        gfnt_hint_iup_run(z, start, end, previous, i, x_axis);
      }
      previous = i;
      ++touched;
    }
    if (any && touched == 1) {
      /* One touched point: the rest of the contour moves with it. */
      int32_t delta = x_axis ? z->cur[first_touched].x - z->org[first_touched].x
          : z->cur[first_touched].y - z->org[first_touched].y;
      for (size_t i = start; i <= end; ++i) {
        if (i == first_touched) continue;
        if (x_axis) z->cur[i].x = gfnt_hint_add(z->cur[i].x, delta);
        else z->cur[i].y = gfnt_hint_add(z->cur[i].y, delta);
      }
    }
    else if (any) {
      gfnt_hint_iup_run(z, start, end, previous, first_touched, x_axis);
    }
    start = end + 1u;
  }
}

/* -- the instructions ---------------------------------------------------------- */

#define ZONE(n) (&run->zone[gs->zp[n]])
/* Something wrong that a tolerant run carries on past, in the way @p action says: a strict
   one fails here. */
#define LENIENT(code) do { \
    if (!run->ctx->params.tolerant) FAIL(code); \
    gfnt_hint_soft(run, (code)); \
  } while (0)

/* A point that does not exist: failure, or the tolerant run's @p action (break, continue). */
/* Not a do/while: the action is a `break` or a `continue` for the caller's loop or switch. */
#define CHECK_POINT_OR(zone, p, action) \
  if ((p) < 0 || (size_t)(p) >= (zone)->count) { \
    LENIENT(GFNT_HINT_FAIL_BAD_INDEX); \
    action; \
  }

/* The shift of the reference point the SHP family follows, as a displacement. */
static GFNT_HintFailure gfnt_hint_ref_shift(GFNT_HintRun * run, unsigned which,
    GFNT_HintZone ** zone, size_t * point, int32_t * dx, int32_t * dy) {
  GFNT_HintGState * gs = &run->gs;
  uint32_t ref = which ? gs->rp[1] : gs->rp[2];
  GFNT_HintZone * z = which ? ZONE(0) : ZONE(1);
  int32_t d;
  int32_t f;

  if (ref >= z->count) {
    return GFNT_HINT_FAIL_BAD_INDEX;
  }
  d = gfnt_hint_project(run, gfnt_hint_sub(z->cur[ref].x, z->org[ref].x),
      gfnt_hint_sub(z->cur[ref].y, z->org[ref].y));
  f = gfnt_hint_freedom_dot(run);
  if (f > -0x400 && f < 0x400) {
    f = 0;
  }
  *dx = f ? gfnt_hint_muldiv(d, gs->fv[0], f) : 0;
  *dy = f ? gfnt_hint_muldiv(d, gs->fv[1], f) : 0;
  *zone = z;
  *point = ref;
  return GFNT_HINT_OK;
}

static int32_t gfnt_hint_delta_step(unsigned nibble, int32_t shift) {
  int32_t steps = nibble < 8u ? (int32_t)nibble - 8 : (int32_t)nibble - 7;

  return (steps * 64) / (1 << shift);
}

GFNT_HintFailure gfnt_hint_point_op(GFNT_HintRun * run, uint8_t op,
    GFNT_HintProgram program) {
  GFNT_HintGState * gs = &run->gs;
  GFNT_HintFailure failure = GFNT_HINT_OK;
  const GFNT_HintContext * ctx = run->ctx;

  (void)program;
  switch (op) {
    case 0x2E: case 0x2F: { /* MDAP[a] */
      GFNT_HintZone * z0 = ZONE(0);
      int32_t p;
      int32_t d = 0;
      NEED(1);
      p = POP();
      CHECK_POINT_OR(z0, p, break);
      if (op & 1) {
        int32_t now = gfnt_hint_project(run, z0->cur[p].x, z0->cur[p].y);
        d = gfnt_hint_sub(gfnt_hint_round(gs, now), now);
      }
      gfnt_hint_move(run, z0, (size_t)p, d);
      gs->rp[0] = gs->rp[1] = (uint32_t)p;
      break;
    }
    case 0x3E: case 0x3F: { /* MIAP[a] */
      GFNT_HintZone * z0 = ZONE(0);
      int32_t n;
      int32_t p;
      int32_t distance;
      int32_t now;
      NEED(2);
      n = POP();
      p = POP();
      if (p < 0 || (size_t)p >= z0->count || n < 0 || (size_t)n >= run->cvt_count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        /* The reference points are set whether or not the move happened. */
        gs->rp[0] = gs->rp[1] = (uint32_t)p & 0xFFFFu;
        break;
      }
      distance = run->cvt[n];
      if (gs->zp[0] == 0) {
        z0->org[p].x = gfnt_hint_mul14(distance, gs->fv[0]);
        z0->org[p].y = gfnt_hint_mul14(distance, gs->fv[1]);
        z0->cur[p] = z0->org[p];
      }
      now = gfnt_hint_project(run, z0->cur[p].x, z0->cur[p].y);
      if (op & 1) {
        int32_t apart = gfnt_hint_sub(distance, now);
        if ((apart < 0 ? -(int64_t)apart : (int64_t)apart) > gs->cvt_cut_in) {
          distance = now;
        }
        distance = gfnt_hint_round(gs, distance);
      }
      gfnt_hint_move(run, z0, (size_t)p, gfnt_hint_sub(distance, now));
      gs->rp[0] = gs->rp[1] = (uint32_t)p;
      break;
    }
    case 0x3A: case 0x3B: { /* MSIRP[a] */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z1 = ZONE(1);
      int32_t d;
      int32_t p;
      uint32_t rp0 = gs->rp[0];
      NEED(2);
      d = POP();
      p = POP();
      CHECK_POINT_OR(z1, p, break);
      if (rp0 >= z0->count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        break;
      }
      if (gs->zp[1] == 0) {
        z1->org[p] = z0->org[rp0];
        z1->cur[p] = z1->org[p];
      }
      gfnt_hint_move(run, z1, (size_t)p, gfnt_hint_sub(d,
          gfnt_hint_current_distance(run, z1, (size_t)p, z0, rp0)));
      gs->rp[1] = gs->rp[0];
      gs->rp[2] = (uint32_t)p;
      if (op & 1) gs->rp[0] = (uint32_t)p;
      break;
    }
    case 0x3C: { /* ALIGNRP */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z1 = ZONE(1);
      uint32_t rp0 = gs->rp[0];
      if (run->ctx->params.tolerant && (run->depth < (size_t)gs->loop || rp0 >= z0->count)) {
        /* Too few operands for the loop: nothing is done. No reference point: the points
           are taken off the stack and nothing is moved. */
        if (run->depth < (size_t)gs->loop) {
          LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
        }
        else {
          LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
          run->depth -= (size_t)gs->loop;
        }
        gs->loop = 1;
        break;
      }
      if (rp0 >= z0->count) FAIL(GFNT_HINT_FAIL_BAD_INDEX);
      for (int32_t k = 0; k < gs->loop; ++k) {
        int32_t p;
        NEED(1);
        p = POP();
        CHECK_POINT_OR(z1, p, continue);
        gfnt_hint_move(run, z1, (size_t)p, gfnt_hint_sub(0,
            gfnt_hint_current_distance(run, z1, (size_t)p, z0, rp0)));
      }
      gs->loop = 1;
      break;
    }
    case 0x27: { /* ALIGNPTS */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z1 = ZONE(1);
      int32_t p1;
      int32_t p2;
      int32_t d;
      NEED(2);
      p1 = POP();
      p2 = POP();
      CHECK_POINT_OR(z1, p1, break);
      CHECK_POINT_OR(z0, p2, break);
      d = gfnt_hint_current_distance(run, z0, (size_t)p2, z1, (size_t)p1) / 2;
      gfnt_hint_move(run, z1, (size_t)p1, d);
      gfnt_hint_move(run, z0, (size_t)p2, gfnt_hint_sub(0, d));
      break;
    }
    case 0x29: { /* UTP */
      GFNT_HintZone * z0 = ZONE(0);
      int32_t p;
      NEED(1);
      p = POP();
      CHECK_POINT_OR(z0, p, break);
      if (gs->fv[0] != 0) z0->flags[p] &= (uint8_t)~GFNT_HINT_TOUCH_X;
      if (gs->fv[1] != 0) z0->flags[p] &= (uint8_t)~GFNT_HINT_TOUCH_Y;
      break;
    }
    case 0x30: case 0x31: { /* IUP[a]: a == 1 is x */
      GFNT_HintZone * z2 = ZONE(2);
      if (gs->zp[2] == 0) {
        LENIENT(GFNT_HINT_FAIL_BAD_ARGUMENT);
        z2 = &run->zone[1];       /* the glyph's own points, whatever zp2 says */
      }
      if (!gfnt_hint_frozen(run)) {
        gfnt_hint_iup(z2, (op & 1) != 0);
      }
      run->iup_called[op & 1u] = true;
      break;
    }
    case 0x32: case 0x33: { /* SHP[a] */
      GFNT_HintZone * z2 = ZONE(2);
      GFNT_HintZone * rz;
      size_t rp;
      int32_t dx;
      int32_t dy;
      if (run->ctx->params.tolerant && run->depth < (size_t)gs->loop) {
        LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
        gs->loop = 1;
        break;
      }
      failure = gfnt_hint_ref_shift(run, op & 1u, &rz, &rp, &dx, &dy);
      if (failure != GFNT_HINT_OK) {
        if (!run->ctx->params.tolerant) goto fault;
        gfnt_hint_soft(run, failure);
        run->depth -= (size_t)gs->loop;      /* the points are taken and nothing is moved */
        gs->loop = 1;
        break;
      }
      for (int32_t k = 0; k < gs->loop; ++k) {
        int32_t p;
        NEED(1);
        p = POP();
        CHECK_POINT_OR(z2, p, continue);
        gfnt_hint_displace(run, z2, (size_t)p, dx, dy);
      }
      gs->loop = 1;
      break;
    }
    case 0x34: case 0x35: { /* SHC[a] */
      GFNT_HintZone * z2 = ZONE(2);
      GFNT_HintZone * rz;
      size_t rp;
      int32_t dx;
      int32_t dy;
      int32_t c;
      size_t first;
      size_t last;
      NEED(1);
      c = POP();
      failure = gfnt_hint_ref_shift(run, op & 1u, &rz, &rp, &dx, &dy);
      if (failure != GFNT_HINT_OK) {
        if (!run->ctx->params.tolerant) goto fault;
        gfnt_hint_soft(run, failure);
        break;
      }
      if (c < 0 || (size_t)c >= z2->contour_count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        break;
      }
      first = c == 0 ? 0 : z2->ends[c - 1] + 1u;
      last = z2->ends[c];
      if (last >= z2->count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        break;
      }
      for (size_t i = first; i <= last; ++i) {
        if (rz == z2 && i == rp) continue;
        gfnt_hint_displace(run, z2, i, dx, dy);
      }
      break;
    }
    case 0x36: case 0x37: { /* SHZ[a] */
      GFNT_HintZone * rz;
      GFNT_HintZone * z;
      size_t rp;
      int32_t dx;
      int32_t dy;
      int32_t e;
      size_t limit;
      NEED(1);
      e = POP();
      if (e != 0 && e != 1) {
        LENIENT(GFNT_HINT_FAIL_BAD_ARGUMENT);
        break;
      }
      failure = gfnt_hint_ref_shift(run, op & 1u, &rz, &rp, &dx, &dy);
      if (failure != GFNT_HINT_OK) {
        if (!run->ctx->params.tolerant) goto fault;
        gfnt_hint_soft(run, failure);
        break;
      }
      /* The operand only has to name a zone; the points moved are those of zp2's. */
      z = ZONE(2);
      limit = gs->zp[2] == 0 ? z->count : (z->contour_count ? z->ends[z->contour_count - 1] + 1u : 0);
      if (limit > z->count) limit = z->count;
      for (size_t i = 0; i < limit; ++i) {
        if (rz == z && i == rp) continue;
        /* The zone shift moves the point without marking it touched. */
        if (gs->fv[0] != 0 && !gfnt_hint_compat(run)) z->cur[i].x = gfnt_hint_add(z->cur[i].x, dx);
        if (gs->fv[1] != 0 && !gfnt_hint_frozen(run)) z->cur[i].y = gfnt_hint_add(z->cur[i].y, dy);
      }
      break;
    }
    case 0x38: { /* SHPIX */
      GFNT_HintZone * z2 = ZONE(2);
      int32_t amount;
      int32_t dx;
      int32_t dy;
      NEED(1);
      amount = POP();
      dx = gfnt_hint_mul14(amount, gs->fv[0]);
      dy = gfnt_hint_mul14(amount, gs->fv[1]);
      if (run->ctx->params.tolerant && run->depth < (size_t)gs->loop) {
        LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
        gs->loop = 1;
        break;
      }
      for (int32_t k = 0; k < gs->loop; ++k) {
        int32_t p;
        NEED(1);
        p = POP();
        CHECK_POINT_OR(z2, p, continue);
        /* The version 40 compatibility behaviour moves only points the program
           has already moved vertically. */
        if (gfnt_hint_compat(run) && !run->composite && !(z2->flags[p] & GFNT_HINT_TOUCH_Y)) {
          continue;
        }
        gfnt_hint_displace(run, z2, (size_t)p, dx, dy);
      }
      gs->loop = 1;
      break;
    }
    case 0x39: { /* IP */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z1 = ZONE(1);
      GFNT_HintZone * z2 = ZONE(2);
      uint32_t rp1 = gs->rp[1];
      uint32_t rp2 = gs->rp[2];
      int32_t old_range;
      int32_t cur_range;
      bool twilight = gs->zp[0] == 0 || gs->zp[1] == 0 || gs->zp[2] == 0;
      if (run->ctx->params.tolerant && run->depth < (size_t)gs->loop) {
        LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
        gs->loop = 1;
        break;
      }
      if (rp1 >= z0->count) {
        /* No reference point: the points are taken off the stack and nothing is moved. */
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        run->depth -= (size_t)gs->loop;
        gs->loop = 1;
        break;
      }
      if (rp2 >= z1->count) {
        /* Some fonts call IP with a bad second reference point. The reference
           interpreter then takes both ranges as nothing, which leaves each point
           moved by how far it was from the first reference point to begin with. */
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        old_range = 0;
        cur_range = 0;
      }
      else {
      /* In the glyph zone the proportion is taken in font units, where it is
         exact, and applied to the current range once. */
      old_range = twilight
          ? gfnt_hint_dual_project(run, gfnt_hint_sub(z1->org[rp2].x, z0->org[rp1].x),
              gfnt_hint_sub(z1->org[rp2].y, z0->org[rp1].y))
          : gfnt_hint_dual_project(run,
              gfnt_hint_sub(z1->orus[rp2].x, z0->orus[rp1].x),
              gfnt_hint_sub(z1->orus[rp2].y, z0->orus[rp1].y));
      cur_range = gfnt_hint_current_distance(run, z1, rp2, z0, rp1);
      }
      for (int32_t k = 0; k < gs->loop; ++k) {
        int32_t p;
        int32_t org_dist;
        int32_t cur_dist;
        int32_t new_dist;
        NEED(1);
        p = POP();
        CHECK_POINT_OR(z2, p, continue);
        org_dist = twilight
            ? gfnt_hint_dual_project(run, gfnt_hint_sub(z2->org[p].x, z0->org[rp1].x),
                gfnt_hint_sub(z2->org[p].y, z0->org[rp1].y))
            : gfnt_hint_dual_project(run, gfnt_hint_sub(z2->orus[p].x, z0->orus[rp1].x),
                gfnt_hint_sub(z2->orus[p].y, z0->orus[rp1].y));
        cur_dist = gfnt_hint_current_distance(run, z2, (size_t)p, z0, rp1);
        if (org_dist != 0 && old_range != 0) {
          new_dist = gfnt_hint_muldiv(org_dist, cur_range, old_range);
        }
        else {
          /* The two reference points coincide along the projection vector, which
             the chapter calls invalid. FreeType then uses the original distance
             as it stands - font units, unscaled, in the glyph zone - and so does
             this, since a font can only mean what its renderer made of it. */
          new_dist = org_dist;
        }
        gfnt_hint_move(run, z2, (size_t)p, gfnt_hint_sub(new_dist, cur_dist));
      }
      gs->loop = 1;
      break;
    }
    case 0x46: case 0x47: { /* GC[a] */
      GFNT_HintZone * z2 = ZONE(2);
      int32_t p;
      NEED(1);
      p = POP();
      if (p < 0 || (size_t)p >= z2->count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        PUSH(0);
        break;
      }
      PUSH(op & 1 ? gfnt_hint_dual_project(run, z2->org[p].x, z2->org[p].y)
                  : gfnt_hint_project(run, z2->cur[p].x, z2->cur[p].y));
      break;
    }
    case 0x48: { /* SCFS */
      GFNT_HintZone * z2 = ZONE(2);
      int32_t value;
      int32_t p;
      NEED(2);
      value = POP();
      p = POP();
      CHECK_POINT_OR(z2, p, break);
      gfnt_hint_move(run, z2, (size_t)p, gfnt_hint_sub(value,
          gfnt_hint_project(run, z2->cur[p].x, z2->cur[p].y)));
      if (gs->zp[2] == 0) z2->org[p] = z2->cur[p];
      break;
    }
    case 0x49: case 0x4A: { /* MD[a]: a == 0 (0x49) measures the grid-fitted outline */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z1 = ZONE(1);
      int32_t p2;
      int32_t p1;
      NEED(2);
      p2 = POP();
      p1 = POP();
      if (p1 < 0 || (size_t)p1 >= z1->count || p2 < 0 || (size_t)p2 >= z0->count) {
        LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
        PUSH(0);
        break;
      }
      PUSH(op == 0x4A ? gfnt_hint_original_distance(run, z1, (size_t)p1, z0, (size_t)p2)
                  : gfnt_hint_current_distance(run, z1, (size_t)p1, z0, (size_t)p2));
      break;
    }
    case 0x80: { /* FLIPPT */
      GFNT_HintZone * z0 = ZONE(0);
      if (run->ctx->params.tolerant && run->depth < (size_t)gs->loop) {
        LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
        gs->loop = 1;
        break;
      }
      for (int32_t k = 0; k < gs->loop; ++k) {
        int32_t p;
        NEED(1);
        p = POP();
        CHECK_POINT_OR(z0, p, continue);
        z0->flags[p] ^= GFNT_HINT_ON_CURVE;
      }
      gs->loop = 1;
      break;
    }
    case 0x81: case 0x82: { /* FLIPRGON, FLIPRGOFF */
      GFNT_HintZone * z0 = ZONE(0);
      int32_t high;
      int32_t low;
      NEED(2);
      high = POP();
      low = POP();
      CHECK_POINT_OR(z0, high, break);
      CHECK_POINT_OR(z0, low, break);
      for (int32_t i = low; i <= high; ++i) {
        if (op == 0x81) z0->flags[i] |= GFNT_HINT_ON_CURVE;
        else z0->flags[i] &= (uint8_t)~GFNT_HINT_ON_CURVE;
      }
      break;
    }
    case 0x5D: case 0x71: case 0x72: { /* DELTAP1-3 */
      GFNT_HintZone * z0 = ZONE(0);
      int32_t n;
      int32_t first_ppem = gs->delta_base + 16 * (int32_t)(op == 0x5D ? 0 : op - 0x70);
      NEED(1);
      n = POP();
      if (n < 0) FAIL(GFNT_HINT_FAIL_BAD_ARGUMENT);
      while (n-- > 0) {
        int32_t p;
        int32_t arg;
        if (run->depth < 2u) {
          /* Fewer pairs than it was told of: the ones there are are done, and what is
             left over stays. */
          LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);
          break;
        }
        p = POP();
        arg = POP();
        CHECK_POINT_OR(z0, p, continue);
        if (gfnt_hint_compat(run) && !run->composite && !(z0->flags[p] & GFNT_HINT_TOUCH_Y)) {
          continue;
        }
        if ((int32_t)ctx->params.ppem == first_ppem + ((arg >> 4) & 15)) {
          gfnt_hint_move(run, z0, (size_t)p,
              gfnt_hint_delta_step((unsigned)(arg & 15), gs->delta_shift));
        }
      }
      break;
    }
    case 0x0F: { /* ISECT */
      GFNT_HintZone * z0 = ZONE(0);
      GFNT_HintZone * z2 = ZONE(2);
      int32_t b1;
      int32_t b0;
      int32_t a1;
      int32_t a0;
      int32_t p;
      int64_t dax, day, dbx, dby, denominator;
      NEED(5);
      b1 = POP(); b0 = POP(); a1 = POP(); a0 = POP(); p = POP();
      CHECK_POINT_OR(z2, p, break);
      CHECK_POINT_OR(z0, b1, break); CHECK_POINT_OR(z0, b0, break);
      CHECK_POINT_OR(z0, a1, break); CHECK_POINT_OR(z0, a0, break);
      dax = (int64_t)z0->cur[a1].x - z0->cur[a0].x;
      day = (int64_t)z0->cur[a1].y - z0->cur[a0].y;
      dbx = (int64_t)z0->cur[b1].x - z0->cur[b0].x;
      dby = (int64_t)z0->cur[b1].y - z0->cur[b0].y;
      denominator = dax * dby - day * dbx;
      /* Lines that cross at a grazing angle are treated as parallel: the
         intersection of nearly parallel lines is wherever rounding puts it. The
         cutoff is tan(angle) = 1/19, about three degrees (settled against
         FreeType: its choice flips between tangents of 0.0525 and 0.0530). */
      {
        int64_t dot = dax * dbx + day * dby;
        int64_t cross = denominator < 0 ? -denominator : denominator;
        int64_t along = dot < 0 ? -dot : dot;

        if (19 * cross <= along) {
          denominator = 0;
        }
      }
      if (denominator == 0) {
        /* Parallel: the midpoint of the four, as the chapter gives it. */
        z2->cur[p].x = (int32_t)(((int64_t)z0->cur[a0].x + z0->cur[a1].x
            + z0->cur[b0].x + z0->cur[b1].x) / 4);
        z2->cur[p].y = (int32_t)(((int64_t)z0->cur[a0].y + z0->cur[a1].y
            + z0->cur[b0].y + z0->cur[b1].y) / 4);
      }
      else {
        /* The chapter does not say how the quotient is rounded. The products
           are taken in whole pixels first, as 26.6 values times 26.6 values are
           otherwise too wide to keep, and the result is a0 plus a share of da. */
        int32_t ex = gfnt_hint_sub(z0->cur[b0].x, z0->cur[a0].x);
        int32_t ey = gfnt_hint_sub(z0->cur[b0].y, z0->cur[a0].y);
        int32_t det = gfnt_hint_sub(gfnt_hint_muldiv((int32_t)dax, (int32_t)dby, 64),
            gfnt_hint_muldiv((int32_t)day, (int32_t)dbx, 64));
        int32_t val = gfnt_hint_sub(gfnt_hint_muldiv(ex, (int32_t)dby, 64),
            gfnt_hint_muldiv(ey, (int32_t)dbx, 64));

        if (det == 0) {
          det = 1;
        }
        z2->cur[p].x = gfnt_hint_add(z0->cur[a0].x, gfnt_hint_muldiv(val, (int32_t)dax, det));
        z2->cur[p].y = gfnt_hint_add(z0->cur[a0].y, gfnt_hint_muldiv(val, (int32_t)day, det));
      }
      z2->flags[p] |= GFNT_HINT_TOUCH_X | GFNT_HINT_TOUCH_Y;
      break;
    }
    case 0x0A: case 0x0B: { /* SPVFS, SFVFS: y is on top */
      run->normalised = true;
      int32_t x;
      int32_t y;
      int16_t unit[2];
      NEED(2);
      y = POP();
      x = POP();
      gfnt_hint_unit((int16_t)x, (int16_t)y, unit);
      if (op == 0x0A) {
        gs->pv[0] = gs->dpv[0] = unit[0];
        gs->pv[1] = gs->dpv[1] = unit[1];
      }
      else {
        gs->fv[0] = unit[0];
        gs->fv[1] = unit[1];
      }
      break;
    }
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x86: case 0x87: {
      /* SPVTL, SFVTL, SDPVTL: a vector along (or across) the line p1 -> p2. */
      GFNT_HintZone * z1 = ZONE(1);
      GFNT_HintZone * z2 = ZONE(2);
      int32_t p2;
      int32_t p1;
      int16_t unit[2];
      run->normalised = true;
      int64_t dx;
      int64_t dy;
      NEED(2);
      p2 = POP();
      p1 = POP();
      CHECK_POINT_OR(z1, p2, break);
      CHECK_POINT_OR(z2, p1, break);
      for (int pass = 0; pass < (op >= 0x86 ? 2 : 1); ++pass) {
        const GFNT_HintPoint * a = pass == 0 || op < 0x86 ? &z2->cur[p1] : &z2->org[p1];
        const GFNT_HintPoint * b = pass == 0 || op < 0x86 ? &z1->cur[p2] : &z1->org[p2];
        /* From the point popped first to the one popped second, as FreeType
           runs it (the chapter's p2 and p1 are named the other way about). */
        dx = (int64_t)a->x - b->x;
        dy = (int64_t)a->y - b->y;
        if (dx == 0 && dy == 0) {
          dx = 0x4000;
          dy = 0;
        }
        else if (op & 1) {
          int64_t t = dx;
          dx = -dy;
          dy = t;
        }
        gfnt_hint_unit(dx, dy, unit);
        if (op == 0x06 || op == 0x07 || (op >= 0x86 && pass == 0)) {
          gs->pv[0] = unit[0];
          gs->pv[1] = unit[1];
          if (op < 0x86) { gs->dpv[0] = unit[0]; gs->dpv[1] = unit[1]; }
        }
        else if (op >= 0x86) {
          gs->dpv[0] = unit[0];
          gs->dpv[1] = unit[1];
        }
        else {
          gs->fv[0] = unit[0];
          gs->fv[1] = unit[1];
        }
      }
      break;
    }
    default:
      if (op >= 0xC0 && op < 0xE0) { /* MDRP[abcde] */
        GFNT_HintZone * z0 = ZONE(0);
        GFNT_HintZone * z1 = ZONE(1);
        uint32_t rp0 = gs->rp[0];
        int32_t p;
        int32_t org_dist;
        int32_t cur_dist;
        NEED(1);
        p = POP();
        if (p < 0 || (size_t)p >= z1->count || rp0 >= z0->count) {
          LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
          gs->rp[1] = gs->rp[0];
          gs->rp[2] = (uint32_t)p & 0xFFFFu;
          if (op & 0x10) gs->rp[0] = (uint32_t)p & 0xFFFFu;
          break;
        }
        org_dist = gfnt_hint_original_distance(run, z1, (size_t)p, z0, rp0);
        {
          int64_t apart = (int64_t)org_dist - gs->sw_value;
          if ((apart < 0 ? -apart : apart) < gs->sw_cut_in) {
            org_dist = org_dist >= 0 ? gs->sw_value : gfnt_hint_sub(0, gs->sw_value);
          }
        }
        {
          /* The side of the reference point the original outline puts it on
             decides which way the minimum distance pushes, and rounding to zero
             must not change that. */
          bool behind = org_dist < 0;

          if (op & 4) org_dist = gfnt_hint_round(gs, org_dist);
          if (op & 8) {
            if (!behind) { if (org_dist < gs->min_distance) org_dist = gs->min_distance; }
            else if (org_dist > -gs->min_distance) org_dist = gfnt_hint_sub(0, gs->min_distance);
          }
        }
        cur_dist = gfnt_hint_current_distance(run, z1, (size_t)p, z0, rp0);
        gfnt_hint_move(run, z1, (size_t)p, gfnt_hint_sub(org_dist, cur_dist));
        gs->rp[1] = gs->rp[0];
        gs->rp[2] = (uint32_t)p;
        if (op & 0x10) gs->rp[0] = (uint32_t)p;
        break;
      }
      if (op >= 0xE0) { /* MIRP[abcde] */
        GFNT_HintZone * z0 = ZONE(0);
        GFNT_HintZone * z1 = ZONE(1);
        uint32_t rp0 = gs->rp[0];
        int32_t n;
        int32_t p;
        int32_t cvt_dist;
        int32_t org_dist;
        int32_t cur_dist;
        int32_t distance;
        NEED(2);
        n = POP();
        p = POP();
        if (p < 0 || (size_t)p >= z1->count || rp0 >= z0->count
            || (n < -1 || (size_t)(n + 1) > run->cvt_count)
            || (!run->ctx->params.tolerant && n < 0)) {
          LENIENT(GFNT_HINT_FAIL_BAD_INDEX);
          gs->rp[1] = gs->rp[0];
          gs->rp[2] = (uint32_t)p & 0xFFFFu;
          if (op & 0x10) gs->rp[0] = (uint32_t)p & 0xFFFFu;
          break;
        }
        cvt_dist = n == -1 ? 0 : run->cvt[n];     /* index -1 is a distance of nothing */
        {
          int64_t apart = (int64_t)cvt_dist - gs->sw_value;
          if ((apart < 0 ? -apart : apart) < gs->sw_cut_in) {
            cvt_dist = cvt_dist >= 0 ? gs->sw_value : gfnt_hint_sub(0, gs->sw_value);
          }
        }
        if (gs->zp[1] == 0) {
          z1->org[p].x = gfnt_hint_add(z0->org[rp0].x, gfnt_hint_mul14(cvt_dist, gs->fv[0]));
          z1->org[p].y = gfnt_hint_add(z0->org[rp0].y, gfnt_hint_mul14(cvt_dist, gs->fv[1]));
          z1->cur[p] = z1->org[p];
        }
        org_dist = gfnt_hint_scaled_original_distance(run, z1, (size_t)p, z0, rp0);
        cur_dist = gfnt_hint_current_distance(run, z1, (size_t)p, z0, rp0);
        if (gs->auto_flip && ((org_dist ^ cvt_dist) < 0)) {
          cvt_dist = gfnt_hint_sub(0, cvt_dist);
        }
        if (op & 4) {
          int64_t apart = (int64_t)cvt_dist - org_dist;
          if ((apart < 0 ? -apart : apart) > gs->cvt_cut_in) cvt_dist = org_dist;
          distance = gfnt_hint_round(gs, cvt_dist);
        }
        else {
          distance = cvt_dist;
        }
        if (op & 8) {
          if (org_dist >= 0) { if (distance < gs->min_distance) distance = gs->min_distance; }
          else if (distance > -gs->min_distance) distance = gfnt_hint_sub(0, gs->min_distance);
        }
        gfnt_hint_move(run, z1, (size_t)p, gfnt_hint_sub(distance, cur_dist));
        gs->rp[1] = gs->rp[0];
        gs->rp[2] = (uint32_t)p;
        if (op & 0x10) gs->rp[0] = (uint32_t)p;
        break;
      }
      return GFNT_HINT_FAIL_UNSUPPORTED;
  }
  return GFNT_HINT_OK;

fault:
  return failure;
}
