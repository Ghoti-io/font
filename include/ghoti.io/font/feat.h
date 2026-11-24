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
 * `feat`: the names of an Apple font's features.
 *
 * documentation/design.md section 9.6. An AAT font's `morx`/`mort` run by feature type and
 * selector (Ligatures, Common Ligatures On); `feat` says which ones the font has and what a
 * person is shown for each, as `name` IDs for ::gfnt_face_name(). A face with no `feat` has no
 * features to list, which is not an error: ::gfnt_face_has_feat() says whether to ask.
 *
 * The table is read in place at each call, so nothing here allocates.
 *
 * Reference: Apple's TrueType Reference Manual, "The 'feat' table".
 */

#ifndef GHOTI_IO_GFNT_FEAT_H
#define GHOTI_IO_GFNT_FEAT_H

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

/** @brief One feature of `feat`: a type, its name, and how its settings behave. */
typedef struct GFNT_FeatFeature {
  uint16_t type;            ///< The feature type (1 is Ligatures, 3 is Letter Case, ...).
  uint16_t name_id;         ///< Its name, a `name` ID.
  uint16_t flags;           ///< The table's own flags word, 0x8000 exclusive, 0x4000 with a default.
  uint16_t selector_count;  ///< How many settings it has.
  bool exclusive;           ///< Only one setting may be on at a time.
  bool has_default;         ///< ::GFNT_FeatFeature::default_index names the setting on by default.
  uint8_t default_index;    ///< Which setting (an index, not a selector) is on by default.
} GFNT_FeatFeature;

/** @brief One setting of a feature. */
typedef struct GFNT_FeatSelector {
  uint16_t selector;        ///< The selector the `morx` subtables test for.
  uint16_t name_id;         ///< Its name, a `name` ID.
} GFNT_FeatSelector;

/** @brief Whether the face has a `feat` table. */
GFNT_API bool gfnt_face_has_feat(const GFNT_Face * face);

/** @brief How many features `feat` lists. `GFNT_ERR_UNSUPPORTED` when the face has no `feat`. */
GFNT_API GFNT_Result gfnt_face_feat_count(const GFNT_Face * face, size_t * out_count, GFNT_Error * error);

/** @brief Feature @p index of `feat`, whose settings are all inside the table. */
GFNT_API GFNT_Result gfnt_face_feat_at(const GFNT_Face * face, size_t index, GFNT_FeatFeature * out,
    GFNT_Error * error);

/** @brief Setting @p selector_index of feature @p feature_index. */
GFNT_API GFNT_Result gfnt_face_feat_selector_at(const GFNT_Face * face, size_t feature_index,
    size_t selector_index, GFNT_FeatSelector * out, GFNT_Error * error);

/** @brief Print every feature and setting, one line each; the line format `ttx_diff.py` reads. */
GFNT_API GFNT_Result gfnt_face_feat_dump(const GFNT_Face * face, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_FEAT_H
