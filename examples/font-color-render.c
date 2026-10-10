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
 * Draw a color font: a small CPU compositor over the library's paint graph.
 *
 * Usage: font-color-render [--size PX] [--palette N] [--location AXIS=V,...]
 *            [--bg RRGGBB] [--width PX] [--text STRING | --glyphs ID,ID,...]
 *            --out file.ppm <font>
 *
 * **The library does not draw color.** It reads COLR and CPAL and hands back the
 * paint graph; compositing it is the caller's job (design.md section 7.6). This
 * example is such a caller, written for clarity rather than speed: it walks the
 * graph itself with gfnt_face_color_paint(), fills each PaintGlyph's outline,
 * rasterized by the library, into a coverage mask that clips everything beneath
 * it, and evaluates the gradients per pixel. Surfaces are premultiplied float
 * RGBA, so a PaintComposite is two surfaces and a Porter-Duff or blend formula.
 * Version 0 glyphs are layers filled in order, and a glyph with neither is drawn
 * in the text color.
 *
 * Not done: the non-separable blend modes (hue, saturation, color, luminosity)
 * draw as source-over, gradients interpolate unpremultiplied, and there is no
 * anti-aliasing of a gradient's own edge beyond the clip's.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

typedef struct Affine {
  double xx, yx, xy, yy, dx, dy;   // x' = xx*x + xy*y + dx;  y' = yx*x + yy*y + dy
} Affine;

typedef struct Surface {
  float * px;                      // premultiplied RGBA
} Surface;

typedef struct R {
  const GFNT_Face * face;
  const GFNT_Variation * at;
  int w, h;
  double scale;                    // pixels per font unit
  uint16_t upem;
  double ox, oy;                   // this glyph's origin on the canvas, y down
  size_t palette;
  float fg[4];
  bool warned;
} R;

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

static Affine translate(double dx, double dy) {
  return (Affine){1, 0, 0, 1, dx, dy};
}

static bool invert(Affine m, Affine * out) {
  double det = m.xx * m.yy - m.xy * m.yx;

  if (fabs(det) < 1e-12) {
    return false;
  }
  out->xx = m.yy / det;
  out->xy = -m.xy / det;
  out->yx = -m.yx / det;
  out->yy = m.xx / det;
  out->dx = -(out->xx * m.dx + out->xy * m.dy);
  out->dy = -(out->yx * m.dx + out->yy * m.dy);
  return true;
}

#define F14(v) ((double)(v) / 16384.0)
#define F16(v) ((double)(v) / 65536.0)

/* The matrix a transform paint stands for (the COLR specification's). */
static bool paint_matrix(const GFNT_ColorPaint * p, Affine * m) {
  const int32_t * v = p->v;
  double cx = 0, cy = 0;
  Affine op = {1, 0, 0, 1, 0, 0};
  bool center = false;

  switch (p->format) {
    case GFNT_PAINT_TRANSFORM: case GFNT_PAINT_VAR_TRANSFORM:
      op = (Affine){F16(v[0]), F16(v[1]), F16(v[2]), F16(v[3]), F16(v[4]), F16(v[5])};
      break;
    case GFNT_PAINT_TRANSLATE: case GFNT_PAINT_VAR_TRANSLATE:
      op = translate(v[0], v[1]);
      break;
    case GFNT_PAINT_SCALE_AROUND_CENTER: case GFNT_PAINT_VAR_SCALE_AROUND_CENTER:
      cx = v[2]; cy = v[3]; center = true;
      // fall through
    case GFNT_PAINT_SCALE: case GFNT_PAINT_VAR_SCALE:
      op.xx = F14(v[0]);
      op.yy = F14(v[1]);
      break;
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_VAR_SCALE_UNIFORM_AROUND_CENTER:
      cx = v[1]; cy = v[2]; center = true;
      // fall through
    case GFNT_PAINT_SCALE_UNIFORM: case GFNT_PAINT_VAR_SCALE_UNIFORM:
      op.xx = op.yy = F14(v[0]);
      break;
    case GFNT_PAINT_ROTATE_AROUND_CENTER: case GFNT_PAINT_VAR_ROTATE_AROUND_CENTER:
      cx = v[1]; cy = v[2]; center = true;
      // fall through
    case GFNT_PAINT_ROTATE: case GFNT_PAINT_VAR_ROTATE: {
      double a = F14(v[0]) * PI;

      op = (Affine){cos(a), sin(a), -sin(a), cos(a), 0, 0};
      break;
    }
    case GFNT_PAINT_SKEW_AROUND_CENTER: case GFNT_PAINT_VAR_SKEW_AROUND_CENTER:
      cx = v[2]; cy = v[3]; center = true;
      // fall through
    case GFNT_PAINT_SKEW: case GFNT_PAINT_VAR_SKEW:
      op = (Affine){1, tan(F14(v[1]) * PI), -tan(F14(v[0]) * PI), 1, 0, 0};
      break;
    default:
      return false;
  }
  if (center) {
    op = compose(translate(cx, cy), compose(op, translate(-cx, -cy)));
  }
  *m = op;
  return true;
}

/* ---------------------------------------------------------- compositing */

static void over(float * dst, const float * src) {
  float k = 1.0f - src[3];

  for (int c = 0; c < 4; c++) {
    dst[c] = src[c] + dst[c] * k;
  }
}

static float blend_fn(int mode, float cb, float cs) {
  switch (mode) {
    case 13: return cb + cs - cb * cs;                                // screen
    case 14: return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb); // overlay (hard light, swapped)
    case 15: return cs < cb ? cs : cb;                                // darken
    case 16: return cs > cb ? cs : cb;                                // lighten
    case 17: return cb == 0 ? 0 : cs >= 1 ? 1 : fminf(1, cb / (1 - cs)); // color dodge
    case 18: return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - fminf(1, (1 - cb) / cs); // color burn
    case 19: return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb); // hard light
    case 20: {                                                        // soft light
      if (cs <= 0.5f) {
        return cb - (1 - 2 * cs) * cb * (1 - cb);
      }
      float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
      return cb + (2 * cs - 1) * (d - cb);
    }
    case 21: return fabsf(cb - cs);                                   // difference
    case 22: return cb + cs - 2 * cb * cs;                            // exclusion
    case 23: return cb * cs;                                          // multiply
    default: return cs;
  }
}

/* One pixel of source composited onto backdrop by a COLR composite mode. */
static void composite_pixel(R * r, int mode, const float * s, const float * b, float * out) {
  float as = s[3], ab = b[3], fa, fb;

  if (mode >= 13 && mode <= 23) {
    float cs[3], cb[3];

    for (int c = 0; c < 3; c++) {
      cs[c] = as > 0 ? s[c] / as : 0;
      cb[c] = ab > 0 ? b[c] / ab : 0;
      out[c] = (1 - ab) * s[c] + (1 - as) * b[c] + as * ab * blend_fn(mode, cb[c], cs[c]);
    }
    out[3] = as + ab - as * ab;
    return;
  }
  switch (mode) {
    case 0: fa = 0; fb = 0; break;
    case 1: fa = 1; fb = 0; break;
    case 2: fa = 0; fb = 1; break;
    case 3: fa = 1; fb = 1 - as; break;
    case 4: fa = 1 - ab; fb = 1; break;
    case 5: fa = ab; fb = 0; break;
    case 6: fa = 0; fb = as; break;
    case 7: fa = 1 - ab; fb = 0; break;
    case 8: fa = 0; fb = 1 - as; break;
    case 9: fa = ab; fb = 1 - as; break;
    case 10: fa = 1 - ab; fb = as; break;
    case 11: fa = 1 - ab; fb = 1 - as; break;
    case 12: fa = 1; fb = 1; break;
    default:
      if (!r->warned) {
        fprintf(stderr, "note: composite mode %d drawn as source-over\n", mode);
        r->warned = true;
      }
      fa = 1; fb = 1 - as;
      break;
  }
  for (int c = 0; c < 4; c++) {
    float v = fa * s[c] + fb * b[c];

    out[c] = v > 1 ? 1 : v;
  }
}

/* --------------------------------------------------------------- colors */

static void palette_color(R * r, uint16_t index, double alpha, float * out) {
  GFNT_Color c;

  if (index == GFNT_COLOR_FOREGROUND) {
    memcpy(out, r->fg, sizeof r->fg);
    for (int k = 0; k < 4; k++) {
      out[k] *= (float)alpha;
    }
    return;
  }
  if (gfnt_face_palette_color(r->face, r->palette, index, &c, NULL) != GFNT_OK) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  float a = (c.alpha / 255.0f) * (float)alpha;

  out[0] = c.red / 255.0f * a;
  out[1] = c.green / 255.0f * a;
  out[2] = c.blue / 255.0f * a;
  out[3] = a;
}

/* The color line at t, unpremultiplied then premultiplied. */
static void line_color(R * r, const GFNT_ColorPaint * p, const GFNT_ColorLine * line,
    double t, float * out) {
  GFNT_ColorStop prev = {0}, next = {0};
  float c0[4], c1[4];
  size_t n = line->stop_count;

  switch (line->extend) {
    case GFNT_COLOR_EXTEND_REPEAT:
      t -= floor(t);
      break;
    case GFNT_COLOR_EXTEND_REFLECT: {
      double m = fmod(t, 2.0);

      if (m < 0) m += 2.0;
      t = m <= 1.0 ? m : 2.0 - m;
      break;
    }
    default:
      break;
  }
  out[0] = out[1] = out[2] = out[3] = 0;
  if (n == 0) {
    return;
  }
  (void)gfnt_face_color_stop(r->face, p, 0, r->at, &prev, NULL);
  if (t <= F14(prev.offset) || n == 1) {
    palette_color(r, prev.palette_index, F14(prev.alpha), out);
    return;
  }
  for (size_t k = 1; k < n; k++) {
    (void)gfnt_face_color_stop(r->face, p, k, r->at, &next, NULL);
    if (t <= F14(next.offset) || k == n - 1) {
      double span = F14(next.offset) - F14(prev.offset);
      double u = span > 0 ? (t - F14(prev.offset)) / span : 1.0;

      if (u > 1) u = 1;
      palette_color(r, prev.palette_index, F14(prev.alpha), c0);
      palette_color(r, next.palette_index, F14(next.alpha), c1);
      // Interpolate unpremultiplied, then premultiply.
      float a0 = c0[3], a1 = c1[3];
      float a = (float)((1 - u) * a0 + u * a1);

      for (int c = 0; c < 3; c++) {
        float u0 = a0 > 0 ? c0[c] / a0 : 0, u1 = a1 > 0 ? c1[c] / a1 : 0;

        out[c] = (float)((1 - u) * u0 + u * u1) * a;
      }
      out[3] = a;
      return;
    }
    prev = next;
  }
}

/* The gradient's parameter at node-space point (x, y); false where it paints nothing. */
static bool gradient_t(const GFNT_ColorPaint * p, double x, double y, double * t) {
  const int32_t * v = p->v;

  switch (p->format) {
    case GFNT_PAINT_LINEAR_GRADIENT: case GFNT_PAINT_VAR_LINEAR_GRADIENT: {
      double d1x = v[2] - v[0], d1y = v[3] - v[1];
      double d2x = v[4] - v[0], d2y = v[5] - v[1];
      double nx = -d2y, ny = d2x;               // isolines run along p0 -> p2
      double den = d1x * nx + d1y * ny;

      if (fabs(den) < 1e-9) return false;
      *t = ((x - v[0]) * nx + (y - v[1]) * ny) / den;
      return true;
    }
    case GFNT_PAINT_RADIAL_GRADIENT: case GFNT_PAINT_VAR_RADIAL_GRADIENT: {
      double c0x = v[0], c0y = v[1], r0 = v[2], c1x = v[3], c1y = v[4], r1 = v[5];
      double cdx = c1x - c0x, cdy = c1y - c0y, dr = r1 - r0;
      double px = x - c0x, py = y - c0y;
      double a = cdx * cdx + cdy * cdy - dr * dr;
      double b = px * cdx + py * cdy + r0 * dr;
      double c = px * px + py * py - r0 * r0;
      double best = 0;
      bool found = false;

      if (fabs(a) < 1e-9) {
        if (fabs(b) < 1e-9) return false;
        double s = c / (2 * b);

        if (r0 + s * dr >= 0) { best = s; found = true; }
      }
      else {
        double disc = b * b - a * c;

        if (disc < 0) return false;
        double sq = sqrt(disc), s1 = (b + sq) / a, s2 = (b - sq) / a;

        if (s1 < s2) { double tmp = s1; s1 = s2; s2 = tmp; }
        if (r0 + s1 * dr >= 0) { best = s1; found = true; }
        else if (r0 + s2 * dr >= 0) { best = s2; found = true; }
      }
      if (!found) return false;
      *t = best;
      return true;
    }
    default: {                                  // sweep
      double cx = v[0], cy = v[1];
      double start = F14(v[2]) * 180.0, end = F14(v[3]) * 180.0;
      double ang = atan2(y - cy, x - cx) * 180.0 / PI;     // y up: counterclockwise

      if (ang < 0) ang += 360.0;
      ang -= start;
      while (ang < 0) ang += 360.0;
      if (fabs(end - start) < 1e-9) return false;
      *t = ang / (end - start);
      return true;
    }
  }
}

/* -------------------------------------------------------------- painting */

static Surface new_surface(const R * r) {
  Surface s = {calloc((size_t)r->w * r->h * 4, sizeof(float))};

  if (!s.px) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  return s;
}

/* Fill every pixel the clip admits with a solid color or a gradient. */
static void fill(R * r, const GFNT_ColorPaint * p, Affine m, const float * clip,
    Surface dst) {
  Affine inv = {1, 0, 0, 1, 0, 0};
  GFNT_ColorLine line = {0};
  float solid[4];
  bool gradient = p->color_line != 0;

  if (gradient) {
    if (!invert(m, &inv) || gfnt_face_color_line(r->face, p, &line, NULL) != GFNT_OK) {
      return;
    }
  }
  else {
    palette_color(r, p->palette_index, F14(p->v[0]), solid);
  }
  for (int j = 0; j < r->h; j++) {
    for (int i = 0; i < r->w; i++) {
      float cover = clip ? clip[(size_t)j * r->w + i] : 1.0f;
      float src[4];

      if (cover <= 0) {
        continue;
      }
      if (gradient) {
        double fx = (i + 0.5 - r->ox) / r->scale, fy = (r->oy - (j + 0.5)) / r->scale;
        double qx = inv.xx * fx + inv.xy * fy + inv.dx;
        double qy = inv.yx * fx + inv.yy * fy + inv.dy;
        double t;

        if (!gradient_t(p, qx, qy, &t)) {
          continue;
        }
        line_color(r, p, &line, t, src);
      }
      else {
        memcpy(src, solid, sizeof src);
      }
      for (int c = 0; c < 4; c++) {
        src[c] *= cover;
      }
      over(&dst.px[((size_t)j * r->w + i) * 4], src);
    }
  }
}

/* The glyph's outline, carried through m, as a coverage mask over the canvas. */
static float * glyph_mask(R * r, uint32_t glyph, Affine m, const float * clip) {
  GFNT_Outline * outline = NULL;
  GFNT_Coverage cov;
  GFNT_RasterOptions options;
  double fx = r->ox - floor(r->ox);
  float * mask = calloc((size_t)r->w * r->h, sizeof(float));

  if (!mask) {
    exit(1);
  }
  if (gfnt_face_glyph_outline(r->face, glyph, r->at, NULL, &outline, NULL) != GFNT_OK) {
    return mask;
  }
  gfnt_outline_transform(outline, (GFNT_F16Dot16)lround(m.xx * 65536),
      (GFNT_F16Dot16)lround(m.xy * 65536), (GFNT_F16Dot16)lround(m.yx * 65536),
      (GFNT_F16Dot16)lround(m.yy * 65536), (GFNT_F26Dot6)lround(m.dx * 64),
      (GFNT_F26Dot6)lround(m.dy * 64));
  gfnt_outline_scale(outline, (GFNT_F16Dot16)lround(r->scale * 65536), NULL);
  memset(&options, 0, sizeof options);
  options.origin_x = (GFNT_F26Dot6)lround(fx * 64);
  if (gfnt_raster_outline(outline, &options, NULL, NULL, &cov, NULL) == GFNT_OK) {
    int x0 = (int)floor(r->ox) + cov.left, y0 = (int)lround(r->oy) - cov.top;

    for (uint32_t row = 0; row < cov.height; row++) {
      for (uint32_t col = 0; col < cov.width; col++) {
        int x = x0 + (int)col, y = y0 + (int)row;

        if (x >= 0 && y >= 0 && x < r->w && y < r->h) {
          float c = cov.data[row * cov.stride + col] / 255.0f;

          mask[(size_t)y * r->w + x] = c * (clip ? clip[(size_t)y * r->w + x] : 1.0f);
        }
      }
    }
    gfnt_coverage_destroy(&cov);
  }
  gfnt_outline_destroy(outline);
  return mask;
}

static int paint(R * r, GFNT_PaintRef ref, Affine m, const float * clip, Surface dst,
    int depth) {
  GFNT_ColorPaint p;
  Affine t;

  if (depth > 64 || gfnt_face_color_paint(r->face, ref, r->at, &p, NULL) != GFNT_OK) {
    return -1;
  }
  if (paint_matrix(&p, &t)) {
    return paint(r, p.child, compose(m, t), clip, dst, depth + 1);
  }
  switch (p.format) {
    case GFNT_PAINT_COLR_LAYERS:
      for (uint32_t k = 0; k < p.layer_count; k++) {
        GFNT_PaintRef layer;

        if (gfnt_face_color_layer_paint(r->face, p.first_layer + k, &layer, NULL)
                == GFNT_OK) {
          paint(r, layer, m, clip, dst, depth + 1);
        }
      }
      return 0;
    case GFNT_PAINT_SOLID: case GFNT_PAINT_VAR_SOLID:
    case GFNT_PAINT_LINEAR_GRADIENT: case GFNT_PAINT_VAR_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT: case GFNT_PAINT_VAR_RADIAL_GRADIENT:
    case GFNT_PAINT_SWEEP_GRADIENT: case GFNT_PAINT_VAR_SWEEP_GRADIENT:
      fill(r, &p, m, clip, dst);
      return 0;
    case GFNT_PAINT_GLYPH: {
      float * mask = glyph_mask(r, p.glyph, m, clip);
      int rc = paint(r, p.child, m, mask, dst, depth + 1);

      free(mask);
      return rc;
    }
    case GFNT_PAINT_COLR_GLYPH: {
      GFNT_PaintRef root = 0;

      if (gfnt_face_color_base_paint(r->face, p.glyph, &root, NULL) != GFNT_OK || !root) {
        return -1;
      }
      return paint(r, root, m, clip, dst, depth + 1);
    }
    case GFNT_PAINT_COMPOSITE: {
      Surface s = new_surface(r), b = new_surface(r);
      size_t n = (size_t)r->w * r->h;

      paint(r, p.child, m, clip, s, depth + 1);
      paint(r, p.backdrop, m, clip, b, depth + 1);
      for (size_t k = 0; k < n; k++) {
        float out[4];

        composite_pixel(r, p.composite_mode, &s.px[k * 4], &b.px[k * 4], out);
        over(&dst.px[k * 4], out);
      }
      free(s.px);
      free(b.px);
      return 0;
    }
    default:
      return -1;
  }
}

/* One glyph, drawn as the font says: v1 graph, else v0 layers, else text color. */
static void draw_glyph(R * r, uint32_t glyph, Surface canvas) {
  GFNT_PaintRef root = 0;
  size_t layers = 0;
  Affine id = {1, 0, 0, 1, 0, 0};

  if (gfnt_face_color_base_paint(r->face, glyph, &root, NULL) == GFNT_OK && root) {
    Surface s = new_surface(r);

    paint(r, root, id, NULL, s, 0);
    for (size_t k = 0; k < (size_t)r->w * r->h; k++) {
      over(&canvas.px[k * 4], &s.px[k * 4]);
    }
    free(s.px);
    return;
  }
  if (gfnt_face_color_layer_count(r->face, glyph, &layers, NULL) == GFNT_OK && layers) {
    for (size_t k = 0; k < layers; k++) {
      GFNT_ColorLayer layer;
      float color[4];
      float * mask;

      if (gfnt_face_color_layer(r->face, glyph, k, &layer, NULL) != GFNT_OK) {
        continue;
      }
      palette_color(r, layer.palette_index, 1.0, color);
      mask = glyph_mask(r, layer.glyph, id, NULL);
      for (size_t q = 0; q < (size_t)r->w * r->h; q++) {
        float src[4] = {color[0] * mask[q], color[1] * mask[q], color[2] * mask[q],
            color[3] * mask[q]};

        over(&canvas.px[q * 4], src);
      }
      free(mask);
    }
    return;
  }
  {
    float * mask = glyph_mask(r, glyph, id, NULL);

    for (size_t q = 0; q < (size_t)r->w * r->h; q++) {
      float src[4] = {r->fg[0] * mask[q], r->fg[1] * mask[q], r->fg[2] * mask[q],
          r->fg[3] * mask[q]};

      over(&canvas.px[q * 4], src);
    }
    free(mask);
  }
}

/* ------------------------------------------------------------------ main */

static int parse_location(const GFNT_Face * face, const char * text,
    GFNT_F2Dot14 * normalized, GFNT_Variation * variation) {
  GFNT_F16Dot16 user[64];
  GFNT_Axis axis;
  GFNT_Error error;
  char copy[512];
  char * cursor;
  size_t axes = 0;

  if (strlen(text) >= sizeof copy
      || gfnt_face_axis_count(face, &axes, &error) != GFNT_OK || axes > 64) {
    return -1;
  }
  memcpy(copy, text, strlen(text) + 1);
  for (size_t k = 0; k < axes; k++) {
    (void)gfnt_face_axis_at(face, k, &axis, &error);
    user[k] = axis.def;
  }
  for (cursor = strtok(copy, ","); cursor; cursor = strtok(NULL, ",")) {
    char * equals = strchr(cursor, '=');
    bool found = false;

    if (!equals || equals - cursor != 4) {
      return -1;
    }
    for (size_t k = 0; k < axes; k++) {
      (void)gfnt_face_axis_at(face, k, &axis, &error);
      if (axis.tag == GFNT_TAG(cursor[0], cursor[1], cursor[2], cursor[3])) {
        double value = strtod(equals + 1, NULL);

        user[k] = (GFNT_F16Dot16)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
        found = true;
      }
    }
    if (!found) {
      return -1;
    }
  }
  if (gfnt_face_normalize(face, user, axes, normalized, 64, &error) != GFNT_OK) {
    return -1;
  }
  memset(variation, 0, sizeof *variation);
  variation->coords = normalized;
  variation->count = axes;
  return 0;
}

static uint32_t next_codepoint(const unsigned char ** s) {
  const unsigned char * p = *s;
  uint32_t c = *p++;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

  if (extra) {
    c &= 0x3F >> extra;
    while (extra-- && (*p & 0xC0) == 0x80) {
      c = (c << 6) | (*p++ & 0x3F);
    }
  }
  *s = p;
  return c;
}

int main(int argc, char ** argv) {
  const char * path = NULL, * out_path = NULL, * text = NULL, * glyph_list = NULL;
  const char * location = NULL;
  double size = 128;
  int width = 1000;
  unsigned bg = 0xFFFFFF;
  size_t palette = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_F2Dot14 normalized[64];
  GFNT_Variation variation;
  uint32_t * glyphs = NULL;
  size_t count = 0;
  R r;
  uint16_t upem = 1000;

  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--size") && a + 1 < argc) size = atof(argv[++a]);
    else if (!strcmp(argv[a], "--palette") && a + 1 < argc) palette = (size_t)atoi(argv[++a]);
    else if (!strcmp(argv[a], "--location") && a + 1 < argc) location = argv[++a];
    else if (!strcmp(argv[a], "--bg") && a + 1 < argc) bg = (unsigned)strtoul(argv[++a], NULL, 16);
    else if (!strcmp(argv[a], "--width") && a + 1 < argc) width = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--text") && a + 1 < argc) text = argv[++a];
    else if (!strcmp(argv[a], "--glyphs") && a + 1 < argc) glyph_list = argv[++a];
    else if (!strcmp(argv[a], "--out") && a + 1 < argc) out_path = argv[++a];
    else if (argv[a][0] != '-') path = argv[a];
    else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
  }
  if (!path || !out_path || (!text && !glyph_list)) {
    fprintf(stderr, "usage: font-color-render [--size PX] [--palette N] [--location axes]"
        " [--bg RRGGBB] [--width PX] (--text STR | --glyphs ID,ID,...) --out f.ppm <font>\n");
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  gfnt_face_units_per_em(face, &upem, NULL);
  memset(&r, 0, sizeof r);
  r.face = face;
  r.upem = upem;
  r.scale = size / upem;
  r.palette = palette;
  r.fg[3] = 1;
  if (location) {
    if (parse_location(face, location, normalized, &variation) != 0) {
      fprintf(stderr, "bad --location\n");
      return 2;
    }
    r.at = &variation;
  }
  glyphs = calloc(strlen(text ? text : glyph_list) + 1, sizeof *glyphs);
  if (text) {
    for (const unsigned char * s = (const unsigned char *)text; *s;) {
      uint32_t g = 0;

      gfnt_face_glyph_for_codepoint(face, next_codepoint(&s), &g, NULL);
      glyphs[count++] = g;
    }
  }
  else {
    char * copy = strdup(glyph_list);

    for (char * tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
      glyphs[count++] = (uint32_t)atoi(tok);
    }
    free(copy);
  }

  // Flow: advance by each glyph's advance, wrap at --width.
  {
    int line_h = (int)(size * 1.25), pad = (int)(size * 0.1);
    double x = pad;
    int line = 0, max_x = 0;
    double * xs = calloc(count, sizeof *xs);
    int * ls = calloc(count, sizeof *ls);

    for (size_t k = 0; k < count; k++) {
      int32_t adv = upem;

      gfnt_face_glyph_advance(face, glyphs[k], r.at, &adv, NULL);
      if (x + adv * r.scale > width - pad && x > pad) {
        x = pad;
        line++;
      }
      xs[k] = x;
      ls[k] = line;
      x += adv * r.scale;
      if ((int)x > max_x) max_x = (int)x;
    }
    r.w = max_x + pad;
    r.h = (line + 1) * line_h + pad;
    Surface canvas = new_surface(&r);

    for (size_t k = 0; k < count; k++) {
      r.ox = xs[k];
      r.oy = ls[k] * line_h + size * 1.0 + pad / 2.0;
      draw_glyph(&r, glyphs[k], canvas);
    }
    FILE * out = fopen(out_path, "wb");

    if (!out) {
      perror(out_path);
      return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", r.w, r.h);
    for (size_t q = 0; q < (size_t)r.w * r.h; q++) {
      const float * s = &canvas.px[q * 4];
      float k = 1.0f - s[3];

      for (int c = 0; c < 3; c++) {
        float back = ((bg >> (16 - 8 * c)) & 0xFF) / 255.0f;
        float v = s[c] + back * k;

        fputc((int)(fminf(fmaxf(v, 0), 1) * 255.0f + 0.5f), out);
      }
    }
    fclose(out);
    free(canvas.px);
    free(xs);
    free(ls);
  }
  free(glyphs);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
