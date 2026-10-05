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
"""Write src/shape/use_data.h from Microsoft's Universal Shaping Engine data.

    use_data.py <IndicSyllabicCategory-Additional.txt> \\
                <IndicPositionalCategory-Additional.txt> > src/shape/use_data.h

The two files are what the Unicode Character Database leaves out for the scripts
the Universal Shaping Engine serves: a syllabic or positional category for a
character that has none, and a correction to one that has the wrong one for
shaping. They are published at

    https://github.com/microsoft/font-tools/tree/master/USE

under the MIT licence, and `fetch-use-data.sh` takes them at a pinned commit and
checks them against pinned hashes. What is generated is a sorted list of ranges for
each, for a binary search. The notice below is Microsoft's, because the data is.
"""

import re
import sys

LICENSE = """/*
 * The data in this file is derived from Microsoft's font-tools repository,
 * USE/IndicSyllabicCategory-Additional.txt and
 * USE/IndicPositionalCategory-Additional.txt, which are used under the MIT licence:
 *
 *   MIT License
 *
 *   Copyright (c) Microsoft Corporation.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */
"""

NOTICE = """/*
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
"""

# The syllabic categories, as the values the shaper compares: the names the
# Unicode database uses are `unicode`'s own constants; the rest are USE's.
ISC = {
    "Bindu": "GUNI_INSC_BINDU", "Cantillation_Mark": "GUNI_INSC_CANTILLATION_MARK",
    "Consonant": "GUNI_INSC_CONSONANT", "Consonant_Dead": "GUNI_INSC_CONSONANT_DEAD",
    "Consonant_Placeholder": "GUNI_INSC_CONSONANT_PLACEHOLDER",
    "Consonant_Subjoined": "GUNI_INSC_CONSONANT_SUBJOINED",
    "Gemination_Mark": "GUNI_INSC_GEMINATION_MARK",
    "Modifying_Letter": "GUNI_INSC_MODIFYING_LETTER", "Nukta": "GUNI_INSC_NUKTA",
    "Number": "GUNI_INSC_NUMBER", "Tone_Mark": "GUNI_INSC_TONE_MARK",
    "Virama": "GUNI_INSC_VIRAMA", "Vowel_Dependent": "GUNI_INSC_VOWEL_DEPENDENT",
    "Vowel_Independent": "GUNI_INSC_VOWEL_INDEPENDENT",
    "Consonant_Final_Modifier": "GFNT_USE_ISC_FINAL_MODIFIER",
    "Symbol_Modifier": "GFNT_USE_ISC_SYMBOL_MODIFIER",
    "Hieroglyph": "GFNT_USE_ISC_HIEROGLYPH",
    "Hieroglyph_Joiner": "GFNT_USE_ISC_HIEROGLYPH_JOINER",
    "Hieroglyph_Mark_Begin": "GFNT_USE_ISC_HIEROGLYPH_SEGMENT_BEGIN",
    "Hieroglyph_Segment_Begin": "GFNT_USE_ISC_HIEROGLYPH_SEGMENT_BEGIN",
    "Hieroglyph_Mark_End": "GFNT_USE_ISC_HIEROGLYPH_SEGMENT_END",
    "Hieroglyph_Segment_End": "GFNT_USE_ISC_HIEROGLYPH_SEGMENT_END",
    "Hieroglyph_Mirror": "GFNT_USE_ISC_HIEROGLYPH_MIRROR",
    "Hieroglyph_Modifier": "GFNT_USE_ISC_HIEROGLYPH_MODIFIER",
}
IPC = {
    "Bottom": "GUNI_INPC_BOTTOM", "Left": "GUNI_INPC_LEFT", "NA": "GUNI_INPC_NA",
    "Overstruck": "GUNI_INPC_OVERSTRUCK", "Right": "GUNI_INPC_RIGHT",
    "Top": "GUNI_INPC_TOP", "Top_And_Right": "GUNI_INPC_TOP_AND_RIGHT",
}


def parse(path, names):
    cps = {}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if ";" not in line:
            continue
        span, value = [x.strip() for x in line.split(";", 1)]
        if value not in names:
            sys.stderr.write("unknown value %r in %s\n" % (value, path))
            sys.exit(1)
        lo, _, hi = span.partition("..")
        lo = int(lo, 16)
        hi = int(hi, 16) if hi else lo
        for cp in range(lo, hi + 1):
            cps[cp] = names[value]
    return cps


def ranges(cps):
    out = []
    for cp in sorted(cps):
        if out and out[-1][1] == cp - 1 and out[-1][2] == cps[cp]:
            out[-1][1] = cp
        else:
            out.append([cp, cp, cps[cp]])
    return out


def main(argv):
    isc = ranges(parse(argv[1], ISC))
    ipc = ranges(parse(argv[2], IPC))
    out = [NOTICE, LICENSE, "",
           "/* Generated by tools/gen/use_data.py. Do not edit. */", "",
           "#ifndef GHOTI_IO_GFNT_USE_DATA_H", "#define GHOTI_IO_GFNT_USE_DATA_H", "",
           "#include <ghoti.io/font/macros.h>", "#include <stdint.h>", "",
           "/* Categories the Unicode database has no name for. */",
           "enum {", "  GFNT_USE_ISC_FINAL_MODIFIER = 100,",
           "  GFNT_USE_ISC_SYMBOL_MODIFIER,", "  GFNT_USE_ISC_HIEROGLYPH,",
           "  GFNT_USE_ISC_HIEROGLYPH_JOINER,",
           "  GFNT_USE_ISC_HIEROGLYPH_SEGMENT_BEGIN,",
           "  GFNT_USE_ISC_HIEROGLYPH_SEGMENT_END,",
           "  GFNT_USE_ISC_HIEROGLYPH_MIRROR,",
           "  GFNT_USE_ISC_HIEROGLYPH_MODIFIER", "};", "",
           "typedef struct GFNT_UseRange {", "  uint32_t first;",
           "  uint32_t last;", "  uint16_t value;", "} GFNT_UseRange;", ""]
    for name, table in (("isc", isc), ("ipc", ipc)):
        out.append("static const GFNT_UseRange gfnt_use_%s_overrides[] = {" % name)
        for lo, hi, value in table:
            out.append("  {0x%X, 0x%X, %s}," % (lo, hi, value))
        out.append("};")
        out.append("#define GFNT_USE_%s_OVERRIDE_COUNT %d" % (name.upper(), len(table)))
        out.append("")
    out.append("#endif")
    print("\n".join(out))


main(sys.argv)
