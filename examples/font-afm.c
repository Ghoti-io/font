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
 * Print what this library reads from an Adobe Font Metrics file.
 *
 * Usage: font-afm <file.afm>
 *
 * One line per character (`C <index> <code> <wx> <wy> <name> <llx> <lly> <urx> <ury> <ligatures>`),
 * one per kerning pair (`K <first> <second> <x>`, in sorted order), and the header's numbers, every number in
 * thousandths exactly as the library holds it. **This is the driver
 * `tools/oracle/afm_diff.py` compares against fontTools' AFM reader.**
 */

#include <ghoti.io/font/afm.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char ** argv) {
  FILE * file;
  long size;
  char * data;
  GFNT_Afm * afm = NULL;
  GFNT_Error error;
  GFNT_Result result;
  GFNT_AfmInfo info;
  size_t i;

  if (argc != 2) {
    fprintf(stderr, "usage: font-afm <file.afm>\n");
    return 2;
  }
  file = fopen(argv[1], "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "font-afm: cannot read %s\n", argv[1]);
    return 2;
  }
  data = malloc((size_t)size + 1);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    fprintf(stderr, "font-afm: cannot read %s\n", argv[1]);
    return 2;
  }
  fclose(file);
  gfnt_error_clear(&error);
  result = gfnt_afm_parse(data, (size_t)size, NULL, NULL, &afm, &error);
  free(data);
  if (result != GFNT_OK) {
    fprintf(stderr, "font-afm: %s: %s\n", argv[1], error.message);
    return 1;
  }
  info = gfnt_afm_info(afm);
  printf("H FontName %s\n", info.font_name ? info.font_name : "");
  printf("H FullName %s\n", info.full_name ? info.full_name : "");
  printf("H FamilyName %s\n", info.family_name ? info.family_name : "");
  printf("H Weight %s\n", info.weight ? info.weight : "");
  printf("H ItalicAngle %d\n", (int)info.italic_angle_milli);
  printf("H IsFixedPitch %d\n", info.is_fixed_pitch ? 1 : 0);
  printf("H UnderlinePosition %d\n", (int)info.underline_position_milli);
  printf("H UnderlineThickness %d\n", (int)info.underline_thickness_milli);
  printf("H FontBBox %d %d %d %d\n", (int)info.bbox_milli[0], (int)info.bbox_milli[1],
      (int)info.bbox_milli[2], (int)info.bbox_milli[3]);
  printf("H CapHeight %d\n", (int)info.cap_height_milli);
  printf("H XHeight %d\n", (int)info.x_height_milli);
  printf("H Ascender %d\n", (int)info.ascender_milli);
  printf("H Descender %d\n", (int)info.descender_milli);
  for (i = 0; i < gfnt_afm_glyph_count(afm); i++) {
    GFNT_AfmGlyph g;

    gfnt_afm_glyph_at(afm, i, &g);
    printf("C %zu %d %d %d %s %d %d %d %d %s\n", i, (int)g.code, (int)g.width_x_milli,
        (int)g.width_y_milli, g.name ? g.name : "-", (int)g.bbox_milli[0], (int)g.bbox_milli[1],
        (int)g.bbox_milli[2], (int)g.bbox_milli[3], g.ligatures ? g.ligatures : "-");
  }
  for (i = 0; i < gfnt_afm_kern_count(afm); i++) {
    const char * first;
    const char * second;
    int32_t x;

    gfnt_afm_kern_at(afm, i, &first, &second, &x);
    printf("K %s %s %d\n", first, second, (int)x);
  }
  gfnt_afm_free(afm);
  return 0;
}
