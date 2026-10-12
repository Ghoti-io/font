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
 * Where a word is stretched to justify it with kashida. Internal.
 *
 * Arabic-script justification lengthens the pen stroke inside a word rather
 * than widening the space between words. One place is chosen per word, by the
 * connection-priority scheme Microsoft documented for Internet Explorer 6 and
 * Word and InDesign follow (notes/font/KASHIDA.md): an author's own tatweel,
 * then after a seen or sad, then before a word-final letter by class, then
 * before a medial beh followed by a reh or yeh, then (last of all) before any
 * medial letter. Which pairs of letters may be pulled apart comes from the
 * Unicode joining data; the pairs that form a mandatory ligature never are.
 */

#ifndef GHOTI_IO_GFNT_KASHIDA_H
#define GHOTI_IO_GFNT_KASHIDA_H

#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** What made a place the chosen one, best first (a lower number wins). */
typedef enum {
  GFNT_KASHIDA_AUTHOR = 1,        ///< The author wrote a tatweel here.
  GFNT_KASHIDA_AFTER_SEEN = 2,    ///< The flat tail of a seen or sad.
  GFNT_KASHIDA_BEFORE_HEH = 3,    ///< Before a final heh or dal.
  GFNT_KASHIDA_BEFORE_ALEF = 4,   ///< Before a final alef, tah, lam, kaf or gaf.
  GFNT_KASHIDA_BEFORE_BEH_RA = 5, ///< Before a medial beh followed by a reh or yeh.
  GFNT_KASHIDA_BEFORE_WAW = 6,    ///< Before a final waw, ain, qaf or feh.
  GFNT_KASHIDA_BEFORE_FINAL = 7,  ///< Before any other final letter.
  GFNT_KASHIDA_MEDIAL = 8         ///< Before any other medial letter: the last resort.
} GFNT_KashidaPriority;

/**
 * Whether a place can be stretched: asked of a candidate only when it would be the best so far.
 * The caller knows what the word was shaped to (a font may draw two letters as one glyph, and
 * then a tatweel cannot go between them).
 */
typedef bool (*GFNT_KashidaAllowed)(void * ctx, size_t before);

/**
 * Chooses the place to stretch a word.
 *
 * @param cp The word's code points (no spaces).
 * @param n How many.
 * @param before Receives the index of the code point the stretch goes in front
 *        of; for an author's tatweel, the first of its run.
 * @param priority Receives the rule that chose it; may be NULL.
 * @param allowed Vetoes places, or NULL to allow them all.
 * @param ctx Passed to @p allowed.
 * @return true if the word has a place; false if it must be left alone.
 */
bool gfnt_kashida_point(const uint32_t * cp, size_t n, size_t * before,
    GFNT_KashidaPriority * priority, GFNT_KashidaAllowed allowed, void * ctx);

#endif
