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
 * Look codepoints up in a font's `cmap`, in bulk.
 *
 * Usage: font-cmap <font> [face-index] [platform encoding]
 *
 * Reads codepoints from stdin, one per line, decimal or `0x`-prefixed, and
 * writes one `<codepoint> <glyph>` line each. With a platform and encoding it
 * uses that subtable by name; without, it uses the one the library's own
 * preference order selects - which is the question a caller actually asks.
 *
 * **This is also the driver `tools/oracle/cmap_diff.py` compares against
 * fontTools**, which is why it speaks a batch protocol rather than taking one
 * codepoint per invocation: a differential over a few hundred fonts and a few
 * thousand codepoints each cannot pay for a process per lookup. The
 * first line of output names the subtable that answered, so the differential can
 * ask its reference about the same one rather than about whichever fontTools
 * would have picked.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>

int main(int argc, char ** argv) {
  const char * path;
  size_t index = 0;
  bool named_subtable = false;
  unsigned long platform = 0;
  unsigned long encoding = 0;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_CmapSubtable subtable;
  GFNT_Error error;
  GFNT_Result result;
  char line[64];

  if (argc < 2) {
    fprintf(stderr,
        "usage: %s <font> [face-index] [platform encoding] < codepoints\n",
        argv[0]);
    return 2;
  }
  path = argv[1];
  if (argc > 2) {
    index = (size_t)strtoul(argv[2], NULL, 0);
  }
  if (argc > 4) {
    named_subtable = true;
    platform = strtoul(argv[3], NULL, 0);
    encoding = strtoul(argv[4], NULL, 0);
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

  memset(&subtable, 0, sizeof subtable);
  if (named_subtable) {
    size_t count = 0;
    bool found = false;

    if (gfnt_face_cmap_count(face, &count, &error) != GFNT_OK) {
      fprintf(stderr, "%s: no cmap\n", path);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
    for (size_t i = 0; i < count; i++) {
      GFNT_CmapSubtable candidate;

      if (gfnt_face_cmap_at(face, i, &candidate, NULL) != GFNT_OK) {
        continue;
      }
      if (candidate.platform_id == platform
          && candidate.encoding_id == encoding) {
        subtable = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "%s: no (%lu,%lu) subtable\n", path, platform, encoding);
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
  }
  else {
    result = gfnt_face_cmap_best(face, &subtable, &error);
    if (result != GFNT_OK) {
      fprintf(stderr, "%s: %s\n", path, gfnt_result_string(result));
      gfnt_face_free(face);
      gfnt_blob_destroy(blob);
      return 1;
    }
  }

  // The subtable that will answer, so the reference can be asked about the same
  // one. Printed before any lookup, and flushed, so a differential that streams
  // its codepoints can read this line first.
  printf("# subtable %u %u %u\n", subtable.platform_id, subtable.encoding_id,
      subtable.format);
  fflush(stdout);

  while (fgets(line, sizeof line, stdin)) {
    char * end = NULL;
    unsigned long codepoint;
    uint32_t glyph = 0;

    if (line[0] == '#' || line[0] == '\n') {
      continue;
    }
    codepoint = strtoul(line, &end, 0);
    if (end == line) {
      continue;
    }

    // Named subtable: exactly that subtable, with no policy on top. Preferred
    // subtable: the whole question, including the Windows symbol range, because
    // that is what a caller gets.
    if (named_subtable) {
      result = gfnt_cmap_lookup(face, &subtable, (uint32_t)codepoint, &glyph,
          &error);
    }
    else {
      result = gfnt_face_glyph_for_codepoint(face, (uint32_t)codepoint, &glyph,
          &error);
    }
    if (result != GFNT_OK) {
      printf("%lu %s\n", codepoint, gfnt_result_string(result));
      continue;
    }
    printf("%lu %" PRIu32 "\n", codepoint, glyph);
  }

  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
