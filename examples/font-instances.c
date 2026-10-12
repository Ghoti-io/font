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

/*
 * Usage: font-instances DIR...
 *
 * Scans the directories and prints one tab-separated line for each named instance of each
 * variable face that is not the face's own default: path, style, full name, PostScript name.
 * The instance fontconfig lists beside the face is the same set.
 */

#include <stdio.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/discover.h>

int main(int argc, char ** argv) {
  GFNT_FontSet * set = NULL;
  GFNT_Error error;
  size_t i;
  size_t k;

  if (argc < 2) {
    fprintf(stderr, "usage: %s DIR...\n", argv[0]);
    return 2;
  }
  if (gfnt_fontset_scan((const char * const *)argv + 1, (size_t)argc - 1, NULL, 0, NULL, &set,
          &error) != GFNT_OK) {
    fprintf(stderr, "scan: %s\n", error.message ? error.message : "failed");
    return 1;
  }
  for (i = 0; i < gfnt_fontset_count(set); i++) {
    const GFNT_FontRecord * r = gfnt_fontset_at(set, i);

    for (k = 0; k < r->instance_count; k++) {
      const GFNT_FontInstance * in = &r->instances[k];

      if (!in->is_default) {
        printf("%s\t%s\t%s\t%s\n", r->path, in->style, in->full_name, in->postscript_name);
      }
    }
  }
  gfnt_fontset_free(set);
  return 0;
}
