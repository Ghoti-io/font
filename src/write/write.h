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
 * What the writer's files share: big-endian stores and the sfnt checksum.
 * Internal to `src/write/`.
 */

#ifndef GHOTI_IO_GFNT_WRITE_INTERNAL_H
#define GHOTI_IO_GFNT_WRITE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/write.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Store a 16-bit value big-endian. */
void gfnt_write_put16(uint8_t * p, uint32_t v);
/** Store a 32-bit value big-endian. */
void gfnt_write_put32(uint8_t * p, uint32_t v);
/** The sfnt checksum of @p length bytes: big-endian words, the tail zero-padded. */
uint32_t gfnt_write_checksum(const uint8_t * data, size_t length);

/**
 * Build the sfnt bytes. The shared body of ::gfnt_write_sfnt() and
 * ::gfnt_write_woff(); the caller frees @p out_bytes with @p allocator.
 */
GFNT_Result gfnt_write_build(GFNT_Tag flavour, const GFNT_WriteTable * tables,
    size_t count, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    uint8_t ** out_bytes, size_t * out_size, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_INTERNAL_H
