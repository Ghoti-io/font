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
 */

/*
 * FreeType's answer for every glyph's outline at a location in a variable font's
 * design space, in the spelling `font-outline` prints.
 *
 * Usage: freetype-variation <font> <face> <stride> <first> [spec]...
 *
 * Each `spec` is `<user location>@<normalised coordinates>`: the user location is
 * `wght=700,opsz=14` and the normalised coordinates are the 2.14 integers **this
 * library** computed for it, comma-separated, or empty. Two things come out of
 * each:
 *
 *   * `variation:` - the coordinates **FreeType's own** normalisation makes of the
 *     user location (`FT_Set_Var_Design_Coordinates`, which applies `avar`),
 *     rounded to 2.14 the way FreeType stores them. This library does the same
 *     arithmetic on purpose, so the comparison is exact and not a tolerance.
 *   * the outlines, drawn at the **normalised coordinates it was handed**
 *     (`FT_Set_Var_Blend_Coordinates`) rather than at its own, so that what is
 *     compared is `gvar`'s arithmetic and not the first comparison's rounding
 *     carried into every point.
 *
 * Drawn **unhinted at one pixel per font unit**: a pixel size equal to the em
 * gives a scale of exactly one, so a coordinate comes out as font units times 64
 * in 26.6 - the same space this library's outlines are in - and nothing is
 * rounded to a pixel on the way. FreeType keeps a variation's fractional part
 * through that scaling; whether it does is the thing being measured.
 *
 * This image links **only FreeType**, as every driver here does: the reference
 * must not be able to reach the implementation it answers for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H

typedef struct Context {
  unsigned long glyph;
  int open;
} Context;

static int on_move(const FT_Vector * to, void * user) {
  Context * context = user;

  if (context->open) {
    printf("glyph %lu path: close\n", context->glyph);
  }
  context->open = 1;
  printf("glyph %lu path: move %ld %ld\n", context->glyph, (long)to->x,
      (long)to->y);
  return 0;
}

static int on_line(const FT_Vector * to, void * user) {
  Context * context = user;

  printf("glyph %lu path: line %ld %ld\n", context->glyph, (long)to->x,
      (long)to->y);
  return 0;
}

static int on_conic(const FT_Vector * control, const FT_Vector * to,
    void * user) {
  Context * context = user;

  printf("glyph %lu path: quad %ld %ld %ld %ld\n", context->glyph,
      (long)control->x, (long)control->y, (long)to->x, (long)to->y);
  return 0;
}

static int on_cubic(const FT_Vector * first, const FT_Vector * second,
    const FT_Vector * to, void * user) {
  Context * context = user;

  printf("glyph %lu path: cubic %ld %ld %ld %ld %ld %ld\n", context->glyph,
      (long)first->x, (long)first->y, (long)second->x, (long)second->y,
      (long)to->x, (long)to->y);
  return 0;
}

/** 16.16 to 2.14 the way FreeType does: add two, then an arithmetic shift. */
static long to_f2dot14(FT_Fixed value) {
  return (long)((value + 2) >> 2);
}

int main(int argc, char ** argv) {
  FT_Library library;
  FT_Face face;
  FT_MM_Var * variations = NULL;
  const FT_Outline_Funcs funcs = { on_move, on_line, on_conic, on_cubic, 0, 0 };
  FT_Fixed design[64];
  FT_Fixed blend[64];
  FT_UInt axes;
  long stride;
  long first;

  if (argc < 5) {
    fprintf(stderr, "usage: %s <font> <face> <stride> <first> [spec]...\n",
        argv[0]);
    return 2;
  }
  stride = strtol(argv[3], NULL, 10);
  first = strtol(argv[4], NULL, 10);
  if (stride < 1) {
    stride = 1;
  }
  if (FT_Init_FreeType(&library) != 0) {
    fprintf(stderr, "FT_Init_FreeType failed\n");
    return 1;
  }
  if (FT_New_Face(library, argv[1], strtol(argv[2], NULL, 10), &face) != 0) {
    fprintf(stderr, "FT_New_Face failed\n");
    return 1;
  }
  if (!FT_HAS_MULTIPLE_MASTERS(face)
      || FT_Get_MM_Var(face, &variations) != 0 || !variations
      || variations->num_axis > 64) {
    printf("outlines: no variation\n");
    return 0;
  }
  axes = variations->num_axis;
  for (FT_UInt i = 0; i < axes; ++i) {
    printf("axis: %.4s %ld %ld %ld\n", (const char *)&(char[5]){
        (char)(variations->axis[i].tag >> 24), (char)(variations->axis[i].tag >> 16),
        (char)(variations->axis[i].tag >> 8), (char)variations->axis[i].tag, 0 },
        (long)variations->axis[i].minimum, (long)variations->axis[i].def,
        (long)variations->axis[i].maximum);
  }
  if (FT_Set_Pixel_Sizes(face, 0, face->units_per_EM) != 0) {
    fprintf(stderr, "FT_Set_Pixel_Sizes failed\n");
    return 1;
  }
  printf("outlines: %ld glyph(s), stride %ld from %ld\n", (long)face->num_glyphs,
      stride, first);

  for (int number = 5; number < argc; ++number) {
    char * spec = argv[number];
    char * at = strchr(spec, '@');
    char * given = at ? at + 1 : NULL;
    char text[512];
    char * cursor;

    if (at) {
      *at = '\0';
    }
    if (strlen(spec) >= sizeof text) {
      fprintf(stderr, "location too long\n");
      return 2;
    }
    memcpy(text, spec, strlen(spec) + 1);
    // Every axis starts at its default, so a location that names one axis of
    // several leaves the rest where the designer put them.
    for (FT_UInt i = 0; i < axes; ++i) {
      design[i] = variations->axis[i].def;
    }
    for (cursor = strtok(text, ","); cursor; cursor = strtok(NULL, ",")) {
      char * equals = strchr(cursor, '=');
      double value;

      if (!equals || equals - cursor != 4) {
        fprintf(stderr, "bad location item '%s'\n", cursor);
        return 2;
      }
      value = strtod(equals + 1, NULL);
      for (FT_UInt i = 0; i < axes; ++i) {
        FT_ULong tag = variations->axis[i].tag;

        if ((char)(tag >> 24) == cursor[0] && (char)(tag >> 16) == cursor[1]
            && (char)(tag >> 8) == cursor[2] && (char)tag == cursor[3]) {
          design[i] = (FT_Fixed)(value * 65536.0 + (value < 0 ? -0.5 : 0.5));
        }
      }
    }
    printf("location %d: %s\n", number - 5, spec);
    if (FT_Set_Var_Design_Coordinates(face, axes, design) != 0
        || FT_Get_Var_Blend_Coordinates(face, axes, blend) != 0) {
      printf("outlines: refused the location\n");
      continue;
    }
    printf("variation: %u axes, normalised", axes);
    for (FT_UInt i = 0; i < axes; ++i) {
      printf(" %ld", to_f2dot14(blend[i]));
    }
    printf("\n");
    if (given && *given) {
      // The coordinates this library computed, as 2.14: widened to the 16.16 that
      // FT_Set_Var_Blend_Coordinates takes, which is exact.
      FT_UInt i = 0;
      char * item;

      for (item = strtok(given, ","); item && i < axes;
          item = strtok(NULL, ","), ++i) {
        blend[i] = (FT_Fixed)strtol(item, NULL, 10) * 4;
      }
      if (FT_Set_Var_Blend_Coordinates(face, axes, blend) != 0) {
        printf("outlines: refused the coordinates\n");
        continue;
      }
    }
    for (long glyph = first; glyph < (long)face->num_glyphs; glyph += stride) {
      Context context;
      FT_Error error = FT_Load_Glyph(face, (FT_UInt)glyph,
          FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT);

      context.glyph = (unsigned long)glyph;
      context.open = 0;
      if (error != 0) {
        printf("glyph %ld: reference refused %d\n", glyph, (int)error);
        continue;
      }
      if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        continue;
      }
      if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &context) != 0) {
        printf("glyph %ld: reference refused decompose\n", glyph);
        continue;
      }
      if (context.open) {
        printf("glyph %lu path: close\n", context.glyph);
      }
    }
  }
  FT_Done_MM_Var(library, variations);
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}
