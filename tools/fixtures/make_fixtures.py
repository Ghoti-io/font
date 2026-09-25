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
"""Build the synthetic fixtures under `tests/data/fonts/`, byte-reproducibly.

documentation/design.md section 14.5: unit tests want fixtures that exercise a
code path, conformance wants the population that is not ours. This tool makes
the first kind. The second kind lives in the oracle image and is never copied
here, so nothing under `tests/data/fonts/` carries a third-party licence - every
fixture is drawn from outlines in this file, and says so in its own `name` table.

**It runs in the pinned `fonttools` image** (`tools/oracle/containers/`), and
that is what makes `check-fixtures` mean anything: a fixture's bytes are
whatever fontTools 4.66.0 wrote, so comparing committed bytes against a
regeneration is a real check on this library's inputs rather than on the host's
Python. `tools/fixtures/check_fixtures.py` is the driver; nothing here reaches
for a container itself.

Three reasons a generated fixture beats a byte array in a test:

  1. **It can be inspected.** `ttx tests/data/fonts/basic.ttf` prints what the
     fixture is. A test that builds its own bytes can only be read by reading
     the builder.
  2. **It is not this library's opinion.** Every expectation in `tests/` is one
     this library wrote for itself; `tests/sfnt_builder.h` says so in its own
     header. A fixture fontTools wrote is the one input to the unit suite that
     did not come from here.
  3. **It is the same bytes on every machine**, so a coverage hash or a golden
     bitmap over it (phase 1) means something.

`tests/sfnt_builder.h` keeps its job: the refusal arms - a checksum that does
not match, a directory entry past the blob, a length that ends mid-entry - are
bytes fontTools will not emit, and they stay hand-built.

Determinism, which is the whole contract here
---------------------------------------------

Everything that would otherwise vary with the clock, the filesystem or the
interpreter is pinned:

  * **`head.created` and `head.modified` are set to 0** (1904-01-01, visibly
    synthetic). `FontBuilder.__init__` sets both to `timestampNow()` - the
    defaults table in `fontBuilder.py` says 0 and is then overridden - so a
    generator that does not pin them writes different bytes every run. It sets
    `recalcTimestamp=False` on the font, so `save()` leaves them alone once set.
  * **No wall clock, no random, no host paths** reach a table. The only inputs
    are the literals below.
  * **The locale is the image's**, pinned to `C.UTF-8` in the Containerfile, per
    `notes/suite/CONTAINERS.md` section 1.1: an unset `LANG` produced fifty
    false disagreements in another library's oracle, and a Mac Roman `name`
    record is encoded through exactly that path.

Fixtures are written only where `--out` says, so this tool never writes into a
repository that a container has mounted read-only.
"""

import argparse
import os
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection, TTFont, newTable
from fontTools.ttLib.tables._n_a_m_e import NameRecord
from fontTools.ttLib.tables._p_o_s_t import standardGlyphOrder
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

UPEM = 1000

# head.created and head.modified: 2026-01-01T00:00:00Z, counted from the sfnt
# epoch of 1904-01-01 as the `head` table requires.
#
# **Not 0**, which was the first choice and was wrong in a way worth writing
# down. fontTools' `head` decompile treats any timestamp below 0x7C259DC0
# (1970-01-01, in this epoch) as a misencoded unix timestamp and silently adds
# that constant to it. A fixture written with 0 therefore reads back as 1970
# rather than 1904 - so `ttx_diff` would have reported `created` as a
# disagreement on every fixture, in a direction that is fontTools being helpful
# and this library being right. Any value at or above the threshold round-trips.
EPOCH = 3850070400

ASCENT = 800
DESCENT = -200

# The glyph repertoire. Five glyphs is enough to have a .notdef that is not
# empty, a blank glyph, two mapped glyphs, and one glyph whose name is *not* in
# the standard Macintosh order - which is the only way a `post` format 2.0
# fixture exercises both the index path and the extraNames path.
GLYPH_ORDER = [".notdef", "space", "A", "B", "ghoti.alt"]

ADVANCES = {".notdef": 600, "space": 300, "A": 640, "B": 620, "ghoti.alt": 700}

VENDOR = "GHTI"

# The last index into the standard Macintosh glyph order, which is the boundary
# `post` format 2.0 turns on: this index means the last standard name and the
# next one means the first stored string. Spelled from the vector's own length so
# it cannot drift from it.
GFNT_POST_LAST_STANDARD = len(standardGlyphOrder) - 1

# Every `OS/2` field this library reads, and every one of them distinctive.
#
# The same values go into all six version fixtures, which is the point. The
# version selects which fields the file *has*, so a version 0 fixture asked for
# a code page range and an x-height does not get them, and a reader of it must
# report zero. If these were left at fontTools' defaults - which are zero -
# "the field was read" and "the field was absent, so zero" would look identical
# and the six fixtures would prove nothing about the version gate.
#
# They are distinct from each other as well as from zero, because a parser that
# transposes two adjacent fields of the same width is the other defect here and
# two fields holding the same number cannot catch it.
OS2_FIELDS = dict(
    sTypoAscender=ASCENT,
    sTypoDescender=DESCENT,
    sTypoLineGap=90,
    usWinAscent=ASCENT,
    usWinDescent=-DESCENT,
    # Version 1 and later.
    ulCodePageRange1=0x00000651,
    ulCodePageRange2=0x10000001,
    # Version 2 and later.
    sxHeight=520,
    sCapHeight=720,
    usDefaultChar=0x003F,
    usBreakChar=0x0020,
    usMaxContext=3,
    # Version 5 only, and in *points*: fontTools multiplies by 20 on the way in
    # because the file holds twentieths of a point. So the fixture contains 160
    # and 1440, which is arithmetic a reader has to get right and which no font
    # in the oracle corpus exercises - it holds no version 5 at all.
    usLowerOpticalPointSize=8,
    usUpperOpticalPointSize=72,
)


def draw_notdef(pen):
    """A hollow box: two contours, so a fixture has a multi-contour glyph."""
    pen.moveTo((60, 0))
    pen.lineTo((540, 0))
    pen.lineTo((540, 700))
    pen.lineTo((60, 700))
    pen.closePath()
    pen.moveTo((140, 80))
    pen.lineTo((140, 620))
    pen.lineTo((460, 620))
    pen.lineTo((460, 80))
    pen.closePath()


def draw_a(pen):
    """A triangle: one contour, three points, all on-curve."""
    pen.moveTo((40, 0))
    pen.lineTo((600, 0))
    pen.lineTo((320, 720))
    pen.closePath()


def draw_b(pen):
    """A box with a bar, so the glyph's bounding box is not its only contour."""
    pen.moveTo((80, 0))
    pen.lineTo((540, 0))
    pen.lineTo((540, 720))
    pen.lineTo((80, 720))
    pen.closePath()
    pen.moveTo((200, 300))
    pen.lineTo((200, 420))
    pen.lineTo((420, 420))
    pen.lineTo((420, 300))
    pen.closePath()


def draw_alt(pen):
    """A chevron, for the glyph whose name is not in the standard order."""
    pen.moveTo((60, 0))
    pen.lineTo((340, 360))
    pen.lineTo((620, 0))
    pen.lineTo((620, 200))
    pen.lineTo((340, 560))
    pen.lineTo((60, 200))
    pen.closePath()


def draw_space(pen):
    """Nothing. A blank glyph is a real shape in a `glyf` table: zero bytes."""


OUTLINES = {
    ".notdef": draw_notdef,
    "space": draw_space,
    "A": draw_a,
    "B": draw_b,
    "ghoti.alt": draw_alt,
}


def tt_glyphs():
    """The outlines as TrueType glyphs, in glyph order."""
    out = {}
    for name in GLYPH_ORDER:
        pen = TTGlyphPen(None)
        OUTLINES[name](pen)
        out[name] = pen.glyph()
    return out


def cff_charstrings():
    """The same outlines as Type 2 charstrings, for the `OTTO` fixture."""
    out = {}
    for name in GLYPH_ORDER:
        pen = T2CharStringPen(ADVANCES[name], None)
        OUTLINES[name](pen)
        out[name] = pen.getCharString()
    return out


def names(label):
    """The `name` table strings, which say whose fixture this is.

    A fixture that escapes into someone's font directory should be able to
    answer for itself, and the licence question this repository does not have
    (section 14.5) is one the fixture states rather than one a reader has to
    infer from its absence.
    """
    return {
        "copyright": "Copyright 2026 Corey Pennycuff. LGPL-3.0-only.",
        "familyName": "Ghoti Fixture %s" % label,
        "styleName": "Regular",
        "uniqueFontIdentifier": "Ghoti.io Font fixture: %s" % label,
        "fullName": "Ghoti Fixture %s Regular" % label,
        "version": "Version 1.000",
        "psName": "GhotiFixture%s-Regular" % label.replace(" ", ""),
        "manufacturer": "Ghoti.io",
        "description": "Synthetic fixture for Ghoti.io Font. Not a typeface.",
        "licenseDescription": "Licensed under the LGPL version 3.",
    }


def subtable(fmt, platform, encoding, mapping, language=0):
    """One `cmap` subtable, spelled explicitly.

    `FontBuilder.setupCharacterMap` decides the formats for you, which is the
    wrong tool here: the point of these fixtures is that a named format is
    present, so the format is named.
    """
    table = CmapSubtable.newSubtable(fmt)
    table.platformID = platform
    table.platEncID = encoding
    table.language = language
    table.cmap = dict(mapping)
    return table


def set_cmap(fb, subtables):
    """Install a `cmap` built from explicit subtables."""
    table = newTable("cmap")
    table.tableVersion = 0
    table.tables = list(subtables)
    fb.font["cmap"] = table


def pin(fb):
    """Take the clock out of the font.

    Called for every fixture, immediately before saving, because
    `FontBuilder.__init__` has already written `timestampNow()` into `head` by
    the time any of this runs.
    """
    _pin_font(fb.font)


def _pin_font(font):
    """The same, for a TTFont that is not behind a FontBuilder."""
    head = font["head"]
    head.created = EPOCH
    head.modified = EPOCH
    # A TTFont opened from a file has recalcTimestamp *True*, unlike one
    # FontBuilder made, so `save()` would write the current time into
    # `modified` and no two generations would agree. That is how
    # collection.ttc - the one fixture that is re-read before it is written -
    # came out different on every run while the other fifteen were stable.
    font.recalcTimestamp = False


def truetype(label, subtables, *, os2_version=4, glyph_names=False,
             mac_names=True, extra_names=None):
    """A TrueType fixture: `glyf`, and every table this library parses.

    The table order here is the order `FontBuilder` requires and not a
    preference: `setupOS2` asserts that `hmtx` and `cmap` are already present,
    because it recalculates `xAvgCharWidth` from one and the Unicode ranges and
    first/last character index from the other.
    """
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupGlyf(tt_glyphs())
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, subtables)
    fb.setupOS2(version=os2_version, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names(label), mac=mac_names)
    fb.setupPost(keepGlyphNames=glyph_names)
    if glyph_names and extra_names is not None:
        fb.font["post"].extraNames = list(extra_names)
    pin(fb)
    return fb


def unicode_cmap():
    """The ordinary case: a Windows BMP format 4, and its Unicode twin."""
    mapping = {0x20: "space", 0x41: "A", 0x42: "B"}
    return [
        subtable(4, 0, 3, mapping),
        subtable(4, 3, 1, mapping),
    ]


def build_basic(out):
    """Everything ordinary, and the baseline every other fixture varies from."""
    fb = truetype("Basic", unicode_cmap())
    fb.save(out)


def build_os2(out, version):
    """One `OS/2` version.

    Six of these exist because the 327-font oracle corpus holds versions 1, 3
    and 4 only - no 0, no 2, no 5 - and a planted defect that read version 2's
    fields out of a version 1 table passed a four-font sample of it
    (design.md section 14.7). The version field selects which fields exist, so
    an unexercised version is an unexercised branch.
    """
    fb = truetype("OS2 v%d" % version, unicode_cmap(), os2_version=version)
    fb.save(out)


def build_cmap_format0(out):
    """A Macintosh byte-encoding table: the whole 256-byte array."""
    mapping = {0x20: "space", 0x41: "A", 0x42: "B"}
    fb = truetype("Cmap Format 0", [subtable(0, 1, 0, mapping)])
    fb.save(out)


def build_cmap_format6(out):
    """A trimmed array: firstCode and a run, with nothing outside it."""
    mapping = {0x41: "A", 0x42: "B", 0x43: "ghoti.alt"}
    fb = truetype("Cmap Format 6", [subtable(6, 1, 0, mapping)])
    fb.save(out)


def build_cmap_format12(out):
    """Groups, and codepoints outside the BMP that a format 4 cannot hold."""
    bmp = {0x20: "space", 0x41: "A", 0x42: "B"}
    wide = dict(bmp)
    wide.update({0x10041: "A", 0x10042: "B", 0x1F600: "ghoti.alt"})
    return_value = truetype("Cmap Format 12", [
        subtable(4, 3, 1, bmp),
        subtable(12, 3, 10, wide),
    ])
    return_value.save(out)


def build_cmap_symbol(out):
    """A (3,0) symbol table and nothing else.

    The only subtable maps 0xF041, so a lookup of U+0041 succeeds only through
    the 0xF0xx rule in section 7.2. A font shaped like this is why that rule is
    in the library, and no font in the oracle corpus is shaped like this.
    """
    mapping = {0xF020: "space", 0xF041: "A", 0xF042: "B"}
    fb = truetype("Cmap Symbol", [subtable(4, 3, 0, mapping)])
    fb.save(out)


def build_post_v2(out):
    """`post` format 2.0, across the boundary the format turns on.

    "A", "B" and "space" are in the standard Macintosh order and are spelled as
    indices into it; "ghoti.alt" is not, and is spelled as a Pascal string in the
    table's own list. Reading this table needs the 258-entry standard vector that
    section 7.2 deferred to exactly this generator.

    **"dcroat" is here because it is standard name 257, the last one.** The first
    version of this fixture used indices 0-3 and 258, which leaves the boundary
    itself untested: a planted off-by-one that treated index 257 as a stored name
    rather than the last standard one passed a two-font smoke run with zero
    disagreements. 38 of the corpus's 291 format 2.0 fonts do use index 257, so
    the full run catches it - but the fixtures are what travel with a thinned
    one, and a boundary is exactly the thing a fixture should hold.
    """
    boundary = standardGlyphOrder[GFNT_POST_LAST_STANDARD]
    order = GLYPH_ORDER + [boundary]
    glyphs = tt_glyphs()
    pen = TTGlyphPen(None)
    draw_a(pen)
    glyphs[boundary] = pen.glyph()
    metrics = {n: (ADVANCES[n], 0) for n in GLYPH_ORDER}
    metrics[boundary] = (580, 0)

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, [subtable(4, 0, 3, {0x20: "space", 0x41: "A", 0x42: "B"}),
                  subtable(4, 3, 1, {0x20: "space", 0x41: "A", 0x42: "B",
                                     0x111: boundary})])
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Post v2"), mac=True)
    fb.setupPost(keepGlyphNames=True)
    pin(fb)
    fb.save(out)


def build_post_v1(out):
    """`post` format 1.0: the font *is* the standard Macintosh glyph order.

    All 258 glyphs, in that order, because that is what the format means - a
    format 1.0 table stores no names at all and a reader answers from the
    generated vector alone. Most of the glyphs are empty, which keeps the
    fixture at a few kilobytes; the three that are also in the ordinary
    repertoire are drawn.

    No font in the oracle corpus is format 1.0 - it holds 291 format 2.0 and 51
    format 3.0 and nothing else - so this fixture is the only cover that path
    has, the same position `os2-v0`, `os2-v2` and `os2-v5` are in.
    """
    order = list(standardGlyphOrder)
    if len(order) != 258:
        raise SystemExit("the standard glyph order is not 258 names")

    glyphs = {}
    metrics = {}
    for name in order:
        pen = TTGlyphPen(None)
        OUTLINES.get(name, draw_space)(pen)
        glyphs[name] = pen.glyph()
        metrics[name] = (ADVANCES.get(name, 500), 0)

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, [subtable(4, 3, 1, {0x20: "space", 0x41: "A", 0x42: "B"})])
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Post v1"), mac=True)
    fb.setupPost(keepGlyphNames=False)
    # fontTools writes a format 1.0 table as the bare header, which is what the
    # format is; it is not offered by setupPost because a font is rarely allowed
    # to claim it.
    fb.font["post"].formatType = 1.0
    pin(fb)
    fb.save(out)


# The Macintosh (platEncID, langID) pairs this fixture exercises, and what each
# one must decode as. Taken from fontTools' own encoding map - see
# tools/vectors/make_vectors.py, which generates the library's table from the
# same place.
#
# The point of the pairs under platEncID 0 is that the encoding ID does not
# decide: 0 with langID 17 is Turkish, not Roman, and a reader that ignores the
# language decodes real letters to the wrong real letters without failing.
MAC_ENCODING_CASES = [
    (0, 0, "mac_roman"),
    (0, 15, "mac_iceland"),
    (0, 17, "mac_turkish"),
    (0, 18, "mac_croatian"),
    (0, 24, "mac_latin2"),
    (0, 37, "mac_romanian"),
    (6, 0, "mac_greek"),
    (7, 0, "mac_cyrillic"),
    (29, 0, "mac_latin2"),
    (35, 0, "mac_turkish"),
    (37, 0, "mac_iceland"),
]


def build_name_mac_encodings(out):
    """One Macintosh record per single-byte encoding, each holding every byte.

    The record's text is **all of 0x80 to 0xFF** decoded through that encoding,
    so two tables that differ anywhere differ here, and a reader that chose the
    wrong one cannot produce the right string by accident. fontTools encodes the
    text back to bytes with the same codec, so the fixture's bytes are the
    reference's opinion of the encoding rather than this repository's.

    Why it has to exist: all 658 Macintosh name records across the 327-font
    corpus are (platEncID 0, Mac Roman). The language-keyed rules and the seven
    non-Roman tables have no coverage from real fonts at all, and a table with
    no coverage is a table nobody has read.
    """
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupGlyf(tt_glyphs())
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Name Mac Encodings"), mac=True)
    fb.setupPost(keepGlyphNames=False)

    table = fb.font["name"]
    high = bytes(range(0x80, 0x100))
    for encoding, language, codec in MAC_ENCODING_CASES:
        record = NameRecord()
        record.platformID = 1
        record.platEncID = encoding
        record.langID = language
        # Sample text (19), so that one nameID can carry every case: records are
        # keyed by (platform, encoding, language, nameID) and these differ in the
        # first three.
        record.nameID = 19
        record.string = high.decode(codec)
        table.names.append(record)
    table.names.sort(key=lambda r: (r.platformID, r.platEncID, r.langID,
                                    r.nameID))
    pin(fb)
    fb.save(out)


def build_post_v3(out):
    """`post` format 3.0: the header, and the statement that there are no names."""
    fb = truetype("Post v3", unicode_cmap(), glyph_names=False)
    fb.save(out)


def build_name_macroman(out):
    """A Macintosh Roman `name` record with a byte above ASCII.

    U+2122 encodes to 0xAA in Mac Roman, and the twelve records that the
    `ttx_diff` oracle counts across 327 real fonts are that same character in
    that same encoding (design.md section 18.1). This is the fixture the Mac
    Roman deferral in section 7.2 is waiting for, and the reason it is here
    rather than in `sfnt_builder.h` is that the encoding must come from
    fontTools rather than from a byte this library chose.
    """
    strings = names("Name MacRoman")
    strings["trademark"] = "Ghoti™ is not a trademark"
    strings["description"] = "Fixture with a Mac Roman ™ above ASCII."
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupGlyf(tt_glyphs())
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(strings, mac=True)
    fb.setupPost(keepGlyphNames=False)
    pin(fb)
    fb.save(out)


def build_cff(out):
    """An `OTTO` face: a `CFF ` table where the `glyf` would be.

    Phase 2 parses the charstrings. Phase 0 reads the flavour, the directory and
    every metric table, and needs a fixture where those are the same questions
    over a different container.
    """
    fb = FontBuilder(UPEM, isTTF=False)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupCFF(
        names("CFF")["psName"],
        {"FullName": names("CFF")["fullName"], "Weight": "Regular"},
        cff_charstrings(),
        {})
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("CFF"), mac=True)
    fb.setupPost(keepGlyphNames=False)
    pin(fb)
    fb.save(out)


def build_collection(out):
    """A `ttcf` holding two faces that share their tables.

    Shared tables are the point: the second face's directory entries point into
    the first face's bytes, so a reader that treats an offset as being relative
    to its own face reads garbage. `shareTables=True` is what makes the sharing
    real rather than two fonts concatenated.
    """
    faces = []
    for index, version in enumerate((4, 1)):
        fb = truetype("Collection %d" % index, unicode_cmap(),
            os2_version=version)
        scratch = out + ".face%d" % index
        fb.save(scratch)
        face = TTFont(scratch)
        _pin_font(face)
        faces.append(face)
    collection = TTCollection()
    collection.fonts = faces
    collection.save(out, shareTables=True)
    for face in faces:
        face.close()
    for index in range(len(faces)):
        os.remove(out + ".face%d" % index)


# The fixture set: filename -> (builder, what it is for).
#
# The description is not decoration. It is written into MANIFEST beside the
# filename, so `tests/data/fonts/` says what each of its files exercises and
# cannot drift from this table - the two are one edit.
FIXTURES = {
    "basic.ttf": (build_basic,
        "TrueType, cmap format 4 on (0,3) and (3,1), OS/2 v4, post v3"),
    "os2-v0.ttf": (lambda out: build_os2(out, 0),
        "OS/2 version 0: no code page ranges, no x-height, no optical size"),
    "os2-v1.ttf": (lambda out: build_os2(out, 1),
        "OS/2 version 1: code page ranges and nothing later"),
    "os2-v2.ttf": (lambda out: build_os2(out, 2),
        "OS/2 version 2: x-height, cap height, default and break char"),
    "os2-v3.ttf": (lambda out: build_os2(out, 3),
        "OS/2 version 3: version 2's fields with version 3's fsType rules"),
    "os2-v4.ttf": (lambda out: build_os2(out, 4),
        "OS/2 version 4: the common case in the oracle corpus"),
    "os2-v5.ttf": (lambda out: build_os2(out, 5),
        "OS/2 version 5: optical point sizes, stored as twentieths"),
    "cmap-format0.ttf": (build_cmap_format0,
        "cmap format 0 on (1,0): the 256-byte glyph array"),
    "cmap-format6.ttf": (build_cmap_format6,
        "cmap format 6 on (1,0): a trimmed array with firstCode"),
    "cmap-format12.ttf": (build_cmap_format12,
        "cmap format 12 on (3,10) past the BMP, beside a format 4"),
    "cmap-symbol.ttf": (build_cmap_symbol,
        "cmap (3,0) symbol only: U+0041 reachable only via the 0xF0xx rule"),
    "post-v1.ttf": (build_post_v1,
        "post format 1.0: all 258 standard glyphs, no stored names"),
    "post-v2.ttf": (build_post_v2,
        "post format 2.0: standard indices including 257, and one extra name"),
    "post-v3.ttf": (build_post_v3,
        "post format 3.0: a header that states it has no names"),
    "name-macroman.ttf": (build_name_macroman,
        "name: a Macintosh Roman record holding U+2122 as byte 0xAA"),
    "name-mac-encodings.ttf": (build_name_mac_encodings,
        "name: every single-byte Macintosh encoding, all 128 high bytes each"),
    "cff.otf": (build_cff,
        "OTTO flavour: a CFF outline table, every metric table unchanged"),
    "collection.ttc": (build_collection,
        "ttcf: two faces sharing tables, with different OS/2 versions"),
}

MANIFEST_HEADER = """\
# The synthetic fixtures, and what each one exercises.
#
# Generated by tools/fixtures/make_fixtures.py in the pinned fonttools image.
# Do not edit either this file or the fonts beside it: `make check-fixtures`
# regenerates them and fails on a byte difference.
#
# Every font here is drawn from outlines in that script, carries this
# repository's copyright, and is LGPL-3.0-only like the code around it. No
# third-party font is committed to this repository at all; the real fonts a
# differential needs live only in the oracle image (design.md section 14.5).
#
# Tab-separated: <filename> <what it exercises>.
"""


def build(out_dir, only=None):
    """Write every fixture, and the MANIFEST that describes them."""
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for name in sorted(FIXTURES):
        if only and name not in only:
            continue
        builder, _ = FIXTURES[name]
        builder(os.path.join(out_dir, name))
        written.append(name)

    with open(os.path.join(out_dir, "MANIFEST"), "w", encoding="utf-8",
            newline="\n") as handle:
        handle.write(MANIFEST_HEADER)
        for name in sorted(FIXTURES):
            handle.write("%s\t%s\n" % (name, FIXTURES[name][1]))
    written.append("MANIFEST")
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True,
        help="directory to write into; never the repository itself when this "
             "runs in a container, where the repository is mounted read-only")
    parser.add_argument("--only", action="append", default=None,
        help="build just this fixture (repeatable); for iterating on one")
    parser.add_argument("--list", action="store_true",
        help="print the fixture set and exit")
    args = parser.parse_args(argv)

    if args.list:
        for name in sorted(FIXTURES):
            print("%-20s %s" % (name, FIXTURES[name][1]))
        return 0

    written = build(args.out, only=args.only)
    print("wrote %d file(s) to %s" % (len(written), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
