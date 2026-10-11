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
 * The `SVG ` table: OpenType-SVG color glyphs, as documents to hand to an SVG renderer.
 *
 * The table is an index from glyph ranges to SVG documents. One document may serve many glyphs
 * (each is the element with the id `glyph<N>` for glyph N), and a document may be stored as a
 * gzip member. This library finds the document for a glyph and, if asked, inflates it; it does
 * not parse or draw SVG, which is the renderer's, and it does not look inside the document.
 * Documents are borrowed from the table in place, so nothing here allocates until a document
 * is inflated.
 *
 * A glyph with an `SVG ` document has a color form to draw; whether the renderer prefers it to
 * `COLR` or a bitmap is the caller's policy. The glyph's outline in `glyf` or `CFF` is the
 * fallback the specification asks for.
 *
 * Reference: OpenType Specification 1.9, "SVG".
 */

#ifndef GHOTI_IO_GFNT_SVG_H
#define GHOTI_IO_GFNT_SVG_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One document of the table's index, borrowed from the table. */
typedef struct GFNT_SvgDocument {
  uint32_t first_glyph;   ///< The first glyph it serves.
  uint32_t last_glyph;    ///< The last (inclusive).
  const uint8_t * data;   ///< The document as stored; valid until the face is freed.
  size_t length;          ///< Its length in bytes.
  bool compressed;        ///< It is a gzip member, and ::gfnt_svg_document_inflate() makes it text.
} GFNT_SvgDocument;

/** @brief Whether the face has an `SVG ` table. */
GFNT_API bool gfnt_face_has_svg(const GFNT_Face * face);

/**
 * @brief How many documents the table indexes.
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a face with no `SVG ` table
 *   or a version other than 0; ::GFNT_ERR_CORRUPT when the index is not inside the table.
 */
GFNT_API GFNT_Result gfnt_face_svg_document_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error);

/**
 * @brief One document of the index, by its place in it (the table's own order, by first glyph).
 *
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a NULL argument or an index out of range;
 *   ::GFNT_ERR_UNSUPPORTED; ::GFNT_ERR_CORRUPT when the record, the document it names or the
 *   range (last before first) is wrong.
 */
GFNT_API GFNT_Result gfnt_face_svg_document_at(const GFNT_Face * face, size_t index,
    GFNT_SvgDocument * out, GFNT_Error * error);

/**
 * @brief The document that serves a glyph.
 *
 * The index is searched as the specification asks it to be kept, in order by first glyph with
 * no ranges overlapping, so a table that breaks that rule may not find a glyph it lists (the
 * same as HarfBuzz).
 *
 * @param glyph The glyph ID.
 * @param out Receives the document. Written only when @p out_found is set.
 * @param out_found Receives whether a document serves the glyph.
 * @return ::GFNT_OK (found or not); ::GFNT_ERR_INVALID; ::GFNT_ERR_UNSUPPORTED for a face with
 *   no `SVG ` table; ::GFNT_ERR_CORRUPT for a record or document outside the table.
 */
GFNT_API GFNT_Result gfnt_face_glyph_svg(const GFNT_Face * face, uint32_t glyph,
    GFNT_SvgDocument * out, bool * out_found, GFNT_Error * error);

/**
 * @brief A document's text: inflated if it is compressed, borrowed if not.
 *
 * @param face The face the document came from (its limits cap the inflated size).
 * @param document The document.
 * @param allocator For the inflated bytes, or NULL for the default.
 * @param out_blob Receives a blob the caller destroys with ::gfnt_blob_destroy(). For a
 *   document that is not compressed it points into the table and lives only as long as the face.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID; ::GFNT_ERR_CORRUPT for a gzip member that is not one or
 *   whose checksum fails; ::GFNT_ERR_LIMIT for one that inflates past the face's cap on a blob;
 *   ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_svg_document_inflate(const GFNT_Face * face,
    const GFNT_SvgDocument * document, const GFNT_Allocator * allocator, GFNT_Blob ** out_blob,
    GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_SVG_H
