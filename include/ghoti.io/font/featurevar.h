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
 * `FeatureVariations`: which layout features a location in the design space
 * replaces.
 *
 * documentation/design.md section 7.7. A `GSUB` or `GPOS` of version 1.1 may carry
 * a list of records, each a set of conditions on the axes and a set of
 * *substitute* feature tables. At a location, the **first** record whose
 * conditions all hold applies, and for every feature it names, the substitute's
 * lookups stand where the feature's own were - which is how a font swaps a glyph
 * for a bolder form past a weight, or turns a feature off at an extreme.
 *
 * This library does not shape, so it does not apply a substitution. What it reads
 * is the part of the table that is about *variation*: which record matches, and
 * what that record substitutes. The feature list and the lookups are not parsed,
 * so a substitute is reported as it is stored, a feature index and the lookup
 * indices that replace it.
 *
 * The conditions are evaluated against **normalised** coordinates, as for every
 * accessor that takes a ::GFNT_Variation. Condition formats 1 (an axis range) and
 * 3, 4 and 5 (and, or, not) are read. Format 2, a value that is itself varied
 * through `GDEF`, is refused by name: it is in the specification and in no font
 * this library has met, and there is no reference here whose evaluation it could
 * be held to.
 *
 * Reference: OpenType Specification 1.9, "GSUB - Glyph Substitution Table"
 * (FeatureVariations Table, ConditionSet Table, Condition Table,
 * FeatureTableSubstitution Table).
 */

#ifndef GHOTI_IO_GFNT_FEATUREVAR_H
#define GHOTI_IO_GFNT_FEATUREVAR_H

#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ::gfnt_face_feature_variations_match()'s answer when no record applies. */
#define GFNT_FEATURE_VARIATIONS_NONE ((size_t)-1)

/**
 * @brief How many `FeatureVariations` records a layout table has.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`, as ::GFNT_TAG('G','S','U','B').
 * @param out_count Receives the count, **0 for a table with no
 *   `FeatureVariations`** (a version 1.0 table, or a null offset). Written only on
 *   success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a tag that is neither; 
 *   ::GFNT_ERR_UNSUPPORTED for a face without that table or a version other than
 *   1.x; ::GFNT_ERR_CORRUPT.
 */
GFNT_API GFNT_Result gfnt_face_feature_variations_count(const GFNT_Face * face,
    GFNT_Tag table, size_t * out_count, GFNT_Error * error);

/**
 * @brief The first record whose conditions all hold at a location.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param coordinates The location, normalised 2.14 in `fvar` order, or NULL.
 * @param coordinate_count How many were given; axes past it are zero.
 * @param out_record Receives the record's index, or
 *   ::GFNT_FEATURE_VARIATIONS_NONE. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_feature_variations_count(), and ::GFNT_ERR_UNSUPPORTED for
 *   a condition of format 2 or one this library does not know.
 */
GFNT_API GFNT_Result gfnt_face_feature_variations_match(const GFNT_Face * face,
    GFNT_Tag table, const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    size_t * out_record, GFNT_Error * error);

/**
 * @brief How many features a record substitutes.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param record Which record, below the count.
 * @param out_count Receives it; 0 for a record with no substitution table.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID for a record the table does not have, or
 *   as ::gfnt_face_feature_variations_count().
 */
GFNT_API GFNT_Result gfnt_face_feature_substitution_count(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t * out_count, GFNT_Error * error);

/**
 * @brief One substitution of a record: which feature, and how many lookups replace it.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param record Which record.
 * @param index Which of its substitutions, below the count.
 * @param out_feature_index Receives the index into the table's feature list that
 *   is replaced. Written only on success.
 * @param out_lookup_count Receives how many lookup indices the substitute lists.
 *   Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_feature_substitution_count().
 */
GFNT_API GFNT_Result gfnt_face_feature_substitution_at(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t index, uint16_t * out_feature_index,
    size_t * out_lookup_count, GFNT_Error * error);

/**
 * @brief One lookup index of a substitution's substitute feature.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param record Which record.
 * @param index Which of its substitutions.
 * @param position Which lookup of the substitute, below the count
 *   ::gfnt_face_feature_substitution_at() gave.
 * @param out_lookup Receives the lookup list index. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As ::gfnt_face_feature_substitution_count().
 */
GFNT_API GFNT_Result gfnt_face_feature_substitution_lookup(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t index, size_t position,
    uint16_t * out_lookup, GFNT_Error * error);

/**
 * @brief Print one table's `FeatureVariations`, one line per condition and substitute.
 *
 * Conditions are printed as the tree they are: `cond N.M: range axis A min X max Y`
 * for format 1, and `and`, `or` and `not` with their children indented below.
 *
 * @param face The face.
 * @param table `GSUB` or `GPOS`.
 * @param out The stream.
 * @return ::GFNT_OK, or what reading the table returned.
 */
GFNT_API GFNT_Result gfnt_face_feature_variations_dump(const GFNT_Face * face,
    GFNT_Tag table, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_FEATUREVAR_H
