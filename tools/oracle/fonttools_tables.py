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
"""Every field this library claims to parse, as fontTools reads it.

    fonttools_tables.py <font> <face-index>

Runs **inside the fonttools image**. Output is `key<TAB>value`, one per line, in
the vocabulary `tools/oracle/ttx_diff.py` compares - so the mapping from
fontTools' names to this library's happens once, here, beside the reference.

Two deliberate choices. It reads the table *objects* rather than `ttx`'s XML,
because the XML writes `created` as a date string and this library reports
seconds since 1904: comparing those would need a second date implementation,
which is the thing an oracle exists to avoid. And where fontTools has renamed a
field across versions - `hhea.ascent` and `hhea.ascender` both exist in the
wild - the candidate names are listed and the first that answers is used, so a
fontTools upgrade that renames something fails loudly on a missing field rather
than quietly comparing nothing.
"""

import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.O_S_2f_2 import panoseFormat

# canonical key -> the attribute names fontTools might use for it.
FIELDS = {
    "head": {
        "unitsPerEm": ("unitsPerEm",),
        "flags": ("flags",),
        "macStyle": ("macStyle",),
        "lowestRecPPEM": ("lowestRecPPEM",),
        "indexToLocFormat": ("indexToLocFormat",),
        "glyphDataFormat": ("glyphDataFormat",),
        "fontDirectionHint": ("fontDirectionHint",),
        "checkSumAdjustment": ("checkSumAdjustment",),
        "created": ("created",),
        "modified": ("modified",),
        "xMin": ("xMin",),
        "yMin": ("yMin",),
        "xMax": ("xMax",),
        "yMax": ("yMax",),
        "fontRevision": ("fontRevision",),
    },
    "hhea": {
        "ascender": ("ascender", "ascent"),
        "descender": ("descender", "descent"),
        "lineGap": ("lineGap",),
        "advanceWidthMax": ("advanceWidthMax",),
        "minLeftSideBearing": ("minLeftSideBearing",),
        "minRightSideBearing": ("minRightSideBearing",),
        "xMaxExtent": ("xMaxExtent",),
        "caretSlopeRise": ("caretSlopeRise",),
        "caretSlopeRun": ("caretSlopeRun",),
        "caretOffset": ("caretOffset",),
        "numberOfHMetrics": ("numberOfHMetrics",),
    },
    "OS/2": {
        "version": ("version",),
        "weightClass": ("usWeightClass",),
        "widthClass": ("usWidthClass",),
        "fsType": ("fsType",),
        "fsSelection": ("fsSelection",),
        "typoAscender": ("sTypoAscender",),
        "typoDescender": ("sTypoDescender",),
        "typoLineGap": ("sTypoLineGap",),
        "winAscent": ("usWinAscent",),
        "winDescent": ("usWinDescent",),
        "xAvgCharWidth": ("xAvgCharWidth",),
        "firstCharIndex": ("usFirstCharIndex",),
        "lastCharIndex": ("usLastCharIndex",),
        "xHeight": ("sxHeight",),
        "capHeight": ("sCapHeight",),
        "vendorID": ("achVendID",),
        # Everything below was parsed by this library, printed by its dump, and
        # compared by nothing. The fixtures are what exposed it: `os2-v5.ttf`
        # was built for the optical point sizes and this differential reported
        # it clean, because `usLowerOpticalPointSize` appeared in neither table.
        # A field the reference never emits is a field the gate cannot score,
        # and an unscored field reads exactly like an agreeing one.
        "subscriptXSize": ("ySubscriptXSize",),
        "subscriptYSize": ("ySubscriptYSize",),
        "subscriptXOffset": ("ySubscriptXOffset",),
        "subscriptYOffset": ("ySubscriptYOffset",),
        "superscriptXSize": ("ySuperscriptXSize",),
        "superscriptYSize": ("ySuperscriptYSize",),
        "superscriptXOffset": ("ySuperscriptXOffset",),
        "superscriptYOffset": ("ySuperscriptYOffset",),
        "strikeoutSize": ("yStrikeoutSize",),
        "strikeoutPosition": ("yStrikeoutPosition",),
        "familyClass": ("sFamilyClass",),
        "unicodeRange1": ("ulUnicodeRange1",),
        "unicodeRange2": ("ulUnicodeRange2",),
        "unicodeRange3": ("ulUnicodeRange3",),
        "unicodeRange4": ("ulUnicodeRange4",),
        "codePageRange1": ("ulCodePageRange1",),
        "codePageRange2": ("ulCodePageRange2",),
        "defaultChar": ("usDefaultChar",),
        "breakChar": ("usBreakChar",),
        "maxContext": ("usMaxContext",),
        "lowerOpticalPointSize": ("usLowerOpticalPointSize",),
        "upperOpticalPointSize": ("usUpperOpticalPointSize",),
        "panose": ("panose",),
    },
    "post": {
        "version": ("formatType",),
        "italicAngle": ("italicAngle",),
        "underlinePosition": ("underlinePosition",),
        "underlineThickness": ("underlineThickness",),
        "isFixedPitch": ("isFixedPitch",),
    },
    "maxp": {
        "numGlyphs": ("numGlyphs",),
    },
}


def escape(text):
    """The same escaping this library's name dump applies."""
    out = []
    for character in text:
        code = ord(character)
        if character == "\\":
            out.append("\\\\")
        elif character == "\n":
            out.append("\\n")
        elif character == "\r":
            out.append("\\r")
        elif character == "\t":
            out.append("\\t")
        elif code < 0x20 or code == 0x7F:
            out.append("\\x%02X" % code)
        else:
            out.append(character)
    return "".join(out)


# The ten PANOSE members, in the order the specification gives them - taken
# from fontTools' own struct description rather than retyped here.
#
# The first version did retype them, spelled the eighth "bLetterform", and used
# `getattr(panose, name, 0)`. fontTools spells it "bLetterForm", so the default
# turned the typo into a zero and the differential reported 262 disagreements
# against this library on a field where the library was right. A default on a
# lookup that should never miss is how a broken instrument produces confident
# numbers; there is no default now, and a name that stops existing raises.
PANOSE_ORDER = tuple(
    line.split(":")[0].strip()
    for line in panoseFormat.strip().splitlines() if ":" in line)
assert len(PANOSE_ORDER) == 10, PANOSE_ORDER


def render(key, value):
    """One field as the differential compares it.

    Two of these are not the plain attribute:

      * `panose` is an object in fontTools and ten bytes in the file. Its ten
        members are emitted in the order the specification gives them, which is
        the order this library's dump prints them.
      * the optical point sizes are stored in the file as twentieths of a point
        and fontTools exposes them **in points**, dividing on the way out and
        multiplying on the way in. This library reports what the file holds, so
        the reference's value is multiplied back. Getting this wrong in either
        direction is a factor of twenty, which no corpus font could have caught:
        there is no OS/2 version 5 in it.
    """
    if key == "panose":
        return " ".join(str(getattr(value, name)) for name in PANOSE_ORDER)
    if key in ("lowerOpticalPointSize", "upperOpticalPointSize"):
        return str(round(float(value) * 20))
    return str(value)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: fonttools_tables.py <font> <face-index>\n")
        return 2
    path, face = argv[1], int(argv[2])
    font = TTFont(path, fontNumber=face, lazy=True)
    out = sys.stdout

    # The directory, from the reader rather than from a re-serialisation: these
    # are the offsets and lengths as the file has them.
    for tag, entry in sorted(font.reader.tables.items()):
        name = tag.decode("latin1") if isinstance(tag, bytes) else tag
        out.write("directory.%s\t%d %d %d\n"
                  % (name, entry.offset, entry.length, entry.checkSum))

    for table, fields in FIELDS.items():
        if table not in font:
            out.write("absent.%s\t1\n" % table)
            continue
        parsed = font[table]
        for key, candidates in fields.items():
            for candidate in candidates:
                if hasattr(parsed, candidate):
                    out.write("%s.%s\t%s\n"
                              % (table, key,
                                 render(key, getattr(parsed, candidate))))
                    break
            else:
                # Not an error: OS/2 version 1 has no sxHeight, and a field a
                # version does not carry is absent rather than wrong. The
                # differential knows which fields are version-gated.
                out.write("missing.%s.%s\t1\n" % (table, key))

    if "cmap" in font:
        for index, subtable in enumerate(font["cmap"].tables):
            out.write("cmap.subtable.%d\t%d %d %d\n"
                      % (index, subtable.platformID, subtable.platEncID,
                         subtable.format))

    # The `post` glyph names, but only where the font actually has them:
    # fontTools *invents* names ("glyph00012") for a format 3.0 font, and
    # emitting those would score this library's honest refusal as a
    # disagreement with a name nobody wrote.
    if "post" in font and font["post"].formatType in (1.0, 2.0):
        order = font.getGlyphOrder()
        for index, name in enumerate(order):
            out.write("glyphname.%d\t%s\n" % (index, escape(name)))
    elif "post" in font:
        out.write("glyphnames.absent\t1\n")

    if "name" in font:
        for record in font["name"].names:
            key = "name.%d.%d.%d.%d" % (record.platformID, record.platEncID,
                                        record.langID, record.nameID)
            try:
                text = record.toUnicode()
            except UnicodeDecodeError:
                # fontTools cannot decode it either. Reported rather than
                # skipped, so that a differential can tell "the reference
                # declined" from "the reference disagreed".
                out.write("undecodable.%s\t1\n" % key)
                continue
            out.write("%s\t%s\n" % (key, escape(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
