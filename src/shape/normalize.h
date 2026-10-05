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
 * Normalisation for shaping: HarfBuzz's `hb-ot-shape-normalize`, which is not
 * UAX #15.
 *
 * A shaper does not want text in a normal form; it wants glyphs the font has.
 * So a precomposed character is kept when the font maps it and taken apart when
 * the font does not, a base and a mark are joined when the font has the
 * composite, and marks are put in canonical order whichever way the text came.
 * The three passes - decompose, reorder, recompose - and the order in which they
 * ask the font are what is held to HarfBuzz, because each of them can change how
 * many glyphs there are.
 */

#ifndef GHOTI_IO_GFNT_NORMALIZE_H
#define GHOTI_IO_GFNT_NORMALIZE_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../layout_tables/layout.h"

/** How far a shaper wants the text composed. */
typedef enum GFNT_NormMode {
  GFNT_NORM_NONE,                  ///< Leave it as it is: Hangul's.
  GFNT_NORM_DECOMPOSED,            ///< Fully, and never recompose.
  GFNT_NORM_COMPOSED_DIACRITICS,   ///< The default: the shortest the font can.
  GFNT_NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT ///< As that, without the shortcut.
} GFNT_NormMode;

/** What a script's shaper may say about how its text is taken apart and joined. */
typedef struct GFNT_NormHooks {
  /** One step of canonical decomposition: @p ab into @p a and @p b (0 for none). */
  bool (*decompose)(uint32_t ab, uint32_t * a, uint32_t * b, void * ctx);
  /** The composite of @p a and @p b, if the shaper allows one. */
  bool (*compose)(uint32_t a, uint32_t b, uint32_t * ab, void * ctx);
  /** Rearrange a run of marks after the canonical sort. */
  void (*reorder_marks)(GFNT_LInfo * info, size_t start, size_t end, void * ctx);
  void * ctx;
} GFNT_NormHooks;

/**
 * One step of canonical decomposition, as the default shaper takes it: @p ab into
 * @p a and @p b (0 if it is a singleton). @p ctx is ignored. For a shaper's hook
 * to fall back on.
 */
bool gfnt_unicode_decompose(uint32_t ab, uint32_t * a, uint32_t * b, void * ctx);

/**
 * Normalise a run in place. @p *info is replaced: it may end longer or shorter.
 *
 * @param face The face whose `cmap` says which characters have glyphs.
 * @param info The run, one record per code point with its properties set and its
 *   `glyph` not yet.
 * @param len How many; updated.
 * @param capacity How many the array holds; updated.
 * @return Whether it succeeded; false means no memory, and the run is unchanged.
 */
bool gfnt_normalize(const GFNT_Face * face, GFNT_LInfo ** info, size_t * len,
    size_t * capacity, const GFNT_Allocator * allocator, GFNT_NormMode mode,
    const GFNT_NormHooks * hooks);

/** Give every code point's cluster the smallest in [start, end), HarfBuzz's merge_clusters. */
void gfnt_merge_clusters(GFNT_LInfo * info, size_t len, size_t start, size_t end);

#endif
