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
 * The table memo: one parse per table per face, published under the face's
 * lock.
 *
 * documentation/design.md section 15.3. A failure is memoised as firmly as a
 * success, because a corrupt table is corrupt every time and reparsing it to
 * rediscover that is work a hostile font would happily charge for.
 *
 * The lock is held around the memo and never around a parse, because a parse
 * may need another table - the numGlyphs minimum needs `hhea` and `hmtx` - and
 * a non-recursive mutex held across that deadlocks against itself. The first
 * version of this file did hold it, and the concurrency test in
 * tests/unit/test_metrics.cpp hung on the first font that had both tables.
 */

#include <ghoti.io/font/macros.h>
#include <string.h>
#include "tables.h"

GFNT_Result gfnt_table_cached(const GFNT_Face * face, GFNT_Cached * state,
    void * storage, void * scratch, size_t size, GFNT_TableParse parse,
    GFNT_Error * error) {
  // The one cast in the library that writes through a const GFNT_Face *. A
  // face is immutable after load *except* for these memos, which is what lets
  // one face be shared read-only across threads; the lock is what makes the
  // exception safe, and keeping the cast here rather than at each accessor is
  // what keeps it reviewable.
  GFNT_Face * cache_owner = (GFNT_Face *)face;
  GFNT_Result result;
  GFNT_Error recorded;
  GFNT_Error attempt;

  if (!face || !state || !storage || !scratch || !parse || size == 0) {
    return GFNT_ERR_INVALID;
  }
  if (!face->lock_ready) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the face has no lock for its table cache");
  }

  if (GCU_MUTEX_LOCK(cache_owner->lock) != 0) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "locking the face's table cache");
  }
  if (state->done) {
    result = state->result;
    recorded = state->error;
    GCU_MUTEX_UNLOCK(cache_owner->lock);
    if (result != GFNT_OK && error) {
      *error = recorded;
    }
    return result;
  }
  GCU_MUTEX_UNLOCK(cache_owner->lock);

  // No lock held: the parse may ask the face for another table, and one of
  // them does. See the header for why that is a requirement rather than a
  // convenience.
  gfnt_error_clear(&attempt);
  result = parse(face, scratch, &attempt);

  if (GCU_MUTEX_LOCK(cache_owner->lock) != 0) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "locking the face's table cache");
  }
  if (!state->done) {
    memcpy(storage, scratch, size);
    state->result = result;
    state->error = attempt;
    state->done = true;
  }
  else {
    // Another thread parsed the same bytes while this one was working. Its
    // answer is already published and identical; take it rather than writing
    // over it, so that storage is written exactly once.
    result = state->result;
    attempt = state->error;
  }
  GCU_MUTEX_UNLOCK(cache_owner->lock);

  if (result != GFNT_OK && error) {
    *error = attempt;
  }
  return result;
}
