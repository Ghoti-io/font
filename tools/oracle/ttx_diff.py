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
    """Our dump, as {key: value} plus the keys it declined to decode."""
    values = {}
    undecodable = set()
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
            elif keys == "name-undecodable":
                platform, encoding, language, name_id = found.groups()
                undecodable.add("name.%d.%d.%d.%d"
                                % (int(platform), int(encoding), int(language),
                                   int(name_id)))
            else:
                for key, value in zip(keys, found.groups()):
                    values[key] = value
            break
    return values, undecodable


def parse_reference(text):
    """The reference's lines, as {key: value} plus what it declined to answer."""
    values = {}
    absent = set()
    undecodable = 0
    for line in text.splitlines():
        key, _, value = line.partition("\t")
        if key.startswith("missing.") or key.startswith("absent."):
            absent.add(key.split(".", 1)[1])
        elif key.startswith("undecodable."):
            undecodable += 1
        else:
            values[key] = value
    return values, absent, undecodable


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
    if key.startswith("name."):
        return ours == theirs
    if key.startswith(("directory.", "cmap.subtable.")):
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


def deferred_mac_roman(key, text):
    """Whether this library declining this record is the documented deferral.

    design.md section 7.2 defers Mac Roman: a Macintosh record is decoded as far
    as ASCII and refused above it, because the 128-codepoint table belongs to an
    oracle rather than to memory. This differential found what that costs - the
    Liberation family's description record says "Courier New(tm)", and the
    trademark sign is 0xAA in Mac Roman - and a permanently red gate is a gate
    nobody reads, so the case is counted rather than scored.

    **It is checked, not excused.** The category is only allowed for a
    Macintosh Roman record whose reference text actually contains something
    above ASCII. A Macintosh record this library declined whose text is pure
    ASCII would be a defect in the ASCII path, and stays a disagreement - which
    is the difference between a known difference and an exclusion that absorbs
    the thing it was meant to expose.
    """
    if not key.startswith("name.1.0."):
        return False
    return any(ord(character) > 0x7F for character in text)


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""


def compare(path, face, report):
    """One font. Returns (keys compared, disagreements, counters)."""
    finished = subprocess.run([DRIVER, path, str(face)], capture_output=True,
        text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip() or "the driver refused the font")
    ours, our_undecodable = parse_ours(finished.stdout)

    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face)])
    answered = subprocess.run(argv, capture_output=True, text=True)
    if answered.returncode != 0:
        tail = answered.stderr.strip().splitlines()
        raise Skip("fontTools: %s" % (tail[-1] if tail else "refused"))
    theirs, absent, their_undecodable = parse_reference(answered.stdout)

    compared = 0
    disagreements = 0
    deferred = 0
    shown = 0
    for key, value in sorted(theirs.items()):
        if key not in ours:
            if key in our_undecodable and deferred_mac_roman(key, value):
                deferred += 1
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
    return compared, disagreements, deferred, their_undecodable, shape


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
    deferred = 0
    theirs_declined = 0
    skipped = []
    report = [0]
    # What the run covered, printed below. A differential that does not say
    # which versions of a table it met is one whose clean result cannot be read:
    # a plant that only affects OS/2 version 1 passed a four-font sample of this
    # corpus, because the four were all version 4.
    coverage = {"OS/2 version": {}, "post version": {}}

    for path, face in fonts:
        try:
            keys, differed, mac_roman, theirs, shape = compare(path, face,
                report)
        except Skip as why:
            skipped.append((path, str(why)))
            continue
        compared += keys
        disagreements += differed
        deferred += mac_roman
        theirs_declined += theirs
        for axis, value in shape.items():
            if value is not None:
                coverage[axis][value] = coverage[axis].get(value, 0) + 1
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
    print("ttx_diff: %d Macintosh Roman name record(s) above ASCII, which this "
          "library defers and this gate counts rather than scores (design.md "
          "section 7.2); %d record(s) fontTools itself could not decode"
          % (deferred, theirs_declined))
    for axis, seen in coverage.items():
        if not seen:
            continue
        print("ttx_diff: %s covered: %s"
              % (axis, ", ".join("%s x%d" % (value, count)
                                 for value, count in sorted(seen.items()))))
    if synthetic:
        print("ttx_diff: the versions the real corpus lacks - OS/2 0, 2 and 5 - "
              "come from the fixtures; a run with --no-fixtures covers neither "
              "them nor the OTTO and ttcf shapes")
    else:
        print("ttx_diff: no fixtures in this run, so OS/2 versions 0, 2 and 5 "
              "went uncompared and the unit tests are their only cover")
    for path, why in skipped:
        print("ttx_diff: skipped %s: %s" % (os.path.basename(path), why))

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
