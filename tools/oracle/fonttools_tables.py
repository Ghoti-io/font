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

import logging
import re
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


# fontTools' own warning when it decides a stored `head` date is implausible.
# `Konatu.ttf` stores a `created` and a `modified` of **zero**, and fontTools'
# `_h_e_a_d` decode decides that is too low to be seconds since 1904 and rewrites
# it as though it were a Unix timestamp, reporting 2082840000 where the eight
# bytes in the file are all zero. That is a sanitisation and not a reading, so the
# differential must not score it against this library - and the honest way to
# exempt it is to let the reference say it happened, which it does, out loud, on
# every decode.
SANITISED = re.compile(r"'(?P<field>\w+)' timestamp seems very low")

# fontTools' own warning when it throws a `post` table's answer away and makes the
# names up instead. `mona.ttf` states `post` **format 1.0** - "the glyph set is the
# standard Macintosh ordering", which is 258 names - and then carries 7,225 glyphs,
# so the table contradicts itself. fontTools discards the standard order entirely
# and derives a name for every glyph from the `cmap`, giving `space` at index 1
# where the standard order has `.null` and `uni6687` at 1000 where the table says
# nothing at all. This library reports the standard order for the 258 the format
# does name and `unreadable` past them, which is also what FreeType's
# `tt_face_get_ps_name` does.
#
# Generated names are not a reading of the font, so they are not compared - and the
# reference announces it, once per font, which is what makes this an allowance that
# expires with the warning rather than a list of fonts to remember.
GENERATED_NAMES = re.compile(r"Not enough names found in the 'post' table")


class Sanitisations(logging.Handler):
    """Record which `head` fields fontTools rewrote rather than read.

    A handler rather than a comparison of values here, for the same reason the
    FreeType driver prints `census unanswered` lines instead of `eblc_diff`
    keeping a list: an exemption that is derived from the reference's own
    statement stops applying the moment the reference stops making it. A list of
    "fields fontTools is known to fudge" would have to be remembered, and would go
    on excusing a real disagreement after a fontTools release fixed the fudge.
    """

    def __init__(self):
        super().__init__()
        self.fields = set()
        self.generated_names = False

    def emit(self, record):
        message = record.getMessage()
        found = SANITISED.match(message)
        if found:
            self.fields.add(found.group("field"))
        if GENERATED_NAMES.match(message):
            self.generated_names = True


def readable(font, table, out):
    """`font[table]`, or None and an `unreadable.` line if it cannot be decoded.

    **A table fontTools cannot decompile must not take the whole font with it.**
    `lazy=True` means a table is decoded where it is first touched, and an
    exception there propagates out of this program, which the differential reads
    as "the reference refused this font" - so every other table of it goes
    uncompared, silently, behind a clean total. `mona.ttf` is the case: its `OS/2`
    says version 2 and is 86 bytes, the length of a version 1 table, and fontTools
    raises `struct.error` reading the version 2 addition. Every one of that font's
    other tables was dropped from this gate because of it.

    The exception's type is printed rather than swallowed. It is the reference's
    own statement that it cannot read the table, which is a fact the differential
    can compare against this library's refusal of the same table - and unlike a
    skip, it is a fact about one *table*.
    """
    try:
        return font[table]
    except Exception as why:  # noqa: BLE001 - the reason is the output
        out.write("unreadable.%s\t%s\n" % (table, type(why).__name__))
        return None


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: fonttools_tables.py <font> <face-index>\n")
        return 2
    path, face = argv[1], int(argv[2])
    # Installed before anything decompiles, because `lazy=True` means `head` is
    # read inside the field loop below.
    sanitised = Sanitisations()
    logging.getLogger("fontTools").addHandler(sanitised)
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
        parsed = readable(font, table, out)
        if parsed is None:
            continue
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
        cmap = readable(font, "cmap", out)
        for index, subtable in enumerate(cmap.tables if cmap else ()):
            out.write("cmap.subtable.%d\t%d %d %d\n"
                      % (index, subtable.platformID, subtable.platEncID,
                         subtable.format))

    # The design space, in the same integers the library's dump prints: 16.16 for
    # an axis and 2.14 for a segment map. fontTools holds both as floats, and
    # rounding them here rather than in the comparison keeps the conversion in one
    # place - and a float of a 16.16 value is exact, so nothing is lost by it.
    if "fvar" in font:
        fvar = readable(font, "fvar", out)
        if fvar is not None:
            out.write("fvar.axes\t%d\n" % len(fvar.axes))
            out.write("fvar.instances\t%d\n" % len(fvar.instances))
            for index, axis in enumerate(fvar.axes):
                out.write("fvar.axis.%d\t%s %d %d %d %d %d\n"
                          % (index, axis.axisTag, round(axis.minValue * 65536),
                             round(axis.defaultValue * 65536),
                             round(axis.maxValue * 65536), axis.flags,
                             axis.axisNameID))
            for index, instance in enumerate(fvar.instances):
                coordinates = " ".join(
                    str(round(instance.coordinates[axis.axisTag] * 65536))
                    for axis in fvar.axes)
                out.write("fvar.instance.%d\t%d %d %d %s\n"
                          % (index, instance.subfamilyNameID, instance.flags,
                             instance.postscriptNameID, coordinates))
            if "avar" in font:
                avar = readable(font, "avar", out)
                for index, axis in enumerate(fvar.axes if avar else ()):
                    segments = avar.segments.get(axis.axisTag, {})
                    out.write("avar.axis.%d\t%s\n"
                              % (index, " ".join(
                                  "%d:%d" % (round(low * 16384),
                                             round(high * 16384))
                                  for low, high in sorted(segments.items()))))

    # Glyph names, from whichever table actually holds them - and where neither
    # does, said rather than invented: fontTools makes up "glyph00012" for a
    # TrueType font with `post` format 3.0, and emitting that would score this
    # library's honest refusal as a disagreement with a name nobody wrote.
    #
    # **A CFF font names its glyphs in its charset**, and `getGlyphOrder()`
    # returns those rather than invented ones - which is why the condition is not
    # just `post`. It was until phase 2, when this library started reading the
    # charset: the reference then reported "no names" for a font whose names it
    # was itself reading, and 42 of them came out as disagreements.
    top = None
    if "CFF " in font:
        table = readable(font, "CFF ", out)
        if table is not None:
            cff = table.cff
            top = cff[cff.fontNames[0]]
    if top is not None and hasattr(top, "ROS"):
        # A CID-keyed font's charset holds CIDs, and a CID is not a name:
        # fontTools invents `cidNNNNN` for them. The one name the format does give
        # is glyph 0's, which is `.notdef` in every CFF - so that, and then
        # nothing.
        out.write("glyphname.0\t.notdef\n")
        out.write("glyphnames.absent\t1\n")
    else:
        # `post` through readable() for the same reason as every other table: a
        # `post` fontTools cannot decompile would otherwise end the program here,
        # after the fields above had already been printed, and a differential
        # reading a truncated dump compares what it got.
        post = readable(font, "post", out) if "post" in font else None
        if top is not None or (post is not None
                and post.formatType in (1.0, 2.0)):
            order = font.getGlyphOrder()
            for index, name in enumerate(order):
                out.write("glyphname.%d\t%s\n" % (index, escape(name)))
        elif post is not None:
            out.write("glyphnames.absent\t1\n")

    name_table = readable(font, "name", out) if "name" in font else None
    if name_table is not None:
        for record in name_table.names:
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

    # Last, so that everything that could trigger one has been decoded. One line
    # per field the reference rewrote rather than read; `ttx_diff.py` skips exactly
    # these and counts them.
    for field in sorted(sanitised.fields):
        out.write("sanitised.head.%s\t1\n" % field)
    if sanitised.generated_names:
        out.write("generated.glyphnames\t1\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
