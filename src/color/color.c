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
