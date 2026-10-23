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
 * The box HarfBuzz gives a glyph of a COLR version 1 font.
 *
 * Not the outline's: a color glyph is measured by its clip box, or with none by
 * what its paint graph draws, and glyph 0 is not measured at all. All three
 * were found by experiment against `hb-shape --show-extents`
 * (`tools/oracle/color_extents_diff.py` holds them there), and they matter
 * because this box is what places a mark when no `GPOS` does.
 *
 * What a graph draws is the union, over its leaf paints, of the clip each sits
 * inside: every PaintGlyph clips to its glyph's box carried through the
 * transforms above it, and a composite combines its two halves by what the mode
 * keeps. Arithmetic is in `float`, as HarfBuzz's is.
 */

#include <ghoti.io/font/color.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <stdint.h>
#include <string.h>
#include "../tables/tables.h"
#include "fallback.h"

#define GFNT_CEXT_DEPTH 80

/* libm is not linked by this library (an example links it before the archive and
 * gets none), and a rotation needs only a sine, so these are written out. */
static float fminf_(float a, float b) { return b < a ? b : a; }
static float fmaxf_(float a, float b) { return b > a ? b : a; }

/* Sine and cosine of an angle in radians, by Taylor series about the nearest
 * multiple of a quarter turn, in double: a float's 24 bits come out right. */
static void sincos_rad(float angle, float * s, float * c) {
  double t = (double)angle;
  double k = (double)(int64_t)(t / 1.57079632679489661923 + (t < 0 ? -0.5 : 0.5));
  double r = t - k * 1.57079632679489661923;     // |r| <= pi/4
  double r2 = r * r, sn = r, cs = 1.0, term_s = r, term_c = 1.0;

  for (int n = 1; n <= 9; n++) {
    term_s *= -r2 / ((2.0 * n) * (2.0 * n + 1.0));
    term_c *= -r2 / ((2.0 * n - 1.0) * (2.0 * n));
    sn += term_s;
    cs += term_c;
  }
  switch (((int64_t)k % 4 + 4) % 4) {
    case 0: *s = (float)sn; *c = (float)cs; break;
    case 1: *s = (float)cs; *c = (float)-sn; break;
    case 2: *s = (float)-sn; *c = (float)-cs; break;
    default: *s = (float)-cs; *c = (float)sn; break;
  }
}

static float tan_rad(float angle) {
  float s, c;

  sincos_rad(angle, &s, &c);
  return s / c;
}

typedef struct CBox {
  float x0, y0, x1, y1;
} CBox;

typedef struct Affine {
  float xx, yx, xy, yy, dx, dy;
} Affine;

typedef struct Level {
  GFNT_PaintFormat format;
  uint8_t mode;
  int index;          // this node's place among its parent's children
  int children;
  Affine m;           // this node's coordinates to the glyph's
  CBox clip;
  bool bounded;       // whether a PaintGlyph above has clipped
  CBox acc;           // what the node has painted (a composite: its backdrop)
  CBox src;           // a composite's source
} Level;

typedef struct Ctx {
  const GFNT_Face * face;
  const GFNT_Variation * variation;
  Level lv[GFNT_CEXT_DEPTH];
  bool failed;
} Ctx;

static bool box_empty(const CBox * b) {
  return !(b->x0 < b->x1 && b->y0 < b->y1);
}

static CBox box_none(void) {
  CBox b = {0, 0, 0, 0};
  return b;
}

static CBox box_union(CBox a, CBox b) {
  if (box_empty(&a)) {
    return b;
  }
  if (box_empty(&b)) {
    return a;
  }
  a.x0 = fminf_(a.x0, b.x0);
  a.y0 = fminf_(a.y0, b.y0);
  a.x1 = fmaxf_(a.x1, b.x1);
  a.y1 = fmaxf_(a.y1, b.y1);
  return a;
}

/* The box a version 0 layer's glyph contributes: its drawn outline, in float, with
 * composites carried through their transforms - not the header's box, which HarfBuzz
 * ignores (a header bumped by 50 units changes nothing) and which rounds a scaled
 * component. The outline is the placed one, with its origin at zero (a `glyf` glyph
 * whose left side bearing is not its xMin is drawn shifted by the difference, as
 * HarfBuzz draws it), so nothing is added here for it. A glyph with no outline
 * contributes the box from (-1, -1) to (0, 0), not nothing: a glyph of two empty
 * layers measures 1 by 1 below and left of the origin, and an empty layer beside a
 * drawn one stretches its box to (-1, -1). */
static bool layer_box(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, CBox * out) {
  GFNT_Outline * outline = NULL;
  GFNT_Box box;
  GFNT_Result r;

  if (gfnt_face_glyph_outline(face, glyph, variation, NULL, &outline, NULL) != GFNT_OK
      && (!variation || !variation->count
          || gfnt_face_glyph_outline(face, glyph, NULL, NULL, &outline, NULL)
              != GFNT_OK)) {
    return false;
  }
  r = gfnt_outline_control_box(outline, &box);
  gfnt_outline_destroy(outline);
  if (r != GFNT_OK) {
    return false;
  }
  if (gfnt_box_is_empty(&box)) {
    *out = (CBox){-1.0f, -1.0f, 0.0f, 0.0f};
    return true;
  }
  *out = (CBox){(float)box.x_min / 64.0f, (float)box.y_min / 64.0f,
      (float)box.x_max / 64.0f, (float)box.y_max / 64.0f};
  return true;
}

static CBox box_intersect(CBox a, CBox b) {
  a.x0 = fmaxf_(a.x0, b.x0);
  a.y0 = fmaxf_(a.y0, b.y0);
  a.x1 = fminf_(a.x1, b.x1);
  a.y1 = fminf_(a.y1, b.y1);
  return box_empty(&a) ? box_none() : a;
}

/* outer applied after inner. */
static Affine compose(Affine o, Affine i) {
  Affine r;

  r.xx = o.xx * i.xx + o.xy * i.yx;
  r.xy = o.xx * i.xy + o.xy * i.yy;
  r.dx = o.xx * i.dx + o.xy * i.dy + o.dx;
  r.yx = o.yx * i.xx + o.yy * i.yx;
  r.yy = o.yx * i.xy + o.yy * i.yy;
  r.dy = o.yx * i.dx + o.yy * i.dy + o.dy;
  return r;
}

static Affine translate(float dx, float dy) {
  Affine a = {1, 0, 0, 1, dx, dy};
  return a;
}

static CBox transform_box(Affine m, float x0, float y0, float x1, float y1) {
  float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
  CBox b = {1e30f, 1e30f, -1e30f, -1e30f};

  for (int k = 0; k < 4; k++) {
    float x = m.xx * xs[k] + m.xy * ys[k] + m.dx;
    float y = m.yx * xs[k] + m.yy * ys[k] + m.dy;

    b.x0 = fminf_(b.x0, x);
    b.y0 = fminf_(b.y0, y);
    b.x1 = fmaxf_(b.x1, x);
    b.y1 = fmaxf_(b.y1, y);
  }
  return b;
}

#define F14(v) ((float)(v) / 16384.0f)
#define F16(v) ((float)(v) / 65536.0f)

#define HALF_TURN_F 3.14159265358979323846f

/* Multiply the paint's transform onto @p m, one factor at a time and in the order
 * HarfBuzz pushes them (translate to the center, the operation, translate back):
 * the same matrix as multiplying the factors first, but not the same floats. */
static bool paint_apply(const GFNT_ColorPaint * p, Affine * m) {
  const int32_t * v = p->v;
  float cx = 0, cy = 0;
  Affine op = {1, 0, 0, 1, 0, 0};
  bool center = false;

  switch (p->format) {
    case GFNT_PAINT_TRANSFORM:
    case GFNT_PAINT_VAR_TRANSFORM:
      op = (Affine){F16(v[0]), F16(v[1]), F16(v[2]), F16(v[3]), F16(v[4]), F16(v[5])};
      break;
    case GFNT_PAINT_TRANSLATE:
    case GFNT_PAINT_VAR_TRANSLATE:
      op = translate((float)v[0], (float)v[1]);
      break;
    case GFNT_PAINT_SCALE:
    case GFNT_PAINT_VAR_SCALE:
      op.xx = F14(v[0]);
      op.yy = F14(v[1]);
      break;
    case GFNT_PAINT_SCALE_AROUND_CENTER:
    case GFNT_PAINT_VAR_SCALE_AROUND_CENTER:
      op.xx = F14(v[0]);
      op.yy = F14(v[1]);
      cx = (float)v[2];
      cy = (float)v[3];
      center = true;
      break;
    case GFNT_PAINT_SCALE_UNIFORM:
    case GFNT_PAINT_VAR_SCALE_UNIFORM:
      op.xx = op.yy = F14(v[0]);
      break;
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_VAR_SCALE_UNIFORM_AROUND_CENTER:
      op.xx = op.yy = F14(v[0]);
      cx = (float)v[1];
      cy = (float)v[2];
      center = true;
      break;
    case GFNT_PAINT_ROTATE:
    case GFNT_PAINT_VAR_ROTATE:
    case GFNT_PAINT_ROTATE_AROUND_CENTER:
    case GFNT_PAINT_VAR_ROTATE_AROUND_CENTER: {
      float s, c;

      sincos_rad(F14(v[0]) * HALF_TURN_F, &s, &c);
      op = (Affine){c, s, -s, c, 0, 0};
      if (p->format == GFNT_PAINT_ROTATE_AROUND_CENTER
          || p->format == GFNT_PAINT_VAR_ROTATE_AROUND_CENTER) {
        cx = (float)v[1];
        cy = (float)v[2];
        center = true;
      }
      break;
    }
    case GFNT_PAINT_SKEW:
    case GFNT_PAINT_VAR_SKEW:
    case GFNT_PAINT_SKEW_AROUND_CENTER:
    case GFNT_PAINT_VAR_SKEW_AROUND_CENTER:
      op = (Affine){1, tan_rad(F14(v[1]) * HALF_TURN_F),
          -tan_rad(F14(v[0]) * HALF_TURN_F), 1, 0, 0};
      if (p->format == GFNT_PAINT_SKEW_AROUND_CENTER
          || p->format == GFNT_PAINT_VAR_SKEW_AROUND_CENTER) {
        cx = (float)v[2];
        cy = (float)v[3];
        center = true;
      }
      break;
    default:
      return false;
  }
  if (center) {
    *m = compose(*m, translate(cx, cy));
  }
  *m = compose(*m, op);
  if (center) {
    *m = compose(*m, translate(-cx, -cy));
  }
  return true;
}

/* What is left of @p src composited onto @p dst by @p mode, as a box. */
static CBox combine(uint8_t mode, CBox src, CBox dst) {
  switch (mode) {
    case 0:                                  // clear
      return box_none();
    case 1:                                  // source
    case 7:                                  // source out
      return src;
    case 2:                                  // destination
    case 8:                                  // destination out
      return dst;
    case 5:                                  // source in
    case 6:                                  // destination in
      return box_intersect(src, dst);
    default:       // over, atop both ways, xor, plus, and the blend modes: both halves
      return box_union(src, dst);
  }
}

static bool enter(void * user, const GFNT_ColorPaint * p, size_t depth) {
  Ctx * c = user;
  Level * l;

  if (depth >= GFNT_CEXT_DEPTH) {
    c->failed = true;
    return false;
  }
  l = &c->lv[depth];
  memset(l, 0, sizeof *l);
  l->format = p->format;
  l->mode = p->composite_mode;
  if (depth == 0) {
    l->m = translate(0, 0);
  }
  else {
    Level * parent = &c->lv[depth - 1];

    l->index = parent->children++;
    l->m = parent->m;
    l->clip = parent->clip;
    l->bounded = parent->bounded;
  }
  if (paint_apply(p, &l->m)) {
    return true;
  }
  switch (p->format) {
    case GFNT_PAINT_GLYPH: {
      CBox b, box;

      if (!layer_box(c->face, p->glyph, c->variation, &b)) {
        c->failed = true;
        return false;
      }
      box = transform_box(l->m, b.x0, b.y0, b.x1, b.y1);
      l->clip = l->bounded ? box_intersect(l->clip, box) : box;
      l->bounded = true;
      return true;
    }
    case GFNT_PAINT_SOLID:
    case GFNT_PAINT_VAR_SOLID:
    case GFNT_PAINT_LINEAR_GRADIENT:
    case GFNT_PAINT_VAR_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT:
    case GFNT_PAINT_VAR_RADIAL_GRADIENT:
    case GFNT_PAINT_SWEEP_GRADIENT:
    case GFNT_PAINT_VAR_SWEEP_GRADIENT:
      // Unclipped paint covers the plane, which is no box at all.
      if (!l->bounded) {
        c->failed = true;
        return false;
      }
      l->acc = l->clip;
      return true;
    default:
      return true;
  }
}

static void leave(void * user, const GFNT_ColorPaint * p, size_t depth) {
  Ctx * c = user;
  Level * l = &c->lv[depth];

  (void)p;
  if (l->format == GFNT_PAINT_COMPOSITE) {
    // The walk gives the source first, then the backdrop (held in acc).
    l->acc = combine(l->mode, l->src, l->acc);
  }
  if (depth > 0) {
    Level * parent = &c->lv[depth - 1];

    if (parent->format == GFNT_PAINT_COMPOSITE && l->index == 0) {
      parent->src = l->acc;
    }
    else {
      parent->acc = box_union(parent->acc, l->acc);
    }
  }
}

/* The box as HarfBuzz states it: truncated toward zero, not floored and ceiled,
 * so a box that is 868.8 high is 868 and -99.5 is -99. */
static bool finish(CBox b, GFNT_Extents * out, bool * handled) {
  memset(out, 0, sizeof *out);
  if (!box_empty(&b)) {
    out->x_bearing = (int32_t)b.x0;
    out->y_bearing = (int32_t)b.y1;
    out->width = (int32_t)(b.x1 - b.x0);
    out->height = (int32_t)(b.y0 - b.y1);
  }
  *handled = true;
  return true;
}

bool gfnt_color_extents(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_Extents * out, bool * handled) {
  uint16_t version = 0;
  GFNT_PaintRef root = 0;
  GFNT_ColorBox clip;
  bool found = false;
  Ctx c;
  GFNT_PaintVisitor visitor = {enter, leave};
  GFNT_Result r;

  *handled = false;
  if (gfnt_face_color_version(face, &version, NULL) != GFNT_OK || version > 1) {
    return true;
  }
  memset(out, 0, sizeof *out);
  if (version == 0
      || gfnt_face_color_base_paint(face, glyph, &root, NULL) != GFNT_OK || !root) {
    // Version 0 layers, in a version 0 table or a version 1 table that carries them
    // for a glyph, are measured as the union of their glyphs' drawn boxes.
    size_t layers = 0;

    if (gfnt_face_color_layer_count(face, glyph, &layers, NULL) == GFNT_OK && layers) {
      CBox u = box_none();

      for (size_t i = 0; i < layers; i++) {
        GFNT_ColorLayer layer;
        CBox b;

        if (gfnt_face_color_layer(face, glyph, i, &layer, NULL) != GFNT_OK
            || !layer_box(face, layer.glyph, variation, &b)) {
          return true;
        }
        u = box_union(u, b);
      }
      return finish(u, out, handled);
    }
    if (glyph == 0 && version == 1) {
      // HarfBuzz measures nothing at glyph 0 of a version 1 font unless the font
      // gives it a paint or layers of its own (Bungee Spice does, and is measured).
      *handled = true;
    }
    return true;
  }
  if (gfnt_face_color_clip(face, glyph, variation, &clip, &found, NULL) == GFNT_OK
      && found) {
    out->x_bearing = clip.x_min;
    out->y_bearing = clip.y_max;
    out->width = clip.x_max - clip.x_min;
    out->height = clip.y_min - clip.y_max;
    *handled = true;
    return true;
  }
  memset(&c, 0, sizeof c);
  c.face = face;
  c.variation = variation;
  r = gfnt_face_color_paint_walk(face, glyph, variation, &visitor, &c, NULL);
  if (r == GFNT_ERR_LIMIT) {
    // A graph that names itself paints nothing past the repeat, and
    // HarfBuzz measures the pair in Google's test font as nothing at all.
    *handled = true;
    return true;
  }
  if (r != GFNT_OK || c.failed) {
    return true;
  }
  return finish(c.lv[0].acc, out, handled);
}
