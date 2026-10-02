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
 * The chain of glyphs a composite walk currently has open, in one place because
 * two tables nest glyphs inside glyphs.
 *
 * `glyf` composes outlines and `EBDT` composes pixels, over different graphs,
 * but the rule about a cycle is one rule: **a glyph that reaches itself is
 * ::GFNT_ERR_CORRUPT named as a cycle, and what `max_composite_depth` is left to
 * refuse is an acyclic chain nested deeper than the caller allowed**
 * (::GFNT_ERR_LIMIT, which a caller can act on by raising the budget).
 * design.md section 7.5 says the two readers should agree about that; two copies
 * of the walk is how they come not to, and for a while they did not - `glyf`
 * named only the direct cycle and left `A -> B -> A` to the depth cap, reporting
 * a limit for a font that is not deep but circular.
 *
 * A linked list of stack frames rather than an array, because the depth a caller
 * allows is ::GFNT_Limits::max_composite_depth and a fixed array would either
 * cap that a second time or need allocating. Each level's node lives in its own
 * frame, so the walk costs nothing and there is nothing to free. `static inline`
 * rather than exported: this is a two-field struct and a loop, not API.
 */

#ifndef GHOTI_IO_GFNT_CHAIN_H
#define GHOTI_IO_GFNT_CHAIN_H

#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One level of a composite walk: the glyph being assembled, and its parent. */
typedef struct GFNT_GlyphChain {
  const struct GFNT_GlyphChain * parent;
  uint32_t glyph;
} GFNT_GlyphChain;

/**
 * Whether @p glyph is already open at any level of @p chain.
 *
 * Checked against the whole chain and not against its innermost frame, which is
 * the distinction the indirect cycle turns on: comparing a component only with
 * the composite it sits in catches `A -> A`, and walks `A -> B -> A` to the cap.
 *
 * @param chain The innermost frame, or NULL at the top of a walk.
 * @param glyph The glyph index a component names.
 * @return Whether some frame already holds it.
 */
static inline bool gfnt_glyph_chain_has(const GFNT_GlyphChain * chain,
    uint32_t glyph) {
  for (; chain; chain = chain->parent) {
    if (chain->glyph == glyph) {
      return true;
    }
  }
  return false;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_CHAIN_H
