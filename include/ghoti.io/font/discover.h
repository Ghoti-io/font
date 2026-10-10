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
 * @ref GFNT_FontSet "GFNT_FontSet": the faces in a directory, and the names
 * that point at them.
 *
 * documentation/design.md section 11. The caller owns the set. There is no
 * process-wide cache and no default face. This header is not pulled in by
 * `font.h`; a program that only reads a face it already has does not scan
 * directories.
 *
 * **What a scan records.** One record per face index. The family is name ID
 * 16, or name ID 1 when the font has no typographic family. The style is name
 * ID 17, or name ID 2. A face with no family is listed with an empty family.
 * Weight, width and slant come from `OS/2`: `usWeightClass`, `usWidthClass`,
 * and `fsSelection` bit 0 (italic) and bit 9 (oblique). Italic wins when both
 * are set. The Unicode ranges are the four `OS/2` words, or zeros when the
 * font has no `OS/2`. The format is the face flavour. The path and the index
 * identify the face.
 *
 * A bitmap face's `FAMILY_NAME` and `WEIGHT_NAME` are that family and that
 * weight. An XLFD — the font's own, or a `fonts.dir` line — fills only the
 * slant, weight or width the face itself did not state.
 *
 * **What a scan walks.** The walk recurses and follows symbolic links.
 * `gcu_path_canonicalize` makes two paths to one directory or file a single
 * visit. A directory deeper than 64 is a skip (::GFNT_ERR_LIMIT) and is not
 * entered. A file that is not a font is neither a record nor a skip. A path
 * the walk found and could not read is a skip (::GFNT_ERR_IO). A font that
 * comes back ::GFNT_ERR_CORRUPT or ::GFNT_ERR_LIMIT is a skip, and the faces
 * already listed stay listed.
 *
 * **Names.** The caller's aliases are copied first. `fonts.alias` adds a name
 * the table does not already have, and does not replace one. A target that
 * starts with `-` is an XLFD: the stored target is its family field.
 * ::gfnt_fontset_resolve_alias() follows the table. The eighth step is
 * ::GFNT_ERR_LIMIT, which is also what a cycle returns. `fonts.dir` fills a
 * missing weight, width or slant for a file in that directory. It does not
 * drop a font the file omits, add a file the walk did not open, or cause
 * `fonts.scale` to be read.
 *
 * **The system list.** Only ::gfnt_fontset_scan_system() reads the
 * environment. On Linux the directories, deduplicated, are
 * `$XDG_DATA_HOME/fonts` or else `$HOME/.local/share/fonts`, `$HOME/.fonts`,
 * each `$XDG_DATA_DIRS` entry (default `/usr/local/share:/usr/share`) plus
 * `/fonts`, and `/usr/share/fonts`. A directory that is not there is not an
 * error. fontconfig is not linked, its XML is not read, and its match rules
 * are not applied. The Windows and macOS lists are compiled on those
 * platforms and marked unverified; the registry, DirectWrite and CoreText
 * are not consulted.
 *
 * Matching a family list, `GFNT_FontProvider`, serialising the set, named
 * instances and `cmap` coverage are not this header.
 */

#ifndef GHOTI_IO_GFNT_DISCOVER_H
#define GHOTI_IO_GFNT_DISCOVER_H

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/face.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief `OS/2` `fsSelection` bit 0: the face is italic.
 *
 * When this and ::GFNT_OS2_FS_OBLIQUE are both set, the face is italic.
 */
#define GFNT_OS2_FS_ITALIC 0x0001u

/** @brief `OS/2` `fsSelection` bit 9: the face is oblique. */
#define GFNT_OS2_FS_OBLIQUE 0x0200u

/**
 * @brief How a face slants.
 *
 * Zero is roman, so a face that does not say is not reported as italic.
 */
typedef enum GFNT_Slant {
  GFNT_SLANT_ROMAN = 0, ///< Upright.
  GFNT_SLANT_ITALIC,    ///< Italic. Wins over oblique.
  GFNT_SLANT_OBLIQUE    ///< Oblique, and not italic.
} GFNT_Slant;

/**
 * @brief One face found by a scan.
 *
 * Every string is owned by the set and lives until ::gfnt_fontset_free().
 * `family` and `style` are never NULL: a face that states neither is listed
 * with an empty string. `weight` is `usWeightClass` (1 to 1000, 400 regular)
 * or a bitmap or XLFD word mapped onto that scale, and 0 when nothing stated
 * it. `width` is `usWidthClass` (1 to 9, 5 medium) or the XLFD setwidth on
 * that scale, and 0 when nothing stated it.
 */
typedef struct GFNT_FontRecord {
  const char * path;            ///< File the face was read from.
  size_t index;                 ///< Face index within that file.
  const char * family;          ///< Name ID 16, else 1. Never NULL.
  const char * style;           ///< Name ID 17, else 2. Never NULL.
  uint16_t weight;              ///< 0 when the face did not state one.
  uint16_t width;               ///< 0 when the face did not state one.
  GFNT_Slant slant;             ///< Roman when the face did not state one.
  uint32_t unicode_range[4];    ///< `OS/2` ranges, or zeros.
  GFNT_Tag flavour;             ///< ::gfnt_face_flavour().
} GFNT_FontRecord;

/**
 * @brief A name that resolves to another name.
 *
 * `target` is the next name in the chain. When the source was an XLFD, this
 * is the XLFD's family field rather than the whole XLFD.
 */
typedef struct GFNT_FontAlias {
  const char * name;   ///< The name being defined. Never NULL.
  const char * target; ///< What it points at. Never NULL.
} GFNT_FontAlias;

/**
 * @brief A path the walk found and did not list.
 *
 * A file that is not a font is not a skip. A directory deeper than 64 is a
 * skip of ::GFNT_ERR_LIMIT and is not entered.
 */
typedef struct GFNT_FontSkip {
  const char * path;   ///< The path. Never NULL.
  GFNT_Result result;  ///< Why it was not listed.
} GFNT_FontSkip;

/**
 * @brief The faces, aliases and skips from one scan.
 *
 * Opaque. The caller creates it with ::gfnt_fontset_scan() or
 * ::gfnt_fontset_scan_system() and releases it with ::gfnt_fontset_free().
 * Nothing in the process shares one.
 */
typedef struct GFNT_FontSet GFNT_FontSet;

/**
 * @brief List the faces under each directory.
 *
 * A root that is not a directory fails the call: no set is returned.
 * `aliases` are copied first, so a `fonts.alias` in the tree cannot replace
 * them. NULL `aliases` with a zero count means the caller has none.
 *
 * @param dirs Directories to walk. NULL is ::GFNT_ERR_INVALID.
 * @param dir_count How many. Zero is ::GFNT_ERR_INVALID.
 * @param aliases Names to copy in before `fonts.alias`, or NULL.
 * @param alias_count How many aliases. Zero when @p aliases is NULL.
 * @param allocator Allocator for the set, or NULL for the default.
 * @param out_set Receives the set. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID, ::GFNT_ERR_IO when a root is not a
 *   directory, or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_fontset_scan(const char * const * dirs,
    size_t dir_count, const GFNT_FontAlias * aliases, size_t alias_count,
    const GFNT_Allocator * allocator, GFNT_FontSet ** out_set,
    GFNT_Error * error);

/**
 * @brief List the faces in the platform's conventional directories.
 *
 * The only call in this library that reads the environment. A directory that
 * is not there is omitted, and that omission is not an error. An empty
 * machine returns an empty set.
 *
 * @param aliases Names to copy in before any `fonts.alias`, or NULL.
 * @param alias_count How many aliases. Zero when @p aliases is NULL.
 * @param allocator Allocator for the set, or NULL for the default.
 * @param out_set Receives the set. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_fontset_scan_system(
    const GFNT_FontAlias * aliases, size_t alias_count,
    const GFNT_Allocator * allocator, GFNT_FontSet ** out_set,
    GFNT_Error * error);

/**
 * @brief How many faces the set lists.
 *
 * @param set The set, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_fontset_count(const GFNT_FontSet * set);

/**
 * @brief The nth face, in the order the walk found it.
 *
 * @param set The set, or NULL.
 * @param index Which face, from 0.
 * @return The record, or NULL when @p set is NULL or @p index is past the end.
 *   The pointer lives as long as the set.
 */
GFNT_API const GFNT_FontRecord * gfnt_fontset_at(const GFNT_FontSet * set,
    size_t index);

/**
 * @brief How many aliases the set holds.
 *
 * @param set The set, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_fontset_alias_count(const GFNT_FontSet * set);

/**
 * @brief The nth alias, caller's first, then each `fonts.alias`.
 *
 * @param set The set, or NULL.
 * @param index Which alias, from 0.
 * @return The alias, or NULL when @p set is NULL or @p index is past the end.
 */
GFNT_API const GFNT_FontAlias * gfnt_fontset_alias_at(
    const GFNT_FontSet * set, size_t index);

/**
 * @brief How many paths were skipped.
 *
 * @param set The set, or NULL.
 * @return The count, or 0 for NULL.
 */
GFNT_API size_t gfnt_fontset_skip_count(const GFNT_FontSet * set);

/**
 * @brief The nth skip.
 *
 * @param set The set, or NULL.
 * @param index Which skip, from 0.
 * @return The skip, or NULL when @p set is NULL or @p index is past the end.
 */
GFNT_API const GFNT_FontSkip * gfnt_fontset_skip_at(const GFNT_FontSet * set,
    size_t index);

/**
 * @brief Follow an alias to the name it settles on.
 *
 * Seven steps still resolve. The eighth step — a chain that long, or a cycle
 * — is ::GFNT_ERR_LIMIT. An alias whose stored target is the same string as
 * its name has already settled (an XLFD family that is the alias's own name);
 * that is success, not a cycle. A name the table does not have is returned as
 * itself: @p out_target then points at @p name, which the set does not own. A
 * name the table does have comes back pointing into the set.
 *
 * @param set The set.
 * @param name The name to follow.
 * @param out_target Receives the settled name. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, ::GFNT_ERR_INVALID or ::GFNT_ERR_LIMIT.
 */
GFNT_API GFNT_Result gfnt_fontset_resolve_alias(const GFNT_FontSet * set,
    const char * name, const char ** out_target, GFNT_Error * error);

/**
 * @brief Release a set. NULL is ignored.
 *
 * @param set The set, or NULL.
 */
GFNT_API void gfnt_fontset_free(GFNT_FontSet * set);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_DISCOVER_H
