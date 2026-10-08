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
 * Unicode properties, as the shaper settles them. See uprops.h.
 */

#include <ghoti.io/unicode/char.h>
#include "uprops.h"

/**
 * Unicode's Default_Ignorable_Code_Point, less the four Hangul fillers (U+115F,
 * U+1160, U+3164, U+FFA0), which HarfBuzz draws with the font's own glyphs.
 */
bool gfnt_u_default_ignorable(uint32_t u) {
  if (u < 0xAD) {
    return false;
  }
  return u == 0x00AD || u == 0x034F || u == 0x061C
      || (u >= 0x17B4 && u <= 0x17B5)
      || (u >= 0x180B && u <= 0x180E) || (u >= 0x200B && u <= 0x200F)
      || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x206F)
      || (u >= 0xFE00 && u <= 0xFE0F) || u == 0xFEFF
      || (u >= 0xFFF0 && u <= 0xFFF8)
      || (u >= 0x1BCA0 && u <= 0x1BCA3) || (u >= 0x1D173 && u <= 0x1D17A)
      || (u >= 0xE0000 && u <= 0xE0FFF);
}

uint8_t gfnt_u_modified_ccc(uint32_t u) {
  // Marks that must sort away from where their class would put them.
  static const uint8_t remap[37] = {
    /* 0..9 */ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    /* Hebrew, 10..26 */ 22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25,
        13, 10, 11, 26,
    /* Arabic, 27..35 */ 28, 29, 30, 31, 32, 33, 27, 34, 35,
    /* Syriac, 36 */ 36,
  };
  uint32_t ccc;

  if (u == 0x1A60) {
    return 254;   // TAI THAM SAKOT sorts after any tone mark.
  }
  if (u == 0x0FC6) {
    return 254;   // TIBETAN SYMBOL PADMA GDAN sorts after any vowel mark.
  }
  if (u == 0x0F39) {
    return 127;   // TIBETAN MARK TSA -PHRU sorts before U+0F74.
  }
  ccc = guni_combining_class(u);
  if (ccc < sizeof remap) {
    return remap[ccc];
  }
  switch (ccc) {
    case 84:
      return 4;   // Telugu length marks sort before the nukta and the virama,
    case 91:
      return 5;   // and the first before the second.
    case 103:
      return 3;   // Thai.
    case 130:
      return 132; // Tibetan.
    case 132:
      return 131;
    default:
      return (uint8_t)ccc;
  }
}

uint8_t gfnt_u_space_type(uint32_t u) {
  switch (u) {
    case 0x0020: case 0x00A0: return GFNT_SPACE;
    case 0x2000: return GFNT_SPACE_EM_2;
    case 0x2001: return GFNT_SPACE_EM;
    case 0x2002: return GFNT_SPACE_EM_2;
    case 0x2003: return GFNT_SPACE_EM;
    case 0x2004: return GFNT_SPACE_EM_3;
    case 0x2005: return GFNT_SPACE_EM_4;
    case 0x2006: return GFNT_SPACE_EM_6;
    case 0x2007: return GFNT_SPACE_FIGURE;
    case 0x2008: return GFNT_SPACE_PUNCTUATION;
    case 0x2009: return GFNT_SPACE_EM_5;
    case 0x200A: return GFNT_SPACE_EM_16;
    case 0x202F: return GFNT_SPACE_NARROW;
    case 0x205F: return GFNT_SPACE_4_EM_18;
    case 0x3000: return GFNT_SPACE_EM;
    default: return GFNT_SPACE_NONE;
  }
}

void gfnt_u_set_props(GFNT_LInfo * info) {
  uint32_t u = info->unicode;
  // The `rtlm` flag is not a property of the character but of the run: it stays.
  uint8_t flags = 0;
  uint8_t rtlm = info->flags & GFNT_GF_RTLM;
  uint32_t gc;

  info->mcc = 0;
  info->space = GFNT_SPACE_NONE;
  if (u < 0x80) {
    // ASCII has no marks, no ignorables and only the one space.
    info->gc = (uint8_t)guni_general_category(u);
    info->flags = rtlm;
    return;
  }
  gc = guni_general_category(u);
  info->gc = (uint8_t)gc;
  if (gfnt_u_default_ignorable(u)) {
    flags |= GFNT_GF_DEFAULT_IGNORABLE;
    if (u == 0x200C) {
      flags |= GFNT_GF_ZWNJ;
    }
    else if (u == 0x200D) {
      flags |= GFNT_GF_ZWJ;
    }
    else if ((u >= 0x180B && u <= 0x180D) || u == 0x180F
        || (u >= 0xE0020 && u <= 0xE007F) || u == 0x034F) {
      // A context must be able to see these: Mongolian's free variation
      // selectors, the tag characters, and the combining grapheme joiner.
      flags |= GFNT_GF_HIDDEN;
    }
  }
  if (gc == GUNI_GC_NONSPACING_MARK || gc == GUNI_GC_SPACING_MARK
      || gc == GUNI_GC_ENCLOSING_MARK) {
    flags |= GFNT_GF_MARK | GFNT_GF_CONTINUATION;
    info->mcc = gfnt_u_modified_ccc(u);
  }
  info->flags = flags | rtlm;
}
