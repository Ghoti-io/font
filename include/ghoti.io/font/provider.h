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
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The seam between a scanned set and a paragraph: a ::GFNT_FontProvider answered by a
 * ::GFNT_FontFallback.
 *
 * documentation/design.md section 11. Layout (`layout.h`) asks a provider for a face when a
 * character of an item is in none of its span's faces; discovery and matching (`match.h`) know
 * how to find one in a set. Neither includes the other, so the two are joined here: the style
 * the provider is asked for is read from the span's first face (its weight, width and slant,
 * and its family, which is tried before the rest), and the answer comes from the fallback.
 */

#ifndef GHOTI_IO_GFNT_PROVIDER_H
#define GHOTI_IO_GFNT_PROVIDER_H

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/match.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A paragraph's font provider that draws on @p fallback.
 *
 * The result is a plain value to put in ::GFNT_ParagraphOptions: it points at @p fallback,
 * which must outlive every paragraph and layout made with it, as must the set under it. A
 * character no face maps gets NULL, and layout goes on with its own faces.
 */
GFNT_API GFNT_FontProvider gfnt_fontfallback_provider(GFNT_FontFallback * fallback);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_PROVIDER_H
