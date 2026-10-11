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
 * Bitmap strikes under a new glyph numbering: `EBLC`/`EBDT`, `CBLC`/`CBDT` and `sbix`.
 *
 * **`EBLC`/`EBDT` and `CBLC`/`CBDT` are one layout and one function.** For each strike
 * the kept glyphs' bytes are copied out of the data table as the font stores them and
 * indexed again: one index subtable (format 1, 32-bit offsets) for each run of
 * consecutive new glyph ids that share an image format. Two image formats do not
 * survive a copy, because they have no metrics of their own and the index that stated
 * them is gone: format 5 becomes 7 and format 19 becomes 18, each gaining the big
 * metrics the old index gave every glyph (the vertical half of those is not kept, as
 * the reader keeps only the horizontal). a table with no kept glyph in any strike is not written. A strike with no kept glyph stays,
 * empty: readers choose a strike by its size alone, and removing one would change which
 * strike answers for the glyphs that remain. A composite (formats 8 and 9) is
 * refused: its components are glyph ids of the same strike, which would have to be
 * kept and renumbered, and nothing in the population has one.
 *
 * **`sbix`** keeps each strike's size and resolution and the kept glyphs' records.
 * A record that is a `dupe` keeps pointing at its target when that is kept too, and
 * otherwise is replaced by a copy of the picture it reached, since a target that is
 * not there cannot be pointed at. Every other record, whatever its type, is copied
 * as it is: nothing about a `mask` or a `pdf ` has to be understood to carry it.
 *
 * `EBSC` is carried as it is by the caller: its records name strikes by position, and
 * every strike stays where it was.
 */

#include <string.h>
#include "../bitmap/eblc.h"
#include "../bitmap/sbix.h"
#include "../sfnt/sfnt.h"
#include "write.h"

#define NO_GLYPH 0xFFFFFFFFu

static void be16(uint8_t * p, uint32_t v) { gfnt_write_put16(p, v); }
static void be32(uint8_t * p, uint32_t v) { gfnt_write_put32(p, v); }

void gfnt_subset_strikes_init(GFNT_SubsetStrikes * s, const GFNT_Allocator * allocator) {
  gfnt_wbuf_init(&s->eblc, allocator);
  gfnt_wbuf_init(&s->ebdt, allocator);
  gfnt_wbuf_init(&s->cblc, allocator);
  gfnt_wbuf_init(&s->cbdt, allocator);
  gfnt_wbuf_init(&s->sbix, allocator);
}

void gfnt_subset_strikes_free(GFNT_SubsetStrikes * s) {
  gfnt_wbuf_free(&s->eblc);
  gfnt_wbuf_free(&s->ebdt);
  gfnt_wbuf_free(&s->cblc);
  gfnt_wbuf_free(&s->cbdt);
  gfnt_wbuf_free(&s->sbix);
}

/** One strike on its way out: its index region, and the glyph range it ended up with. */
typedef struct OutStrike {
  GFNT_WBuf region;     /* indexSubTableArray then the subtables */
  GFNT_WBuf array;      /* the array alone, built beside the bodies and joined after */
  uint32_t subtables;
  uint32_t first, last; /* in new glyph ids */
  bool any;
} OutStrike;

/** One run of consecutive new glyph ids with one image format, being written. */
typedef struct Run {
  uint32_t first, last, format;
  size_t data_at;       /* where in the data table this run's glyphs start */
  GFNT_WBuf offsets;    /* u32 offsets from data_at, one per glyph and a sentinel */
  bool open;
} Run;

/** Close a run into the strike's array and region. */
static void close_run(OutStrike * out, Run * run) {
  size_t body_at;
  size_t count;

  if (!run->open) {
    return;
  }
  count = run->last - run->first + 1;
  if (run->offsets.oom) {
    out->region.oom = true;   // a failed append drops the bytes, so the index is short
  }
  // The array entry: the glyph range and where the subtable is from the array's start,
  // which is not known until the array is complete; it is patched when they are joined.
  gfnt_wbuf_u16(&out->array, run->first);
  gfnt_wbuf_u16(&out->array, run->last);
  body_at = out->region.length;
  gfnt_wbuf_u32(&out->array, (uint32_t)body_at);
  // The subtable: indexFormat 1, imageFormat, the data offset, then the offsets.
  gfnt_wbuf_u16(&out->region, 1);
  gfnt_wbuf_u16(&out->region, run->format);
  gfnt_wbuf_u32(&out->region, (uint32_t)run->data_at);
  gfnt_wbuf_bytes(&out->region, run->offsets.data, run->offsets.length);
  (void)count;
  out->subtables++;
  gfnt_wbuf_free(&run->offsets);
  run->open = false;
}

static GFNT_Result carry_bitmaps(const GFNT_Face * face, GFNT_Tag index_tag,
    GFNT_Tag data_tag, const uint32_t * old_of_new, size_t count, size_t new_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_WBuf * out_index,
    GFNT_WBuf * out_data, GFNT_Error * error) {
  const GFNT_Eblc * list = NULL;
  GFNT_Reader index_reader;
  GFNT_Reader data_reader;
  OutStrike * strikes = NULL;
  size_t kept = 0;
  size_t any_strike = 0;
  GFNT_Result result;
  uint32_t data_version = 0;

  result = index_tag == GFNT_TAG_CBLC ? gfnt_face_cblc(face, &list, error)
                                      : gfnt_face_eblc(face, &list, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, index_tag, &index_reader, error);
  if (result == GFNT_OK) {
    result = gfnt_face_table_reader(face, data_tag, &data_reader, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_reader_u32_at(&data_reader, 0, &data_version) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, data_tag, 0, GFNT_GLYPH_NONE,
        "a bitmap data table shorter than its own header");
  }
  if (list->strike_count == 0) {
    return GFNT_OK;
  }
  strikes = allocator->calloc_fn(allocator->ctx, list->strike_count, sizeof *strikes);
  if (!strikes) {
    return gfnt_error_set(error, GFNT_ERR_OOM, index_tag, 0, GFNT_GLYPH_NONE,
        "allocating the strikes");
  }
  for (size_t s = 0; s < list->strike_count; ++s) {
    gfnt_wbuf_init(&strikes[s].region, allocator);
    gfnt_wbuf_init(&strikes[s].array, allocator);
  }
  gfnt_wbuf_u32(out_data, data_version);

  for (size_t s = 0; s < list->strike_count && result == GFNT_OK; ++s) {
    const GFNT_EblcStrike * strike = &list->strikes[s];
    GFNT_EblcSubtable * subtables = NULL;
    size_t subtable_count = 0;
    Run run;

    memset(&run, 0, sizeof run);
    result = gfnt_eblc_strike_index(face, strike, &subtables, &subtable_count, error);
    for (size_t n = 0; n < new_count && result == GFNT_OK; ++n) {
      uint32_t old = old_of_new[n];
      const GFNT_EblcSubtable * sub;
      size_t offset = 0;
      size_t length = 0;
      bool found = false;
      uint32_t format;
      GFNT_Reader at;
      const uint8_t * bytes = NULL;
      size_t prefix = 0;

      if (old == NO_GLYPH || old >= count || old > UINT16_MAX
          || old < strike->start_glyph || old > strike->end_glyph) {
        continue;
      }
      sub = gfnt_eblc_subtable_for(subtables, subtable_count, old);
      if (!sub) {
        continue;
      }
      result = gfnt_eblc_locate_glyph(face, sub, old, &offset, &length, &found, error);
      if (result != GFNT_OK) {
        break;
      }
      if (!found) {
        continue;
      }
      format = sub->image_format;
      if (format == 8 || format == 9) {
        result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, data_tag, offset, old,
            "a composite bitmap glyph (image formats 8 and 9) cannot be subset: its "
            "components are glyphs of the strike that would have to be renumbered");
        break;
      }
      at = data_reader;
      if (gfnt_reader_seek(&at, offset) != GFNT_OK
          || gfnt_read_bytes(&at, length, &bytes) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, data_tag, offset, old,
            "a bitmap glyph that runs past its data table");
        break;
      }
      if (format == 5 || format == 19) {
        format = format == 5 ? 7 : 18;
        prefix = 8;
      }
      if (n > UINT16_MAX) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, index_tag, 0, old,
            "a glyph id above 65535 cannot be indexed by a bitmap strike");
        break;
      }
      // A new run when the ids are not consecutive or the image format changes.
      if (run.open && (run.last + 1 != n || run.format != format)) {
        gfnt_wbuf_u32(&run.offsets, (uint32_t)(out_data->length - run.data_at));
        close_run(&strikes[s], &run);
      }
      if (!run.open) {
        gfnt_wbuf_init(&run.offsets, allocator);
        run.open = true;
        run.first = (uint32_t)n;
        run.format = format;
        run.data_at = out_data->length;
      }
      run.last = (uint32_t)n;
      gfnt_wbuf_u32(&run.offsets, (uint32_t)(out_data->length - run.data_at));
      if (prefix) {
        // BigGlyphMetrics from the index's horizontal five, the vertical three zero.
        gfnt_wbuf_u8(out_data, sub->metrics.height);
        gfnt_wbuf_u8(out_data, sub->metrics.width);
        gfnt_wbuf_u8(out_data, (uint8_t)sub->metrics.bearing_x);
        gfnt_wbuf_u8(out_data, (uint8_t)sub->metrics.bearing_y);
        gfnt_wbuf_u8(out_data, sub->metrics.advance);
        gfnt_wbuf_u8(out_data, 0);
        gfnt_wbuf_u8(out_data, 0);
        gfnt_wbuf_u8(out_data, 0);
      }
      gfnt_wbuf_bytes(out_data, bytes, length);
      if (strikes[s].any) {
        if (n > strikes[s].last) {
          strikes[s].last = (uint32_t)n;
        }
      }
      else {
        strikes[s].any = true;
        strikes[s].first = strikes[s].last = (uint32_t)n;
      }
      if (out_data->oom || out_data->length > limits->max_blob_bytes) {
        result = gfnt_error_set(error, out_data->oom ? GFNT_ERR_OOM : GFNT_ERR_LIMIT,
            data_tag, 0, old, "building the bitmap data");
      }
    }
    if (run.open && result == GFNT_OK) {
      gfnt_wbuf_u32(&run.offsets, (uint32_t)(out_data->length - run.data_at));
      close_run(&strikes[s], &run);
    }
    if (run.open) {
      gfnt_wbuf_free(&run.offsets);
    }
    // The index was allocated by the face, not by the caller's allocator.
    face->allocator->free_fn(face->allocator->ctx, subtables);
    if (result == GFNT_OK && strikes[s].any) {
      ++any_strike;
    }
  }
  // Every strike stays when any has a glyph, an empty one with no index subtable: a
  // reader picks its strike by size alone, so dropping the larger of two strikes that
  // carry no kept glyph would hand every glyph to the smaller.
  kept = any_strike ? list->strike_count : 0;
  for (size_t s = 0; s < list->strike_count; ++s) {
    if (strikes[s].region.oom || strikes[s].array.oom) {
      out_data->oom = true;
    }
  }
  if (result == GFNT_OK && out_data->oom) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, index_tag, 0, GFNT_GLYPH_NONE,
        "building the bitmap data");
  }

  if (result == GFNT_OK && kept > 0) {
    // Assemble: header, one 48-byte record per kept strike, then every strike's index.
    uint32_t version = 0;
    size_t region_at = 8 + 48 * kept;
    size_t placed = 0;

    (void)gfnt_reader_u32_at(&index_reader, 0, &version);
    gfnt_wbuf_u32(out_index, version);
    gfnt_wbuf_u32(out_index, (uint32_t)kept);
    for (size_t s = 0; s < list->strike_count; ++s) {
      uint8_t record[48];
      GFNT_Reader at = index_reader;
      const uint8_t * source = NULL;
      size_t array_bytes;

      if (gfnt_reader_seek(&at, 8 + 48 * s) != GFNT_OK
          || gfnt_read_bytes(&at, 48, &source) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, index_tag, 8 + 48 * s,
            GFNT_GLYPH_NONE, "a bitmapSizeTable that runs past its table");
        break;
      }
      memcpy(record, source, 48);
      array_bytes = (size_t)strikes[s].subtables * 8u;
      be32(record, strikes[s].subtables ? (uint32_t)(region_at + placed) : 8u);
      be32(record + 4, (uint32_t)(array_bytes + strikes[s].region.length));
      be32(record + 8, strikes[s].subtables);
      be16(record + 40, strikes[s].any ? strikes[s].first : 0);
      be16(record + 42, strikes[s].any ? strikes[s].last : 0);
      gfnt_wbuf_bytes(out_index, record, 48);
      placed += array_bytes + strikes[s].region.length;
    }
    for (size_t s = 0; s < list->strike_count && result == GFNT_OK; ++s) {
      // The array entries' offsets were from the start of the bodies; the bodies now
      // follow the array.
      for (uint32_t k = 0; k < strikes[s].subtables; ++k) {
        uint8_t * entry = strikes[s].array.data + 8u * k + 4u;
        uint32_t body = ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16)
            | ((uint32_t)entry[2] << 8) | entry[3];

        be32(entry, body + strikes[s].subtables * 8u);
      }
      gfnt_wbuf_bytes(out_index, strikes[s].array.data, strikes[s].array.length);
      gfnt_wbuf_bytes(out_index, strikes[s].region.data, strikes[s].region.length);
    }
    if (result == GFNT_OK && (out_index->oom || out_data->oom)) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, index_tag, 0, GFNT_GLYPH_NONE,
          "building the bitmap index");
    }
  }
  if (kept == 0) {
    // Nothing to carry: neither table is written.
    out_index->length = 0;
    out_data->length = 0;
  }
  for (size_t s = 0; s < list->strike_count; ++s) {
    gfnt_wbuf_free(&strikes[s].region);
    gfnt_wbuf_free(&strikes[s].array);
  }
  allocator->free_fn(allocator->ctx, strikes);
  return result;
}

/** The record of @p glyph in a strike starting at @p start, or its absence. */
static bool sbix_record(const GFNT_Reader * table, size_t start, uint32_t glyph,
    const uint8_t ** bytes, size_t * length) {
  uint32_t begin = 0;
  uint32_t end = 0;
  GFNT_Reader at = *table;

  if (gfnt_reader_u32_at(table, start + 4u + 4u * (size_t)glyph, &begin) != GFNT_OK
      || gfnt_reader_u32_at(table, start + 8u + 4u * (size_t)glyph, &end) != GFNT_OK
      || end <= begin || end - begin < 8u) {
    return false;
  }
  if (gfnt_reader_seek(&at, start + begin) != GFNT_OK
      || gfnt_read_bytes(&at, end - begin, bytes) != GFNT_OK) {
    return false;
  }
  *length = end - begin;
  return true;
}

static GFNT_Result carry_sbix(const GFNT_Face * face, const uint32_t * new_of_old,
    const uint32_t * old_of_new, size_t count, size_t new_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_WBuf * out,
    GFNT_Error * error) {
  GFNT_Reader table;
  size_t strikes = 0;
  size_t kept = 0;
  size_t strikes_out = 0;
  GFNT_WBuf body;
  uint32_t flags_word = 0;
  uint32_t * starts = NULL;
  GFNT_Result result = gfnt_sbix_strike_count(face, &strikes, error);

  if (result != GFNT_OK || strikes == 0) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_SBIX, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  (void)gfnt_reader_u32_at(&table, 0, &flags_word);   /* version and flags together */
  gfnt_wbuf_init(&body, allocator);
  starts = allocator->calloc_fn(allocator->ctx, strikes, sizeof *starts);
  if (!starts) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_SBIX, 0, GFNT_GLYPH_NONE,
        "allocating the strike offsets");
  }
  for (size_t s = 0; s < strikes && result == GFNT_OK; ++s) {
    uint32_t start = 0;
    uint16_t ppem = 0;
    uint16_t ppi = 0;
    size_t strike_at = body.length;
    GFNT_WBuf records;
    uint32_t offset;
    bool any = false;
    bool was_any;

    if (gfnt_reader_u32_at(&table, 8 + 4 * s, &start) != GFNT_OK
        || gfnt_reader_u16_at(&table, start, &ppem) != GFNT_OK
        || gfnt_reader_u16_at(&table, (size_t)start + 2u, &ppi) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, 8 + 4 * s,
          GFNT_GLYPH_NONE, "an sbix strike that starts past its table");
      break;
    }
    gfnt_wbuf_init(&records, allocator);
    offset = (uint32_t)(4 + 4 * (new_count + 1));
    gfnt_wbuf_u16(&body, ppem);
    gfnt_wbuf_u16(&body, ppi);
    for (size_t n = 0; n < new_count; ++n) {
      uint32_t old = old_of_new[n];
      const uint8_t * rec = NULL;
      size_t len = 0;

      gfnt_wbuf_u32(&body, offset);
      if (old == NO_GLYPH || old >= count
          || !sbix_record(&table, start, old, &rec, &len)) {
        continue;
      }
      was_any = any;
      any = true;
      if (len >= 10 && memcmp(rec + 4, "dupe", 4) == 0) {
        uint32_t target = ((uint32_t)rec[8] << 8) | rec[9];

        if (target < count && new_of_old[target] != NO_GLYPH) {
          // The target is kept: still a dupe of it, in the new numbering.
          gfnt_wbuf_bytes(&records, rec, 8);
          gfnt_wbuf_u16(&records, new_of_old[target]);
          offset += 10;
          continue;
        }
        // The target is gone: carry the picture the chain reaches, as a record of its
        // own. A chain that never reaches one leaves the glyph without a picture.
        {
          GFNT_GlyphImage image;
          GFNT_Error ignored;

          if (gfnt_sbix_find(face, s, old, &image, &ignored) == GFNT_OK) {
            const char * type = image.format == GFNT_IMAGE_PNG ? "png "
                : image.format == GFNT_IMAGE_JPEG ? "jpg " : "tiff";

            gfnt_wbuf_u16(&records, (uint32_t)(uint16_t)image.bearing_x);
            gfnt_wbuf_u16(&records, (uint32_t)(uint16_t)image.origin_y);
            gfnt_wbuf_bytes(&records, (const uint8_t *)type, 4);
            gfnt_wbuf_bytes(&records, image.data, image.length);
            offset += (uint32_t)(8 + image.length);
          }
          else {
            any = was_any;
          }
        }
        continue;
      }
      gfnt_wbuf_bytes(&records, rec, len);
      offset += (uint32_t)len;
    }
    gfnt_wbuf_u32(&body, offset);
    if (records.oom) {
      body.oom = true;
    }
    // The offsets were written before their records; the records follow them.
    gfnt_wbuf_bytes(&body, records.data, records.length);
    gfnt_wbuf_free(&records);
    // An empty strike stays (every offset equal): strikes are chosen by size alone.
    starts[strikes_out++] = (uint32_t)strike_at;
    if (any) {
      ++kept;
    }
    if (body.oom || body.length > limits->max_blob_bytes) {
      result = gfnt_error_set(error, body.oom ? GFNT_ERR_OOM : GFNT_ERR_LIMIT,
          GFNT_TAG_SBIX, 0, GFNT_GLYPH_NONE, "building sbix");
    }
  }
  if (result == GFNT_OK && kept > 0) {
    size_t head = 8 + 4 * strikes_out;

    gfnt_wbuf_u32(out, flags_word);
    gfnt_wbuf_u32(out, (uint32_t)strikes_out);
    for (size_t k = 0; k < strikes_out; ++k) {
      gfnt_wbuf_u32(out, (uint32_t)(head + starts[k]));
    }
    gfnt_wbuf_bytes(out, body.data, body.length);
    if (out->oom) {
      result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_SBIX, 0, GFNT_GLYPH_NONE,
          "building sbix");
    }
  }
  gfnt_wbuf_free(&body);
  allocator->free_fn(allocator->ctx, starts);
  return result;
}

GFNT_Result gfnt_subset_strikes(const GFNT_Face * face, const uint32_t * new_of_old,
    const uint32_t * old_of_new, size_t count, size_t new_count,
    const GFNT_Limits * limits, const GFNT_Allocator * allocator, GFNT_SubsetStrikes * out,
    GFNT_Error * error) {
  GFNT_Result result = GFNT_OK;

  if (gfnt_face_has_eblc(face)) {
    result = carry_bitmaps(face, GFNT_TAG_EBLC, GFNT_TAG_EBDT, old_of_new, count,
        new_count, limits, allocator, &out->eblc, &out->ebdt, error);
  }
  if (result == GFNT_OK && gfnt_face_has_cblc(face)) {
    result = carry_bitmaps(face, GFNT_TAG_CBLC, GFNT_TAG_CBDT, old_of_new, count,
        new_count, limits, allocator, &out->cblc, &out->cbdt, error);
  }
  if (result == GFNT_OK && gfnt_face_has_sbix(face)) {
    result = carry_sbix(face, new_of_old, old_of_new, count, new_count, limits,
        allocator, &out->sbix, error);
  }
  return result;
}
