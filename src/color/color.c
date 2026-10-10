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
 * The `CPAL` and `COLR` (version 0) readers. See `color.h` in the public headers.
 *
 * Nothing is parsed ahead of a call: each reads the header, then the records it
 * was asked for, through the checked reader. Version 1 of `COLR` extends the
 * header and leaves the version 0 records where they were, so these readers
 * serve both.
 */

#include <ghoti.io/font/color.h>
#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"

#define GFNT_TAG_CPAL GFNT_TAG('C', 'P', 'A', 'L')
#define GFNT_TAG_COLR GFNT_TAG('C', 'O', 'L', 'R')

// CPAL version 0's header, up to the per-palette first-color indexes.
#define GFNT_CPAL_HEADER_0 12u
// A color record is blue, green, red, alpha.
#define GFNT_CPAL_RECORD 4u
// COLR version 0's header, and the sizes of its two record kinds.
#define GFNT_COLR_HEADER_0 14u
#define GFNT_COLR_BASE_RECORD 6u
#define GFNT_COLR_LAYER_RECORD 4u

typedef struct GFNT_CpalHeader {
  uint16_t version;
  uint16_t entries;
  uint16_t palettes;
  uint16_t records;
  uint32_t records_offset;
  uint32_t types_offset;
  uint32_t labels_offset;
  uint32_t entry_labels_offset;
} GFNT_CpalHeader;

bool gfnt_face_has_palettes(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_CPAL);
}

bool gfnt_face_has_color_layers(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_COLR);
}

static GFNT_Result gfnt_cpal_open(const GFNT_Face * face, GFNT_Reader * table,
    GFNT_CpalHeader * h, GFNT_Error * error) {
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_CPAL)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CPAL, 0,
        GFNT_GLYPH_NONE, "the face has no CPAL");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CPAL, table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table->error = error;
  memset(h, 0, sizeof *h);
  if (table->length < GFNT_CPAL_HEADER_0
      || gfnt_reader_u16_at(table, 0, &h->version) != GFNT_OK
      || gfnt_reader_u16_at(table, 2, &h->entries) != GFNT_OK
      || gfnt_reader_u16_at(table, 4, &h->palettes) != GFNT_OK
      || gfnt_reader_u16_at(table, 6, &h->records) != GFNT_OK
      || gfnt_reader_u32_at(table, 8, &h->records_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL, 0,
        GFNT_GLYPH_NONE, "a CPAL shorter than its own header");
  }
  if (h->version > 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CPAL, 0,
        GFNT_GLYPH_NONE, "a CPAL version above 1");
  }
  if (h->version == 1) {
    size_t at = GFNT_CPAL_HEADER_0 + 2u * h->palettes;

    if (gfnt_reader_u32_at(table, at, &h->types_offset) != GFNT_OK
        || gfnt_reader_u32_at(table, at + 4, &h->labels_offset) != GFNT_OK
        || gfnt_reader_u32_at(table, at + 8, &h->entry_labels_offset)
               != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL, at,
          GFNT_GLYPH_NONE, "a CPAL 1 with no room for its array offsets");
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_palette_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_CpalHeader h;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cpal_open(face, &table, &h, error);
  if (result == GFNT_OK) {
    *out_count = h.palettes;
  }
  return result;
}

GFNT_Result gfnt_face_palette_entry_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_CpalHeader h;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cpal_open(face, &table, &h, error);
  if (result == GFNT_OK) {
    *out_count = h.entries;
  }
  return result;
}

GFNT_Result gfnt_face_palette_color(const GFNT_Face * face, size_t palette,
    size_t entry, GFNT_Color * out_color, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_CpalHeader h;
  GFNT_Result result;
  uint16_t first = 0;
  size_t index, at;
  uint32_t bgra = 0;

  if (!out_color) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cpal_open(face, &table, &h, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (palette >= h.palettes || entry >= h.entries) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_reader_u16_at(&table, GFNT_CPAL_HEADER_0 + 2u * palette, &first)
      != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL,
        GFNT_CPAL_HEADER_0 + 2u * palette, GFNT_GLYPH_NONE,
        "a CPAL palette index past the table");
  }
  index = (size_t)first + entry;
  if (index >= h.records) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL, 8,
        GFNT_GLYPH_NONE, "a CPAL palette that runs past its color records");
  }
  at = (size_t)h.records_offset + index * GFNT_CPAL_RECORD;
  if (gfnt_reader_u32_at(&table, at, &bgra) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL, at,
        GFNT_GLYPH_NONE, "a CPAL color record past the table");
  }
  out_color->blue = (uint8_t)(bgra >> 24);
  out_color->green = (uint8_t)(bgra >> 16);
  out_color->red = (uint8_t)(bgra >> 8);
  out_color->alpha = (uint8_t)bgra;
  return GFNT_OK;
}

GFNT_Result gfnt_face_palette_info(const GFNT_Face * face, size_t palette,
    GFNT_PaletteInfo * out_info, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_CpalHeader h;
  GFNT_Result result;
  GFNT_PaletteInfo info = {0, GFNT_PALETTE_NO_LABEL};

  if (!out_info) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cpal_open(face, &table, &h, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (palette >= h.palettes) {
    return GFNT_ERR_INVALID;
  }
  if (h.version == 1) {
    if (h.types_offset
        && gfnt_reader_u32_at(&table, (size_t)h.types_offset + 4u * palette,
               &info.flags) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL,
          (size_t)h.types_offset + 4u * palette, GFNT_GLYPH_NONE,
          "a CPAL palette type past the table");
    }
    if (h.labels_offset
        && gfnt_reader_u16_at(&table, (size_t)h.labels_offset + 2u * palette,
               &info.label) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL,
          (size_t)h.labels_offset + 2u * palette, GFNT_GLYPH_NONE,
          "a CPAL palette label past the table");
    }
  }
  *out_info = info;
  return GFNT_OK;
}

GFNT_Result gfnt_face_palette_entry_label(const GFNT_Face * face, size_t entry,
    uint16_t * out_label, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_CpalHeader h;
  GFNT_Result result;
  uint16_t label = GFNT_PALETTE_NO_LABEL;

  if (!out_label) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_cpal_open(face, &table, &h, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (entry >= h.entries) {
    return GFNT_ERR_INVALID;
  }
  if (h.version == 1 && h.entry_labels_offset
      && gfnt_reader_u16_at(&table, (size_t)h.entry_labels_offset + 2u * entry,
             &label) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CPAL,
        (size_t)h.entry_labels_offset + 2u * entry, GFNT_GLYPH_NONE,
        "a CPAL entry label past the table");
  }
  *out_label = label;
  return GFNT_OK;
}

/* ------------------------------------------------------------------- COLR */

typedef struct GFNT_ColrHeader {
  uint16_t version;
  uint16_t base_count;
  uint32_t base_offset;
  uint32_t layer_offset;
  uint16_t layer_count;
} GFNT_ColrHeader;

static GFNT_Result gfnt_colr_open(const GFNT_Face * face, GFNT_Reader * table,
    GFNT_ColrHeader * h, GFNT_Error * error) {
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_COLR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_COLR, 0,
        GFNT_GLYPH_NONE, "the face has no COLR");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_COLR, table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table->error = error;
  if (table->length < GFNT_COLR_HEADER_0
      || gfnt_reader_u16_at(table, 0, &h->version) != GFNT_OK
      || gfnt_reader_u16_at(table, 2, &h->base_count) != GFNT_OK
      || gfnt_reader_u32_at(table, 4, &h->base_offset) != GFNT_OK
      || gfnt_reader_u32_at(table, 8, &h->layer_offset) != GFNT_OK
      || gfnt_reader_u16_at(table, 12, &h->layer_count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, 0,
        GFNT_GLYPH_NONE, "a COLR shorter than its own header");
  }
  if (h->version > 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_COLR, 0,
        GFNT_GLYPH_NONE, "a COLR version above 1");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_version(const GFNT_Face * face,
    uint16_t * out_version, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrHeader h;
  GFNT_Result result;

  if (!out_version) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open(face, &table, &h, error);
  if (result == GFNT_OK) {
    *out_version = h.version;
  }
  return result;
}

/** The base glyph record for @p glyph by binary search; false when there is none. */
static GFNT_Result gfnt_colr_base(const GFNT_Reader * table,
    const GFNT_ColrHeader * h, uint32_t glyph, bool * found, uint16_t * first,
    uint16_t * count, GFNT_Error * error) {
  size_t lo = 0, hi = h->base_count;

  *found = false;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t at = (size_t)h->base_offset + mid * GFNT_COLR_BASE_RECORD;
    uint16_t gid = 0;

    if (gfnt_reader_u16_at(table, at, &gid) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, at,
          GFNT_GLYPH_NONE, "a COLR base glyph record past the table");
    }
    if (glyph < gid) {
      hi = mid;
    }
    else if (glyph > gid) {
      lo = mid + 1;
    }
    else {
      if (gfnt_reader_u16_at(table, at + 2, first) != GFNT_OK
          || gfnt_reader_u16_at(table, at + 4, count) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, at,
            GFNT_GLYPH_NONE, "a COLR base glyph record past the table");
      }
      *found = true;
      return GFNT_OK;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_layer_count(const GFNT_Face * face, uint32_t glyph,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrHeader h;
  GFNT_Result result;
  bool found = false;
  uint16_t first = 0, count = 0;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open(face, &table, &h, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_colr_base(&table, &h, glyph, &found, &first, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_count = found ? count : 0;
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_layer(const GFNT_Face * face, uint32_t glyph,
    size_t index, GFNT_ColorLayer * out_layer, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrHeader h;
  GFNT_Result result;
  bool found = false;
  uint16_t first = 0, count = 0, gid = 0, palette = 0;
  size_t at;

  if (!out_layer) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open(face, &table, &h, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_colr_base(&table, &h, glyph, &found, &first, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!found || index >= count) {
    return GFNT_ERR_INVALID;
  }
  if ((size_t)first + index >= h.layer_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, 12,
        GFNT_GLYPH_NONE, "a COLR glyph whose layers run past the layer records");
  }
  at = (size_t)h.layer_offset + ((size_t)first + index) * GFNT_COLR_LAYER_RECORD;
  if (gfnt_reader_u16_at(&table, at, &gid) != GFNT_OK
      || gfnt_reader_u16_at(&table, at + 2, &palette) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, at,
        GFNT_GLYPH_NONE, "a COLR layer record past the table");
  }
  out_layer->glyph = gid;
  out_layer->palette_index = palette;
  return GFNT_OK;
}

/* -------------------------------------------------------- COLR version 1 */

#include "../core/fixed.h"
#include "../var/ivs.h"

// COLR version 1's header: the version 0 fields, then five offsets.
#define GFNT_COLR_HEADER_1 34u

typedef struct GFNT_ColrV1 {
  uint32_t base_list;
  uint32_t layer_list;
  uint32_t clip_list;
  uint32_t var_map;
  uint32_t var_store;
} GFNT_ColrV1;

static GFNT_Result gfnt_colr_open_v1(const GFNT_Face * face, GFNT_Reader * table,
    GFNT_ColrV1 * v1, GFNT_Error * error) {
  GFNT_ColrHeader h;
  GFNT_Result result = gfnt_colr_open(face, table, &h, error);

  memset(v1, 0, sizeof *v1);
  if (result != GFNT_OK || h.version == 0) {
    return result;
  }
  if (table->length < GFNT_COLR_HEADER_1
      || gfnt_reader_u32_at(table, 14, &v1->base_list) != GFNT_OK
      || gfnt_reader_u32_at(table, 18, &v1->layer_list) != GFNT_OK
      || gfnt_reader_u32_at(table, 22, &v1->clip_list) != GFNT_OK
      || gfnt_reader_u32_at(table, 26, &v1->var_map) != GFNT_OK
      || gfnt_reader_u32_at(table, 30, &v1->var_store) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, 14,
        GFNT_GLYPH_NONE, "a COLR 1 shorter than its header");
  }
  return GFNT_OK;
}

#define RD_FAIL(at, what) \
  gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, (at), GFNT_GLYPH_NONE, (what))

static GFNT_Result rd_u24(const GFNT_Reader * t, size_t at, uint32_t * out) {
  uint8_t a, b, c;

  if (gfnt_reader_u8_at(t, at, &a) != GFNT_OK
      || gfnt_reader_u8_at(t, at + 1, &b) != GFNT_OK
      || gfnt_reader_u8_at(t, at + 2, &c) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out = (uint32_t)a << 16 | (uint32_t)b << 8 | c;
  return GFNT_OK;
}

static GFNT_Result rd_s16(const GFNT_Reader * t, size_t at, int32_t * out) {
  int16_t v = 0;

  if (gfnt_reader_s16_at(t, at, &v) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out = v;
  return GFNT_OK;
}

static GFNT_Result rd_u16v(const GFNT_Reader * t, size_t at, int32_t * out) {
  uint16_t v = 0;

  if (gfnt_reader_u16_at(t, at, &v) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out = v;
  return GFNT_OK;
}

static GFNT_Result rd_s32(const GFNT_Reader * t, size_t at, int32_t * out) {
  uint32_t v = 0;

  if (gfnt_reader_u32_at(t, at, &v) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  *out = (int32_t)v;
  return GFNT_OK;
}

/** The delta of variation index @p index at the location, in the field's units. */
static GFNT_Result gfnt_colr_delta(const GFNT_Face * face, const GFNT_Reader * table,
    const GFNT_ColrV1 * v1, const GFNT_Variation * variation, uint32_t index,
    int32_t * out, GFNT_Error * error) {
  GFNT_Reader store, map;
  uint32_t outer, inner;
  int64_t delta = 0;
  GFNT_Result result;

  *out = 0;
  if (!variation || !variation->coords || variation->count == 0
      || index == 0xFFFFFFFFu || v1->var_store == 0) {
    return GFNT_OK;
  }
  if (v1->var_map) {
    result = gfnt_reader_sub(table, v1->var_map, GFNT_READER_REST, &map);
    if (result != GFNT_OK) {
      return RD_FAIL(v1->var_map, "a COLR variation index map past the table");
    }
    result = gfnt_ivs_map(&map, GFNT_TAG_COLR, index, &outer, &inner, error);
    if (result != GFNT_OK) {
      return result;
    }
    /* An entry of 0xFFFF:0xFFFF is the map's own "no variation", the mapped
     * counterpart of a VarIndexBase of 0xFFFFFFFF. Google's test-glyph font
     * has them for every field it leaves fixed; fontTools reads them as a delta
     * of zero. */
    if (outer == 0xFFFFu && inner == 0xFFFFu) {
      return GFNT_OK;
    }
  }
  else {
    outer = index >> 16;
    inner = index & 0xFFFFu;
  }
  result = gfnt_reader_sub(table, v1->var_store, GFNT_READER_REST, &store);
  if (result != GFNT_OK) {
    return RD_FAIL(v1->var_store, "a COLR item variation store past the table");
  }
  result = gfnt_ivs_delta(face, &store, GFNT_TAG_COLR, variation->coords,
      variation->count, outer, inner, &delta, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out = gfnt_saturate32(gfnt_round_shift_mode(gfnt_clamp64(delta),
      GFNT_GVAR_FRACTION_BITS, variation->delta_rounding));
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_base_paint(const GFNT_Face * face, uint32_t glyph,
    GFNT_PaintRef * out_ref, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result result;
  uint32_t count = 0;
  size_t lo = 0, hi;

  if (!out_ref) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open_v1(face, &table, &v1, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_ref = 0;
  if (!v1.base_list) {
    return GFNT_OK;
  }
  if (gfnt_reader_u32_at(&table, v1.base_list, &count) != GFNT_OK) {
    return RD_FAIL(v1.base_list, "a COLR base glyph list past the table");
  }
  hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t at = (size_t)v1.base_list + 4u + mid * 6u;
    uint16_t gid = 0;

    if (gfnt_reader_u16_at(&table, at, &gid) != GFNT_OK) {
      return RD_FAIL(at, "a COLR base glyph paint record past the table");
    }
    if (glyph < gid) {
      hi = mid;
    }
    else if (glyph > gid) {
      lo = mid + 1;
    }
    else {
      uint32_t offset = 0;

      if (gfnt_reader_u32_at(&table, at + 2, &offset) != GFNT_OK || offset == 0
          || (uint64_t)v1.base_list + offset >= table.length) {
        return RD_FAIL(at + 2, "a COLR base glyph paint offset outside the table");
      }
      *out_ref = v1.base_list + offset;
      return GFNT_OK;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_layer_list_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result result;
  uint32_t count = 0;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open_v1(face, &table, &v1, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (v1.layer_list
      && gfnt_reader_u32_at(&table, v1.layer_list, &count) != GFNT_OK) {
    return RD_FAIL(v1.layer_list, "a COLR layer list past the table");
  }
  *out_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_layer_paint(const GFNT_Face * face, size_t index,
    GFNT_PaintRef * out_ref, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result result;
  uint32_t count = 0, offset = 0;
  size_t at;

  if (!out_ref) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_colr_open_v1(face, &table, &v1, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!v1.layer_list
      || gfnt_reader_u32_at(&table, v1.layer_list, &count) != GFNT_OK) {
    return v1.layer_list ? RD_FAIL(v1.layer_list, "a COLR layer list past the table")
                         : GFNT_ERR_INVALID;
  }
  if (index >= count) {
    return GFNT_ERR_INVALID;
  }
  at = (size_t)v1.layer_list + 4u + 4u * index;
  if (gfnt_reader_u32_at(&table, at, &offset) != GFNT_OK || offset == 0
      || (uint64_t)v1.layer_list + offset >= table.length) {
    return RD_FAIL(at, "a COLR layer paint offset outside the table");
  }
  *out_ref = v1.layer_list + offset;
  return GFNT_OK;
}

/** A child offset (a 24-bit offset from the paint) as a ref; 0 is corrupt. */
static GFNT_Result child_at(const GFNT_Reader * t, GFNT_PaintRef ref, size_t at,
    GFNT_PaintRef * out, GFNT_Error * error) {
  uint32_t offset = 0;

  if (rd_u24(t, ref + at, &offset) != GFNT_OK) {
    return RD_FAIL(ref + at, "a COLR paint offset past the table");
  }
  if (offset == 0 || (uint64_t)ref + offset >= t->length) {
    return RD_FAIL(ref + at, "a COLR paint offset of 0 or outside the table");
  }
  *out = ref + offset;
  return GFNT_OK;
}

/** Field @p slot of a Var paint: the stored value plus its delta. */
typedef struct Fields {
  const GFNT_Face * face;
  const GFNT_Reader * table;
  const GFNT_ColrV1 * v1;
  const GFNT_Variation * variation;
  GFNT_ColorPaint * paint;
  GFNT_PaintRef ref;
  uint32_t var_base;
  bool variable;
  GFNT_Error * error;
} Fields;

/* Slot @p slot of a field group whose VarIndexBase is @p base. A base of
 * 0xFFFFFFFF means the whole group does not vary, so every slot is that same
 * "no variation" index; adding the slot to it would wrap to row 0 and apply
 * deltas the font never meant (Google's test-glyph font has such paints). */
#define GFNT_COLR_VAR_INDEX(base, slot) \
  ((base) == 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)((base) + (uint32_t)(slot)))

/** Read value slot @p slot of the paint from @p at, 16-bit (signed or not) and varied. */
static GFNT_Result field16(Fields * f, size_t slot, size_t at, bool is_signed) {
  int32_t v = 0, d = 0;
  GFNT_Result r = is_signed ? rd_s16(f->table, f->ref + at, &v)
                            : rd_u16v(f->table, f->ref + at, &v);

  if (r != GFNT_OK) {
    return gfnt_error_set(f->error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, f->ref + at,
        GFNT_GLYPH_NONE, "a COLR paint field past the table");
  }
  if (f->variable) {
    r = gfnt_colr_delta(f->face, f->table, f->v1, f->variation,
        GFNT_COLR_VAR_INDEX(f->var_base, slot), &d, f->error);
    if (r != GFNT_OK) {
      return r;
    }
  }
  f->paint->v[slot] = gfnt_saturate32((int64_t)v + d);
  return GFNT_OK;
}

#define F16(slot, at, sgn) \
  do { \
    r = field16(&f, (slot), (at), (sgn)); \
    if (r != GFNT_OK) { \
      return r; \
    } \
  } while (0)

GFNT_Result gfnt_face_color_paint(const GFNT_Face * face, GFNT_PaintRef ref,
    const GFNT_Variation * variation, GFNT_ColorPaint * out, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result r;
  GFNT_ColorPaint paint;
  Fields f;
  uint8_t format = 0;

  if (!out || ref == 0) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_colr_open_v1(face, &table, &v1, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (gfnt_reader_u8_at(&table, ref, &format) != GFNT_OK) {
    return RD_FAIL(ref, "a COLR paint reference past the table");
  }
  memset(&paint, 0, sizeof paint);
  paint.format = (GFNT_PaintFormat)format;
  f = (Fields) {face, &table, &v1, variation, &paint, ref, 0, false, error};
  // Every Var form is the odd number after its fixed one, except the first run.
  switch (format) {
    case GFNT_PAINT_COLR_LAYERS: {
      uint8_t n = 0;

      if (gfnt_reader_u8_at(&table, ref + 1, &n) != GFNT_OK
          || gfnt_reader_u32_at(&table, ref + 2, &paint.first_layer) != GFNT_OK) {
        return RD_FAIL(ref, "a PaintColrLayers past the table");
      }
      paint.layer_count = n;
      break;
    }
    case GFNT_PAINT_SOLID:
    case GFNT_PAINT_VAR_SOLID:
      paint.variable = format == GFNT_PAINT_VAR_SOLID;
      f.variable = paint.variable;
      if (paint.variable
          && gfnt_reader_u32_at(&table, ref + 5, &f.var_base) != GFNT_OK) {
        return RD_FAIL(ref + 5, "a PaintVarSolid past the table");
      }
      if (gfnt_reader_u16_at(&table, ref + 1, &paint.palette_index) != GFNT_OK) {
        return RD_FAIL(ref, "a PaintSolid past the table");
      }
      F16(0, 3, true);
      break;
    case GFNT_PAINT_LINEAR_GRADIENT:
    case GFNT_PAINT_VAR_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT:
    case GFNT_PAINT_VAR_RADIAL_GRADIENT: {
      bool radial = format >= GFNT_PAINT_RADIAL_GRADIENT;

      paint.variable = (format & 1) != 0;
      f.variable = paint.variable;
      r = child_at(&table, ref, 1, &paint.color_line, error);
      if (r != GFNT_OK) {
        return r;
      }
      if (paint.variable
          && gfnt_reader_u32_at(&table, ref + 16, &f.var_base) != GFNT_OK) {
        return RD_FAIL(ref + 16, "a variable gradient past the table");
      }
      // Linear: x0 y0 x1 y1 x2 y2 (all FWORD). Radial: x0 y0 r0 x1 y1 r1, the radii unsigned.
      F16(0, 4, true);
      F16(1, 6, true);
      F16(2, 8, !radial);
      F16(3, 10, true);
      F16(4, 12, true);
      F16(5, 14, !radial);
      break;
    }
    case GFNT_PAINT_SWEEP_GRADIENT:
    case GFNT_PAINT_VAR_SWEEP_GRADIENT:
      paint.variable = format == GFNT_PAINT_VAR_SWEEP_GRADIENT;
      f.variable = paint.variable;
      r = child_at(&table, ref, 1, &paint.color_line, error);
      if (r != GFNT_OK) {
        return r;
      }
      if (paint.variable
          && gfnt_reader_u32_at(&table, ref + 12, &f.var_base) != GFNT_OK) {
        return RD_FAIL(ref + 12, "a PaintVarSweepGradient past the table");
      }
      F16(0, 4, true);
      F16(1, 6, true);
      F16(2, 8, true);
      F16(3, 10, true);
      break;
    case GFNT_PAINT_GLYPH:
      r = child_at(&table, ref, 1, &paint.child, error);
      if (r != GFNT_OK) {
        return r;
      }
      {
        uint16_t g = 0;

        if (gfnt_reader_u16_at(&table, ref + 4, &g) != GFNT_OK) {
          return RD_FAIL(ref + 4, "a PaintGlyph past the table");
        }
        paint.glyph = g;
      }
      break;
    case GFNT_PAINT_COLR_GLYPH: {
      uint16_t g = 0;

      if (gfnt_reader_u16_at(&table, ref + 1, &g) != GFNT_OK) {
        return RD_FAIL(ref + 1, "a PaintColrGlyph past the table");
      }
      paint.glyph = g;
      break;
    }
    case GFNT_PAINT_TRANSFORM:
    case GFNT_PAINT_VAR_TRANSFORM: {
      uint32_t at = 0;

      paint.variable = format == GFNT_PAINT_VAR_TRANSFORM;
      f.variable = paint.variable;
      r = child_at(&table, ref, 1, &paint.child, error);
      if (r != GFNT_OK) {
        return r;
      }
      if (rd_u24(&table, ref + 4, &at) != GFNT_OK || at == 0) {
        return RD_FAIL(ref + 4, "a PaintTransform with no matrix");
      }
      at += ref;
      if (paint.variable
          && gfnt_reader_u32_at(&table, at + 24, &f.var_base) != GFNT_OK) {
        return RD_FAIL(at + 24, "a variable matrix past the table");
      }
      for (size_t k = 0; k < 6; k++) {
        int32_t v = 0, d = 0;

        if (rd_s32(&table, at + 4 * k, &v) != GFNT_OK) {
          return RD_FAIL(at + 4 * k, "a matrix past the table");
        }
        if (paint.variable) {
          r = gfnt_colr_delta(face, &table, &v1, variation, GFNT_COLR_VAR_INDEX(f.var_base, k),
              &d, error);
          if (r != GFNT_OK) {
            return r;
          }
        }
        paint.v[k] = gfnt_saturate32((int64_t)v + d);
      }
      break;
    }
    case GFNT_PAINT_TRANSLATE: case GFNT_PAINT_VAR_TRANSLATE:
    case GFNT_PAINT_SCALE: case GFNT_PAINT_VAR_SCALE:
    case GFNT_PAINT_SCALE_AROUND_CENTER: case GFNT_PAINT_VAR_SCALE_AROUND_CENTER:
    case GFNT_PAINT_SCALE_UNIFORM: case GFNT_PAINT_VAR_SCALE_UNIFORM:
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_VAR_SCALE_UNIFORM_AROUND_CENTER:
    case GFNT_PAINT_ROTATE: case GFNT_PAINT_VAR_ROTATE:
    case GFNT_PAINT_ROTATE_AROUND_CENTER: case GFNT_PAINT_VAR_ROTATE_AROUND_CENTER:
    case GFNT_PAINT_SKEW: case GFNT_PAINT_VAR_SKEW:
    case GFNT_PAINT_SKEW_AROUND_CENTER: case GFNT_PAINT_VAR_SKEW_AROUND_CENTER: {
      // How many 16-bit values follow the child offset, by the fixed form.
      static const uint8_t values[] = {
        /* 14 translate */ 2, /* 16 scale */ 2, /* 18 */ 4, /* 20 */ 1, /* 22 */ 3,
        /* 24 rotate */ 1, /* 26 */ 3, /* 28 skew */ 2, /* 30 */ 4};
      size_t n = values[(format - GFNT_PAINT_TRANSLATE) / 2];

      paint.variable = (format & 1) != 0;
      f.variable = paint.variable;
      r = child_at(&table, ref, 1, &paint.child, error);
      if (r != GFNT_OK) {
        return r;
      }
      if (paint.variable
          && gfnt_reader_u32_at(&table, ref + 4 + 2 * n, &f.var_base) != GFNT_OK) {
        return RD_FAIL(ref + 4 + 2 * n, "a variable transform past the table");
      }
      for (size_t k = 0; k < n; k++) {
        F16(k, 4 + 2 * k, true);
      }
      break;
    }
    case GFNT_PAINT_COMPOSITE: {
      uint8_t mode = 0;

      r = child_at(&table, ref, 1, &paint.child, error);
      if (r != GFNT_OK) {
        return r;
      }
      if (gfnt_reader_u8_at(&table, ref + 4, &mode) != GFNT_OK) {
        return RD_FAIL(ref + 4, "a PaintComposite past the table");
      }
      paint.composite_mode = mode;
      r = child_at(&table, ref, 5, &paint.backdrop, error);
      if (r != GFNT_OK) {
        return r;
      }
      break;
    }
    default:
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_COLR, ref,
          GFNT_GLYPH_NONE, "a COLR paint format this library does not know");
  }
  *out = paint;
  return GFNT_OK;
}

/* A color line's header is an extend byte and a stop count; its stops follow. */
#define GFNT_COLR_LINE_HEADER 3u

GFNT_Result gfnt_face_color_line(const GFNT_Face * face,
    const GFNT_ColorPaint * paint, GFNT_ColorLine * out, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result r;
  uint8_t extend = 0;
  uint16_t count = 0;

  if (!paint || !out || paint->color_line == 0) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_colr_open_v1(face, &table, &v1, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (gfnt_reader_u8_at(&table, paint->color_line, &extend) != GFNT_OK
      || gfnt_reader_u16_at(&table, paint->color_line + 1, &count) != GFNT_OK) {
    return RD_FAIL(paint->color_line, "a COLR color line past the table");
  }
  out->extend = (GFNT_ColorExtend)extend;
  out->stop_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_stop(const GFNT_Face * face,
    const GFNT_ColorPaint * paint, size_t index, const GFNT_Variation * variation,
    GFNT_ColorStop * out, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result r;
  GFNT_ColorLine line;
  size_t size, at;
  int32_t offset = 0, alpha = 0, d0 = 0, d1 = 0;
  uint16_t palette = 0;
  uint32_t base = 0;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_face_color_line(face, paint, &line, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (index >= line.stop_count) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_colr_open_v1(face, &table, &v1, error);
  if (r != GFNT_OK) {
    return r;
  }
  size = paint->variable ? 10u : 6u;
  at = (size_t)paint->color_line + GFNT_COLR_LINE_HEADER + index * size;
  if (rd_s16(&table, at, &offset) != GFNT_OK
      || gfnt_reader_u16_at(&table, at + 2, &palette) != GFNT_OK
      || rd_s16(&table, at + 4, &alpha) != GFNT_OK) {
    return RD_FAIL(at, "a COLR color stop past the table");
  }
  if (paint->variable) {
    if (gfnt_reader_u32_at(&table, at + 6, &base) != GFNT_OK) {
      return RD_FAIL(at + 6, "a COLR variable color stop past the table");
    }
    r = gfnt_colr_delta(face, &table, &v1, variation, base, &d0, error);
    if (r == GFNT_OK) {
      r = gfnt_colr_delta(face, &table, &v1, variation, GFNT_COLR_VAR_INDEX(base, 1), &d1, error);
    }
    if (r != GFNT_OK) {
      return r;
    }
  }
  out->offset = gfnt_saturate32((int64_t)offset + d0);
  out->palette_index = palette;
  out->alpha = gfnt_saturate32((int64_t)alpha + d1);
  return GFNT_OK;
}

GFNT_Result gfnt_face_color_clip(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, GFNT_ColorBox * out, bool * found,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_ColrV1 v1;
  GFNT_Result r;
  uint32_t count = 0;
  size_t lo = 0, hi;

  if (!out || !found) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_colr_open_v1(face, &table, &v1, error);
  if (r != GFNT_OK) {
    return r;
  }
  *found = false;
  if (!v1.clip_list) {
    return GFNT_OK;
  }
  if (gfnt_reader_u32_at(&table, v1.clip_list + 1, &count) != GFNT_OK) {
    return RD_FAIL(v1.clip_list, "a COLR clip list past the table");
  }
  hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t at = (size_t)v1.clip_list + 5u + mid * 7u;
    uint16_t first = 0, last = 0;
    uint32_t box = 0;

    if (gfnt_reader_u16_at(&table, at, &first) != GFNT_OK
        || gfnt_reader_u16_at(&table, at + 2, &last) != GFNT_OK
        || rd_u24(&table, at + 4, &box) != GFNT_OK) {
      return RD_FAIL(at, "a COLR clip record past the table");
    }
    if (glyph < first) {
      hi = mid;
    }
    else if (glyph > last) {
      lo = mid + 1;
    }
    else {
      uint8_t format = 0;
      size_t b = (size_t)v1.clip_list + box;
      int32_t v[4] = {0, 0, 0, 0};
      uint32_t base = 0;

      if (gfnt_reader_u8_at(&table, b, &format) != GFNT_OK || format < 1 || format > 2) {
        return RD_FAIL(b, "a COLR clip box of an unknown format");
      }
      for (size_t k = 0; k < 4; k++) {
        if (rd_s16(&table, b + 1 + 2 * k, &v[k]) != GFNT_OK) {
          return RD_FAIL(b, "a COLR clip box past the table");
        }
      }
      if (format == 2) {
        if (gfnt_reader_u32_at(&table, b + 9, &base) != GFNT_OK) {
          return RD_FAIL(b + 9, "a COLR variable clip box past the table");
        }
        for (size_t k = 0; k < 4; k++) {
          int32_t d = 0;

          r = gfnt_colr_delta(face, &table, &v1, variation, GFNT_COLR_VAR_INDEX(base, k), &d,
              error);
          if (r != GFNT_OK) {
            return r;
          }
          v[k] = gfnt_saturate32((int64_t)v[k] + d);
        }
      }
      out->x_min = v[0];
      out->y_min = v[1];
      out->x_max = v[2];
      out->y_max = v[3];
      *found = true;
      return GFNT_OK;
    }
  }
  return GFNT_OK;
}

/* ------------------------------------------------------------------- walk */

typedef struct Walk {
  const GFNT_Face * face;
  const GFNT_Variation * variation;
  const GFNT_PaintVisitor * visitor;
  void * user;
  size_t nodes;
  size_t max_depth;
  GFNT_Error * error;
} Walk;

static GFNT_Result walk_node(Walk * w, GFNT_PaintRef ref, size_t depth);

static GFNT_Result walk_child(Walk * w, GFNT_PaintRef ref, size_t depth) {
  return ref ? walk_node(w, ref, depth) : GFNT_OK;
}

static GFNT_Result walk_node(Walk * w, GFNT_PaintRef ref, size_t depth) {
  GFNT_ColorPaint paint;
  GFNT_Result r;

  if (depth >= w->max_depth) {
    return gfnt_error_set(w->error, GFNT_ERR_LIMIT, GFNT_TAG_COLR, ref,
        GFNT_GLYPH_NONE, "a COLR paint graph nested deeper than the limit");
  }
  if (++w->nodes > GFNT_PAINT_WALK_NODES) {
    return gfnt_error_set(w->error, GFNT_ERR_LIMIT, GFNT_TAG_COLR, ref,
        GFNT_GLYPH_NONE, "a COLR paint graph with more nodes than the limit");
  }
  r = gfnt_face_color_paint(w->face, ref, w->variation, &paint, w->error);
  if (r != GFNT_OK) {
    return r;
  }
  if (!w->visitor->enter(w->user, &paint, depth)) {
    return GFNT_ERR_INVALID;
  }
  if (paint.format == GFNT_PAINT_COLR_LAYERS) {
    for (uint32_t k = 0; k < paint.layer_count && r == GFNT_OK; k++) {
      GFNT_PaintRef layer = 0;

      r = gfnt_face_color_layer_paint(w->face, (size_t)paint.first_layer + k,
          &layer, w->error);
      if (r == GFNT_ERR_INVALID) {
        r = gfnt_error_set(w->error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, ref,
            GFNT_GLYPH_NONE, "a PaintColrLayers past the end of the layer list");
      }
      if (r == GFNT_OK) {
        r = walk_node(w, layer, depth + 1);
      }
    }
  }
  else if (paint.format == GFNT_PAINT_COLR_GLYPH) {
    GFNT_PaintRef root = 0;

    r = gfnt_face_color_base_paint(w->face, paint.glyph, &root, w->error);
    if (r == GFNT_OK && root == 0) {
      r = gfnt_error_set(w->error, GFNT_ERR_CORRUPT, GFNT_TAG_COLR, ref,
          paint.glyph, "a PaintColrGlyph naming a glyph with no paint");
    }
    if (r == GFNT_OK) {
      r = walk_node(w, root, depth + 1);
    }
  }
  else {
    r = walk_child(w, paint.child, depth + 1);
    if (r == GFNT_OK) {
      r = walk_child(w, paint.backdrop, depth + 1);
    }
  }
  if (r == GFNT_OK && w->visitor->leave) {
    w->visitor->leave(w->user, &paint, depth);
  }
  return r;
}

GFNT_Result gfnt_face_color_paint_walk(const GFNT_Face * face, uint32_t glyph,
    const GFNT_Variation * variation, const GFNT_PaintVisitor * visitor,
    void * user, GFNT_Error * error) {
  GFNT_PaintRef root = 0;
  GFNT_Limits limits;
  Walk w;
  GFNT_Result r;

  if (!visitor || !visitor->enter) {
    return GFNT_ERR_INVALID;
  }
  r = gfnt_face_color_base_paint(face, glyph, &root, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (root == 0) {
    return GFNT_ERR_INVALID;
  }
  gfnt_limits_default(&limits);
  w = (Walk) {face, variation, visitor, user, 0, limits.max_paint_depth, error};
  return walk_node(&w, root, 0);
}
