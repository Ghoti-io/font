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
 * This library as `shape-scene`'s glyph path provider (scene.h; story 14b,
 * AD-5). documentation/design.md section 8.7.
 *
 * Two objects. A ::GFNT_SceneFont is per ::GFNT_Face: the key, the names and
 * the facts every instance shares, and the provider's counters. A binding is per
 * face resource (that is, per instance in a store): the instance's coordinates
 * and its cache of hint contexts (D8). The binding is the context the store
 * retains and releases, so the resource's last release frees the cache.
 *
 * Nothing here is floating point.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/scene.h>

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/font/color.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/variation.h>
#include <ghoti.io/shape-geometry/device.h>
#include <ghoti.io/shape-geometry/path.h>
#include <ghoti.io/shape-scene/resource.h>
#include <stdbool.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "unhinted.h"

/** FNV-1a 64's offset basis and prime. */
#define GFNT_SCENE_FNV_BASIS 0xCBF29CE484222325ull
#define GFNT_SCENE_FNV_PRIME 0x00000100000001B3ull

struct GFNT_SceneFont {
  const GFNT_Face * face;
  const GFNT_Allocator * allocator;
  GCU_Atomic_Size refs;
  uint64_t key;
  uint32_t upem;
  uint32_t axis_count;
  uint32_t glyph_count;
  uint32_t flags;      ///< GSHSCENE_FACE_*.
  uint32_t max_ppem;
  bool truetype;       ///< It has `glyf` outlines, the only ones the hinter runs.
  char * family;       ///< Or NULL.
  char * postscript;   ///< Or NULL.
  GCU_Atomic_Size info_calls;
  GCU_Atomic_Size general_calls;
  GCU_Atomic_Size fast_calls;
  GCU_Atomic_Size box_calls;
  GCU_Atomic_Size contexts_made;
  GCU_Atomic_Size contexts_dropped;
  GCU_Atomic_Size contexts_live;
  GCU_Atomic_Size bindings_live;
};

/** One entry of a binding's hint-context cache; empty while `context` is NULL. */
typedef struct GFNT_SceneSlot {
  GFNT_HintContext * context;
  uint32_t ppem;
  uint32_t mode;
  uint32_t version;
  uint64_t used;  ///< The cache's clock when it was last taken.
  size_t refs;    ///< Calls using it now; an entry in use is never dropped.
} GFNT_SceneSlot;

/** A face resource's binding: the provider's context. */
typedef struct GFNT_SceneBinding {
  GFNT_SceneFont * font;
  GCU_Atomic_Size refs;
  size_t coord_count;
  GFNT_F2Dot14 coords[GSHSCENE_FACE_MAX_AXES];
  GCU_MUTEX_T lock;
  uint64_t clock;
  GFNT_SceneSlot slots[GFNT_SCENE_HINT_CONTEXTS];
} GFNT_SceneBinding;

// --- results ----------------------------------------------------------------

/** This library's failure in the seam's spelling. */
static GSHSCENE_Result gfnt_scene_result(GFNT_Result r) {
  switch (r) {
    case GFNT_OK:
      return GSHSCENE_OK;
    case GFNT_ERR_IO:
      return GSHSCENE_ERR_IO;
    case GFNT_ERR_UNSUPPORTED:
      return GSHSCENE_ERR_UNSUPPORTED;
    case GFNT_ERR_LIMIT:
      return GSHSCENE_ERR_LIMIT;
    case GFNT_ERR_OOM:
      return GSHSCENE_ERR_OOM;
    case GFNT_ERR_INVALID:
      return GSHSCENE_ERR_INVALID;
    case GFNT_ERR_INTERNAL:
      return GSHSCENE_ERR_INTERNAL;
    default:
      return GSHSCENE_ERR_CORRUPT;   // a font with bytes that are wrong, or not a font
  }
}

/** The seam's failure in this library's spelling. */
static GFNT_Result gfnt_scene_from(GSHSCENE_Result r) {
  switch (r) {
    case GSHSCENE_OK:
      return GFNT_OK;
    case GSHSCENE_ERR_LIMIT:
      return GFNT_ERR_LIMIT;
    case GSHSCENE_ERR_OOM:
      return GFNT_ERR_OOM;
    case GSHSCENE_ERR_UNSUPPORTED:
      return GFNT_ERR_UNSUPPORTED;
    case GSHSCENE_ERR_CORRUPT:
      return GFNT_ERR_CORRUPT;
    case GSHSCENE_ERR_INTERNAL:
      return GFNT_ERR_INTERNAL;
    default:
      return GFNT_ERR_INVALID;
  }
}

static GSHSCENE_Result gfnt_scene_geometry(GSHGEOM_Result r) {
  return r == GSHGEOM_OK ? GSHSCENE_OK
      : r == GSHGEOM_ERR_OOM ? GSHSCENE_ERR_OOM
      : r == GSHGEOM_ERR_LIMIT ? GSHSCENE_ERR_LIMIT
      : GSHSCENE_ERR_INVALID;
}

// --- the scene font ---------------------------------------------------------

static void gfnt_scene_font_retain(GFNT_SceneFont * font) {
  gcu_atomic_size_fetch_add(&font->refs, 1);
}

static void gfnt_scene_font_release(GFNT_SceneFont * font) {
  if (!font || gcu_atomic_size_fetch_sub(&font->refs, 1) != 1) {
    return;
  }
  gfnt_name_free(font->allocator, font->family);
  gfnt_name_free(font->allocator, font->postscript);
  font->allocator->free_fn(font->allocator->ctx, font);
}

/** A name, or NULL where the face has none this library can read. */
static GFNT_Result gfnt_scene_name(const GFNT_SceneFont * font, uint16_t id,
    char ** out) {
  GFNT_Result r = gfnt_face_name(font->face, id, GFNT_LANGUAGE_ANY,
      font->allocator, out, NULL, NULL);

  if (r == GFNT_ERR_OOM) {
    return r;
  }
  if (r != GFNT_OK) {
    *out = NULL;   // an identity a writer cannot have is not a font it cannot draw
  }
  return GFNT_OK;
}

/** Whether the face carries a hinting program: TrueType outlines and an `fpgm`, a `prep` or a glyph program. */
static bool gfnt_scene_has_program(const GFNT_Face * face) {
  const uint8_t * maxp = NULL;
  size_t length = 0;

  if (gfnt_face_has_table(face, GFNT_TAG('f', 'p', 'g', 'm'))
      || gfnt_face_has_table(face, GFNT_TAG('p', 'r', 'e', 'p'))) {
    return true;
  }
  // maxp version 1.0's maxSizeOfInstructions, at offset 26: the longest glyph
  // program. A face whose glyphs carry programs and no fpgm or prep still hints.
  if (gfnt_face_table_data(face, GFNT_TAG('m', 'a', 'x', 'p'), &maxp, &length)
      != GFNT_OK || length < 28u || maxp[0] != 0x00 || maxp[1] != 0x01) {
    return false;
  }
  return ((uint32_t)maxp[26] << 8 | maxp[27]) != 0;
}

GFNT_Result gfnt_scene_font_create(const GFNT_Face * face,
    GFNT_SceneFont ** out_font, GFNT_Error * error) {
  const GFNT_Allocator * allocator;
  GFNT_SceneFont * font;
  uint16_t upem = 0;
  size_t axes = 0;
  size_t glyphs = 0;
  const uint8_t * bytes;
  size_t size;
  uint64_t hash = GFNT_SCENE_FNV_BASIS;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !out_font) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_face_units_per_em(face, &upem, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (upem < GSHSCENE_FACE_UPEM_MIN || upem > GSHSCENE_FACE_UPEM_MAX) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG('h', 'e', 'a', 'd'),
        0, GFNT_GLYPH_NONE, "a scene's face takes a unitsPerEm from 16 to 16384");
  }
  r = gfnt_face_num_glyphs(face, &glyphs, error);
  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_face_axis_count(face, &axes, error);
  if (r != GFNT_OK) {
    return r;
  }
  allocator = face->allocator ? face->allocator : gfnt_allocator_default();
  font = allocator->calloc_fn(allocator->ctx, 1, sizeof *font);
  if (!font) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for a scene font");
  }
  font->face = face;
  font->allocator = allocator;
  gcu_atomic_size_init(&font->refs, 1);
  font->upem = upem;
  font->axis_count = (uint32_t)(axes > GSHSCENE_FACE_MAX_AXES ? GSHSCENE_FACE_MAX_AXES : axes);
  font->glyph_count = (uint32_t)(glyphs > UINT32_MAX ? UINT32_MAX : glyphs);
  font->max_ppem = (uint32_t)(face->limits.max_ppem < GSHSCENE_GLYPH_MAX_PPEM
      ? face->limits.max_ppem : GSHSCENE_GLYPH_MAX_PPEM);
  font->truetype = gfnt_face_has_table(face, GFNT_TAG('g', 'l', 'y', 'f'));
  if (font->truetype && gfnt_scene_has_program(face)) {
    font->flags |= GSHSCENE_FACE_HINTING;
  }
  if (gfnt_face_has_color_layers(face)) {
    font->flags |= GSHSCENE_FACE_COLOR;
  }
  // The key is the font's bytes, as loaded: equal keys are equal fonts.
  bytes = gfnt_blob_data(face->blob);
  size = gfnt_blob_size(face->blob);
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * GFNT_SCENE_FNV_PRIME;
  }
  font->key = hash ? hash : 1u;
  r = gfnt_scene_name(font, GFNT_NAME_TYPOGRAPHIC_FAMILY, &font->family);
  if (r == GFNT_OK && !font->family) {
    r = gfnt_scene_name(font, GFNT_NAME_FAMILY, &font->family);
  }
  if (r == GFNT_OK) {
    r = gfnt_scene_name(font, GFNT_NAME_POSTSCRIPT, &font->postscript);
  }
  if (r != GFNT_OK) {
    gfnt_scene_font_release(font);
    return gfnt_error_set(error, r, 0, 0, GFNT_GLYPH_NONE,
        "no memory for a scene font's names");
  }
  *out_font = font;
  return GFNT_OK;
}

void gfnt_scene_font_destroy(GFNT_SceneFont * font) {
  gfnt_scene_font_release(font);
}

uint64_t gfnt_scene_font_key(const GFNT_SceneFont * font) {
  return font ? font->key : 0;
}

GFNT_Result gfnt_scene_font_stats(const GFNT_SceneFont * font,
    GFNT_SceneStats * out_stats) {
  if (!font || !out_stats) {
    return GFNT_ERR_INVALID;
  }
  memset(out_stats, 0, sizeof *out_stats);
  out_stats->info_calls = gcu_atomic_size_load(&font->info_calls);
  out_stats->general_calls = gcu_atomic_size_load(&font->general_calls);
  out_stats->fast_calls = gcu_atomic_size_load(&font->fast_calls);
  out_stats->box_calls = gcu_atomic_size_load(&font->box_calls);
  out_stats->contexts_made = gcu_atomic_size_load(&font->contexts_made);
  out_stats->contexts_dropped = gcu_atomic_size_load(&font->contexts_dropped);
  out_stats->contexts_live = gcu_atomic_size_load(&font->contexts_live);
  out_stats->bindings_live = gcu_atomic_size_load(&font->bindings_live);
  return GFNT_OK;
}

// --- the binding and its hint contexts (D8) ----------------------------------

static void gfnt_scene_binding_retain(void * ctx) {
  gcu_atomic_size_fetch_add(&((GFNT_SceneBinding *)ctx)->refs, 1);
}

static void gfnt_scene_binding_release(void * ctx) {
  GFNT_SceneBinding * b = ctx;
  GFNT_SceneFont * font;

  if (!b || gcu_atomic_size_fetch_sub(&b->refs, 1) != 1) {
    return;
  }
  font = b->font;
  for (size_t i = 0; i < GFNT_SCENE_HINT_CONTEXTS; ++i) {
    if (b->slots[i].context) {
      gfnt_hint_context_destroy(b->slots[i].context);
      gcu_atomic_size_fetch_sub(&font->contexts_live, 1);
    }
  }
  GCU_MUTEX_DESTROY(b->lock);
  gcu_atomic_size_fetch_sub(&font->bindings_live, 1);
  font->allocator->free_fn(font->allocator->ctx, b);
  gfnt_scene_font_release(font);
}

/** The binding's instance as a variation (default delta rounding). */
static GFNT_Variation gfnt_scene_variation(const GFNT_SceneBinding * b) {
  GFNT_Variation v;

  memset(&v, 0, sizeof v);
  v.coords = b->coord_count ? b->coords : NULL;
  v.count = b->coord_count;
  return v;
}

/** The live slot for a key, or -1. Called with the lock held. */
static int gfnt_scene_slot_find(const GFNT_SceneBinding * b, uint32_t ppem,
    uint32_t mode, uint32_t version) {
  for (int i = 0; i < (int)GFNT_SCENE_HINT_CONTEXTS; ++i) {
    const GFNT_SceneSlot * s = &b->slots[i];

    if (s->context && s->ppem == ppem && s->mode == mode && s->version == version) {
      return i;
    }
  }
  return -1;
}

/**
 * A hint context for `(ppem, mode, version)`: the cache's, or a new one put in
 * it, dropping the least recently used entry nobody is using; when every entry
 * is in use, one for this call alone (@p cached false), which the caller frees.
 * The context is made outside the lock, since its programs can be long.
 */
static GFNT_Result gfnt_scene_context_acquire(GFNT_SceneBinding * b,
    uint32_t ppem, uint32_t mode, uint32_t version, GFNT_HintContext ** out,
    bool * cached, GFNT_Error * error) {
  GFNT_SceneFont * font = b->font;
  GFNT_Variation variation = gfnt_scene_variation(b);
  GFNT_HintContext * made = NULL;
  GFNT_HintContext * dropped = NULL;
  GFNT_Result r;
  int i;

  if (GCU_MUTEX_LOCK(b->lock) != 0) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the hint-context cache's lock failed");
  }
  i = gfnt_scene_slot_find(b, ppem, mode, version);
  if (i >= 0) {
    b->slots[i].refs += 1;
    b->slots[i].used = ++b->clock;
    *out = b->slots[i].context;
    *cached = true;
    GCU_MUTEX_UNLOCK(b->lock);
    return GFNT_OK;
  }
  GCU_MUTEX_UNLOCK(b->lock);

  r = gfnt_hint_context_create(font->face, ppem, (GFNT_HintVersion)version,
      (GFNT_HintMode)mode, &variation, &made, error);
  if (r != GFNT_OK) {
    return r;
  }
  gcu_atomic_size_fetch_add(&font->contexts_made, 1);

  if (GCU_MUTEX_LOCK(b->lock) != 0) {
    gfnt_hint_context_destroy(made);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the hint-context cache's lock failed");
  }
  i = gfnt_scene_slot_find(b, ppem, mode, version);
  if (i >= 0) {
    // Another call made the same one meanwhile: take that, free this.
    b->slots[i].refs += 1;
    b->slots[i].used = ++b->clock;
    *out = b->slots[i].context;
    *cached = true;
    GCU_MUTEX_UNLOCK(b->lock);
    gfnt_hint_context_destroy(made);
    return GFNT_OK;
  }
  for (int k = 0; k < (int)GFNT_SCENE_HINT_CONTEXTS; ++k) {
    const GFNT_SceneSlot * s = &b->slots[k];

    if (!s->context) {
      i = k;
      break;
    }
    if (s->refs == 0 && (i < 0 || s->used < b->slots[i].used)) {
      i = k;
    }
  }
  if (i < 0) {
    // Every entry is in use: this call's context is its own.
    GCU_MUTEX_UNLOCK(b->lock);
    *out = made;
    *cached = false;
    return GFNT_OK;
  }
  if (b->slots[i].context) {
    dropped = b->slots[i].context;
    gcu_atomic_size_fetch_add(&font->contexts_dropped, 1);
  }
  else {
    gcu_atomic_size_fetch_add(&font->contexts_live, 1);
  }
  b->slots[i].context = made;
  b->slots[i].ppem = ppem;
  b->slots[i].mode = mode;
  b->slots[i].version = version;
  b->slots[i].refs = 1;
  b->slots[i].used = ++b->clock;
  GCU_MUTEX_UNLOCK(b->lock);
  gfnt_hint_context_destroy(dropped);
  *out = made;
  *cached = true;
  return GFNT_OK;
}

static void gfnt_scene_context_release(GFNT_SceneBinding * b,
    GFNT_HintContext * context, bool cached) {
  if (!cached) {
    gfnt_hint_context_destroy(context);
    return;
  }
  // A lock that fails here leaves the entry pinned (never dropped) rather than
  // freed under a user: the cache is then one smaller, and nothing is wrong.
  if (GCU_MUTEX_LOCK(b->lock) != 0) {
    return;
  }
  for (size_t i = 0; i < GFNT_SCENE_HINT_CONTEXTS; ++i) {
    if (b->slots[i].context == context && b->slots[i].refs > 0) {
      b->slots[i].refs -= 1;
      break;
    }
  }
  GCU_MUTEX_UNLOCK(b->lock);
}

// --- the ops ----------------------------------------------------------------

static GSHSCENE_Result gfnt_scene_face_info(void * ctx, const GSHSCENE_Face * face,
    GSHSCENE_Face_Info * out) {
  GFNT_SceneFont * font = ((GFNT_SceneBinding *)ctx)->font;

  (void)face;
  gcu_atomic_size_fetch_add(&font->info_calls, 1);
  memset(out, 0, sizeof *out);
  out->upem = font->upem;
  out->axis_count = font->axis_count;
  out->flags = font->flags;
  out->max_ppem = font->max_ppem;
  out->glyph_count = font->glyph_count;
  out->family_name = font->family;
  out->postscript_name = font->postscript;
  return GSHSCENE_OK;
}

/** The glyph's outline in design units at the binding's instance. */
static GFNT_Result gfnt_scene_units(const GFNT_SceneBinding * b, uint32_t glyph,
    GFNT_Outline ** out) {
  GFNT_Variation variation = gfnt_scene_variation(b);

  return gfnt_face_glyph_outline(b->font->face, glyph, &variation,
      b->font->allocator, out, NULL);
}

static GFNT_Result gfnt_scene_fixed_move(void * user, GFNT_Point to) {
  return gfnt_unhinted_geometry_error(gshgeom_path_move_to_fixed(user, to.x,
      to.y, GSHSCENE_GLYPH_OUTLINE_BITS), NULL);
}

static GFNT_Result gfnt_scene_fixed_line(void * user, GFNT_Point to) {
  return gfnt_unhinted_geometry_error(gshgeom_path_line_to_fixed(user, to.x,
      to.y, GSHSCENE_GLYPH_OUTLINE_BITS), NULL);
}

static GFNT_Result gfnt_scene_fixed_quad(void * user, GFNT_Point c,
    GFNT_Point to) {
  return gfnt_unhinted_geometry_error(gshgeom_path_quadratic_to_fixed(user, c.x,
      c.y, to.x, to.y, GSHSCENE_GLYPH_OUTLINE_BITS), NULL);
}

static GFNT_Result gfnt_scene_fixed_cubic(void * user, GFNT_Point a,
    GFNT_Point c, GFNT_Point to) {
  return gfnt_unhinted_geometry_error(gshgeom_path_cubic_to_fixed(user, a.x,
      a.y, c.x, c.y, to.x, to.y, GSHSCENE_GLYPH_OUTLINE_BITS), NULL);
}

static GFNT_Result gfnt_scene_fixed_close(void * user) {
  return gfnt_unhinted_geometry_error(gshgeom_path_close(user), NULL);
}

static GSHSCENE_Result gfnt_scene_glyph_outline(void * ctx,
    const GSHSCENE_Face * face, uint32_t glyph, GSHGEOM_Path * out) {
  static const GFNT_OutlineSink sink = {
    gfnt_scene_fixed_move,
    gfnt_scene_fixed_line,
    gfnt_scene_fixed_quad,
    gfnt_scene_fixed_cubic,
    gfnt_scene_fixed_close,
  };
  GFNT_SceneBinding * b = ctx;
  GFNT_Outline * units = NULL;
  GFNT_Result r;

  (void)face;
  gcu_atomic_size_fetch_add(&b->font->general_calls, 1);
  if (!out) {
    return GSHSCENE_ERR_INVALID;
  }
  r = gfnt_scene_units(b, glyph, &units);
  if (r == GFNT_OK) {
    // Walked in design units, so the implied midpoints are the ones the fast
    // tier's walk takes (unhinted.c), and given exactly: 26.6 of units.
    r = gfnt_outline_decompose(units, &sink, out, NULL);
  }
  gfnt_outline_destroy(units);
  return gfnt_scene_result(r);
}

/** A hinted outline's walk into a device path, each point moved by the fraction, y turned down. */
typedef struct GFNT_SceneHinted {
  GSHGEOM_Device_Path * path;
  int32_t fx;
  int32_t fy;
} GFNT_SceneHinted;

static GFNT_Result gfnt_scene_hinted_point(const GFNT_SceneHinted * h,
    GFNT_Point p, GSHGEOM_Device_Point * out) {
  const int64_t x = (int64_t)h->fx + p.x;
  const int64_t y = (int64_t)h->fy - p.y;

  if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
    return GFNT_ERR_LIMIT;
  }
  out->x = (int32_t)x;
  out->y = (int32_t)y;
  return GFNT_OK;
}

static GFNT_Result gfnt_scene_hinted_move(void * user, GFNT_Point to) {
  GFNT_SceneHinted * h = user;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_scene_hinted_point(h, to, &p);

  return r != GFNT_OK ? r
      : gfnt_unhinted_geometry_error(gshgeom_device_path_move_to(h->path, p), NULL);
}

static GFNT_Result gfnt_scene_hinted_line(void * user, GFNT_Point to) {
  GFNT_SceneHinted * h = user;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_scene_hinted_point(h, to, &p);

  return r != GFNT_OK ? r
      : gfnt_unhinted_geometry_error(gshgeom_device_path_line_to(h->path, p), NULL);
}

static GFNT_Result gfnt_scene_hinted_quad(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_SceneHinted * h = user;
  GSHGEOM_Device_Point c;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_scene_hinted_point(h, control, &c);

  if (r == GFNT_OK) {
    r = gfnt_scene_hinted_point(h, to, &p);
  }
  return r != GFNT_OK ? r
      : gfnt_unhinted_geometry_error(gshgeom_device_path_quadratic_to(h->path, c, p), NULL);
}

static GFNT_Result gfnt_scene_hinted_cubic(void * user, GFNT_Point c1,
    GFNT_Point c2, GFNT_Point to) {
  GFNT_SceneHinted * h = user;
  GSHGEOM_Device_Point a;
  GSHGEOM_Device_Point c;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_scene_hinted_point(h, c1, &a);

  if (r == GFNT_OK) {
    r = gfnt_scene_hinted_point(h, c2, &c);
  }
  if (r == GFNT_OK) {
    r = gfnt_scene_hinted_point(h, to, &p);
  }
  return r != GFNT_OK ? r
      : gfnt_unhinted_geometry_error(gshgeom_device_path_cubic_to(h->path, a, c, p), NULL);
}

static GFNT_Result gfnt_scene_hinted_close(void * user) {
  return gfnt_unhinted_geometry_error(
      gshgeom_device_path_close(((GFNT_SceneHinted *)user)->path), NULL);
}

/** The unhinted fast tier (D1) and its advance. */
static GFNT_Result gfnt_scene_unhinted(const GFNT_SceneBinding * b,
    const GSHSCENE_Glyph_Request * rq, GSHGEOM_Device_Path * out,
    int32_t * advance) {
  GFNT_Variation variation = gfnt_scene_variation(b);
  GFNT_Outline * units = NULL;
  int32_t units_advance = 0;
  GFNT_Result r = gfnt_scene_units(b, rq->glyph, &units);

  if (r == GFNT_OK) {
    r = gfnt_unhinted_device_path(units, rq->scale, (int32_t)rq->frac_x,
        (int32_t)rq->frac_y, out, NULL);
  }
  gfnt_outline_destroy(units);
  if (r != GFNT_OK) {
    return r;
  }
  // The advance does not draw anything: a face that states none (or none this
  // library can read at the instance) has 0, and the glyph is still drawn.
  *advance = 0;
  if (gfnt_face_glyph_advance(b->font->face, rq->glyph, &variation,
          &units_advance, NULL) == GFNT_OK
      && units_advance >= -(INT32_MAX / 64) && units_advance <= INT32_MAX / 64) {
    int32_t scaled = 0;

    if (gshgeom_scale_26_6(units_advance * 64, rq->scale, 0, &scaled) == GSHGEOM_OK) {
      *advance = scaled;
    }
  }
  return GFNT_OK;
}

static GSHSCENE_Result gfnt_scene_glyph_device_path(void * ctx,
    const GSHSCENE_Face * face, const GSHSCENE_Glyph_Request * rq,
    GSHGEOM_Device_Path * out, GSHSCENE_Glyph_Answer * answer) {
  static const GFNT_OutlineSink hinted_sink = {
    gfnt_scene_hinted_move,
    gfnt_scene_hinted_line,
    gfnt_scene_hinted_quad,
    gfnt_scene_hinted_cubic,
    gfnt_scene_hinted_close,
  };
  GFNT_SceneBinding * b = ctx;
  GFNT_SceneFont * font = b->font;
  uint32_t status = GSHSCENE_HINT_STATUS_NO_PROGRAM;
  int32_t advance = 0;
  int32_t scale = 0;
  GFNT_Result r;

  (void)face;
  gcu_atomic_size_fetch_add(&font->fast_calls, 1);
  if (!rq || !out || !answer || rq->ppem == 0 || rq->ppem > font->max_ppem
      || rq->frac_x >= 64u || rq->frac_y >= 64u
      || rq->hint_mode >= GSHSCENE_HINT_MODE_COUNT
      || rq->hint_version > GFNT_HINT_VERSION_40) {
    return GSHSCENE_ERR_INVALID;
  }
  // The request's scale is the seam's snapped scale of its size, which is this
  // library's own (gfnt_scale_for_ppem) for every upem the seam takes.
  if (gshscene_glyph_scale(rq->ppem, font->upem, &scale) != GSHSCENE_OK
      || scale != rq->scale) {
    return GSHSCENE_ERR_INVALID;
  }
  if (rq->hint_mode != GSHSCENE_HINT_NONE && font->truetype
      && gfnt_face_gasp_gridfit(font->face, rq->ppem)) {
    GFNT_HintContext * context = NULL;
    bool cached = false;
    GFNT_HintStatus hinted = GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;

    r = gfnt_scene_context_acquire(b, rq->ppem, rq->hint_mode, rq->hint_version,
        &context, &cached, NULL);
    if (r == GFNT_OK) {
      GFNT_Outline * pixels = NULL;
      GFNT_F26Dot6 hinted_advance = 0;

      r = gfnt_face_hinted_outline(font->face, context, rq->glyph,
          font->allocator, &pixels, &hinted, &hinted_advance, NULL);
      if (r == GFNT_OK && (hinted == GFNT_HINT_STATUS_HINTED
          || hinted == GFNT_HINT_STATUS_HINTED_WITH_ERRORS)) {
        GFNT_SceneHinted h;

        h.path = out;
        h.fx = (int32_t)rq->frac_x;
        h.fy = (int32_t)rq->frac_y;
        r = gfnt_outline_decompose(pixels, &hinted_sink, &h, NULL);
        gfnt_outline_destroy(pixels);
        gfnt_scene_context_release(b, context, cached);
        if (r != GFNT_OK) {
          return gfnt_scene_result(r);
        }
        answer->status = hinted == GFNT_HINT_STATUS_HINTED
            ? GSHSCENE_HINT_STATUS_HINTED : GSHSCENE_HINT_STATUS_WITH_ERRORS;
        answer->advance = hinted_advance;
        return GSHSCENE_OK;
      }
      gfnt_outline_destroy(pixels);
      gfnt_scene_context_release(b, context, cached);
      if (r != GFNT_OK) {
        return gfnt_scene_result(r);
      }
      if (hinted == GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED) {
        if (rq->hint_mode == GSHSCENE_HINT_FONT_STRICT) {
          return GSHSCENE_ERR_REFUSED;   // hinted or refused (D6)
        }
        status = GSHSCENE_HINT_STATUS_PROGRAM_FAILED;
      }
      // Otherwise the glyph has no program: unhinted, NO_PROGRAM.
    }
    else if (r != GFNT_ERR_UNSUPPORTED) {
      return gfnt_scene_result(r);
    }
  }
  // Unhinted (D1): mode NONE, no TrueType outlines, a size gasp does not fit,
  // no program, or a program that failed.
  r = gfnt_scene_unhinted(b, rq, out, &advance);
  if (r != GFNT_OK) {
    return gfnt_scene_result(r);
  }
  answer->status = status;
  answer->advance = advance;
  return GSHSCENE_OK;
}

static GSHSCENE_Result gfnt_scene_glyph_box(void * ctx,
    const GSHSCENE_Face * face, uint32_t glyph, GSHSCENE_Glyph_Box * out) {
  GFNT_SceneBinding * b = ctx;
  GFNT_Outline * units = NULL;
  GFNT_Box box;
  GFNT_Result r;

  (void)face;
  gcu_atomic_size_fetch_add(&b->font->box_calls, 1);
  if (!out) {
    return GSHSCENE_ERR_INVALID;
  }
  r = gfnt_scene_units(b, glyph, &units);
  if (r == GFNT_OK) {
    r = gfnt_outline_control_box(units, &box);
  }
  gfnt_outline_destroy(units);
  if (r != GFNT_OK) {
    return gfnt_scene_result(r);
  }
  memset(out, 0, sizeof *out);
  if (gfnt_box_is_empty(&box)) {
    out->empty = 1;
    return GSHSCENE_OK;
  }
  out->x_min = box.x_min;
  out->y_min = box.y_min;
  out->x_max = box.x_max;
  out->y_max = box.y_max;
  return GSHSCENE_OK;
}

static GSHSCENE_Result gfnt_scene_face_data(void * ctx,
    const GSHSCENE_Face * face, const uint8_t ** data, size_t * size) {
  const GFNT_SceneFont * font = ((GFNT_SceneBinding *)ctx)->font;

  (void)face;
  if (!data || !size) {
    return GSHSCENE_ERR_INVALID;
  }
  *data = gfnt_blob_data(font->face->blob);
  *size = gfnt_blob_size(font->face->blob);
  return GSHSCENE_OK;
}

static const GSHSCENE_Glyph_Provider gfnt_scene_ops = {
  GSHSCENE_GLYPH_PROVIDER_VERSION,
  gfnt_scene_face_info,
  gfnt_scene_glyph_outline,
  gfnt_scene_glyph_device_path,
  gfnt_scene_glyph_box,
  gfnt_scene_face_data,
  gfnt_scene_binding_retain,
  gfnt_scene_binding_release,
};

const GSHSCENE_Glyph_Provider * gfnt_scene_provider(void) {
  return &gfnt_scene_ops;
}

GFNT_Result gfnt_scene_intern_face(GFNT_SceneFont * font,
    GSHSCENE_Store * store, const GFNT_Variation * variation,
    const GSHSCENE_Resource ** out_face, GFNT_Error * error) {
  GFNT_SceneBinding * b;
  GSHSCENE_Face_Desc desc;
  GSHSCENE_Result r;
  size_t count = variation ? variation->count : 0;

  gfnt_error_clear(error);
  if (!font || !store || !out_face) {
    return GFNT_ERR_INVALID;
  }
  if (count && !variation->coords) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a variation with a count and no coordinates");
  }
  if (count > font->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG('f', 'v', 'a', 'r'), 0,
        GFNT_GLYPH_NONE, "a variation with more coordinates than the face has axes");
  }
  if (variation && variation->delta_rounding != 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "a face resource's content has no place for a delta rounding other than the default");
  }
  for (size_t i = 0; i < count; ++i) {
    if (variation->coords[i] < -GSHSCENE_F2DOT14_ONE
        || variation->coords[i] > GSHSCENE_F2DOT14_ONE) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a normalised coordinate outside [-1, 1]");
    }
  }
  b = font->allocator->calloc_fn(font->allocator->ctx, 1, sizeof *b);
  if (!b) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for a face binding");
  }
  if (GCU_MUTEX_CREATE(b->lock) != 0) {
    font->allocator->free_fn(font->allocator->ctx, b);
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "a face binding's lock could not be made");
  }
  gfnt_scene_font_retain(font);
  gcu_atomic_size_fetch_add(&font->bindings_live, 1);
  b->font = font;
  gcu_atomic_size_init(&b->refs, 1);   // this call's, dropped below
  b->coord_count = count;
  if (count) {
    memcpy(b->coords, variation->coords, count * sizeof *b->coords);
  }

  memset(&desc, 0, sizeof desc);
  desc.font_key = font->key;
  desc.face_index = (uint32_t)font->face->index;
  desc.coord_count = (uint32_t)count;
  desc.coords = count ? b->coords : NULL;
  desc.provider = &gfnt_scene_ops;
  desc.ctx = b;
  r = gshscene_store_intern_face(store, &desc, out_face);
  // The store retained the binding if it built a face with it; an equal face it
  // already held keeps its own, and then this one goes here.
  gfnt_scene_binding_release(b);
  if (r != GSHSCENE_OK) {
    return gfnt_error_set(error, gfnt_scene_from(r), 0, 0, GFNT_GLYPH_NONE,
        "the scene's store refused the face");
  }
  return GFNT_OK;
}
