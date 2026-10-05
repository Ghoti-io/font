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
"""Every table's fields, this library against fontTools, over the real corpus.

documentation/design.md section 14: "every table of every synthetic fixture and
every real font in the image dumped and compared field by field with this
library's `_dump`". This is that gate, and it compares the dumps rather than
reaching into the library, because the dump is what a person reads when a font
misbehaves - a differential that bypassed it would leave the dumps unchecked and
compare a second code path nobody uses.

Both sides speak the same vocabulary: `fonttools_tables.py` maps fontTools'
names onto it inside the image, and the parsers below map this library's dump
lines onto it here. A field the dump stops printing therefore fails as a missing
key rather than as a silently smaller comparison - which is the failure mode a
text-parsing differential has to be built against.

Four values need converting rather than comparing, and each is a real format
difference rather than a fudge:

  * `head.fontRevision` and `post.italicAngle` are 16.16 fixed point here and
    floats there, so they are compared to within one unit of the fixed-point
    type.
  * `post.version` is 0x00020000 here and 2.0 there.
  * `OS/2.vendorID` is sanitised for printing here - a tag is four arbitrary
    bytes and a dump must survive a terminal - so the reference's copy is
    sanitised the same way before comparing.

A field a version does not carry is not a disagreement: the reference says
`missing.OS/2.xHeight` for an OS/2 version 1, and this library prints zero. The
zero is checked *because* of that: a version-gated field that came out non-zero
would mean something was read from the wrong offset.

Usage:
    ttx_diff.py [--fonts N] [--quiet] [font...]

`--fonts N` thins the corpus deterministically and is a smoke test, not a run:
this gate's own output says which table versions it met, because a planted defect
that only affects OS/2 version 1 passed a four-font sample of a corpus whose
version-1 fonts are eight of three hundred and twenty-seven.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env
import unskippable

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-dump")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_tables.py")

PER_FONT = 6
TOTAL = 60

# Our dump's lines, as (pattern, [keys]). A key of None discards that group.
PATTERNS = [
    (r"^table '(.{1,4})': offset (\d+), length (\d+), checksum 0x([0-9A-F]+)$",
     "directory"),
    # The dump's own per-table refusal, read rather than inferred from the absence
    # of that table's fields. The result's name distinguishes "the font has no
    # such table" from "it has one and this library would not read it", and the
    # second is the half that has to line up with the reference's `unreadable.`.
    (r"^(head|hhea|OS/2|post|fvar|avar): absent \((.*)\)$", "declined"),
    (r"^head: version \d+\.\d+, revision 0x([0-9A-F]+), unitsPerEm (\d+), "
     r"flags 0x([0-9A-F]+)$",
     ["head.fontRevision", "head.unitsPerEm", "head.flags"]),
    (r"^head: bbox (-?\d+) (-?\d+) (-?\d+) (-?\d+), macStyle 0x([0-9A-F]+)$",
     ["head.xMin", "head.yMin", "head.xMax", "head.yMax", "head.macStyle"]),
    (r"^head: created (-?\d+), modified (-?\d+), lowestRecPPEM (\d+)$",
     ["head.created", "head.modified", "head.lowestRecPPEM"]),
    (r"^head: indexToLocFormat (-?\d+), glyphDataFormat (-?\d+), "
     r"fontDirectionHint (-?\d+), checkSumAdjustment 0x([0-9A-F]+)$",
     ["head.indexToLocFormat", "head.glyphDataFormat",
      "head.fontDirectionHint", "head.checkSumAdjustment"]),
    (r"^hhea: version \d+\.\d+, ascender (-?\d+), descender (-?\d+), "
     r"lineGap (-?\d+)$",
     ["hhea.ascender", "hhea.descender", "hhea.lineGap"]),
    (r"^hhea: advanceWidthMax (\d+), minLSB (-?\d+), minRSB (-?\d+), "
     r"xMaxExtent (-?\d+)$",
     ["hhea.advanceWidthMax", "hhea.minLeftSideBearing",
      "hhea.minRightSideBearing", "hhea.xMaxExtent"]),
    (r"^hhea: caretSlope (-?\d+)/(-?\d+), caretOffset (-?\d+), "
     r"numberOfHMetrics (\d+)$",
     ["hhea.caretSlopeRise", "hhea.caretSlopeRun", "hhea.caretOffset",
      "hhea.numberOfHMetrics"]),
    (r"^OS/2: version (\d+), weightClass (\d+), widthClass (\d+), "
     r"fsType 0x([0-9A-F]+), fsSelection 0x([0-9A-F]+)$",
     ["OS/2.version", "OS/2.weightClass", "OS/2.widthClass", "OS/2.fsType",
      "OS/2.fsSelection"]),
    (r"^OS/2: typo (-?\d+)/(-?\d+)/(-?\d+), win (\d+)/(\d+), "
     r"xAvgCharWidth (-?\d+), vendor '(.{0,4})'$",
     ["OS/2.typoAscender", "OS/2.typoDescender", "OS/2.typoLineGap",
      "OS/2.winAscent", "OS/2.winDescent", "OS/2.xAvgCharWidth",
      "OS/2.vendorID"]),
    (r"^OS/2: firstCharIndex (\d+), lastCharIndex (\d+), xHeight (-?\d+), "
     r"capHeight (-?\d+)$",
     ["OS/2.firstCharIndex", "OS/2.lastCharIndex", "OS/2.xHeight",
      "OS/2.capHeight"]),
    # The four lines below were printed by the dump and read by nothing until
    # the fixtures arrived. See FIELDS in fonttools_tables.py.
    (r"^OS/2: subscript (-?\d+)/(-?\d+)/(-?\d+)/(-?\d+), "
     r"superscript (-?\d+)/(-?\d+)/(-?\d+)/(-?\d+), "
     r"strikeout (-?\d+)/(-?\d+)$",
     ["OS/2.subscriptXSize", "OS/2.subscriptYSize", "OS/2.subscriptXOffset",
      "OS/2.subscriptYOffset", "OS/2.superscriptXSize",
      "OS/2.superscriptYSize", "OS/2.superscriptXOffset",
      "OS/2.superscriptYOffset", "OS/2.strikeoutSize",
      "OS/2.strikeoutPosition"]),
    (r"^OS/2: familyClass (-?\d+), panose ((?:\d+ ){9}\d+)$",
     ["OS/2.familyClass", "OS/2.panose"]),
    (r"^OS/2: unicodeRange 0x([0-9A-F]+) 0x([0-9A-F]+) 0x([0-9A-F]+) "
     r"0x([0-9A-F]+), codePageRange 0x([0-9A-F]+) 0x([0-9A-F]+)$",
     ["OS/2.unicodeRange1", "OS/2.unicodeRange2", "OS/2.unicodeRange3",
      "OS/2.unicodeRange4", "OS/2.codePageRange1", "OS/2.codePageRange2"]),
    (r"^OS/2: defaultChar (\d+), breakChar (\d+), maxContext (\d+), "
     r"opticalSize (\d+)\.\.(\d+)$",
     ["OS/2.defaultChar", "OS/2.breakChar", "OS/2.maxContext",
      "OS/2.lowerOpticalPointSize", "OS/2.upperOpticalPointSize"]),
    (r"^post: version 0x([0-9A-F]+), italicAngle 0x([0-9A-F]+), "
     r"isFixedPitch (\d+)$",
     ["post.version", "post.italicAngle", "post.isFixedPitch"]),
    (r"^post: underlinePosition (-?\d+), underlineThickness (-?\d+)$",
     ["post.underlinePosition", "post.underlineThickness"]),
    (r"^numGlyphs: (\d+) maxp (\d+)$", ["numGlyphs.minimum", "maxp.numGlyphs"]),
    (r"^cmap subtable (\d+): platform (\d+), encoding (\d+), format (\d+), "
     r"offset \d+$", "cmap"),
    # The design space. Printed in the tables' own integers - 16.16 for an axis
    # and 2.14 for a segment map - because the reference is converted to them too,
    # and a comparison in floats would be agreement to within a tolerance that
    # nobody chose.
    (r"^fvar: (\d+) axes, (\d+) instances$", "fvar-count"),
    (r"^fvar axis (\d+): '(.{4})' min (-?\d+) default (-?\d+) max (-?\d+) "
     r"flags 0x([0-9A-F]+) name (\d+)$", "fvar-axis"),
    (r"^fvar instance (\d+): name (\d+) flags 0x([0-9A-F]+) postscript (\d+) "
     r"coordinates ?(.*)$", "fvar-instance"),
    (r"^avar axis (\d+): \d+ pairs ?(.*)$", "avar-axis"),
    (r"^glyph name (\d+): '(.*)'$", "glyphname"),
    (r"^glyph names: none in this font$", "glyphnames-absent"),
    (r"^name record \d+: platform (\d+), encoding (\d+), language (\d+), "
     r"name (\d+): '(.*)'$", "name"),
    (r"^name record \d+: platform (\d+), encoding (\d+), language (\d+), "
     r"name (\d+), \d+ bytes, not decodable$", "name-undecodable"),
    # The order matters: the decodable pattern above would also match a
    # not-decodable line if that line ended in a quote, which it does not - but
    # the pair is listed adjacently so the next person can see the pairing.
]
PATTERNS = [(re.compile(pattern), keys) for pattern, keys in PATTERNS]

# 16.16 fixed point, as a float, to within one unit of the type.
FIXED = 1.0 / 65536.0


def sanitise(tag):
    """What this library's dumps print for four arbitrary bytes."""
    return "".join(c if 0x20 <= ord(c) < 0x7F else "." for c in tag)


def parse_ours(text):
    """Our dump, as {key: value}, the keys it would not decode, and the tables it
    would not read at all (by the result it reported for each)."""
    values = {}
    undecodable = set()
    declined = {}
    for line in text.splitlines():
        for pattern, keys in PATTERNS:
            found = pattern.match(line)
            if not found:
                continue
            if keys == "directory":
                tag, offset, length, checksum = found.groups()
                values["directory.%s" % tag] = "%d %d %d" % (
                    int(offset), int(length), int(checksum, 16))
            elif keys == "cmap":
                index, platform, encoding, fmt = found.groups()
                values["cmap.subtable.%d" % int(index)] = "%d %d %d" % (
                    int(platform), int(encoding), int(fmt))
            elif keys == "name":
                platform, encoding, language, name_id, value = found.groups()
                values["name.%d.%d.%d.%d" % (int(platform), int(encoding),
                                             int(language), int(name_id))] = value
            elif keys == "fvar-count":
                axes, instances = found.groups()
                values["fvar.axes"] = axes
                values["fvar.instances"] = instances
            elif keys == "fvar-axis":
                index, tag, low, default, high, flags, name = found.groups()
                values["fvar.axis.%d" % int(index)] = "%s %s %s %s %d %s" % (
                    tag, low, default, high, int(flags, 16), name)
            elif keys == "fvar-instance":
                index, name, flags, postscript, coordinates = found.groups()
                values["fvar.instance.%d" % int(index)] = "%s %d %s %s" % (
                    name, int(flags, 16), postscript, coordinates)
            elif keys == "avar-axis":
                index, pairs = found.groups()
                values["avar.axis.%d" % int(index)] = pairs
            elif keys == "glyphname":
                index, value = found.groups()
                values["glyphname.%d" % int(index)] = value
            elif keys == "declined":
                table, why = found.groups()
                declined[table] = why
            elif keys == "glyphnames-absent":
                values["glyphnames.absent"] = "1"
            elif keys == "name-undecodable":
                platform, encoding, language, name_id = found.groups()
                undecodable.add("name.%d.%d.%d.%d"
                                % (int(platform), int(encoding), int(language),
                                   int(name_id)))
            else:
                for key, value in zip(keys, found.groups()):
                    values[key] = value
            break
    return values, undecodable, declined


def parse_reference(text):
    """The reference's lines, as {key: value} plus what it declined to answer."""
    values = {}
    absent = set()
    undecodable = 0
    # Fields the reference says it **rewrote rather than read**. See the adapter:
    # fontTools decides a `head` date of zero is implausible and reports it as
    # though it had been a Unix timestamp, which is a sanitisation and not a
    # reading of the font.
    sanitised = set()
    # Tables the reference says it could not decompile, by the exception it raised.
    # One table and not the whole font: see readable() in the adapter.
    unreadable = {}
    # Whether the reference announced that it *generated* the glyph names rather
    # than reading them, which it does when `post` does not name every glyph.
    generated_names = False
    for line in text.splitlines():
        key, _, value = line.partition("\t")
        if key.startswith("missing.") or key.startswith("absent."):
            absent.add(key.split(".", 1)[1])
        elif key.startswith("undecodable."):
            undecodable += 1
        elif key == "generated.glyphnames":
            generated_names = True
        elif key.startswith("unreadable."):
            unreadable[key.split(".", 1)[1]] = value
        elif key.startswith("sanitised."):
            sanitised.add(key.split(".", 1)[1])
        else:
            values[key] = value
    return values, absent, undecodable, sanitised, unreadable, generated_names


def agree(key, ours, theirs):
    """Whether the two values say the same thing, converting where they must."""
    if key in ("head.fontRevision", "post.italicAngle"):
        mine = int(ours, 16)
        if mine >= 0x80000000:
            mine -= 0x100000000
        return abs(mine / 65536.0 - float(theirs)) <= FIXED
    if key == "post.version":
        return abs(int(ours, 16) / 65536.0 - float(theirs)) <= FIXED
    if key == "OS/2.vendorID":
        return ours == sanitise(theirs)
    if key == "OS/2.panose":
        return ours.split() == theirs.split()
    if key.startswith(("OS/2.unicodeRange", "OS/2.codePageRange")):
        return int(ours, 16) == int(float(theirs))
    if key.startswith(("name.", "glyphname.")):
        return ours == theirs
    if key.startswith(("directory.", "cmap.subtable.", "fvar.", "avar.")):
        return ours == theirs
    # Everything else is an integer on both sides; the reference prints some of
    # them as floats where fontTools stores them that way.
    try:
        return int(ours, 16 if key.endswith(("flags", "macStyle", "fsType",
                                             "fsSelection",
                                             "checkSumAdjustment")) else 10) \
            == int(float(theirs))
    except ValueError:
        return ours == theirs


# Macintosh platEncID values this library refuses: the multi-byte CJK encodings,
# which are a data set of their own rather than a 128-entry table.
MAC_MULTIBYTE = {1, 2, 3, 25}

# Microsoft platEncID values that are not UTF-16BE, and are therefore multi-byte
# legacy encodings this library refuses for the same reason.
MICROSOFT_MULTIBYTE = {2, 3, 4, 5, 6}


UNIQUE_SUFFIX = re.compile(r"^(?P<base>.+)\.(?P<ordinal>\d+)$")


def uniquified_glyph_name(key, ours, mine, theirs):
    """Whether the reference renamed a duplicate glyph name and we did not.

    Four Liberation faces name two different glyphs `uni00AD` in the same `post`
    table - verified in the raw bytes, at indices 111 and 2578. fontTools' glyph
    order has to be a set of unique keys, so its `post` decode renames the
    second to `uni00AD.1`. This library reports what the file says, which is what
    a caller asking "what is this glyph called" should get.

    **Checked, not excused**, and the third clause is the check: the reference's
    name must be ours plus a `.<digits>` suffix, *and* an earlier glyph must
    actually carry the bare name. A font that genuinely contains a glyph named
    `foo.1` which this library misread as `foo` fails the third clause and stays
    a disagreement - which is the difference between a known difference and an
    exclusion that absorbs what it was meant to expose.
    """
    if not key.startswith("glyphname."):
        return False
    found = UNIQUE_SUFFIX.match(theirs)
    if not found or found.group("base") != mine:
        return False
    index = int(key.split(".", 1)[1])
    # An *earlier* index, because that is the one the reference would have left
    # alone; a later duplicate does not explain a rename of this one.
    for other in range(index):
        if ours.get("glyphname.%d" % other) == mine:
            return True
    return False


INVENTED_NAME = re.compile(r"^glyph(?P<ordinal>\d{5})$")


def invented_glyph_name(key, mine, theirs):
    """Whether the reference invented a name for a glyph the file leaves unnamed.

    `UKIJMe.ttf` stores two **empty** Pascal strings in its `post` format 2.0 name
    array - verified in the raw bytes: glyph 800's name index is 820, which is
    stored name 562, whose length byte is zero. fontTools' glyph order has to be a
    set of unique non-empty keys, so it substitutes `glyph00800`. This library
    reports what the file says, which for "what is this glyph called" is the empty
    answer the file gives.

    **Narrowed rather than excused**, as `uniquified_glyph_name()` is: ours must be
    empty *and* the reference's must match its five-digit `glyph` spelling, *and*
    that ordinal must be
    this glyph's index - so a reference name that is any other string stays a
    disagreement.

    Two things that narrowing does **not** establish, both stated because the
    alternative is implying they are checked:

      * The **index clause is exercised by nothing.** Making it unconditional
        leaves every number in this gate identical, because no font in the corpus
        has the reference answering a synthetic name at some other glyph's index.
        It is a precaution, not a tested one; the clause that does the work here is
        the empty-name one.
      * A font that genuinely stores the literal name `glyph00800` for glyph 800
        while this library reads it as empty would be excused. Separating that
        needs the raw `post` bytes, which this function does not have.
    """
    if not key.startswith("glyphname.") or mine != "":
        return False
    found = INVENTED_NAME.match(theirs)
    if not found:
        return False
    return int(found.group("ordinal")) == int(key.split(".", 1)[1])


def refusal_is_documented(key):
    """Whether this library declining this record is a documented refusal.

    **The Mac Roman deferral is gone.** Until the vectors were generated this
    function excused every Macintosh record above ASCII, and the category had
    fourteen members across 344 faces. Those records are now decoded and scored
    like any other, so what is left is the genuinely unimplemented set: the
    multi-byte encodings.

    It stays checked rather than becoming an exclusion. A refusal of a
    single-byte Macintosh encoding - anything this library has a table for - is
    a disagreement, because that is the path the tables exist to serve. So is a
    refusal of a Unicode or Windows UTF-16BE record. Only an encoding ID from
    the two sets above may be declined.
    """
    parts = key.split(".")
    if len(parts) != 5 or parts[0] != "name":
        return False
    platform, encoding = int(parts[1]), int(parts[2])
    if platform == 1:
        return encoding in MAC_MULTIBYTE
    if platform == 3:
        return encoding in MICROSOFT_MULTIBYTE
    return False


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing.

    `expected` is carried for the same reason as in the other differentials, and
    here it is always false: every skip this gate can raise is a driver or a
    reference that failed, which is exactly what tools/oracle/unskippable.py
    refuses to let an sfnt leave the denominator for. `mona.ttf` used to be one of
    them, over a short `OS/2`, and that cost the gate every other field of that
    font until the adapter learned to fail per table.
    """

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def compare(path, face, report):
    """One font. Returns (keys compared, disagreements, counters)."""
    finished = subprocess.run([DRIVER, path, str(face)], capture_output=True,
        text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip() or "the driver refused the font")
    ours, our_undecodable, our_declined = parse_ours(finished.stdout)

    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face)])
    answered = subprocess.run(argv, capture_output=True, text=True)
    if answered.returncode != 0:
        tail = answered.stderr.strip().splitlines()
        raise Skip("fontTools: %s" % (tail[-1] if tail else "refused"))
    (theirs, absent, their_undecodable, sanitised, unreadable,
        generated_names) = parse_reference(answered.stdout)

    compared = 0
    disagreements = 0
    declined = 0
    shown = 0
    uniquified = 0
    rewritten = 0
    one_sided = 0
    invented = 0
    mac_pairs = set()

    # A table the reference could not decompile. **The agreement is asserted, not
    # assumed**: this library must have declined the same table, and declined it as
    # corrupt rather than as absent, because the table is in the directory. If it
    # read one the reference cannot, that is not a disagreement - our reading may
    # be the right one - but it is a field with a single reader, so it is counted
    # and named rather than left to look like agreement.
    #
    # `mona.ttf` is the whole of this category today: its `OS/2` states version 2
    # in 86 bytes, which is a version 1 length. fontTools raises reading the
    # version 2 addition; this library refuses the table and reads the rest of the
    # font. Until the adapter failed per table, that one short table took every
    # other table of this font out of the gate.
    for table, why in sorted(unreadable.items()):
        if table in our_declined:
            if "Corrupt" not in our_declined[table]:
                disagreements += 1
                sys.stderr.write(
                    "  %s: the reference could not decompile %s (%s) and this "
                    "library called it %r rather than corrupt\n"
                    % (os.path.basename(path), table, why, our_declined[table]))
            continue
        one_sided += 1
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write(
                "  %s: %s has one reader - the reference raised %s and this "
                "library read the table\n"
                % (os.path.basename(path), table, why))
            shown += 1
            report[0] += 1

    for key, value in sorted(theirs.items()):
        if generated_names and key.startswith("glyphname."):
            # The reference said, on its own stderr, that it made these up from the
            # `cmap` because `post` does not name every glyph. A generated name is
            # not a reading of the font. Counted, and the count is printed, so a
            # run that rests on this says how much it rests on it.
            invented += 1
            continue
        if key not in ours:
            if key in our_undecodable and refusal_is_documented(key):
                declined += 1
                continue
            # A key the reference has and the dump does not: either the dump
            # stopped printing a field, or this library did not parse a table
            # that fontTools did. Both are findings.
            disagreements += 1
            if shown < PER_FONT and report[0] < TOTAL:
                sys.stderr.write("  %s: %s: fontTools %r, the dump says "
                                 "nothing\n"
                                 % (os.path.basename(path), key, value))
                shown += 1
                report[0] += 1
            continue
        compared += 1
        if agree(key, ours[key], value):
            continue
        if uniquified_glyph_name(key, ours, ours[key], value):
            uniquified += 1
            continue
        if invented_glyph_name(key, ours[key], value):
            uniquified += 1
            continue
        if key in sanitised:
            # The reference said, on its own stderr, that it rewrote this field.
            # Not compared, and counted so the report says how many.
            rewritten += 1
            continue
        disagreements += 1
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write("  %s: %s: fontTools %r, this library %r\n"
                             % (os.path.basename(path), key, value, ours[key]))
            shown += 1
            report[0] += 1

    # A field the reference does not carry must be zero here, not garbage: the
    # version gate is the reason it is absent, and a non-zero value would mean
    # something was read from an offset the version does not define.
    for key in sorted(absent):
        if key in ours and ours[key] not in ("0", "0.0", "00000000"):
            disagreements += 1
            if shown < PER_FONT and report[0] < TOTAL:
                sys.stderr.write(
                    "  %s: %s: the reference does not carry it and this "
                    "library printed %r rather than zero\n"
                    % (os.path.basename(path), key, ours[key]))
                shown += 1
                report[0] += 1

    # The glyph count is this library's minimum (M12) and maxp's claim is the
    # reference's number; the minimum may be smaller and never larger.
    if "numGlyphs.minimum" in ours and "maxp.numGlyphs" in theirs:
        if int(ours["numGlyphs.minimum"]) > int(theirs["maxp.numGlyphs"]):
            disagreements += 1
            sys.stderr.write(
                "  %s: the glyph count %s exceeds maxp's %s\n"
                % (os.path.basename(path), ours["numGlyphs.minimum"],
                   theirs["maxp.numGlyphs"]))

    shape = {
        "OS/2 version": ours.get("OS/2.version"),
        "post version": ours.get("post.version"),
    }
    # Which Macintosh (encoding, language) pairs this face carried. All 658
    # Macintosh records in the real corpus are (0, Mac Roman), so without the
    # fixtures this axis has exactly one value and seven generated tables go
    # unexercised - the same shape as the OS/2 versions the corpus lacks.
    for key in ours:
        if key.startswith("name.1."):
            parts = key.split(".")
            if len(parts) == 5:
                shape.setdefault("Macintosh name (encoding, language)", None)
                mac_pairs.add("(%s,%s)" % (parts[2], parts[3]))
    return compared, disagreements, declined, their_undecodable, shape, \
        mac_pairs, uniquified, rewritten, one_sided, invented


FIXTURES = os.path.join(oracle_env.ROOT, "tests", "data", "fonts")


def fixtures():
    """The committed synthetic fixtures, as (path, face) pairs.

    They are in the population by default, and not as a convenience. The real
    corpus holds OS/2 versions 1, 3 and 4 only, and a planted defect that read
    version 2's fields out of a version 1 table survived a sample of it; the
    fixtures are where versions 0, 2 and 5 exist at all. Until they were
    generated this gate had no way to compare an OS/2 version 5 against
    anything, and the sentence it printed about that was the honest form of a
    gap rather than a measurement.

    No materialising step: the repository is mounted read-only at its own path
    inside the image, so both sides open the same file (design.md section 14.7).
    Every face of a collection is listed, because a `ttcf` whose second face is
    never asked is a shared-table arithmetic nobody checked.
    """
    if not os.path.isdir(FIXTURES):
        return []
    found = []
    for name in sorted(os.listdir(FIXTURES)):
        if not name.endswith((".ttf", ".otf", ".ttc")):
            continue
        path = os.path.join(FIXTURES, name)
        faces = 1
        if name.endswith(".ttc"):
            with open(path, "rb") as handle:
                handle.seek(8)
                faces = int.from_bytes(handle.read(4), "big")
        for face in range(faces):
            found.append((path, face))
    return found


def main(argv):
    quiet = "--quiet" in argv
    limit = None
    rest = []
    skip_next = False
    for index, argument in enumerate(argv[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if argument == "--fonts":
            limit = int(argv[index + 1])
            skip_next = True
        elif argument in ("--quiet", "--no-fixtures"):
            continue
        else:
            rest.append(argument)

    if not os.path.exists(DRIVER):
        sys.stderr.write("%s is not built; run `make examples`\n" % DRIVER)
        return 1

    if rest:
        fonts = [(path, 0) for path in rest]
        synthetic = 0
    else:
        fonts = [(path, 0) for path in corpus.fonts("sfnt")]
        if limit is not None:
            step = max(1, len(fonts) // limit)
            fonts = fonts[::step][:limit]
        # --fonts thins the *corpus*, never the fixtures: thinning is a smoke
        # test and the fixtures are the only cover for three OS/2 versions, so
        # dropping them is how a smoke test stops covering the arm a defect is
        # in. They are cheap - sixteen faces of about 1.7 kB.
        chosen = [] if "--no-fixtures" in argv else fixtures()
        synthetic = len(chosen)
        fonts = chosen + fonts
    if not fonts:
        sys.stderr.write("no fonts to compare\n")
        return 1

    compared = 0
    disagreements = 0
    declined = 0
    uniquified_names = 0
    theirs_declined = 0
    skipped = []
    report = [0]
    # What the run covered, printed below. A differential that does not say
    # which versions of a table it met is one whose clean result cannot be read:
    # a plant that only affects OS/2 version 1 passed a four-font sample of this
    # corpus, because the four were all version 4.
    coverage = {"OS/2 version": {}, "post version": {}}
    mac_coverage = {}
    sanitised_fields = 0
    single_reader = 0
    generated_names = 0

    for path, face in fonts:
        try:
            (keys, differed, refused, theirs, shape, pairs, renamed,
                rewritten, lonely, made_up) = compare(path, face, report)
        except Skip as why:
            skipped.append((path, str(why), why.expected))
            continue
        compared += keys
        disagreements += differed
        declined += refused
        uniquified_names += renamed
        sanitised_fields += rewritten
        single_reader += lonely
        generated_names += made_up
        theirs_declined += theirs
        for axis, value in shape.items():
            if value is not None:
                coverage[axis][value] = coverage[axis].get(value, 0) + 1
        for pair in pairs:
            mac_coverage[pair] = mac_coverage.get(pair, 0) + 1
        if not quiet:
            label = os.path.basename(path)
            if face:
                label += ":%d" % face
            sys.stdout.write("  %-52s %4d fields%s\n"
                % (label, keys,
                   ", disagreements: %d" % differed if differed else ""))

    print("ttx_diff: %d faces (%d synthetic fixtures, %d real fonts), %d "
          "fields compared, %d disagreements"
          % (len(fonts) - len(skipped), synthetic, len(fonts) - synthetic,
             compared, disagreements))
    if uniquified_names:
        print("ttx_diff: %d glyph name(s) the reference renamed or invented, "
              "where the file repeats a name or stores an empty one and this "
              "library reports what the file says; a rename is checked against an "
              "earlier glyph actually carrying the bare name, and an invention "
              "against the reference's own glyph%%05d spelling for that index"
              % uniquified_names)
    if sanitised_fields:
        print("ttx_diff: %d field(s) the reference rewrote rather than read, and "
              "said so on its own stderr while doing it - a `head` date of zero, "
              "which fontTools reports as though it had been a Unix timestamp. "
              "Not compared, because a sanitisation is not a reading; the "
              "exemption comes from the reference's own warning rather than from a "
              "list here, so it stops applying when the warning does"
              % sanitised_fields)
    print("ttx_diff: %d record(s) this library declined and is documented to "
          "decline - the multi-byte Macintosh and Microsoft encodings only; "
          "%d record(s) fontTools itself could not decode"
          % (declined, theirs_declined))
    if mac_coverage:
        print("ttx_diff: Macintosh name (encoding, language) covered: %s"
              % ", ".join("%s x%d" % (pair, count)
                          for pair, count in sorted(mac_coverage.items())))
    for axis, seen in coverage.items():
        if not seen:
            continue
        print("ttx_diff: %s covered: %s"
              % (axis, ", ".join("%s x%d" % (value, count)
                                 for value, count in sorted(seen.items()))))
    if synthetic:
        print("ttx_diff: what only the fixtures cover: OS/2 versions 0, 2 and "
              "5, post format 1.0, every Macintosh encoding but Roman, and the "
              "OTTO and ttcf shapes. --no-fixtures covers none of them.")
    else:
        print("ttx_diff: no fixtures in this run, so OS/2 versions 0, 2 and 5, "
              "post format 1.0 and every non-Roman Macintosh encoding went "
              "uncompared; the unit tests are their only cover")
    for path, why, _ in skipped:
        print("ttx_diff: skipped %s: %s" % (os.path.basename(path), why))
    print("ttx_diff: %d glyph name(s) the reference generated from the `cmap` "
          "rather than reading, and said so on its own stderr while doing it - a "
          "`post` of format 1.0 names the 258 standard Macintosh glyphs and "
          "mona.ttf has 7,225, so fontTools discards the table and derives a name "
          "per glyph. This library reports the standard order for the 258 the "
          "format does name and nothing past them, which is FreeType's reading "
          "too. Not compared, because a generated name is not a reading"
          % generated_names)
    print("ttx_diff: %d table(s) with a single reader - the reference could not "
          "decompile them and this library read them, or the reverse, which is "
          "counted per *table* so that one short table does not take a font's "
          "other fields out of the gate with it" % single_reader)

    # The gate: see tools/oracle/unskippable.py. `mona.ttf` was leaving this
    # differential entirely on a short `OS/2`, which is what it is for.
    dropped = unskippable.check("ttx_diff",
        [(path, why) for path, why, expected in skipped if not expected])
    if dropped:
        return 1

    if not compared:
        sys.stderr.write("ttx_diff: nothing was compared\n")
        return 1
    if compared < 20 * (len(fonts) - len(skipped)):
        sys.stderr.write(
            "ttx_diff: only %d fields over %d fonts, which is too few - a dump "
            "that stopped printing would look like this\n"
            % (compared, len(fonts) - len(skipped)))
        return 1
    if len(skipped) > len(fonts) // 4:
        sys.stderr.write(
            "ttx_diff: %d of %d fonts were skipped; the run is not a "
            "measurement of the corpus\n" % (len(skipped), len(fonts)))
        return 1
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
