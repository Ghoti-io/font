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
 * ::gfnt_fontfallback_provider(): layout's font provider over a ::GFNT_FontFallback.
 */

#include <ghoti.io/font/provider.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>

#include <string.h>

/** The width class (1 to 9) as the CSS percentage, in 16.16. */
static GFNT_F16Dot16 width_percent(uint16_t class_) {
  static const int32_t eighths[10] = {800, 400, 500, 600, 700, 800, 900, 1000, 1200, 1600};

  if (class_ < 1 || class_ > 9) {
    return 100 << 16;
  }
  return (GFNT_F16Dot16)((int64_t)eighths[class_] * 65536 / 8);
}

static const GFNT_Face * font_for(void * ctx, uint32_t codepoint, GFNT_Tag script,
    GFNT_Tag language, const GFNT_TextStyle * style) {
  GFNT_FontFallback * fallback = ctx;
  GFNT_FontQuery wanted;
  const GFNT_Allocator * allocator = gfnt_allocator_default();
  const GFNT_Os2 * os2 = NULL;
  char * family = NULL;
  const char * families[1];
  const GFNT_Face * found = NULL;
  bool ok = false;

  gfnt_fontquery_init(&wanted);
  if (style && style->face_count > 0 && style->faces && style->faces[0]) {
    const GFNT_Face * primary = style->faces[0];

    if (gfnt_face_os2(primary, &os2, NULL) == GFNT_OK && os2) {
      wanted.weight = (GFNT_F16Dot16)((int32_t)os2->weight_class << 16);
      wanted.width = width_percent(os2->width_class);
      wanted.slant = (os2->fs_selection & GFNT_OS2_FS_ITALIC) ? GFNT_SLANT_ITALIC
          : (os2->fs_selection & GFNT_OS2_FS_OBLIQUE) ? GFNT_SLANT_OBLIQUE : GFNT_SLANT_ROMAN;
    }
    if (gfnt_face_name(primary, GFNT_NAME_TYPOGRAPHIC_FAMILY, GFNT_LANGUAGE_ANY, allocator,
            &family, NULL, NULL) != GFNT_OK || !family) {
      family = NULL;
      if (gfnt_face_name(primary, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY, allocator, &family,
              NULL, NULL) != GFNT_OK) {
        family = NULL;
      }
    }
    if (family) {
      families[0] = family;
      wanted.families = families;
      wanted.family_count = 1;
    }
  }
  if (gfnt_fontfallback_face(fallback, codepoint, script, language, &wanted, &found, NULL, &ok,
          NULL) != GFNT_OK || !ok) {
    found = NULL;
  }
  if (family) {
    allocator->free_fn(allocator->ctx, family);
  }
  return found;
}

GFNT_FontProvider gfnt_fontfallback_provider(GFNT_FontFallback * fallback) {
  GFNT_FontProvider provider;

  memset(&provider, 0, sizeof provider);
  provider.ctx = fallback;
  provider.font_for = font_for;
  return provider;
}
