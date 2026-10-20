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
#include <ghoti.io/shape-scene/glyph_provider.h>

/**
 * @brief The ::GFNT_Face a face resource is bound to by this library's
 *   provider, or NULL for NULL, a resource that is not a face, an unbound face,
 *   or a face another provider binds.
 */
const GFNT_Face * gfnt_scene_face_of(const GSHSCENE_Resource * face);

#endif // GHOTI_IO_GFNT_SCENE_BINDING_H
