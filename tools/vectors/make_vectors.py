#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Generate the two vectors documentation/design.md section 7.2 deferred.

Section 14's rule is that **a vector comes from an oracle and is never written
from memory**, and these are the two this library owed:

  * the 258-entry standard Macintosh glyph order, which `post` format 1.0 *is*
    and which format 2.0 spells its first 258 names as indices into;
  * the single-byte Macintosh `name` encodings, which is how a Macintosh record
    above ASCII is read at all.

Both come out of the pinned `fonttools` image, so the tables this library
compiles agree with the reference the differentials compare it against - by
construction rather than by hope. The emitted sources are committed; the
generator is what makes them auditable, and `make check-vectors` fails when a
committed table is not what it emits.

The Macintosh encoding is not selected by the encoding ID
---------------------------------------------------------

This was the trap, and it is why the table below is a selector rather than a
list. Macintosh `platEncID` 0 does **not** mean Mac Roman. fontTools keys it by
`langID`, and six other encodings live under it:

    langID 15 -> mac_iceland     24-28, 36, 38-40 -> mac_latin2
    langID 17 -> mac_turkish     37               -> mac_romanian
    langID 18 -> mac_croatian    anything else    -> mac_roman

A library that reads "Macintosh, encoding 0" as Mac Roman is wrong for every
Icelandic, Turkish, Croatian, Central European and Romanian record in the world,
and would be wrong *silently*: the bytes decode, they just decode to the wrong
letters. The relation has to be restricted at both ends.

`platEncID` 6, 7, 29, 35 and 37 name single-byte encodings directly. 1, 2, 3 and
25 are the multi-byte CJK encodings, which are a data set of their own and are
refused rather than guessed.

The cross-check, and the two bytes it isolated
----------------------------------------------

The table is generated from CPython's codecs, because that is what fontTools'
`name` decoding uses and therefore what the differential's agreement means. That
alone would be circular, so it is checked against **glibc's `MACINTOSH`
charmap** through `iconv` - a genuinely separate transcription of Apple's
mapping that happens to be in the same image.

126 of the 128 high bytes agree. The two that do not are listed in
`GLIBC_KNOWN_DIFFERENCES` with the reading each side gives, and any *third*
byte disagreeing fails this generator rather than being absorbed. Both are
cases where glibc's charmap departs from Apple's published `ROMAN.TXT`:

  * 0xC6 - glibc says U+0394 GREEK CAPITAL DELTA, CPython says U+2206
    INCREMENT. glibc is internally inconsistent here: it gives 0xB7 as U+2211
    N-ARY SUMMATION rather than Greek Sigma, so it treats one of the pair as a
    mathematical operator and the other as a Greek letter.
  * 0xF0 - the Apple logo, which is private use either way. CPython uses
    U+F8FF, Apple's registered corporate-use codepoint; glibc uses U+E01E.

Neither is a case where this library has a choice: agreeing with the reference
is the requirement, and the cross-check's job is to prove the other 126 were
transcribed rather than remembered.

What is emitted
---------------

    src/tables/post_names.h        the 258 standard names
    src/name/mac_encodings.h       the tables and the selector
    tests/data/vectors/*.txt       the same facts as text, for testVectors

The text files are the reason `make test` covers this without a container: they
are committed, so a hand-edited table fails the unit suite on a fresh clone with
no image, while a generator changed without regenerating fails `check-vectors`
wherever the image is. Neither gate skips.
"""

import argparse
import os
import subprocess
import sys

from fontTools.misc.encodingTools import getEncoding
from fontTools.ttLib.tables._p_o_s_t import standardGlyphOrder

# The single-byte Macintosh encodings, in the order they are emitted. Roman is
# first so that index 0 is the common case and a zeroed selector is not silently
# some other language's table.
MAC_TABLES = [
    "mac_roman",
    "mac_iceland",
    "mac_turkish",
    "mac_croatian",
    "mac_latin2",
    "mac_romanian",
    "mac_greek",
    "mac_cyrillic",
]

# Every (platEncID, langID) this library answers for, asked of fontTools rather
# than asserted here. The language IDs are the ones its own map keys on; the
# sentinel -1 is "any other language", which is how a table becomes the default
# for its encoding ID.
MAC_LANGUAGES = [15, 17, 18, 24, 25, 26, 27, 28, 36, 37, 38, 39, 40, -1]
MAC_ENCODINGS = [0, 1, 2, 3, 6, 7, 25, 29, 35, 37]

# Bytes where glibc's MACINTOSH charmap and CPython's mac_roman disagree, with
# why. A third disagreement fails the build: this is a checked category, not an
# exclusion that absorbs whatever lands in it.
GLIBC_KNOWN_DIFFERENCES = {
    0xC6: ("glibc U+0394 GREEK CAPITAL LETTER DELTA, CPython U+2206 INCREMENT; "
           "glibc gives 0xB7 as U+2211 N-ARY SUMMATION rather than Greek "
           "Sigma, so it splits a pair Apple's ROMAN.TXT keeps together"),
    0xF0: ("the Apple logo, private use either way: CPython uses U+F8FF, "
           "Apple's registered corporate-use codepoint, and glibc U+E01E"),
}

HEADER = """\
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
 * **Generated by tools/vectors/make_vectors.py. Do not edit.**
 *
 * %(what)s
 *
 * Regenerate with `make gen-vectors`, which runs that script in the pinned
 * `fonttools` image; `make check-vectors` fails when this file is not what it
 * emits. documentation/design.md sections 7.2 and 14.
 *
 * Copyright 2026 by Corey Pennycuff
 */
"""


def mac_high(encoding):
    """One encoding's 0x80..0xFF as codepoints.

    The low half is asserted to be ASCII rather than emitted: all eight of these
    are ASCII below 0x80, and a table that stored it would be 1 kB of identity
    map that a reader has to check anyway.
    """
    low = bytes(range(0x80)).decode(encoding)
    if low != "".join(chr(i) for i in range(0x80)):
        raise SystemExit("%s is not ASCII below 0x80" % encoding)
    high = []
    for byte in range(0x80, 0x100):
        text = bytes([byte]).decode(encoding)
        if len(text) != 1:
            raise SystemExit("%s maps 0x%02X to %d codepoints" %
                             (encoding, byte, len(text)))
        point = ord(text)
        if point > 0xFFFF:
            raise SystemExit("%s maps 0x%02X above the BMP (U+%04X); the "
                             "emitted table is uint16_t" %
                             (encoding, byte, point))
        high.append(point)
    return high


def glibc_mac_roman():
    """glibc's MACINTOSH charmap, or None if iconv cannot answer."""
    try:
        finished = subprocess.run(
            ["iconv", "-f", "MACINTOSH", "-t", "UTF-32LE"],
            input=bytes(range(0x80, 0x100)), capture_output=True)
    except FileNotFoundError:
        return None
    if finished.returncode != 0:
        return None
    raw = finished.stdout
    if len(raw) != 128 * 4:
        return None
    return [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(128)]


def cross_check(roman):
    """Compare the emitted Mac Roman against glibc, and account for every byte.

    Raises unless the disagreements are exactly the two documented ones. A byte
    that stops disagreeing matters as much as a new one that starts: a known
    difference that has silently become agreement is a category still excusing
    something, and the next reader would not know it could be deleted.
    """
    theirs = glibc_mac_roman()
    if theirs is None:
        raise SystemExit(
            "iconv could not read the MACINTOSH charmap, so the Mac Roman "
            "table would be generated from one source only. That is the whole "
            "thing this check exists to prevent; fix the image rather than "
            "skipping it.")

    differ = {}
    for index, (mine, other) in enumerate(zip(roman, theirs)):
        if mine != other:
            differ[0x80 + index] = (other, mine)

    unexpected = sorted(set(differ) - set(GLIBC_KNOWN_DIFFERENCES))
    vanished = sorted(set(GLIBC_KNOWN_DIFFERENCES) - set(differ))
    if unexpected:
        raise SystemExit(
            "glibc and CPython disagree on Mac Roman byte(s) this generator "
            "does not account for: %s. Each is either a codec change worth "
            "understanding or a transcription error; neither may be added to "
            "GLIBC_KNOWN_DIFFERENCES without a reason beside it."
            % ", ".join("0x%02X (glibc U+%04X, CPython U+%04X)"
                        % (byte, differ[byte][0], differ[byte][1])
                        for byte in unexpected))
    if vanished:
        raise SystemExit(
            "glibc and CPython now agree on byte(s) listed as known "
            "differences: %s. Delete the entries - a category that excuses "
            "nothing is a category that stops being read."
            % ", ".join("0x%02X" % byte for byte in vanished))
    return len(roman) - len(differ), differ


def selector():
    """The (platEncID, langID) -> table decisions, taken from fontTools.

    Every pair is *asked*, not derived: the encoding ID alone does not decide,
    and writing out what the map must contain is how a library ends up confident
    and wrong. Unsupported encodings are recorded as such so that the emitted
    source refuses them by name rather than by falling off the end of a table.
    """
    by_encoding = {}
    for encoding in MAC_ENCODINGS:
        languages = {}
        for language in MAC_LANGUAGES:
            # fontTools keys encoding 0 by language and the rest by encoding
            # alone; asking with a language either way costs nothing and means
            # this loop does not have to know which is which.
            ask = 0 if language < 0 else language
            name = getEncoding(1, encoding, ask, default=None)
            languages[language] = name
        by_encoding[encoding] = languages
    return by_encoding


def emit_post_names(out_dir, names):
    path = os.path.join(out_dir, "src", "tables", "post_names.h")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER % {"what":
            "The %d names of the standard Macintosh glyph order, which `post`\n"
            " * format 1.0 is and whose indices format 2.0 uses for its first "
            "%d\n * glyphs. From fontTools' `standardGlyphOrder`."
            % (len(names), len(names))})
        handle.write("""
#ifndef GHOTI_IO_GFNT_POST_NAMES_H
#define GHOTI_IO_GFNT_POST_NAMES_H

// Before anything is declared, so that namespace.h's renames are already in
// effect - CONVENTIONS.md section 4, enforced by check-symbols, which caught
// the first version of this generator for skipping it.
#include <ghoti.io/font/macros.h>

/** @brief How many names the standard Macintosh glyph order has. */
#define GFNT_POST_STANDARD_NAME_COUNT %d

/**
 * @brief The standard Macintosh glyph order.
 *
 * A `post` format 2.0 glyph name index below
 * ::GFNT_POST_STANDARD_NAME_COUNT names one of these; an index at or above it
 * names a Pascal string in the table itself.
 */
static const char * const gfnt_post_standard_names[
    GFNT_POST_STANDARD_NAME_COUNT] = {
""" % len(names))
        for index, name in enumerate(names):
            handle.write('  "%s", // %d\n' % (name, index))
        handle.write("};\n\n#endif // GHOTI_IO_GFNT_POST_NAMES_H\n")
    return path


def emit_mac_encodings(out_dir, tables, chosen):
    path = os.path.join(out_dir, "src", "name", "mac_encodings.h")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER % {"what":
            "The single-byte Macintosh `name` encodings, and the\n"
            " * (platEncID, langID) selector that chooses between them. From\n"
            " * CPython's codecs, cross-checked against glibc's MACINTOSH\n"
            " * charmap; see the generator for the two bytes they differ on."})
        handle.write("""
#ifndef GHOTI_IO_GFNT_MAC_ENCODINGS_H
#define GHOTI_IO_GFNT_MAC_ENCODINGS_H

// Before anything is declared: this header declares a type
// (GFNT_MacEncodingRule), which is exactly the case CONVENTIONS.md section 4 is
// about - a type named before namespace.h has renamed it is a second type under
// one spelling.
#include <ghoti.io/font/macros.h>
#include <stdint.h>

/** @brief How many single-byte Macintosh encodings are tabulated. */
#define GFNT_MAC_TABLE_COUNT %d

/** @brief No table: the encoding is one this library does not decode. */
#define GFNT_MAC_TABLE_NONE (-1)

/** @brief Which table each index is, for diagnostics. */
static const char * const gfnt_mac_table_names[GFNT_MAC_TABLE_COUNT] = {
""" % len(MAC_TABLES))
        for name in MAC_TABLES:
            handle.write('  "%s",\n' % name)
        handle.write("""};

/**
 * @brief Bytes 0x80 to 0xFF of each encoding, as Unicode codepoints.
 *
 * The low half is not stored: all of these are ASCII below 0x80, which a
 * decoder checks anyway, and storing an identity map invites a reader to trust
 * it instead.
 */
static const uint16_t gfnt_mac_high[GFNT_MAC_TABLE_COUNT][128] = {
""")
        for name in MAC_TABLES:
            handle.write("  { // %s\n" % name)
            row = tables[name]
            for start in range(0, 128, 8):
                handle.write("    " + " ".join(
                    "0x%04X," % point for point in row[start:start + 8])
                    + " // 0x%02X\n" % (0x80 + start))
            handle.write("  },\n")
        handle.write("""};

/**
 * @brief One (encoding, language) decision.
 *
 * @p language is ::GFNT_MAC_LANGUAGE_ANY for the encoding's default, and the
 * table is scanned for an exact language match before that default is used.
 * Macintosh `platEncID` 0 is **not** Mac Roman: it is keyed by language, and
 * Icelandic, Turkish, Croatian, Central European and Romanian all live under
 * it. That is the whole reason this is a table and not a switch.
 */
typedef struct GFNT_MacEncodingRule {
  uint16_t encoding; ///< platEncID.
  int32_t language;  ///< langID, or ::GFNT_MAC_LANGUAGE_ANY.
  int32_t table;     ///< Index into ::gfnt_mac_high, or ::GFNT_MAC_TABLE_NONE.
} GFNT_MacEncodingRule;

/** @brief Any language not named explicitly for this encoding. */
#define GFNT_MAC_LANGUAGE_ANY (-1)

""")
        rules = []
        for encoding in MAC_ENCODINGS:
            languages = chosen[encoding]
            default = languages[-1]
            for language in MAC_LANGUAGES:
                if language < 0:
                    continue
                if languages[language] != default:
                    rules.append((encoding, language, languages[language]))
            rules.append((encoding, -1, default))

        handle.write("#define GFNT_MAC_RULE_COUNT %d\n\n" % len(rules))
        handle.write("static const GFNT_MacEncodingRule gfnt_mac_rules["
                     "GFNT_MAC_RULE_COUNT] = {\n")
        for encoding, language, name in rules:
            if name in MAC_TABLES:
                table = "%d" % MAC_TABLES.index(name)
                comment = name
            else:
                table = "GFNT_MAC_TABLE_NONE"
                comment = name if name else "no encoding named"
            handle.write("  { %2d, %s, %s }, // %s\n"
                % (encoding,
                   "GFNT_MAC_LANGUAGE_ANY" if language < 0 else "%d" % language,
                   table, comment))
        handle.write("};\n\n#endif // GHOTI_IO_GFNT_MAC_ENCODINGS_H\n")
    return path, rules


def emit_text(out_dir, names, tables, rules, agreed, differ):
    """The same facts as text, committed, so that `make test` can check them."""
    directory = os.path.join(out_dir, "tests", "data", "vectors")
    os.makedirs(directory, exist_ok=True)

    path = os.path.join(directory, "standard_glyph_order.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# The standard Macintosh glyph order, from fontTools.\n"
                     "# Generated by tools/vectors/make_vectors.py; read by "
                     "testVectors.\n# <index> <name>\n")
        for index, name in enumerate(names):
            handle.write("%d\t%s\n" % (index, name))

    path = os.path.join(directory, "mac_encodings.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# The single-byte Macintosh encodings, from CPython's "
                     "codecs.\n"
                     "# Generated by tools/vectors/make_vectors.py; read by "
                     "testVectors.\n"
                     "# glibc's MACINTOSH charmap agrees on %d of 128 Mac "
                     "Roman bytes; the\n# %d that differ are listed in the "
                     "generator with the reason.\n"
                     "# <table> <byte> <codepoint> <utf-8 bytes, hex>\n"
                     % (agreed, len(differ)))
        for name in MAC_TABLES:
            for index, point in enumerate(tables[name]):
                handle.write("%s\t0x%02X\tU+%04X\t%s\n"
                    % (name, 0x80 + index, point,
                       chr(point).encode("utf-8").hex()))

    path = os.path.join(directory, "mac_selector.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# Which Macintosh encoding a (platEncID, langID) names, "
                     "from fontTools.\n"
                     "# Generated by tools/vectors/make_vectors.py; read by "
                     "testVectors.\n"
                     "# platEncID 0 is keyed by language and is NOT Mac Roman "
                     "for six of them.\n"
                     "# <platEncID> <langID or ANY> <table or ->\n")
        for encoding, language, name in rules:
            handle.write("%d\t%s\t%s\n"
                % (encoding, "ANY" if language < 0 else str(language),
                   name if name in MAC_TABLES else "-"))
    return directory


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=None,
        help="write under this directory instead of the repository; the paths "
             "below it are the same, so a check can diff tree against tree")
    args = parser.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = args.out or os.path.dirname(os.path.dirname(here))

    names = list(standardGlyphOrder)
    if len(names) != 258:
        raise SystemExit("fontTools' standardGlyphOrder has %d entries, not "
                         "258; the format 2.0 index boundary is that number "
                         "and this generator will not guess it" % len(names))

    tables = {name: mac_high(name) for name in MAC_TABLES}
    agreed, differ = cross_check(tables["mac_roman"])
    chosen = selector()

    written = [emit_post_names(root, names)]
    path, rules = emit_mac_encodings(root, tables, chosen)
    written.append(path)
    written.append(emit_text(root, names, tables, rules, agreed, differ))

    print("vectors: %d standard glyph names, %d single-byte Macintosh "
          "encodings, %d (encoding, language) rules"
          % (len(names), len(MAC_TABLES), len(rules)))
    print("vectors: Mac Roman cross-checked against glibc's MACINTOSH charmap: "
          "%d of 128 bytes agree, %d accounted for (%s)"
          % (agreed, len(differ),
             ", ".join("0x%02X" % byte for byte in sorted(differ))))
    for item in written:
        print("vectors: wrote %s" % os.path.relpath(item, root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
