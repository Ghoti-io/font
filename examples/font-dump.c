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
 * Print every table this library parses, through each one's `_dump`.
 *
 * Usage: font-dump <font> [face-index]
 *
 * **This is the driver `tools/oracle/ttx_diff.py` compares against fontTools.**
 * documentation/design.md section 14 names the `_dump` functions as that
 * differential's input, which is why this prints them rather than reaching for
 * the fields itself: the thing under comparison is the dump a person reads when
 * a font misbehaves, so a differential that bypassed it would leave the dumps
 * unchecked and compare a second code path nobody uses.
 *
 * It also prints one line that is not a dump - `numGlyphs:` - because the glyph
 * count is the *minimum* across the tables that index glyphs (M12) and `maxp`'s
 * own claim is the number a reference reports. Both are printed, which is what
 * lets the differential check the claim against fontTools and the minimum
 * against the claim.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/font/font.h>

int main(int argc, char ** argv) {
  const char * path;
  size_t index = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  GFNT_Result result;
  const GFNT_Head * head = NULL;
  const GFNT_Hhea * hhea = NULL;
  const GFNT_Os2 * os2 = NULL;
  const GFNT_Post * post = NULL;
  size_t glyphs = 0;
  size_t claimed = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index]\n", argv[0]);
    return 2;
  }
  path = argv[1];
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 0);
  }

  gfnt_error_clear(&error);
  result = gfnt_blob_create_file(path, NULL, NULL, &blob, &error);
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", path, gfnt_result_string(result));
    return 1;
  }
  result = gfnt_face_load(blob, index, NULL, NULL, &face, &error);
  if (result != GFNT_OK) {
    fprintf(stderr, "%s: %s\n", path, gfnt_result_string(result));
    gfnt_blob_destroy(blob);
    return 1;
  }

  gfnt_blob_dump(blob, stdout);
  gfnt_face_dump(face, stdout);

  // Each table is printed when it parses and skipped with a line when it does
  // not, so that a differential can tell "this font has no OS/2" from "the dump
  // stopped early".
  if (gfnt_face_head(face, &head, &error) == GFNT_OK) {
    gfnt_head_dump(head, stdout);
  }
  else {
    printf("head: absent (%s)\n", gfnt_result_string(error.result));
  }
  if (gfnt_face_hhea(face, &hhea, &error) == GFNT_OK) {
    gfnt_hhea_dump(hhea, stdout);
  }
  else {
    printf("hhea: absent (%s)\n", gfnt_result_string(error.result));
  }
  if (gfnt_face_os2(face, &os2, &error) == GFNT_OK) {
    gfnt_os2_dump(os2, stdout);
  }
  else {
    printf("OS/2: absent (%s)\n", gfnt_result_string(error.result));
  }
  if (gfnt_face_post(face, &post, &error) == GFNT_OK) {
    gfnt_post_dump(post, stdout);
  }
  else {
    printf("post: absent (%s)\n", gfnt_result_string(error.result));
  }

  if (gfnt_face_num_glyphs(face, &glyphs, &error) == GFNT_OK) {
    gfnt_face_num_glyphs_disagreement(face, &claimed);
    printf("numGlyphs: %zu maxp %zu\n", glyphs, claimed);
  }
  else {
    printf("numGlyphs: absent (%s)\n", gfnt_result_string(error.result));
  }

  if (gfnt_face_cmap_dump(face, stdout) != GFNT_OK) {
    printf("cmap: absent\n");
  }
  if (gfnt_face_name_dump(face, stdout) != GFNT_OK) {
    printf("name: absent\n");
  }
  // The design space, for a font that has one. A font with none prints nothing
  // here and the reference prints nothing for it either, so a table that this
  // library failed to read is said in a line of its own rather than left as
  // silence a differential would read as agreement.
  if (gfnt_face_is_variable(face)) {
    size_t axes = 0;

    // fvar first and on its own, so a refusal is attributed to the table that
    // made it: the dump below fails for an avar too, and "absent (Corrupt data)"
    // under the wrong name would make the differential compare the wrong table's
    // verdict.
    if (gfnt_face_axis_count(face, &axes, &error) != GFNT_OK) {
      printf("fvar: absent (%s)\n", gfnt_result_string(error.result));
    }
    else {
      result = gfnt_face_variation_dump(face, stdout);
      if (result != GFNT_OK) {
        printf("avar: absent (%s)\n", gfnt_result_string(result));
      }
    }
  }
  // `STAT`, which names the places in the design space. A face without one
  // prints nothing, as the reference does.
  if (gfnt_face_has_stat(face)) {
    result = gfnt_face_stat_dump(face, stdout);
    if (result != GFNT_OK) {
      printf("STAT: absent (%s)\n", gfnt_result_string(result));
    }
  }
  // What a variable font's layout tables replace at a location. A table with no
  // FeatureVariations prints nothing, and so does the reference.
  {
    static const char * layout[] = {"GSUB", "GPOS"};

    for (size_t i = 0; i < 2; ++i) {
      GFNT_Tag tag = GFNT_TAG(layout[i][0], layout[i][1], layout[i][2],
          layout[i][3]);

      if (gfnt_face_feature_variations_dump(face, tag, stdout) != GFNT_OK
          && gfnt_face_has_table(face, tag)) {
        printf("%s FeatureVariations: absent (unreadable)\n", layout[i]);
      }
    }
  }
  // The `post` glyph names, last because they are the longest section. A font
  // with none says so on a line of its own (see the dump), because a
  // differential reads silence as agreement.
  if (gfnt_face_glyph_names_dump(face, stdout) != GFNT_OK) {
    printf("glyph names: absent\n");
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
