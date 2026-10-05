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
 * Umbrella header for **tier 0**: the reader, the metric tables, `cmap`, names,
 * glyph access and the bitmap strikes.
 *
 * `outline.h` and `raster.h` are implemented and are deliberately **not** here.
 * They are tier 1 (design.md section 4), and the tier boundary is the dependency
 * boundary: a consumer that only needs to know which glyph a codepoint maps to
 * and how wide it is should not link a rasteriser. Include them by name.
 *
 * Shaping, layout, font discovery and writing are not implemented at all. See
 * documentation/design.md section 18.
 */

#ifndef GHOTI_IO_GFNT_FONT_H
#define GHOTI_IO_GFNT_FONT_H

#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/cvt.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/featurevar.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/stat.h>
#include <ghoti.io/font/variation.h>
#include <ghoti.io/font/macros.h>

#endif // GHOTI_IO_GFNT_FONT_H
