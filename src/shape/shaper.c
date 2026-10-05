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
 * Which shaper a run gets, and the default one. See plan.h.
 */

#include "plan.h"

const GFNT_Shaper gfnt_shaper_default = {
  .name = "default",
  .normalization = GFNT_NORM_COMPOSED_DIACRITICS,
  .zero_width_marks = 2,
  .fallback_position = true,
};

/** Whether a script joins its letters the way Arabic does. */
static bool gfnt_script_is_joining(GFNT_Tag script) {
  static const GFNT_Tag joining[] = {
    GFNT_TAG('a', 'r', 'a', 'b'), GFNT_TAG('s', 'y', 'r', 'c'),
    GFNT_TAG('m', 'o', 'n', 'g'), GFNT_TAG('n', 'k', 'o', ' '),
    GFNT_TAG('p', 'h', 'a', 'g'), GFNT_TAG('m', 'a', 'n', 'd'),
    GFNT_TAG('m', 'a', 'n', 'i'), GFNT_TAG('p', 'h', 'l', 'p'),
    GFNT_TAG('a', 'd', 'l', 'm'), GFNT_TAG('r', 'o', 'h', 'g'),
    GFNT_TAG('s', 'o', 'g', 'd'), GFNT_TAG('c', 'h', 'r', 's'),
    GFNT_TAG('o', 'u', 'g', 'r'),
  };
  size_t i;

  for (i = 0; i < sizeof joining / sizeof joining[0]; i++) {
    if (joining[i] == script) {
      return true;
    }
  }
  return false;
}

const GFNT_Shaper * gfnt_shaper_select(GFNT_Tag script, GFNT_Tag chosen,
    bool horizontal) {
  bool font_has_script = chosen != 0 && chosen != GFNT_TAG_DFLT
      && chosen != GFNT_TAG_dflt && chosen != GFNT_TAG_latn;

  if (script == GFNT_TAG('a', 'r', 'a', 'b')) {
    // Arabic is always shaped as Arabic: there is a fallback for a font that has
    // no features of its own. Not so for vertical text.
    return horizontal ? &gfnt_shaper_arabic : &gfnt_shaper_default;
  }
  if (script == GFNT_TAG('h', 'e', 'b', 'r')) {
    return &gfnt_shaper_hebrew;
  }
  if (script == GFNT_TAG('t', 'h', 'a', 'i') || script == GFNT_TAG('l', 'a', 'o', ' ')) {
    return &gfnt_shaper_thai;
  }
  if (gfnt_script_is_joining(script)) {
    // The others only when the font was made for the script.
    return horizontal && font_has_script ? &gfnt_shaper_arabic
                                         : &gfnt_shaper_default;
  }
  return &gfnt_shaper_default;
}
