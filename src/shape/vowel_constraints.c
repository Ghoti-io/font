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
 * See vowel_constraints.h. The pairs are what HarfBuzz sets off, found by trying
 * every pair of characters of each script against it; a script's pairs are
 * independent of the font, since the circle is put in before the font is asked
 * about anything.
 */

#include <stdlib.h>
#include <string.h>
#include "uprops.h"
#include "vowel_constraints.h"

typedef struct Pair {
  uint32_t first, second;
} Pair;

// Sorted, for a binary search.
static const Pair pairs[] = {
  {0x0905, 0x093A}, {0x0905, 0x093B}, {0x0905, 0x093E}, {0x0905, 0x0945}, {0x0905, 0x0946},
  {0x0905, 0x0949}, {0x0905, 0x094A}, {0x0905, 0x094B}, {0x0905, 0x094C}, {0x0905, 0x094F},
  {0x0905, 0x0956}, {0x0905, 0x0957}, {0x0906, 0x093A}, {0x0906, 0x0945}, {0x0906, 0x0946},
  {0x0906, 0x0947}, {0x0906, 0x0948}, {0x0909, 0x0941}, {0x090F, 0x0945}, {0x090F, 0x0946},
  {0x090F, 0x0947}, {0x0985, 0x09BE}, {0x098B, 0x09C3}, {0x098C, 0x09E2}, {0x0A05, 0x0A3E},
  {0x0A05, 0x0A48}, {0x0A05, 0x0A4C}, {0x0A72, 0x0A3F}, {0x0A72, 0x0A40}, {0x0A72, 0x0A47},
  {0x0A73, 0x0A41}, {0x0A73, 0x0A42}, {0x0A73, 0x0A4B}, {0x0A85, 0x0ABE}, {0x0A85, 0x0AC5},
  {0x0A85, 0x0AC7}, {0x0A85, 0x0AC8}, {0x0A85, 0x0AC9}, {0x0A85, 0x0ACB}, {0x0A85, 0x0ACC},
  {0x0AC5, 0x0ABE}, {0x0B05, 0x0B3E}, {0x0B0F, 0x0B57}, {0x0B13, 0x0B57}, {0x0B85, 0x0BC2},
  {0x0C12, 0x0C4C}, {0x0C12, 0x0C55}, {0x0C3F, 0x0C55}, {0x0C46, 0x0C55}, {0x0C4A, 0x0C55},
  {0x0C89, 0x0CBE}, {0x0C8B, 0x0CBE}, {0x0C92, 0x0CCC}, {0x0D07, 0x0D57}, {0x0D09, 0x0D57},
  {0x0D0E, 0x0D46}, {0x0D12, 0x0D3E}, {0x0D12, 0x0D57}, {0x0D85, 0x0DCF}, {0x0D85, 0x0DD0},
  {0x0D85, 0x0DD1}, {0x0D8B, 0x0DDF}, {0x0D8D, 0x0DD8}, {0x0D8F, 0x0DDF}, {0x0D91, 0x0DCA},
  {0x0D91, 0x0DD9}, {0x0D91, 0x0DDA}, {0x0D91, 0x0DDC}, {0x0D91, 0x0DDD}, {0x0D91, 0x0DDE},
  {0x0D94, 0x0DDF},
};

static bool constrained(uint32_t a, uint32_t b) {
  size_t low = 0;
  size_t high = sizeof pairs / sizeof pairs[0];

  if (a < 0x0905 || a > 0x0D94) {
    return false;
  }
  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (pairs[mid].first < a || (pairs[mid].first == a && pairs[mid].second < b)) {
      low = mid + 1;
    }
    else if (pairs[mid].first == a && pairs[mid].second == b) {
      return true;
    }
    else {
      high = mid;
    }
  }
  return false;
}

void gfnt_vowel_constraints(GFNT_ShapeCtx * ctx, GFNT_LInfo ** chars,
    size_t * len, size_t * capacity) {
  GFNT_LInfo * in = *chars;
  size_t count = *len;
  size_t matches = 0;
  GFNT_LInfo * out;
  size_t w = 0;
  size_t i;

  for (i = 0; i + 1 < count; i++) {
    if (constrained(in[i].unicode, in[i + 1].unicode)) {
      matches++;
    }
  }
  if (!matches) {
    return;
  }
  out = ctx->allocator->calloc_fn(ctx->allocator->ctx, count + matches,
      sizeof *out);
  if (!out) {
    ctx->oom = true;
    return;
  }
  for (i = 0; i < count; i++) {
    if (i && constrained(in[i - 1].unicode, in[i].unicode)) {
      out[w] = in[i];
      out[w].unicode = 0x25CC;
      gfnt_u_set_props(&out[w]);
      w++;
    }
    out[w++] = in[i];
  }
  ctx->allocator->free_fn(ctx->allocator->ctx, in);
  *chars = out;
  *len = w;
  *capacity = count + matches;
}
