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
"""The color strike fixtures: `CBLC`/`CBDT` and `sbix`, written byte by byte.

documentation/design.md section 7.5. The oracle image holds one font with color
strikes (Noto Color Emoji: `CBDT`, one strike, index format 1, image format 17, every
picture an RGBA PNG) and none with `sbix`. The other cells of the grid - the other
index formats under a PNG, image formats 18 and 19, several strikes, a glyph in range
and absent, and every PNG color type the encoder offers - are here and nowhere else.

The tables are assembled by hand, as the `EBLC` fixtures are, for the same reason: a
writer picks one cell of the cross-product. The PNGs are Pillow's. fontTools reads
the result back in `tools/oracle/strike_image_diff.py`, so the bytes are checkable
without this file being the only opinion about them.
"""

import io
import struct
import zlib

from PIL import Image

ORDER = [".notdef"] + ["g%02d" % i for i in range(1, 12)]


def picture(glyph, width, height, kind):
    """A small picture, different for every glyph, in one of the PNG color types.

    Distinct per glyph (a stripe at row `glyph % height`, and a color that moves with
    the glyph) so a glyph that read its neighbor's bytes is a different picture.
    """
    stripe = glyph % height
    if kind == "rgba":
        image = Image.new("RGBA", (width, height))
        for y in range(height):
            for x in range(width):
                alpha = 255 if (x + y) % 5 else 90
                image.putpixel((x, y), ((glyph * 37 + x * 9) % 256,
                                        (glyph * 11 + y * 17) % 256,
                                        200 if y == stripe else 30, alpha))
    elif kind == "rgb":
        image = Image.new("RGB", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), ((glyph * 29 + x * 13) % 256,
                                        255 if y == stripe else (y * 20) % 256,
                                        (x * y * 3 + glyph) % 256))
    elif kind == "palette":
        # Four entries, the first fully transparent and the second half: the
        # tRNS chunk, which the decoder has to fold into alpha.
        image = Image.new("P", (width, height))
        image.putpalette([0, 0, 0, 200, 40, 40, 40, 200, 40, 40, 40, 200])
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), (x + y + glyph) % 4)
        image.info["transparency"] = bytes([0, 128])
    elif kind == "gray":
        image = Image.new("L", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), 255 if y == stripe else (x * 23 + glyph) % 256)
    elif kind == "graya":
        image = Image.new("LA", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), ((x * 31 + glyph) % 256, 255 if x % 3 else 64))
    elif kind == "bilevel":
        image = Image.new("1", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), 1 if (x + y + glyph) % 3 == 0 else 0)
    else:
        raise ValueError(kind)
    out = io.BytesIO()
    # No optimize pass and no metadata: the bytes are what Pillow's zlib level 6
    # writes, which the pinned image makes the same on every machine.
    image.save(out, format="PNG", optimize=False)
    return out.getvalue()


def line_metrics(ascender, descender, width_max):
    return struct.pack(">bbBbbbbbbbbb", ascender, descender, width_max,
                       1, 0, 0, 0, 0, ascender, descender, 0, 0)


def small(height, width, bearing_x, bearing_y, advance):
    return struct.pack(">BBbbB", height, width, bearing_x, bearing_y, advance)


def big(height, width, bearing_x, bearing_y, advance):
    return struct.pack(">BBbbBbbB", height, width, bearing_x, bearing_y, advance,
                       0, 0, 0)


KINDS = ["rgba", "rgb", "palette", "gray", "graya", "bilevel", "rgba", "rgb",
         "palette", "rgba", "gray"]


def build_color_cbdt(mf, out):
    """Two color strikes; every index subtable format under a PNG image format.

    Strike 0 (16 ppem), glyphs 1 to 9, one subtable per cell:

      glyphs 1-2   index 1, image 17   offsets and small metrics
      glyphs 3-4   index 3, image 18   16-bit offsets and big metrics
      glyphs 5-6   index 5, image 19   sparse and constant: metrics in CBLC, every
                                       glyph padded to one size
      glyphs 7-9   index 4, image 17   sparse offsets; glyph 8 is **in range and
                                       absent**

    Strike 1 (24 ppem), glyphs 1 to 4:

      glyphs 1-2   index 2, image 19   constant size, metrics in CBLC
      glyphs 3-4   index 1, image 17   with bearings of both signs

    Glyphs 10 and 11 are in no strike at all. The pictures cover every PNG color
    type Pillow writes (RGBA, RGB, palette with transparency, grey, grey with alpha,
    one bit), so the decode to RGBA is held to a second decoder for each.
    """
    mapping = {0x41 + i: name for i, name in enumerate(ORDER[1:])}
    fb = mf.truetype("Color Strikes", [mf.subtable(4, 3, 1, mapping)], order=ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in ORDER},
                     advances={name: 600 for name in ORDER})

    cbdt = bytearray(struct.pack(">I", 0x00030000))
    strikes = []  # (ppem, first, last, subtables, ascent, descent)

    def png(glyph, width, height):
        return picture(glyph, width, height, KINDS[glyph - 1])

    def glyph17(glyph, width, height, bx, by, adv):
        data = png(glyph, width, height)
        return small(height, width, bx, by, adv) + struct.pack(">I", len(data)) + data

    def glyph18(glyph, width, height, bx, by, adv):
        data = png(glyph, width, height)
        return big(height, width, bx, by, adv) + struct.pack(">I", len(data)) + data

    # ---- strike 0
    subtables = []

    at = len(cbdt)
    offsets = [0]
    for g in (1, 2):
        cbdt += glyph17(g, 10, 12, 1, 11, 12)
        offsets.append(len(cbdt) - at)
    subtables.append((1, 2, struct.pack(">HHI", 1, 17, at)
                      + b"".join(struct.pack(">I", o) for o in offsets)))

    at = len(cbdt)
    offsets = [0]
    for g in (3, 4):
        cbdt += glyph18(g, 9, 11, -2, 10, 13)
        offsets.append(len(cbdt) - at)
    body = struct.pack(">HHI", 3, 18, at) + b"".join(struct.pack(">H", o) for o in offsets)
    if len(offsets) % 2:
        body += b"\x00\x00"
    subtables.append((3, 4, body))

    # Format 19 under index 5: every glyph padded to the size of the largest, with
    # its own dataLen saying where the PNG stops.
    at = len(cbdt)
    pngs = [png(g, 8, 8) for g in (5, 6)]
    size = max(len(p) for p in pngs) + 4
    for p in pngs:
        cbdt += struct.pack(">I", len(p)) + p + b"\x00" * (size - 4 - len(p))
    body = (struct.pack(">HHI", 5, 19, at) + struct.pack(">I", size)
            + big(8, 8, 0, 8, 9) + struct.pack(">I", 2) + struct.pack(">HH", 5, 6))
    subtables.append((5, 6, body))

    # Index 4: glyph 8 is in the range 7..9 and not in the list.
    at = len(cbdt)
    pairs = []
    for g in (7, 9):
        pairs.append((g, len(cbdt) - at))
        cbdt += glyph17(g, 7, 9, 0, 9, 8)
    pairs.append((0xFFFF, len(cbdt) - at))
    subtables.append((7, 9, struct.pack(">HHI", 4, 17, at) + struct.pack(">I", 2)
                      + b"".join(struct.pack(">HH", g, o) for g, o in pairs)))
    strikes.append((16, 1, 9, subtables, 14, -4))

    # ---- strike 1
    subtables = []
    at = len(cbdt)
    pngs = [png(g, 16, 18) for g in (1, 2)]
    size = max(len(p) for p in pngs) + 4
    for p in pngs:
        cbdt += struct.pack(">I", len(p)) + p + b"\x00" * (size - 4 - len(p))
    subtables.append((1, 2, struct.pack(">HHI", 2, 19, at) + struct.pack(">I", size)
                      + big(18, 16, 2, 17, 19)))
    at = len(cbdt)
    offsets = [0]
    for g, (bx, by) in zip((3, 4), ((-3, 20), (4, -1))):
        cbdt += glyph17(g, 14, 15, bx, by, 17)
        offsets.append(len(cbdt) - at)
    subtables.append((3, 4, struct.pack(">HHI", 1, 17, at)
                      + b"".join(struct.pack(">I", o) for o in offsets)))
    strikes.append((24, 1, 4, subtables, 21, -6))

    # ---- CBLC: header, the bitmapSizeTables, then each strike's index region.
    base = 8 + 48 * len(strikes)
    size_tables = bytearray()
    regions = bytearray()
    for ppem, first, last, subtables, ascent, descent in strikes:
        array = bytearray()
        cursor = 8 * len(subtables)
        for lo, hi, body in subtables:
            array += struct.pack(">HHI", lo, hi, cursor)
            cursor += len(body)
        region = bytes(array) + b"".join(body for _, _, body in subtables)
        size_tables += (struct.pack(">IIII", base + len(regions), len(region),
                                    len(subtables), 0)
                        + line_metrics(ascent, descent, 20)
                        + line_metrics(0, 0, 0)
                        + struct.pack(">HHBBBb", first, last, ppem, ppem, 32, 1))
        regions += region
    cblc = struct.pack(">II", 0x00030000, len(strikes)) + bytes(size_tables) + bytes(regions)

    mf.set_raw_table(fb, "CBLC", cblc)
    mf.set_raw_table(fb, "CBDT", bytes(cbdt))
    fb.save(out)


METRIC_ORDER = [".notdef"] + ["m%03d" % i for i in range(1, 128)]


def build_color_cbdt_metrics(mf, out):
    """Two strikes whose metrics sweep every value a byte can state.

    A glyph's box is the strike's metrics scaled to the em, and the scaling rounds.
    Ties fall at 16 ppem in a 1000-unit em (a pixel is 62.5 units), and fractions of
    every kind at 7; the metrics here run through the signed and unsigned byte
    ranges, so the rounding is tried on the values it will meet and the ones it
    will not. Every picture is one pixel: the metrics are what is held, and they
    are independent of the picture they sit over.
    """
    mapping = {0x100 + i: name for i, name in enumerate(METRIC_ORDER[1:])}
    fb = mf.truetype("CBDT Metrics", [mf.subtable(4, 3, 1, mapping)],
                     order=METRIC_ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in METRIC_ORDER},
                     advances={name: 600 for name in METRIC_ORDER})
    cbdt = bytearray(struct.pack(">I", 0x00030000))
    strikes = []
    for ppem, flip in ((16, 1), (7, -1)):
        at = len(cbdt)
        offsets = [0]
        for g in range(1, 128):
            data = picture(g, 1, 1, "rgba")
            cbdt += (small((g * 37) % 256, g * 2, flip * (g - 64), flip * (2 * g - 128), 8)
                     + struct.pack(">I", len(data)) + data)
            offsets.append(len(cbdt) - at)
        body = struct.pack(">HHI", 1, 17, at) + b"".join(struct.pack(">I", o) for o in offsets)
        strikes.append((ppem, body))
    base = 8 + 48 * len(strikes)
    size_tables = bytearray()
    regions = bytearray()
    for ppem, body in strikes:
        region = struct.pack(">HHI", 1, 127, 8) + body
        size_tables += (struct.pack(">IIII", base + len(regions), len(region), 1, 0)
                        + line_metrics(ppem, -2, ppem) + line_metrics(0, 0, 0)
                        + struct.pack(">HHBBBb", 1, 127, ppem, ppem, 32, 1))
        regions += region
    cblc = struct.pack(">II", 0x00030000, len(strikes)) + bytes(size_tables) + bytes(regions)
    mf.set_raw_table(fb, "CBLC", cblc)
    mf.set_raw_table(fb, "CBDT", bytes(cbdt))
    fb.save(out)


SIZES_ORDER = [".notdef"] + ["s%02d" % i for i in range(1, 7)]


def build_color_cbdt_sizes(mf, out):
    """Which strike answers, and what a glyph it lacks falls back to.

    Four strikes, all index format 1 under image format 17, all of one-pixel
    pictures (the metrics are what is held):

      strike 0   24 x 12 ppem   glyphs 1, 2, 3, 4
      strike 1   20 x 20 ppem   glyphs 1, 2, 5 (3 and 4 are in its range, empty)
      strike 2   24 x 12 ppem   glyph 1, with other metrics: equal to strike 0 in
                                size, so it is the one that must lose
      strike 3   10 x 30 ppem   glyphs 1, 2

    Strike 0 is wider than it is high, strike 3 the other way, so a measure that
    scaled an axis by the other's ppem, or that chose by one ppem and not the larger
    of the two, answers differently. Glyphs 1, 4 and 6 have outlines: glyph 1 has a
    picture as well and the picture must win, glyph 4 has one only in strike 0 and
    glyph 5 only in strike 1, so a size that picks the other strike leaves each to
    its outline, and glyph 6 has no picture at all.
    """
    mapping = {0x41 + i: name for i, name in enumerate(SIZES_ORDER[1:])}
    square = [[(100, 100, True), (500, 100, True), (500, 500, True), (100, 500, True)]]
    tall = [[(50, -100, True), (250, -100, True), (250, 900, True), (50, 900, True)]]
    outlines = {"s01": square, "s04": tall, "s05": square, "s06": tall}
    fb = mf.truetype("CBDT Sizes", [mf.subtable(4, 3, 1, mapping)], order=SIZES_ORDER,
                     glyphs={name: mf.raw_glyph(outlines.get(name, []))
                             for name in SIZES_ORDER},
                     advances={name: 600 for name in SIZES_ORDER})
    cbdt = bytearray(struct.pack(">I", 0x00030000))
    specs = [
        ((24, 12), {1: (8, 8, 0, 8, 8), 2: (7, 5, -1, 6, 9), 3: (4, 4, 2, 3, 5),
                    4: (3, 9, 1, 8, 4)}),
        ((20, 20), {1: (6, 6, 1, 6, 7), 2: (9, 3, 0, 4, 10), 5: (5, 5, 0, 5, 6)}),
        ((24, 12), {1: (2, 2, 0, 2, 3)}),
        ((10, 30), {1: (8, 8, 3, 8, 9), 2: (4, 7, -2, 5, 6)}),
    ]
    strikes = []
    for (px, py), glyphs in specs:
        at = len(cbdt)
        offsets = [0]
        first, last = min(glyphs), max(glyphs)
        # Index format 1 covers a contiguous range; a glyph in it the strike lacks is
        # an entry of no bytes (two equal offsets), which is how this format says so.
        for g in range(first, last + 1):
            if g in glyphs:
                height, width, bx, by, adv = glyphs[g]
                data = picture(g, 1, 1, "rgba")
                cbdt += (small(height, width, bx, by, adv)
                         + struct.pack(">I", len(data)) + data)
            offsets.append(len(cbdt) - at)
        body = struct.pack(">HHI", 1, 17, at) + b"".join(struct.pack(">I", o) for o in offsets)
        strikes.append((px, py, first, last, body))
    base = 8 + 48 * len(strikes)
    size_tables = bytearray()
    regions = bytearray()
    for px, py, first, last, body in strikes:
        region = struct.pack(">HHI", first, last, 8) + body
        size_tables += (struct.pack(">IIII", base + len(regions), len(region), 1, 0)
                        + line_metrics(py, -2, px) + line_metrics(0, 0, 0)
                        + struct.pack(">HHBBBb", first, last, px, py, 32, 1))
        regions += region
    cblc = struct.pack(">II", 0x00030000, len(strikes)) + bytes(size_tables) + bytes(regions)
    mf.set_raw_table(fb, "CBLC", cblc)
    mf.set_raw_table(fb, "CBDT", bytes(cbdt))
    fb.save(out)


SBIX_ORDER = [".notdef"] + ["x%02d" % i for i in range(1, 10)]


def jpeg_picture(width, height):
    """A small JPEG: Pillow's encoder at quality 90, 4:4:4, so the chroma is not
    resampled and two decoders have less room to differ."""
    image = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), ((x * 255) // max(1, width - 1),
                                    (y * 255) // max(1, height - 1), 128))
    out = io.BytesIO()
    image.save(out, format="JPEG", quality=90, subsampling=0)
    return out.getvalue()


def tiff_picture(width, height):
    """An uncompressed RGBA TIFF."""
    image = Image.new("RGBA", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), ((x * 31) % 256, (y * 47) % 256, 99,
                                    255 if (x + y) % 4 else 100))
    out = io.BytesIO()
    image.save(out, format="TIFF", compression=None)
    return out.getvalue()


def sbix_glyph(kind, origin_x, origin_y, payload):
    return struct.pack(">hh4s", origin_x, origin_y, kind) + payload


def build_color_sbix(mf, out):
    """Apple's `sbix`: strikes of pictures, three formats, `dupe` and a hole.

    Two strikes, in the order 40 then 20 ppem (so an index is not a sorted
    position), over nine glyphs:

      glyph 1   png    RGBA, origin (1, -2)
      glyph 2   png    RGB, origin (-3, 0)
      glyph 3   dupe   of glyph 1
      glyph 4   dupe   of glyph 3: a chain, which must end at the picture
      glyph 5   jpg    a JPEG, decoded by image's JPEG codec
      glyph 6   tiff   an uncompressed RGBA TIFF
      glyph 7   mask   a format this library declines by name
      glyph 8   (none) no bytes: the strike does not carry it
      glyph 9   dupe   of itself: a cycle, which is refused and does not hang

    The 40 ppem strike has bigger pictures for glyphs 1 and 2 and a `dupe` at glyph
    3 of glyph 2, so the same record means a different picture in each strike.
    """
    mapping = {0x41 + i: name for i, name in enumerate(SBIX_ORDER[1:])}
    fb = mf.truetype("Sbix", [mf.subtable(4, 3, 1, mapping)], order=SBIX_ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in SBIX_ORDER},
                     advances={name: 600 for name in SBIX_ORDER})
    count = len(SBIX_ORDER)

    def strike(ppem, ppi, glyphs):
        """The strike's bytes: ppem, ppi, then count + 1 offsets, then the glyphs."""
        head = 4 + 4 * (count + 1)
        body = bytearray()
        offsets = []
        for g in range(count):
            offsets.append(head + len(body))
            if g in glyphs:
                body += glyphs[g]
        offsets.append(head + len(body))
        return (struct.pack(">HH", ppem, ppi)
                + b"".join(struct.pack(">I", o) for o in offsets) + bytes(body))

    def dupe(target):
        return sbix_glyph(b"dupe", 0, 0, struct.pack(">H", target))

    small = {
        1: sbix_glyph(b"png ", 1, -2, picture(1, 10, 12, "rgba")),
        2: sbix_glyph(b"png ", -3, 0, picture(2, 8, 8, "rgb")),
        3: dupe(1),
        4: dupe(3),
        5: sbix_glyph(b"jpg ", 0, 4, jpeg_picture(16, 16)),
        6: sbix_glyph(b"tiff", 2, 2, tiff_picture(8, 6)),
        7: sbix_glyph(b"mask", 0, 0, b"\x01\x02\x03\x04"),
        9: dupe(9),
    }
    large = {
        1: sbix_glyph(b"png ", 2, -4, picture(1, 20, 24, "rgba")),
        2: sbix_glyph(b"png ", -6, 0, picture(2, 16, 16, "rgb")),
        3: dupe(2),
    }
    strikes = [strike(40, 72, large), strike(20, 72, small)]
    head = 8 + 4 * len(strikes)
    offsets = []
    body = b""
    for s in strikes:
        offsets.append(head + len(body))
        body += s
    table = (struct.pack(">HHI", 1, 1, len(strikes))
             + b"".join(struct.pack(">I", o) for o in offsets) + body)
    mf.set_raw_table(fb, "sbix", table)
    fb.save(out)


SBIX_METRICS_ORDER = [".notdef"] + ["n%03d" % i for i in range(1, 128)]


def solid_png(width, height):
    """A PNG of one color: its size is what is held, and it compresses to nothing."""
    out = io.BytesIO()
    Image.new("RGBA", (width, height), (200, 40, 90, 255)).save(out, format="PNG")
    return out.getvalue()


def build_color_sbix_metrics(mf, out):
    """Two `sbix` strikes (16 and 7 ppem) whose pictures and origins sweep a range.

    A glyph's box is the PNG's size and the record's origin scaled from the strike's
    pixels to the em, and the scaling rounds, with halves at 16 ppem in a 1000-unit
    em. Widths run 1 to 127, heights are scattered, origins run through both signs, and
    nothing is large enough to overflow the 16 bits HarfBuzz keeps a box in.
    """
    mapping = {0x100 + i: name for i, name in enumerate(SBIX_METRICS_ORDER[1:])}
    fb = mf.truetype("Sbix Metrics", [mf.subtable(4, 3, 1, mapping)],
                     order=SBIX_METRICS_ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in SBIX_METRICS_ORDER},
                     advances={name: 600 for name in SBIX_METRICS_ORDER})
    count = len(SBIX_METRICS_ORDER)
    strikes = []
    for ppem, flip in ((16, 1), (7, -1)):
        glyphs = {g: sbix_glyph(b"png ", flip * (g - 64), flip * (g - 64),
                                solid_png(g, (g * 37) % 100 + 1))
                  for g in range(1, 128)}
        head = 4 + 4 * (count + 1)
        body = bytearray()
        offsets = []
        for g in range(count):
            offsets.append(head + len(body))
            if g in glyphs:
                body += glyphs[g]
        offsets.append(head + len(body))
        strikes.append(struct.pack(">HH", ppem, 72)
                       + b"".join(struct.pack(">I", o) for o in offsets) + bytes(body))
    head = 8 + 4 * len(strikes)
    offsets = []
    body = b""
    for s in strikes:
        offsets.append(head + len(body))
        body += s
    mf.set_raw_table(fb, "sbix", struct.pack(">HHI", 1, 1, len(strikes))
                     + b"".join(struct.pack(">I", o) for o in offsets) + body)
    fb.save(out)


DUPE_ORDER = [".notdef"] + ["d%03d" % i for i in range(1, 300)]


def build_color_sbix_dupes(mf, out):
    """`sbix` dupe chains, and a dupe whose target needs both bytes of its glyph id.

    Glyph 1 is a picture and glyph k of 2 to 12 is a dupe of k - 1, so glyph 9 is
    eight hops from the picture and glyph 10 is nine: HarfBuzz follows eight and
    reads the ninth as empty. Glyph 270 is a picture and glyph 280 a dupe of it, an
    id above 255. Glyph 20 dupes itself.
    """
    mapping = {0x100 + i: name for i, name in enumerate(DUPE_ORDER[1:])}
    fb = mf.truetype("Sbix Dupes", [mf.subtable(4, 3, 1, mapping)], order=DUPE_ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in DUPE_ORDER},
                     advances={name: 600 for name in DUPE_ORDER})
    count = len(DUPE_ORDER)
    glyphs = {1: sbix_glyph(b"png ", 0, 0, solid_png(8, 8)),
              270: sbix_glyph(b"png ", 3, 1, solid_png(6, 5)),
              280: sbix_glyph(b"dupe", 0, 0, struct.pack(">H", 270)),
              20: sbix_glyph(b"dupe", 0, 0, struct.pack(">H", 20))}
    for k in range(2, 13):
        glyphs[k] = sbix_glyph(b"dupe", 0, 0, struct.pack(">H", k - 1))
    head = 4 + 4 * (count + 1)
    body = bytearray()
    offsets = []
    for g in range(count):
        offsets.append(head + len(body))
        if g in glyphs:
            body += glyphs[g]
    offsets.append(head + len(body))
    strike = (struct.pack(">HH", 20, 72) + b"".join(struct.pack(">I", o) for o in offsets)
              + bytes(body))
    mf.set_raw_table(fb, "sbix", struct.pack(">HHII", 1, 1, 1, 12) + strike)
    fb.save(out)


def png16(width, height, color_type, sample):
    """A 16-bit PNG written by hand: Pillow cannot write RGBA at 16 bits.

    `sample(x, y, c)` gives channel `c` of a pixel as 0..65535. Color type 0 is grey
    (one channel), 2 is RGB, 4 grey with alpha, 6 RGBA. Filter 0 on every row, so the
    decoded samples are exactly what `sample` returned - which is the oracle for the
    pixels: no other decoder is asked.
    """
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            for c in range(channels):
                rows += struct.pack(">H", sample(x, y, c))

    def chunk(kind, data):
        body = kind + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 16, color_type, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


DEEP_ORDER = [".notdef"] + ["h%02d" % i for i in range(1, 6)]


def deep_sample(glyph, x, y, c):
    """The samples of the 16-bit pictures: every channel a different, wide value, so a
    decoder that kept only the high byte, or swapped the bytes, is a different picture."""
    return (glyph * 4099 + x * 7919 + y * 12289 + c * 20011) % 65536


def build_color_cbdt_deep(mf, out):
    """16-bit PNGs in a CBDT strike: RGBA, RGB, grey, grey with alpha, and an 8-bit one.

    Glyphs 1 to 4 are the four 16-bit color types at 6 x 5 pixels, 5 is an 8-bit RGBA
    so that one strike holds both depths. Index format 1 under image format 17, one
    strike at 20 ppem.
    """
    mapping = {0x41 + i: name for i, name in enumerate(DEEP_ORDER[1:])}
    fb = mf.truetype("CBDT Deep", [mf.subtable(4, 3, 1, mapping)], order=DEEP_ORDER,
                     glyphs={name: mf.raw_glyph([]) for name in DEEP_ORDER},
                     advances={name: 600 for name in DEEP_ORDER})
    cbdt = bytearray(struct.pack(">I", 0x00030000))
    at = len(cbdt)
    offsets = [0]
    for glyph, color_type in ((1, 6), (2, 2), (3, 0), (4, 4)):
        data = png16(6, 5, color_type, lambda x, y, c, g=glyph: deep_sample(g, x, y, c))
        cbdt += small(5, 6, 0, 5, 7) + struct.pack(">I", len(data)) + data
        offsets.append(len(cbdt) - at)
    data = picture(5, 6, 5, "rgba")
    cbdt += small(5, 6, 0, 5, 7) + struct.pack(">I", len(data)) + data
    offsets.append(len(cbdt) - at)
    body = struct.pack(">HHI", 1, 17, at) + b"".join(struct.pack(">I", o) for o in offsets)
    region = struct.pack(">HHI", 1, 5, 8) + body
    cblc = (struct.pack(">II", 0x00030000, 1)
            + struct.pack(">IIII", 8 + 48, len(region), 1, 0)
            + line_metrics(5, -1, 7) + line_metrics(0, 0, 0)
            + struct.pack(">HHBBBb", 1, 5, 20, 20, 32, 1) + region)
    mf.set_raw_table(fb, "CBLC", cblc)
    mf.set_raw_table(fb, "CBDT", bytes(cbdt))
    fb.save(out)
