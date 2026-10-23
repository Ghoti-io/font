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
 * What src/scene/colr.c (story 14c) needs of src/scene/scene.c's binding: the
 * face a face resource was bound to by this library's provider.
 */

#ifndef GHOTI_IO_GFNT_SCENE_BINDING_H
#define GHOTI_IO_GFNT_SCENE_BINDING_H

#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>
#include <stdbool.h>
#include <ghoti.io/shape-scene/glyph_provider.h>

/**
 * @brief The ::GFNT_Face a face resource is bound to by this library's
 *   provider, or NULL for NULL, a resource that is not a face, an unbound face,
 *   or a face another provider binds.
 */
const GFNT_Face * gfnt_scene_face_of(const GSHSCENE_Resource * face);

/**
 * @brief The variation instance a face resource was interned at (L1 of color-paint story P4): the normalised
 *   coordinates of its binding and the default delta rounding. The coordinates are valid while the resource is held.
 *
 * @return false for NULL, a resource that is not a face, an unbound face, or a face another provider binds.
 */
bool gfnt_scene_instance_of(const GSHSCENE_Resource * face, GFNT_Variation * out);

#endif // GHOTI_IO_GFNT_SCENE_BINDING_H
