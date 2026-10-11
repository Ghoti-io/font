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
 * `SVG ` for a subset that keeps its glyph ids.
 *
 * documentation/design.md section 12. A document of the table is an SVG file whose elements are
 * named for glyph ids (`glyph7`), and the ids are inside the document, so renumbering the glyphs
 * would mean rewriting every document: not done, and the table is then left out. When the ids are
 * retained every document is valid as it is, so the table is written again with only the
 * documents that serve a kept glyph, in their order, each with the range it had.
 */

#include <ghoti.io/font/svg.h>
#include "../sfnt/sfnt.h"
#include "write.h"

GFNT_Result gfnt_subset_svg(const GFNT_Face * face, const uint8_t * keep, size_t count,
    const GFNT_Allocator * allocator, GFNT_WBuf * out, GFNT_Error * error) {
  const GFNT_Tag tag = GFNT_TAG('S', 'V', 'G', ' ');
  GFNT_SvgDocument doc;
  size_t total = 0;
  size_t i;
  size_t kept = 0;
  size_t offset;
  uint8_t * chosen;
  GFNT_Result r;

  if (!gfnt_face_has_svg(face)) {
    return GFNT_OK;
  }
  r = gfnt_face_svg_document_count(face, &total, error);
  if (r == GFNT_ERR_UNSUPPORTED) {
    return GFNT_OK;   // A version this library does not read is not carried.
  }
  if (r != GFNT_OK) {
    return r;
  }
  chosen = allocator->calloc_fn(allocator->ctx, total ? total : 1, 1);
  if (!chosen) {
    return gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE, "choosing SVG documents");
  }
  for (i = 0; i < total && r == GFNT_OK; i++) {
    uint32_t g;

    r = gfnt_face_svg_document_at(face, i, &doc, error);
    for (g = doc.first_glyph; r == GFNT_OK && g <= doc.last_glyph && g < count; g++) {
      if (keep[g]) {
        chosen[i] = 1;
        kept++;
        break;
      }
    }
  }
  if (r != GFNT_OK || !kept) {
    allocator->free_fn(allocator->ctx, chosen);
    return r;
  }
  gfnt_wbuf_u16(out, 0);
  gfnt_wbuf_u32(out, 10);
  gfnt_wbuf_u32(out, 0);
  gfnt_wbuf_u16(out, (uint32_t)kept);
  offset = 2 + 12 * kept;
  for (i = 0; i < total; i++) {
    if (chosen[i]) {
      gfnt_face_svg_document_at(face, i, &doc, error);
      gfnt_wbuf_u16(out, doc.first_glyph);
      gfnt_wbuf_u16(out, doc.last_glyph);
      gfnt_wbuf_u32(out, (uint32_t)offset);
      gfnt_wbuf_u32(out, (uint32_t)doc.length);
      offset += doc.length;
    }
  }
  for (i = 0; i < total; i++) {
    if (chosen[i]) {
      gfnt_face_svg_document_at(face, i, &doc, error);
      gfnt_wbuf_bytes(out, doc.data, doc.length);
    }
  }
  allocator->free_fn(allocator->ctx, chosen);
  return out->oom ? gfnt_error_set(error, GFNT_ERR_OOM, tag, 0, GFNT_GLYPH_NONE,
      "building SVG") : GFNT_OK;
}
