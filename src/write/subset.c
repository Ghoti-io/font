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
 * The subsetter: which glyphs to keep, and the tables that follow from it.
 *
 * documentation/design.md section 12.3. The order is the one the data forces:
 * the glyph set (requested characters, requested ids, then closed over `GSUB`
 * and over composite components), the numbering, and only then the tables,
 * because every one of them is indexed by glyph.
 */

#include <stdlib.h>
#include <string.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/metrics.h>
#include "../glyf/glyf.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "write.h"

#define T_HEAD GFNT_TAG('h', 'e', 'a', 'd')
#define T_HHEA GFNT_TAG('h', 'h', 'e', 'a')
#define T_MAXP GFNT_TAG('m', 'a', 'x', 'p')
#define T_HMTX GFNT_TAG('h', 'm', 't', 'x')
#define T_CMAP GFNT_TAG('c', 'm', 'a', 'p')
#define T_LOCA GFNT_TAG('l', 'o', 'c', 'a')
#define T_GLYF GFNT_TAG('g', 'l', 'y', 'f')
#define T_POST GFNT_TAG('p', 'o', 's', 't')
#define T_OS2 GFNT_TAG('O', 'S', '/', '2')
#define T_NAME GFNT_TAG('n', 'a', 'm', 'e')
#define T_GASP GFNT_TAG('g', 'a', 's', 'p')
#define T_CVT GFNT_TAG('c', 'v', 't', ' ')
#define T_FPGM GFNT_TAG('f', 'p', 'g', 'm')
#define T_PREP GFNT_TAG('p', 'r', 'e', 'p')
#define T_GDEF GFNT_TAG('G', 'D', 'E', 'F')
#define T_GSUB GFNT_TAG('G', 'S', 'U', 'B')
#define T_GPOS GFNT_TAG('G', 'P', 'O', 'S')
#define T_KERN GFNT_TAG('k', 'e', 'r', 'n')
#define T_VHEA GFNT_TAG('v', 'h', 'e', 'a')
#define T_VMTX GFNT_TAG('v', 'm', 't', 'x')
#define T_VORG GFNT_TAG('V', 'O', 'R', 'G')

#define NO_GLYPH 0xFFFFFFFFu

#define COMPOSITE_ARGS_ARE_WORDS 0x0001u
#define COMPOSITE_HAVE_SCALE 0x0008u
#define COMPOSITE_MORE 0x0020u
#define COMPOSITE_HAVE_XY_SCALE 0x0040u
#define COMPOSITE_HAVE_2X2 0x0080u
#define COMPOSITE_HAVE_INSTRUCTIONS 0x0100u

void gfnt_subset_options_init(GFNT_SubsetOptions * options) {
  if (options) {
    memset(options, 0, sizeof *options);
  }
}

/** The bytes of a table of the source face, or false when it has none. */
static bool source_table(const GFNT_Face * face, GFNT_Tag tag,
    const uint8_t ** out, size_t * out_length) {
  GFNT_Reader reader;

  if (!gfnt_face_has_table(face, tag)
      || gfnt_face_table_reader(face, tag, &reader, NULL) != GFNT_OK
      || gfnt_read_bytes(&reader, reader.length, out) != GFNT_OK) {
    return false;
  }
  *out_length = reader.length;
  return true;
}

static int compare_u32(const void * a, const void * b) {
  const uint32_t * x = a;
  const uint32_t * y = b;

  return *x < *y ? -1 : *x > *y;
}

/** The size in bytes of the arguments and transform after a component's glyph id. */
static size_t component_tail(uint16_t flags) {
  size_t n = (flags & COMPOSITE_ARGS_ARE_WORDS) ? 4 : 2;

  if (flags & COMPOSITE_HAVE_SCALE) {
    n += 2;
  }
  else if (flags & COMPOSITE_HAVE_XY_SCALE) {
    n += 4;
  }
  else if (flags & COMPOSITE_HAVE_2X2) {
    n += 8;
  }
  return n;
}

/** The bytes of glyph @p glyph in `glyf`, and whether it is a composite. */
static GFNT_Result glyph_data(const GFNT_Face * face, const GFNT_Reader * glyf,
    uint32_t glyph, const uint8_t ** out, size_t * out_length,
    GFNT_Error * error) {
  size_t offset = 0;
  size_t length = 0;
  GFNT_Reader part;
  GFNT_Result result = gfnt_loca_range(face, glyph, &offset, &length, error);

  *out = NULL;
  *out_length = 0;
  if (result != GFNT_OK || length == 0) {
    return result;
  }
  result = gfnt_reader_sub(glyf, offset, length, &part);
  if (result == GFNT_OK) {
    result = gfnt_read_bytes(&part, length, out);
  }
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, offset, glyph,
        "a glyph runs past the end of glyf");
  }
  *out_length = length;
  return GFNT_OK;
}

/** Add the components of a composite glyph to the kept set. */
static GFNT_Result scan_composite(const uint8_t * data, size_t length,
    uint8_t * keep, size_t count, uint32_t * stack, size_t * sp, uint32_t glyph,
    GFNT_Error * error) {
  GFNT_Reader r;
  int16_t contours = 0;
  uint16_t flags;
  uint16_t component;

  if (gfnt_reader_init(&r, data, length, T_GLYF, NULL) != GFNT_OK
      || gfnt_read_s16(&r, &contours) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, 0, glyph,
        "a glyph shorter than its header");
  }
  if (contours >= 0) {
    return GFNT_OK;
  }
  if (gfnt_reader_skip(&r, 8) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, 2, glyph,
        "a glyph shorter than its header");
  }
  do {
    if (gfnt_read_u16(&r, &flags) != GFNT_OK
        || gfnt_read_u16(&r, &component) != GFNT_OK
        || gfnt_reader_skip(&r, component_tail(flags)) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, gfnt_reader_tell(&r),
          glyph, "a composite glyph's component list is cut short");
    }
    if (component >= count) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, gfnt_reader_tell(&r),
          glyph, "a composite names a glyph the font does not have");
    }
    if (!keep[component]) {
      keep[component] = 1;
      stack[(*sp)++] = component;
    }
  } while (flags & COMPOSITE_MORE);
  return GFNT_OK;
}

/**
 * Append one glyph's description with its component ids renumbered and, when
 * asked, its instructions removed.
 */
static GFNT_Result rewrite_glyph(const uint8_t * data, size_t length,
    const uint32_t * new_of_old, size_t count, bool drop_hinting, GFNT_WBuf * out,
    uint32_t glyph, GFNT_Error * error) {
  GFNT_Reader r;
  int16_t contours = 0;
  uint16_t flags = 0;
  uint16_t component = 0;
  uint16_t instructions = 0;
  const uint8_t * tail = NULL;
  size_t n;

  if (length == 0) {
    return GFNT_OK;
  }
  if (gfnt_reader_init(&r, data, length, T_GLYF, NULL) != GFNT_OK
      || gfnt_read_s16(&r, &contours) != GFNT_OK
      || gfnt_reader_skip(&r, 8) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, 0, glyph,
        "a glyph shorter than its header");
  }
  if (contours >= 0) {
    if (!drop_hinting) {
      gfnt_wbuf_bytes(out, data, length);
      return GFNT_OK;
    }
    // Header and end points as they are, then a zero instruction length, then
    // everything after the instructions.
    n = 10 + 2 * (size_t)contours;
    if (n + 2 > length || gfnt_reader_skip(&r, 2 * (size_t)contours) != GFNT_OK
        || gfnt_read_u16(&r, &instructions) != GFNT_OK
        || gfnt_reader_skip(&r, instructions) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, 10, glyph,
          "a simple glyph's instructions run past its end");
    }
    gfnt_wbuf_bytes(out, data, n);
    gfnt_wbuf_u16(out, 0);
    gfnt_wbuf_bytes(out, data + gfnt_reader_tell(&r), length - gfnt_reader_tell(&r));
    return GFNT_OK;
  }
  gfnt_wbuf_bytes(out, data, 10);
  do {
    if (gfnt_read_u16(&r, &flags) != GFNT_OK
        || gfnt_read_u16(&r, &component) != GFNT_OK
        || gfnt_read_bytes(&r, component_tail(flags), &tail) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, gfnt_reader_tell(&r),
          glyph, "a composite glyph's component list is cut short");
    }
    if (component >= count || new_of_old[component] == NO_GLYPH) {
      return gfnt_error_set(error, GFNT_ERR_INTERNAL, T_GLYF, 0, glyph,
          "a composite's component was not kept");
    }
    gfnt_wbuf_u16(out, drop_hinting ? (flags & ~COMPOSITE_HAVE_INSTRUCTIONS) : flags);
    gfnt_wbuf_u16(out, new_of_old[component]);
    gfnt_wbuf_bytes(out, tail, component_tail(flags));
  } while (flags & COMPOSITE_MORE);
  if ((flags & COMPOSITE_HAVE_INSTRUCTIONS) && !drop_hinting) {
    const uint8_t * rest = NULL;
    size_t left = gfnt_reader_remaining(&r);

    if (gfnt_read_bytes(&r, left, &rest) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, T_GLYF, 0, glyph,
          "a composite's instructions are cut short");
    }
    gfnt_wbuf_bytes(out, rest, left);
  }
  return GFNT_OK;
}

/** A copy of a source table, for patching. */
static GFNT_Result copy_table(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_WBuf * out, size_t min_length, GFNT_Error * error) {
  const uint8_t * data;
  size_t length;

  if (!source_table(face, tag, &data, &length) || length < min_length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a required table is missing or shorter than its fixed part");
  }
  gfnt_wbuf_bytes(out, data, length);
  return out->oom ? gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
      "copying a table") : GFNT_OK;
}

#define MAX_OUT_TABLES 20

typedef struct GFNT_SubsetWork {
  const GFNT_Allocator * a;
  uint8_t * keep;
  uint32_t * stack;
  uint32_t * mapped_cp;
  uint32_t * mapped_old;
  uint32_t * mapped_new;
  uint32_t * new_of_old;
  uint32_t * old_of_new;
  uint32_t * offsets;
  GFNT_WBuf bufs[8]; // head hhea maxp hmtx glyf loca cmap post-or-os2 scratch
  GFNT_WBuf os2;
  GFNT_WBuf post;
} GFNT_SubsetWork;

enum { B_HEAD, B_HHEA, B_MAXP, B_HMTX, B_GLYF, B_LOCA, B_CMAP };

static void work_free(GFNT_SubsetWork * w) {
  size_t i;

  for (i = 0; i < 7; ++i) {
    gfnt_wbuf_free(&w->bufs[i]);
  }
  gfnt_wbuf_free(&w->os2);
  gfnt_wbuf_free(&w->post);
  w->a->free_fn(w->a->ctx, w->keep);
  w->a->free_fn(w->a->ctx, w->stack);
  w->a->free_fn(w->a->ctx, w->mapped_cp);
  w->a->free_fn(w->a->ctx, w->mapped_old);
  w->a->free_fn(w->a->ctx, w->mapped_new);
  w->a->free_fn(w->a->ctx, w->new_of_old);
  w->a->free_fn(w->a->ctx, w->old_of_new);
  w->a->free_fn(w->a->ctx, w->offsets);
}

GFNT_Result gfnt_subset(const GFNT_Face * face, const GFNT_SubsetOptions * options,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_SubsetOptions defaults;
  GFNT_Limits default_limits;
  GFNT_SubsetWork w;
  GFNT_Reader glyf;
  GFNT_Reader hmtx;
  GFNT_WriteTable tables[MAX_OUT_TABLES];
  size_t ntables = 0;
  const GFNT_Hhea * hhea = NULL;
  size_t count = 0;
  size_t new_count = 0;
  size_t mapped = 0;
  size_t sp = 0;
  size_t i;
  uint32_t g;
  bool retain;
  GFNT_Result result;

  if (!face || !out_blob) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the font");
  }
  if (!options) {
    gfnt_subset_options_init(&defaults);
    options = &defaults;
  }
  if (!limits) {
    gfnt_limits_default(&default_limits);
    limits = &default_limits;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if ((options->codepoint_count && !options->codepoints)
      || (options->glyph_count && !options->glyphs)
      || (options->feature_count && !options->features)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a count with no array");
  }
  if (gfnt_sfnt_producer(face) != GFNT_PRODUCER_GLYF
      || gfnt_face_has_table(face, GFNT_TAG('f', 'v', 'a', 'r'))
      || gfnt_face_has_table(face, GFNT_TAG('g', 'v', 'a', 'r'))) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "only static TrueType-outline fonts can be subset");
  }
  if (!gfnt_face_has_table(face, T_HEAD) || !gfnt_face_has_table(face, T_HHEA)
      || !gfnt_face_has_table(face, T_MAXP) || !gfnt_face_has_table(face, T_HMTX)
      || !gfnt_face_has_table(face, T_CMAP)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "a font without head, hhea, maxp, hmtx and cmap cannot be subset");
  }
  retain = options->retain_gids;
  memset(&w, 0, sizeof w);
  w.a = allocator;
  for (i = 0; i < 7; ++i) {
    gfnt_wbuf_init(&w.bufs[i], allocator);
  }
  gfnt_wbuf_init(&w.os2, allocator);
  gfnt_wbuf_init(&w.post, allocator);

  result = gfnt_face_num_glyphs(face, &count, error);
  if (result != GFNT_OK) {
    goto done;
  }
  if (count == 0 || count > limits->max_glyphs) {
    result = gfnt_error_set(error, count ? GFNT_ERR_LIMIT : GFNT_ERR_CORRUPT, T_MAXP,
        0, GFNT_GLYPH_NONE, "the face has no glyphs, or more than the limit");
    goto done;
  }
  result = gfnt_face_hhea(face, &hhea, error);
  if (result != GFNT_OK) {
    goto done;
  }
  w.keep = allocator->calloc_fn(allocator->ctx, count, 1);
  w.stack = allocator->calloc_fn(allocator->ctx, count, sizeof *w.stack);
  w.new_of_old = allocator->calloc_fn(allocator->ctx, count, sizeof *w.new_of_old);
  w.mapped_cp = allocator->calloc_fn(allocator->ctx,
      options->codepoint_count + 1, sizeof *w.mapped_cp);
  w.mapped_old = allocator->calloc_fn(allocator->ctx,
      options->codepoint_count + 1, sizeof *w.mapped_old);
  w.mapped_new = allocator->calloc_fn(allocator->ctx,
      options->codepoint_count + 1, sizeof *w.mapped_new);
  if (!w.keep || !w.stack || !w.new_of_old || !w.mapped_cp || !w.mapped_old
      || !w.mapped_new) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the glyph set");
    goto done;
  }

  // The requested characters, sorted and unique, with the glyph each maps to.
  {
    uint32_t * sorted = allocator->calloc_fn(allocator->ctx,
        options->codepoint_count + 1, sizeof *sorted);

    if (!sorted) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "allocating the character list");
      goto done;
    }
    memcpy(sorted, options->codepoints, options->codepoint_count * sizeof *sorted);
    qsort(sorted, options->codepoint_count, sizeof *sorted, compare_u32);
    for (i = 0; i < options->codepoint_count; ++i) {
      uint32_t glyph = 0;

      if (i && sorted[i] == sorted[i - 1]) {
        continue;
      }
      result = gfnt_face_glyph_for_codepoint(face, sorted[i], &glyph, error);
      if (result != GFNT_OK) {
        allocator->free_fn(allocator->ctx, sorted);
        goto done;
      }
      if (glyph != 0 && glyph < count) {
        w.mapped_cp[mapped] = sorted[i];
        w.mapped_old[mapped] = glyph;
        ++mapped;
      }
    }
    allocator->free_fn(allocator->ctx, sorted);
  }
  w.keep[0] = 1;
  for (i = 0; i < mapped; ++i) {
    w.keep[w.mapped_old[i]] = 1;
  }
  for (i = 0; i < options->glyph_count; ++i) {
    if (options->glyphs[i] >= count) {
      result = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, options->glyphs[i],
          "a requested glyph id the face does not have");
      goto done;
    }
    w.keep[options->glyphs[i]] = 1;
  }
  if (!options->no_gsub_closure) {
    result = gfnt_subset_gsub_closure(face, w.keep, count, options->features,
        options->feature_count, limits, allocator, error);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  result = gfnt_face_table_reader(face, T_GLYF, &glyf, error);
  if (result != GFNT_OK) {
    goto done;
  }
  for (g = 0; g < count; ++g) {
    if (w.keep[g]) {
      w.stack[sp++] = g;
    }
  }
  while (sp) {
    const uint8_t * data;
    size_t length;
    uint32_t top = w.stack[--sp];

    result = glyph_data(face, &glyf, top, &data, &length, error);
    if (result == GFNT_OK && length) {
      result = scan_composite(data, length, w.keep, count, w.stack, &sp, top, error);
    }
    if (result != GFNT_OK) {
      goto done;
    }
  }

  // The numbering.
  w.old_of_new = allocator->calloc_fn(allocator->ctx, count, sizeof *w.old_of_new);
  if (!w.old_of_new) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the glyph numbering");
    goto done;
  }
  for (g = 0; g < count; ++g) {
    w.new_of_old[g] = NO_GLYPH;
  }
  if (retain) {
    for (g = 0; g < count; ++g) {
      w.old_of_new[g] = w.keep[g] ? g : NO_GLYPH;
      if (w.keep[g]) {
        w.new_of_old[g] = g;
        new_count = (size_t)g + 1;
      }
    }
  }
  else {
    for (g = 0; g < count; ++g) {
      if (w.keep[g]) {
        w.new_of_old[g] = (uint32_t)new_count;
        w.old_of_new[new_count++] = g;
      }
    }
  }

  // glyf and loca.
  w.offsets = allocator->calloc_fn(allocator->ctx, new_count + 1, sizeof *w.offsets);
  if (!w.offsets) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "allocating the glyph offsets");
    goto done;
  }
  for (i = 0; i < new_count; ++i) {
    w.offsets[i] = (uint32_t)w.bufs[B_GLYF].length;
    if (w.old_of_new[i] != NO_GLYPH) {
      const uint8_t * data;
      size_t length;

      result = glyph_data(face, &glyf, w.old_of_new[i], &data, &length, error);
      if (result == GFNT_OK) {
        result = rewrite_glyph(data, length, w.new_of_old, count, options->drop_hinting,
            &w.bufs[B_GLYF], w.old_of_new[i], error);
      }
      if (result != GFNT_OK) {
        goto done;
      }
      gfnt_wbuf_align(&w.bufs[B_GLYF], 4);
    }
    if (w.bufs[B_GLYF].oom || w.bufs[B_GLYF].length > limits->max_blob_bytes) {
      result = gfnt_error_set(error, w.bufs[B_GLYF].oom ? GFNT_ERR_OOM : GFNT_ERR_LIMIT,
          T_GLYF, 0, GFNT_GLYPH_NONE, "building glyf");
      goto done;
    }
  }
  w.offsets[new_count] = (uint32_t)w.bufs[B_GLYF].length;
  {
    bool long_loca = w.bufs[B_GLYF].length > 0x1FFFEu;

    for (i = 0; i <= new_count; ++i) {
      if (long_loca) {
        gfnt_wbuf_u32(&w.bufs[B_LOCA], w.offsets[i]);
      }
      else {
        gfnt_wbuf_u16(&w.bufs[B_LOCA], w.offsets[i] / 2);
      }
    }
    result = copy_table(face, T_HEAD, &w.bufs[B_HEAD], 54, error);
    if (result != GFNT_OK) {
      goto done;
    }
    gfnt_write_put16(w.bufs[B_HEAD].data + 50, long_loca ? 1u : 0u);
  }

  // hmtx, hhea, maxp.
  {
    size_t nhm = hhea->number_of_h_metrics;
    const uint8_t * data = NULL;
    size_t span = 0;
    uint16_t last_advance = 0;
    size_t metrics = new_count;
    uint16_t * adv;
    int16_t * lsb;

    if (nhm == 0 || !source_table(face, T_HMTX, &data, &span)) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, T_HMTX, 0, GFNT_GLYPH_NONE,
          "hmtx is missing or hhea says it has no metrics");
      goto done;
    }
    (void)hmtx;
    adv = allocator->calloc_fn(allocator->ctx, new_count, sizeof *adv);
    lsb = allocator->calloc_fn(allocator->ctx, new_count, sizeof *lsb);
    if (!adv || !lsb) {
      allocator->free_fn(allocator->ctx, adv);
      allocator->free_fn(allocator->ctx, lsb);
      result = gfnt_error_set(error, GFNT_ERR_OOM, T_HMTX, 0, GFNT_GLYPH_NONE,
          "allocating the metrics");
      goto done;
    }
    for (i = 0; i < new_count; ++i) {
      uint32_t old = w.old_of_new[i];
      size_t a_at;
      size_t l_at;

      if (old == NO_GLYPH) {
        continue;
      }
      a_at = 4 * (old < nhm ? (size_t)old : nhm - 1);
      l_at = old < nhm ? 4 * (size_t)old + 2 : 4 * nhm + 2 * ((size_t)old - nhm);
      if (a_at + 2 <= span) {
        adv[i] = (uint16_t)((data[a_at] << 8) | data[a_at + 1]);
      }
      if (l_at + 2 <= span) {
        lsb[i] = (int16_t)((data[l_at] << 8) | data[l_at + 1]);
      }
    }
    last_advance = adv[new_count - 1];
    while (metrics > 1 && adv[metrics - 2] == last_advance) {
      --metrics;
    }
    for (i = 0; i < new_count; ++i) {
      if (i < metrics) {
        gfnt_wbuf_u16(&w.bufs[B_HMTX], adv[i]);
      }
      gfnt_wbuf_u16(&w.bufs[B_HMTX], (uint16_t)lsb[i]);
    }
    allocator->free_fn(allocator->ctx, adv);
    allocator->free_fn(allocator->ctx, lsb);
    result = copy_table(face, T_HHEA, &w.bufs[B_HHEA], 36, error);
    if (result == GFNT_OK) {
      gfnt_write_put16(w.bufs[B_HHEA].data + 34, (uint32_t)metrics);
      result = copy_table(face, T_MAXP, &w.bufs[B_MAXP], 6, error);
    }
    if (result != GFNT_OK) {
      goto done;
    }
    gfnt_write_put16(w.bufs[B_MAXP].data + 4, (uint32_t)new_count);
  }

  // cmap.
  for (i = 0; i < mapped; ++i) {
    w.mapped_new[i] = w.new_of_old[w.mapped_old[i]];
  }
  result = gfnt_subset_cmap(w.mapped_cp, w.mapped_new, mapped, &w.bufs[B_CMAP], error);
  if (result != GFNT_OK) {
    goto done;
  }

  // OS/2 with the character range brought in line, post without glyph names.
  if (gfnt_face_has_table(face, T_OS2)) {
    result = copy_table(face, T_OS2, &w.os2, 4, error);
    if (result != GFNT_OK) {
      goto done;
    }
    if (w.os2.length >= 68) {
      uint32_t first = mapped ? (w.mapped_cp[0] > 0xFFFFu ? 0xFFFFu : w.mapped_cp[0]) : 0xFFFFu;
      uint32_t last = 0;

      for (i = 0; i < mapped; ++i) {
        last = w.mapped_cp[i] > 0xFFFFu ? 0xFFFFu : w.mapped_cp[i];
      }
      gfnt_write_put16(w.os2.data + 64, first);
      gfnt_write_put16(w.os2.data + 66, last);
    }
  }
  {
    const uint8_t * data;
    size_t length;

    if (source_table(face, T_POST, &data, &length) && length >= 32) {
      gfnt_wbuf_bytes(&w.post, data, 32);
      if (!w.post.oom) {
        gfnt_write_put32(w.post.data, 0x00030000u);
      }
    }
  }
  if (w.os2.oom || w.post.oom || w.bufs[B_HMTX].oom || w.bufs[B_LOCA].oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "building the tables");
    goto done;
  }

  // The tables, in the order they will be sorted into anyway.
#define ADD_BUF(tagv, buf) \
  do { tables[ntables].tag = (tagv); tables[ntables].data = (buf).data; \
       tables[ntables].length = (buf).length; ++ntables; } while (0)
#define ADD_SRC(tagv) \
  do { const uint8_t * d_; size_t l_; \
       if (source_table(face, (tagv), &d_, &l_)) { \
         tables[ntables].tag = (tagv); tables[ntables].data = d_; \
         tables[ntables].length = l_; ++ntables; } } while (0)
  ADD_BUF(T_HEAD, w.bufs[B_HEAD]);
  ADD_BUF(T_HHEA, w.bufs[B_HHEA]);
  ADD_BUF(T_MAXP, w.bufs[B_MAXP]);
  ADD_BUF(T_HMTX, w.bufs[B_HMTX]);
  ADD_BUF(T_CMAP, w.bufs[B_CMAP]);
  ADD_BUF(T_LOCA, w.bufs[B_LOCA]);
  ADD_BUF(T_GLYF, w.bufs[B_GLYF]);
  if (w.os2.length) {
    ADD_BUF(T_OS2, w.os2);
  }
  if (w.post.length) {
    ADD_BUF(T_POST, w.post);
  }
  ADD_SRC(T_NAME);
  ADD_SRC(T_GASP);
  if (!options->drop_hinting) {
    ADD_SRC(T_CVT);
    ADD_SRC(T_FPGM);
    ADD_SRC(T_PREP);
  }
  if (retain && !options->drop_layout) {
    ADD_SRC(T_GDEF);
    ADD_SRC(T_GSUB);
    ADD_SRC(T_GPOS);
    // Indexed by glyph id and valid wherever the ids have not moved: the legacy
    // kerning table, and the vertical metrics (which may run past the last kept
    // glyph, harmlessly).
    ADD_SRC(T_KERN);
    ADD_SRC(T_VHEA);
    ADD_SRC(T_VMTX);
    ADD_SRC(T_VORG);
  }
#undef ADD_BUF
#undef ADD_SRC
  result = gfnt_write_sfnt(0x00010000u, tables, ntables, limits, allocator,
      out_blob, error);

done:
  work_free(&w);
  return result;
}
