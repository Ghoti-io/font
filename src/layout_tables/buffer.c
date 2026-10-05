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
 * The glyph buffer lookups rewrite: the input array, the output written so far,
 * and the cursor between them. See layout.h for why it is shaped this way.
 */

#include <string.h>
#include "layout.h"

bool gfnt_lbuf_init(GFNT_LBuffer * b, const GFNT_Allocator * allocator,
    size_t length) {
  size_t capacity = length ? length : 1;

  memset(b, 0, sizeof *b);
  b->allocator = allocator;
  b->info = allocator->calloc_fn(allocator->ctx, capacity, sizeof *b->info);
  b->pos = allocator->calloc_fn(allocator->ctx, capacity, sizeof *b->pos);
  if (!b->info || !b->pos) {
    gfnt_lbuf_free(b);
    return false;
  }
  b->out_info = b->info;
  b->capacity = capacity;
  b->len = length;
  b->max_ops = length * (size_t)GFNT_LAYOUT_OPS_FACTOR > GFNT_LAYOUT_OPS_MINIMUM
      ? (int64_t)length * GFNT_LAYOUT_OPS_FACTOR : GFNT_LAYOUT_OPS_MINIMUM;
  // How far a lookup may grow the run: a multiple substitution of one glyph into
  // sixty-five thousand, applied to every glyph by every lookup, is how a font
  // asks for more memory than there is.
  b->max_len = length * (size_t)GFNT_LAYOUT_LEN_FACTOR > GFNT_LAYOUT_LEN_MINIMUM
      ? length * (size_t)GFNT_LAYOUT_LEN_FACTOR : GFNT_LAYOUT_LEN_MINIMUM;
  if (b->max_len > GFNT_LAYOUT_MAX_LEN) {
    b->max_len = GFNT_LAYOUT_MAX_LEN;
  }
  return true;
}

void gfnt_lbuf_free(GFNT_LBuffer * b) {
  const GFNT_Allocator * a = b->allocator;

  if (!a) {
    return;
  }
  if (b->info) {
    a->free_fn(a->ctx, b->info);
  }
  if (b->spare) {
    a->free_fn(a->ctx, b->spare);
  }
  if (b->pos) {
    a->free_fn(a->ctx, b->pos);
  }
  memset(b, 0, sizeof *b);
}

bool gfnt_lbuf_enlarge(GFNT_LBuffer * b, size_t size) {
  const GFNT_Allocator * a = b->allocator;
  size_t capacity;
  bool aliased;
  GFNT_LInfo * info;
  GFNT_LInfo * spare = NULL;
  GFNT_LPos * pos;

  if (b->oom) {
    return false;
  }
  if (size <= b->capacity) {
    return true;
  }
  if (size > b->max_len) {
    b->limit = true;
    b->oom = true;
    return false;
  }
  capacity = b->capacity;
  while (capacity < size) {
    capacity *= 2;
  }
  aliased = b->out_info == b->info;
  info = a->realloc_fn(a->ctx, b->info, capacity * sizeof *info);
  if (!info) {
    b->oom = true;
    return false;
  }
  b->info = info;
  pos = a->realloc_fn(a->ctx, b->pos, capacity * sizeof *pos);
  if (!pos) {
    b->oom = true;
    return false;
  }
  b->pos = pos;
  if (b->spare) {
    bool spare_is_out = b->out_info == b->spare;

    spare = a->realloc_fn(a->ctx, b->spare, capacity * sizeof *spare);
    if (!spare) {
      b->oom = true;
      return false;
    }
    b->spare = spare;
    if (spare_is_out) {
      b->out_info = spare;
    }
  }
  if (aliased) {
    b->out_info = b->info;
  }
  b->capacity = capacity;
  return true;
}

/** Make room to write @p num_out entries while consuming @p num_in. */
static bool gfnt_lbuf_make_room_for(GFNT_LBuffer * b, size_t num_in,
    size_t num_out) {
  if (!gfnt_lbuf_enlarge(b, b->out_len + num_out)) {
    return false;
  }
  if (b->out_info == b->info && b->out_len + num_out > b->idx + num_in) {
    if (!b->spare) {
      b->spare = b->allocator->calloc_fn(b->allocator->ctx, b->capacity,
          sizeof *b->spare);
      if (!b->spare) {
        b->oom = true;
        return false;
      }
    }
    b->out_info = b->spare;
    memcpy(b->out_info, b->info, b->out_len * sizeof *b->out_info);
  }
  return true;
}

void gfnt_lbuf_clear_output(GFNT_LBuffer * b) {
  b->have_output = true;
  b->out_len = 0;
  b->idx = 0;
  b->out_info = b->info;
}

static bool gfnt_lbuf_next_glyphs(GFNT_LBuffer * b, size_t count) {
  if (b->have_output) {
    if (b->out_info != b->info || b->out_len != b->idx) {
      if (!gfnt_lbuf_make_room_for(b, count, count)) {
        return false;
      }
      memmove(b->out_info + b->out_len, b->info + b->idx,
          count * sizeof *b->out_info);
    }
    b->out_len += count;
  }
  b->idx += count;
  return true;
}

void gfnt_lbuf_sync(GFNT_LBuffer * b) {
  if (!b->have_output) {
    return;
  }
  if (!b->oom && gfnt_lbuf_next_glyphs(b, b->len - b->idx)) {
    if (b->out_info != b->info) {
      GFNT_LInfo * swap = b->info;

      b->info = b->out_info;
      b->spare = swap;
    }
    b->len = b->out_len;
  }
  b->idx = 0;
  b->out_len = 0;
  b->have_output = false;
  b->out_info = b->info;
}

bool gfnt_lbuf_move_to(GFNT_LBuffer * b, size_t i) {
  if (!b->have_output) {
    if (i > b->len) {
      return false;
    }
    b->idx = i;
    return true;
  }
  if (i > b->out_len + (b->len - b->idx)) {
    return false;
  }
  if (b->out_len < i) {
    size_t count = i - b->out_len;

    if (!gfnt_lbuf_make_room_for(b, count, count)) {
      return false;
    }
    memmove(b->out_info + b->out_len, b->info + b->idx,
        count * sizeof *b->out_info);
    b->idx += count;
    b->out_len += count;
  }
  else if (b->out_len > i) {
    // Rewind: what was written goes back to the input, in front of the cursor.
    size_t count = b->out_len - i;

    if (b->idx < count) {
      size_t shift = count - b->idx;

      if (!gfnt_lbuf_enlarge(b, b->len + shift)) {
        return false;
      }
      memmove(b->info + b->idx + shift, b->info + b->idx,
          (b->len - b->idx) * sizeof *b->info);
      b->len += shift;
      b->idx += shift;
    }
    b->idx -= count;
    b->out_len -= count;
    memmove(b->info + b->idx, b->out_info + b->out_len,
        count * sizeof *b->info);
  }
  return true;
}

bool gfnt_lbuf_next_glyph(GFNT_LBuffer * b) {
  return gfnt_lbuf_next_glyphs(b, 1);
}

bool gfnt_lbuf_replace_glyph(GFNT_LBuffer * b, uint32_t glyph) {
  if (b->out_info != b->info || b->out_len != b->idx) {
    if (!gfnt_lbuf_make_room_for(b, 1, 1)) {
      return false;
    }
    b->out_info[b->out_len] = b->info[b->idx];
  }
  b->out_info[b->out_len].glyph = glyph;
  b->idx++;
  b->out_len++;
  return true;
}

bool gfnt_lbuf_output_glyph(GFNT_LBuffer * b, uint32_t glyph) {
  if (!gfnt_lbuf_make_room_for(b, 0, 1)) {
    return false;
  }
  if (b->idx == b->len && !b->out_len) {
    return false;
  }
  b->out_info[b->out_len] = b->idx < b->len ? b->info[b->idx]
                                            : b->out_info[b->out_len - 1];
  b->out_info[b->out_len].glyph = glyph;
  b->out_len++;
  return true;
}

bool gfnt_lbuf_skip_glyph(GFNT_LBuffer * b) {
  b->idx++;
  return true;
}

bool gfnt_lbuf_delete_glyph(GFNT_LBuffer * b) {
  uint32_t cluster = b->info[b->idx].cluster;

  if ((b->idx + 1 < b->len && cluster == b->info[b->idx + 1].cluster)
      || (b->out_len && cluster == b->out_info[b->out_len - 1].cluster)) {
    // The cluster survives in a neighbour.
  }
  else if (b->out_len) {
    uint32_t old = b->out_info[b->out_len - 1].cluster;

    if (cluster < old) {
      size_t i;

      for (i = b->out_len; i && b->out_info[i - 1].cluster == old; i--) {
        b->out_info[i - 1].cluster = cluster;
      }
    }
  }
  else if (b->idx + 1 < b->len) {
    gfnt_lbuf_merge_clusters(b, b->idx, b->idx + 2);
  }
  return gfnt_lbuf_skip_glyph(b);
}

void gfnt_lbuf_merge_clusters(GFNT_LBuffer * b, size_t start, size_t end) {
  uint32_t cluster;
  size_t i;

  if (end - start < 2) {
    return;
  }
  cluster = b->info[start].cluster;
  for (i = start + 1; i < end; i++) {
    if (b->info[i].cluster < cluster) {
      cluster = b->info[i].cluster;
    }
  }
  // Extend the end, and the start, over neighbours already in the same cluster.
  while (end < b->len && b->info[end - 1].cluster == b->info[end].cluster) {
    end++;
  }
  while (b->idx < start && b->info[start - 1].cluster == b->info[start].cluster) {
    start--;
  }
  // At the start of the input, the cluster may continue in what was written.
  if (b->idx == start) {
    for (i = b->out_len; start && i && b->out_info[i - 1].cluster == b->info[start].cluster; i--) {
      b->out_info[i - 1].cluster = cluster;
    }
  }
  for (i = start; i < end; i++) {
    b->info[i].cluster = cluster;
  }
}

size_t gfnt_lbuf_backtrack_len(const GFNT_LBuffer * b) {
  return b->have_output ? b->out_len : b->idx;
}

size_t gfnt_lbuf_lookahead_len(const GFNT_LBuffer * b) {
  return b->len - b->idx;
}

uint32_t gfnt_lbuf_allocate_lig_id(GFNT_LBuffer * b) {
  uint32_t id = ++b->serial & 0x07u;

  if (!id) {
    id = gfnt_lbuf_allocate_lig_id(b);
  }
  return id;
}

void gfnt_lbuf_reverse(GFNT_LBuffer * b) {
  size_t i;

  for (i = 0; i < b->len / 2; i++) {
    GFNT_LInfo info = b->info[i];
    GFNT_LPos pos = b->pos[i];

    b->info[i] = b->info[b->len - 1 - i];
    b->info[b->len - 1 - i] = info;
    b->pos[i] = b->pos[b->len - 1 - i];
    b->pos[b->len - 1 - i] = pos;
  }
}
