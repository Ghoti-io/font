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
 * Choose a face from the fonts on this machine, as a browser would.
 *
 * Usage: font-match [--dir D]... [--weight N] [--width PERCENT] [--italic | --oblique [DEG]]
 *                   [--char U+XXXX [--script TAG]] FAMILY[,FAMILY]...
 *
 * Scans the system's font directories (or the --dir ones), matches the family list by CSS
 * Fonts 4, and prints the chosen face's family, style, weight and path, with the axis values to
 * apply. With --char it instead asks the fallback for a face that maps the character.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/match.h>

int main(int argc, char ** argv) {
  GFNT_FontSet * set = NULL;
  GFNT_FontQuery query;
  GFNT_FontMatch match;
  GFNT_Error error;
  const char * dirs[16];
  size_t ndirs = 0;
  const char * families[32];
  char * list = NULL;
  uint32_t cp = 0;
  GFNT_Tag script = 0;
  bool have_char = false;
  bool found = false;
  GFNT_Result result;
  int i;

  gfnt_fontquery_init(&query);
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc && ndirs < 16) {
      dirs[ndirs++] = argv[++i];
    } else if (!strcmp(argv[i], "--weight") && i + 1 < argc) {
      query.weight = (GFNT_F16Dot16)(atof(argv[++i]) * 65536.0);
    } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
      query.width = (GFNT_F16Dot16)(atof(argv[++i]) * 65536.0);
    } else if (!strcmp(argv[i], "--italic")) {
      query.slant = GFNT_SLANT_ITALIC;
    } else if (!strcmp(argv[i], "--oblique")) {
      query.slant = GFNT_SLANT_OBLIQUE;
      if (i + 1 < argc && argv[i + 1][0] != '-' && (argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')) {
        query.oblique_angle = (GFNT_F16Dot16)(atof(argv[++i]) * 65536.0);
        query.oblique_angle_set = true;
      }
    } else if (!strcmp(argv[i], "--char") && i + 1 < argc) {
      const char * t = argv[++i];
      cp = (uint32_t)strtoul(t[0] == 'U' || t[0] == 'u' ? t + 2 : t, NULL, 16);
      have_char = true;
    } else if (!strcmp(argv[i], "--script") && i + 1 < argc && strlen(argv[i + 1]) == 4) {
      const char * s = argv[++i];
      script = GFNT_TAG(s[0], s[1], s[2], s[3]);
    } else if (argv[i][0] != '-' && !list) {
      list = argv[i];
    } else {
      fprintf(stderr, "usage: %s [--dir D] [--weight N] [--width P] [--italic|--oblique [DEG]] "
          "[--char U+XXXX [--script TAG]] FAMILY[,FAMILY]...\n", argv[0]);
      return 2;
    }
  }
  if (!list && !have_char) {
    fprintf(stderr, "no family list\n");
    return 2;
  }
  if (list) {
    char * at = list;

    query.families = families;
    while (at && *at && query.family_count < 32) {
      char * comma = strchr(at, ',');

      while (*at == ' ') {
        at++;
      }
      families[query.family_count++] = at;
      if (comma) {
        *comma = '\0';
        at = comma + 1;
      } else {
        at = NULL;
      }
    }
  }
  result = ndirs ? gfnt_fontset_scan(dirs, ndirs, NULL, 0, NULL, &set, &error)
                 : gfnt_fontset_scan_system(NULL, 0, NULL, &set, &error);
  if (result != GFNT_OK) {
    fprintf(stderr, "scan: %s\n", error.message ? error.message : "failed");
    return 1;
  }
  printf("%zu faces\n", gfnt_fontset_count(set));
  if (have_char) {
    GFNT_FontFallback * fallback = NULL;
    const GFNT_Face * face = NULL;
    size_t index = 0;

    result = gfnt_fontfallback_create(set, NULL, NULL, &fallback, &error);
    if (result == GFNT_OK) {
      result = gfnt_fontfallback_face(fallback, cp, script, 0, &query, &face, &index, &found, &error);
    }
    if (result == GFNT_OK && found) {
      const GFNT_FontRecord * r = gfnt_fontset_at(set, index);

      printf("U+%04" PRIX32 ": %s / %s  weight %u  %s #%zu\n", cp, r->family, r->style,
          (unsigned)r->weight, r->path, r->index);
    } else {
      printf("U+%04" PRIX32 ": no face maps it\n", cp);
    }
    gfnt_fontfallback_free(fallback);
  } else {
    result = gfnt_fontset_match(set, &query, NULL, &match, &found, &error);
    if (result == GFNT_OK && found) {
      const GFNT_FontRecord * r = gfnt_fontset_at(set, match.index);

      printf("%s / %s  weight %u  %s #%zu  (family %zu of the list)\n  apply: weight %.2f width %.2f "
          "italic %.2f oblique %.2f\n", r->family, r->style, (unsigned)r->weight, r->path, r->index,
          match.family, match.weight / 65536.0, match.width / 65536.0, match.italic / 65536.0,
          match.oblique / 65536.0);
    } else {
      printf("no family of the list has a face\n");
    }
  }
  gfnt_fontset_free(set);
  return result == GFNT_OK ? 0 : 1;
}
