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
 * FreeType's answer for every glyph's advance, and the line metrics, at a
 * location in a variable font's design space, in the spelling `font-metrics`
 * prints.
 *
 * Usage: freetype-metrics <font> <face> <stride> <first> [spec]...
 *
 * Each `spec` is `<user location>@<normalised coordinates>`, as for
 * `freetype-variation`: the normalised coordinates are the 2.14 integers **this
 * library** computed, and are the ones the metrics are read at, so that what is
 * compared is `HVAR`'s and `MVAR`'s arithmetic and not normalisation's.
 *
 * The advance is read **unscaled** (`FT_LOAD_NO_SCALE`), which is font units, and
 * is FreeType's own whichever way the font states it: `HVAR`'s delta when there
 * is one, and the phantom points `gvar` carries when there is not.
 *
 * **A fresh face for every location, and one setting of the coordinates on it.**
 * FreeType applies `MVAR` to the values it keeps by adding a delta, and does not
 * undo it: a face moved to a location and then back to the default still reports
 * the first location's `OS/2` values (seen at 2.14.3: the window ascent stayed 20
 * up after returning to the default), and two settings on one face add their
 * deltas (the line metrics came out at about twice the delta, whenever the two
 * sets of coordinates differed by a unit of 2.14). A reference whose answer
 * depended on the order of the questions would be comparing this library with its
 * own history. So FreeType's own normalisation is read from one face, which is
 * then dropped, and the metrics are read from another that has been given this
 * library's coordinates once and has been nowhere else.
 *
 * The line metrics are the two FreeType states: `hhea`'s, as the face's own
 * ascender, descender and height (`MVAR`'s `hasc`, `hdsc` and `hlgp`, applied),
 * and the window ascent and descent from the `OS/2` table it keeps. **Not the
 * typographic ones**: at 2.14.3 the `OS/2` `sTypo*` fields come out carrying the
 * `hasc`, `hdsc` and `hlgp` deltas and not the `tasc`, `tdsc` and `tlgp` ones the
 * font states for them, so comparing them would measure that and not this library.
 * fontTools reads all three sets, and this library agrees with it on every one.
 *
 * No left side bearing is printed. FreeType's is a box edge - the varied
 * outline's `xMin` less the first phantom point - and not the table's own value,
 * so it answers a different question from `HVAR`'s bearing mapping and comparing
 * the two would measure that difference and not a defect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H

/** 16.16 to 2.14 the way FreeType does: add two, then an arithmetic shift. */
static long to_f2dot14(FT_Fixed value) {
  return (long)((value + 2) >> 2);
}

int main(int argc, char ** argv) {
  FT_Library library;
  FT_Face face = NULL;
  FT_MM_Var * variations = NULL;
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
  printf("metrics: stride %ld from %ld\n", stride, first);

  for (int number = 5; number < argc; ++number) {
    char * spec = argv[number];
    char * at = strchr(spec, '@');
    char * given = at ? at + 1 : NULL;
    char text[512];
    char * cursor;
    const TT_OS2 * os2;

    if (at) {
      *at = '\0';
    }
    if (strlen(spec) >= sizeof text) {
      fprintf(stderr, "location too long\n");
      return 2;
    }
    memcpy(text, spec, strlen(spec) + 1);
    if (FT_New_Face(library, argv[1], strtol(argv[2], NULL, 10), &face) != 0) {
      fprintf(stderr, "FT_New_Face failed\n");
      return 1;
    }
    if (!FT_HAS_MULTIPLE_MASTERS(face)
        || FT_Get_MM_Var(face, &variations) != 0 || !variations
        || variations->num_axis > 64) {
      printf("metrics: no variation\n");
      return 0;
    }
    axes = variations->num_axis;
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
      printf("metrics: refused the location\n");
      FT_Done_MM_Var(library, variations);
      FT_Done_Face(face);
      continue;
    }
    printf("variation: %u axes, normalised", axes);
    for (FT_UInt i = 0; i < axes; ++i) {
      printf(" %ld", to_f2dot14(blend[i]));
    }
    printf("\n");
    if (given && *given) {
      FT_UInt i = 0;
      char * item;

      FT_Done_MM_Var(library, variations);
      FT_Done_Face(face);
      variations = NULL;
      if (FT_New_Face(library, argv[1], strtol(argv[2], NULL, 10), &face) != 0
          || !FT_HAS_MULTIPLE_MASTERS(face)) {
        fprintf(stderr, "FT_New_Face failed\n");
        return 1;
      }

      for (item = strtok(given, ","); item && i < axes;
          item = strtok(NULL, ","), ++i) {
        blend[i] = (FT_Fixed)strtol(item, NULL, 10) * 4;
      }
      if (FT_Set_Var_Blend_Coordinates(face, axes, blend) != 0) {
        printf("metrics: refused the coordinates\n");
        FT_Done_Face(face);
        continue;
      }
    }
    for (long glyph = first; glyph < (long)face->num_glyphs; glyph += stride) {
      FT_Error error = FT_Load_Glyph(face, (FT_UInt)glyph,
          FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP
          | FT_LOAD_NO_AUTOHINT);

      if (error != 0) {
        printf("glyph %ld advance refused %d\n", glyph, (int)error);
        continue;
      }
      printf("glyph %ld advance %ld\n", glyph,
          (long)face->glyph->metrics.horiAdvance);
    }
    // `height` is the line's pitch, ascender less descender plus the gap.
    printf("line hhea %ld %ld %ld\n", (long)face->ascender,
        (long)face->descender,
        (long)face->height - (long)face->ascender + (long)face->descender);
    os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2) {
      printf("line win %ld %ld\n", (long)os2->usWinAscent,
          -(long)os2->usWinDescent);
    }
    if (variations) {
      FT_Done_MM_Var(library, variations);
    }
    FT_Done_Face(face);
    variations = NULL;
    face = NULL;
  }
  FT_Done_FreeType(library);
  return 0;
}
