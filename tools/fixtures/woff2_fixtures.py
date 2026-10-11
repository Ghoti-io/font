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
"""WOFF 2 fixtures, written by hand because fontTools writes only some of the format.

documentation/design.md sections 7.1 and 14.5. fontTools writes a WOFF 2 with the
smallest triplet for each point, never an explicit bounding box for a simple glyph,
no overlap bitmap, no collection and no metadata; this module is a small WOFF 2
writer that does all of those, and then asserts that fontTools reads what it wrote
back to the font it started from. So each fixture is two implementations agreeing:
the encoder here, and fontTools' decoder, before this library reads a byte.

  * `woff2-triplets.woff2` - a transformed `glyf` holding **all 128 triplet
    classes**, on and off the curve, a composite glyph with instructions and a
    scale, a glyph with an explicit box, an overlap bitmap, and a transformed
    `hmtx` with both left-side-bearing arrays dropped. `woff2-triplets-source.ttf`
    is the font it was made from.
  * `woff2-collection.woff2` - two faces that share every table but `name`, with
    the shared `glyf`/`loca` transformed.
  * `woff2-stored.woff2` - `basic.ttf` with every table in its null transform
    (including `glyf`, version 3), and a metadata block and a private block after
    the tables.
"""

import io
import random
import struct

import brotli
from fontTools.ttLib import TTFont
from fontTools.ttLib import woff2 as ftw2
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates

KNOWN = list(ftw2.woff2KnownTags)


def base128(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def u255(n):
    if n < 253:
        return bytes([n])
    if n < 506:
        return bytes([255, n - 253])
    if n < 762:
        return bytes([254, n - 506])
    return bytes([253]) + struct.pack(">H", n)


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


class Table:
    """One directory entry: what the file states and what the stream holds."""

    def __init__(self, tag, original, stream=None, version=0, transformed=False):
        self.tag = tag
        self.original = original
        self.stream = original if stream is None else stream
        self.version = version
        self.transformed = transformed

    def entry(self):
        tag = self.tag
        index = KNOWN.index(tag) if tag in KNOWN else 63
        out = bytes([index | (self.version << 6)])
        if index == 63:
            out += tag.encode("latin-1")
        out += base128(len(self.original))
        if self.transformed:
            out += base128(len(self.stream))
        return out


def container(flavor, tables, collection=None, meta=None, private=None,
              ttc_version=0x00010000):
    """Assemble a WOFF 2 file. @p collection is a list of (flavor, [indices])."""
    directory = b"".join(t.entry() for t in tables)
    if collection is not None:
        directory += struct.pack(">I", ttc_version) + u255(len(collection))
        for face_flavor, indices in collection:
            directory += u255(len(indices)) + struct.pack(">I", face_flavor)
            directory += b"".join(u255(i) for i in indices)
    stream = b"".join(t.stream for t in tables)
    packed = brotli.compress(stream, mode=brotli.MODE_FONT, quality=11)
    body = pad4(packed) if (meta or private) else packed
    cursor = 48 + len(directory) + len(body)
    meta_off = meta_len = meta_orig = priv_off = priv_len = 0
    tail = b""
    if meta:
        packed_meta = brotli.compress(meta, quality=11)
        meta_off, meta_len, meta_orig = cursor, len(packed_meta), len(meta)
        tail += pad4(packed_meta) if private else packed_meta
        cursor += len(tail)
    if private:
        priv_off, priv_len = cursor, len(private)
        tail += private
    total = 48 + len(directory) + len(body) + len(tail)
    sfnt_size = 12 + 16 * len(tables) + sum(len(pad4(t.original)) for t in tables)
    header = struct.pack(">4sIIHHIIHHIIIII", b"wOF2", flavor, total, len(tables), 0,
                         sfnt_size, len(packed), 1, 0, meta_off, meta_len,
                         meta_orig, priv_off, priv_len)
    return header + directory + body + tail


# ---------------------------------------------------------------------------
# The glyf transform, written from the specification's description of it.

def with_sign(flag, value):
    return value if flag & 1 else -value


def triplet_bytes(index, raw):
    """How many bytes class @p index reads."""
    return 1 if index < 84 else 2 if index < 120 else 3 if index < 124 else 4


def triplet_delta(index, b):
    """The (dx, dy) of class @p index with data bytes @p b - the table's formulas."""
    if index < 10:
        return 0, with_sign(index, ((index & 14) << 7) + b[0])
    if index < 20:
        return with_sign(index, (((index - 10) & 14) << 7) + b[0]), 0
    if index < 84:
        k = index - 20
        return (with_sign(index, 1 + (k & 0x30) + (b[0] >> 4)),
                with_sign(index >> 1, 1 + ((k & 0x0C) << 2) + (b[0] & 15)))
    if index < 120:
        k = index - 84
        return (with_sign(index, 1 + ((k // 12) << 8) + b[0]),
                with_sign(index >> 1, 1 + (((k % 12) >> 2) << 8) + b[1]))
    if index < 124:
        return (with_sign(index, (b[0] << 4) + (b[1] >> 4)),
                with_sign(index >> 1, ((b[1] & 15) << 8) + b[2]))
    return (with_sign(index, (b[0] << 8) + b[1]),
            with_sign(index >> 1, (b[2] << 8) + b[3]))


def all_triplets(seed):
    """128 points, one per class, with data bytes chosen to keep them near the origin.

    Consecutive classes alternate their sign, so the running position stays small.
    Returns the (flag index, data bytes) of each and the points they reach.
    """
    rng = random.Random(seed)
    steps = []
    x = y = 0
    points = []
    for index in range(128):
        n = triplet_bytes(index, None)
        data = bytearray(rng.randrange(256) for _ in range(n))
        if index >= 124:
            # 16 bits each could leave the 16 the glyph's coordinates live in.
            data[0] &= 7
            data[2] &= 7
        data = bytes(data)
        dx, dy = triplet_delta(index, data)
        x += dx
        y += dy
        steps.append((index, data))
        points.append((x, y))
    assert all(-32768 <= p <= 32767 for pt in points for p in pt), "left 16 bits"
    return steps, points


def transform_glyf(font, forced, explicit_bbox, overlap, long_loca):
    """The transformed `glyf` of @p font and the `loca` to go with it.

    @p forced maps a glyph name to ([(index, bytes)...], on_curve) to encode its
    points with; every other simple glyph uses classes 124 to 127 (16 bits each),
    which can say anything, the index choosing the signs. @p explicit_bbox names simple glyphs whose box is stated, and
    @p overlap simple glyphs that carry OVERLAP_SIMPLE.
    """
    glyf = font["glyf"]
    order = font.getGlyphOrder()
    ncont = b""
    npts = flag = glyph = comp = bbox_values = instr = b""
    bitmap = bytearray(4 * ((len(order) + 31) // 32))
    overlap_bits = bytearray((len(order) + 7) // 8)
    for i, name in enumerate(order):
        g = glyf[name]
        if g.numberOfContours == 0:
            ncont += struct.pack(">h", 0)
            continue
        if g.isComposite():
            ncont += struct.pack(">h", -1)
            data = g.compile(glyf)
            at, more, wants = 10, True, False
            while more:
                f = struct.unpack(">H", data[at:at + 2])[0]
                size = 4 + (4 if f & 1 else 2)
                size += 2 if f & 8 else 4 if f & 0x40 else 8 if f & 0x80 else 0
                at += size
                more = bool(f & 0x20)
                wants = wants or bool(f & 0x100)
            comp += data[10:at]
            if wants:
                n = struct.unpack(">H", data[at:at + 2])[0]
                glyph += u255(n)
                instr += data[at + 2:at + 2 + n]
            bitmap[i >> 3] |= 0x80 >> (i & 7)
            bbox_values += struct.pack(">hhhh", g.xMin, g.yMin, g.xMax, g.yMax)
            continue
        ncont += struct.pack(">h", g.numberOfContours)
        coords, ends, flags = g.getCoordinates(glyf)
        start = 0
        for end in ends:
            npts += u255(end + 1 - start)
            start = end + 1
        if name in forced:
            steps, on_curve = forced[name]
            for index, data in steps:
                flag += bytes([index | (0 if on_curve else 0x80)])
                glyph += data
        else:
            px = py = 0
            for (x, y), f in zip(coords, flags):
                index = 124 | (1 if x >= px else 0) | (2 if y >= py else 0)
                flag += bytes([(0 if f & 1 else 0x80) | index])
                glyph += struct.pack(">HH", abs(x - px), abs(y - py))
                px, py = x, y
        glyph += u255(len(g.program.getBytecode()))
        instr += bytes(g.program.getBytecode())
        if name in explicit_bbox:
            bitmap[i >> 3] |= 0x80 >> (i & 7)
            bbox_values += struct.pack(">hhhh", g.xMin, g.yMin, g.xMax, g.yMax)
        if name in overlap:
            overlap_bits[i >> 3] |= 0x80 >> (i & 7)
    options = 1 if overlap else 0
    streams = [ncont, npts, flag, glyph, comp, bytes(bitmap) + bbox_values, instr]
    head = struct.pack(">HHHH", 0, options, len(order), 1 if long_loca else 0)
    head += b"".join(struct.pack(">I", len(s)) for s in streams)
    out = head + b"".join(streams) + (bytes(overlap_bits) if overlap else b"")
    return out


def transform_hmtx(font):
    """The transformed `hmtx`: the flags byte, the advances, and any bearings kept."""
    hhea = font["hhea"]
    glyf = font["glyf"]
    order = font.getGlyphOrder()
    metrics = font["hmtx"].metrics
    count = hhea.numberOfHMetrics
    proportional = all(
        metrics[n][1] == (glyf[n].xMin if glyf[n].numberOfContours else 0)
        for n in order[:count])
    monospaced = all(
        metrics[n][1] == (glyf[n].xMin if glyf[n].numberOfContours else 0)
        for n in order[count:])
    flags = (1 if proportional else 0) | (2 if monospaced else 0)
    out = bytes([flags])
    out += b"".join(struct.pack(">H", metrics[n][0]) for n in order[:count])
    if not proportional:
        out += b"".join(struct.pack(">h", metrics[n][1]) for n in order[:count])
    if not monospaced:
        out += b"".join(struct.pack(">h", metrics[n][1]) for n in order[count:])
    return out, flags


def font_tables(data):
    """(font, {tag: bytes}) of an sfnt held in memory."""
    font = TTFont(io.BytesIO(data), recalcBBoxes=False, recalcTimestamp=False)
    return font, {t: font.reader[t] for t in font.reader.tables}


def directory_order(tags):
    """Sorted, with `loca` straight after `glyf` as the specification asks."""
    rest = sorted(t for t in tags if t != "loca")
    if "loca" in tags:
        rest.insert(rest.index("glyf") + 1, "loca")
    return rest


def sfnt_to_tables(data, forced=None, explicit=(), overlap=(), transform=True,
                   hmtx=True):
    """The Table entries of an sfnt file, with the transforms applied."""
    font, raw = font_tables(data)
    long_loca = font["head"].indexToLocFormat == 1
    tables = []
    for tag in directory_order(raw):
        original = raw[tag]
        if tag == "glyf" and transform and font.sfntVersion == "\0\1\0\0":
            stream = transform_glyf(font, forced or {}, set(explicit), set(overlap),
                                    long_loca)
            tables.append(Table(tag, original, stream, 0, True))
        elif tag == "loca" and transform and font.sfntVersion == "\0\1\0\0":
            tables.append(Table(tag, original, b"", 0, True))
        elif tag == "hmtx" and hmtx and transform and "glyf" in raw:
            stream, _ = transform_hmtx(font)
            tables.append(Table(tag, original, stream, 1, True))
        else:
            version = 3 if tag in ("glyf", "loca") else 0
            tables.append(Table(tag, original, None, version, False))
    return font, tables


def verify(woff2_bytes, sfnt_bytes):
    """fontTools reads @p woff2_bytes to the same glyphs as @p sfnt_bytes."""
    theirs = TTFont(io.BytesIO(woff2_bytes))
    mine = TTFont(io.BytesIO(sfnt_bytes))
    assert theirs.getGlyphOrder() == mine.getGlyphOrder()
    for name in mine.getGlyphOrder():
        a, b = theirs["glyf"][name], mine["glyf"][name]
        assert a.numberOfContours == b.numberOfContours, name
        if a.numberOfContours > 0:
            ca, ea, fa = a.getCoordinates(theirs["glyf"])
            cb, eb, fb = b.getCoordinates(mine["glyf"])
            assert list(ca) == list(cb) and list(ea) == list(eb), name
            assert [f & 1 for f in fa] == [f & 1 for f in fb], name
            assert a.program.getBytecode() == b.program.getBytecode(), name
        elif a.numberOfContours < 0:
            assert [(c.glyphName, c.x, c.y, c.flags & 0xFEFF)
                    for c in a.components] == [
                    (c.glyphName, c.x, c.y, c.flags & 0xFEFF) for c in b.components]
    for tag in ("hmtx", "cmap", "maxp", "hhea"):
        assert theirs.reader[tag] == mine.reader[tag] or tag == "hhea", tag


# ---------------------------------------------------------------------------
# The fixtures.

def _polygon(points):
    pen = TTGlyphPen(None)
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()
    return pen.glyph()


def _program(glyph, code):
    from fontTools.ttLib.tables import ttProgram
    glyph.program = ttProgram.Program()
    glyph.program.fromBytecode(code)
    return glyph


def _long_contour(points, on_curve, overlap=False):
    import array
    g = Glyph()
    g.numberOfContours = 1
    g.coordinates = GlyphCoordinates(points)
    g.flags = array.array("B", [1 if on_curve else 0] * len(points))
    if overlap:
        g.flags[0] |= 0x40
    g.endPtsOfContours = [len(points) - 1]
    return _program(g, b"")


TRIPLET_STEPS, TRIPLET_POINTS = all_triplets(2026)
TRIPLET_ORDER = [".notdef", "trip_on", "trip_off", "box", "pair", "comp", "long",
                 "i300", "i600", "i800", "empty", "last"]
TRIPLET_ADVANCES = {".notdef": 700, "trip_on": 900, "trip_off": 910, "box": 520,
                    "pair": 640, "comp": 800, "long": 660, "i300": 530, "i600": 540,
                    "i800": 550, "empty": 500, "last": 500}
# Contour sizes that take each spelling of 255UInt16: a byte, 253 plus a byte,
# 506 plus a byte, and 253 followed by a 16-bit value.
LONG_CONTOURS = [1, 252, 300, 600, 800]


def _long_glyph():
    import array
    points = []
    ends = []
    for k, n in enumerate(LONG_CONTOURS):
        for i in range(n):
            points.append((20 * k + (i % 50) * 3, (i // 50) * 5 + 7 * k))
        ends.append(len(points) - 1)
    g = Glyph()
    g.numberOfContours = len(LONG_CONTOURS)
    g.coordinates = GlyphCoordinates(points)
    g.flags = array.array("B", [1] * len(points))
    g.endPtsOfContours = ends
    return _program(g, b"")


def _boxed(code_length):
    """A rectangle whose instructions are @p code_length bytes long."""
    return _program(_polygon([(5, 0), (5, 90), (60, 90), (60, 0)]),
                    bytes((i * 7) & 0xFF for i in range(code_length)))


def triplets_font(mf):
    """The source font of `woff2-triplets.woff2`, as a FontBuilder."""
    box = [(30, 0), (30, 500), (430, 500), (430, 0)]
    pair = TTGlyphPen(None)
    pair.moveTo((10, 10))
    for p in [(10, 300), (300, 300), (300, 10)]:
        pair.lineTo(p)
    pair.closePath()
    pair.moveTo((400, 20))
    for p in [(500, 320), (600, 20)]:
        pair.lineTo(p)
    pair.closePath()
    pair_glyph = _program(pair.glyph(), bytes([0xB0, 0x01, 0x2C, 0x40, 0x01, 0x02]))
    comp = TTGlyphPen({"box": None, "pair": None})
    comp.addComponent("box", (0.5, 0, 0, 0.5, 20, 30))
    comp.addComponent("pair", (1, 0, 0, 1, 100, 50))
    comp_glyph = _program(comp.glyph(), bytes([0xB0, 0x07, 0x2F]))
    glyphs = {
        ".notdef": _polygon([(50, 0), (50, 700), (650, 700), (650, 0)]),
        "trip_on": _long_contour(TRIPLET_POINTS, True),
        "trip_off": _long_contour(TRIPLET_POINTS, False),
        "box": _long_contour(box, True, overlap=True),
        "pair": pair_glyph,
        "comp": comp_glyph,
        "long": _long_glyph(),
        "i300": _boxed(300),
        "i600": _boxed(600),
        "i800": _boxed(800),
        "empty": TTGlyphPen(None).glyph(),
        "last": _polygon([(20, 0), (20, 400), (200, 400), (200, 0)]),
    }
    fb = mf.truetype("Woff2 Triplets", [mf.subtable(4, 3, 1, {0x41: "box",
                                                              0x42: "pair"})],
                     order=TRIPLET_ORDER, glyphs=glyphs,
                     advances=TRIPLET_ADVANCES, glyph_names=True)
    font = fb.font
    table = font["glyf"]
    for name in TRIPLET_ORDER:
        table[name].recalcBounds(table)
    font["hmtx"].metrics = {
        n: (TRIPLET_ADVANCES[n], table[n].xMin if table[n].numberOfContours else 0)
        for n in TRIPLET_ORDER}
    font["head"].flags |= 0x800
    return fb


def build_woff2_triplets_source(mf, out):
    triplets_font(mf).save(out)


def build_woff2_triplets(mf, out):
    source = out + ".ttf"
    triplets_font(mf).save(source)
    with open(source, "rb") as handle:
        data = handle.read()
    import os
    os.remove(source)
    forced = {"trip_on": ([s for s in TRIPLET_STEPS], True),
              "trip_off": ([s for s in TRIPLET_STEPS], False)}
    font, tables = sfnt_to_tables(data, forced=forced, explicit={"box", "pair"},
                                  overlap={"box"})
    # Names the encoder looks glyphs up by must be the font's own: a font without
    # a glyph-name table is read back with names made up from its cmap.
    assert font.getGlyphOrder() == TRIPLET_ORDER, font.getGlyphOrder()
    assert font["hhea"].numberOfHMetrics < len(TRIPLET_ORDER)
    flags = next(t for t in tables if t.tag == "hmtx").stream[0]
    assert flags == 3, flags
    packed = container(0x00010000, tables)
    verify(packed, data)
    with open(out, "wb") as handle:
        handle.write(packed)


def build_woff2_collection(mf, out):
    faces = []
    for label in ("Face A", "Face B"):
        fb = mf.truetype(label, mf.unicode_cmap())
        buffer = io.BytesIO()
        fb.save(buffer)
        faces.append(buffer.getvalue())
    font_a, tables_a = sfnt_to_tables(faces[0])
    _, tables_b = sfnt_to_tables(faces[1])
    unique = []
    index_of = {}
    layout = []
    for tables in (tables_a, tables_b):
        indices = []
        for t in tables:
            key = (t.tag, t.original)
            if key not in index_of:
                index_of[key] = len(unique)
                unique.append(t)
            indices.append(index_of[key])
        layout.append((0x00010000, indices))
    shared = [t.tag for t in unique]
    assert shared.count("name") == 2 and shared.count("glyf") == 1, shared
    packed = container(0x74746366, unique, collection=layout)
    with open(out, "wb") as handle:
        handle.write(packed)


def build_woff2_stored(mf, out):
    fb = mf.truetype("Basic", mf.unicode_cmap())
    buffer = io.BytesIO()
    fb.save(buffer)
    _, tables = sfnt_to_tables(buffer.getvalue(), transform=False)
    packed = container(0x00010000, tables,
                       meta=b'<?xml version="1.0" encoding="UTF-8"?>\n'
                            b'<metadata version="1.0"/>\n',
                       private=b"ghoti private data, not read")
    with open(out, "wb") as handle:
        handle.write(packed)


def _fonttools_woff2(mf, out, transformed):
    mf.build_basic(out + ".plain")
    font = TTFont(out + ".plain", recalcTimestamp=False)
    font.flavor = "woff2"
    font.flavorData = ftw2.WOFF2FlavorData()
    font.flavorData.transformedTables = set(transformed)
    font.save(out)
    import os
    os.remove(out + ".plain")


def build_woff2_basic(mf, out):
    _fonttools_woff2(mf, out, {"glyf", "loca"})


def build_woff2_hmtx(mf, out):
    _fonttools_woff2(mf, out, {"glyf", "loca", "hmtx"})
