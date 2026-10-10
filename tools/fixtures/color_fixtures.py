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
"""The color fixtures: `CPAL` palettes and `COLR` layers, written by fontTools.

documentation/design.md sections 7.6 and 14.5. The oracle image holds no font
with a `COLR` or `CPAL` table (the only color font in it, Noto Color Emoji, is
`CBDT`), so the real-font population for this feature is empty and the fixtures
are the whole of it. They are written by `fontTools.colorLib.builder`, which is
the point: the bytes are one implementation's opinion of the format, read back by
this library and, in `tools/oracle/color_diff.py`, by fontTools again.

  * `color-v0.ttf` - `COLR` version 0 with three base glyphs of one, two and
    three layers (one layer painted in the foreground color), over a `CPAL`
    version 1 with three palettes of three colors: light and dark usability
    flags, palette labels and entry labels, colors with every alpha.
  * `color-cpal0.ttf` - the same layers over a `CPAL` version 0, which has no
    types or labels, and two palettes that share color records.
"""

from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.pens.ttGlyphPen import TTGlyphPen

# Layer glyphs are plain outlines the base glyphs name; each is a different
# shape so a layer read as the wrong glyph is a different outline.
LAYERS = ["layer.a", "layer.b", "layer.c"]
ORDER = [".notdef", "space", "A", "B", "ghoti.alt"] + LAYERS

# (red, green, blue, alpha) in 8 bits; buildCPAL takes floats.
PALETTES = [
    [(255, 0, 0, 255), (0, 128, 0, 255), (0, 0, 255, 128)],
    [(16, 32, 48, 255), (200, 210, 220, 64), (1, 2, 3, 0)],
    [(255, 255, 255, 255), (128, 128, 128, 255), (0, 0, 0, 255)],
]

COLOR_LAYERS = {
    "A": [("layer.a", 0), ("layer.b", 1), ("layer.c", 0xFFFF)],
    "B": [("layer.b", 2)],
    "ghoti.alt": [("layer.c", 1), ("layer.a", 2)],
}


def _glyphs(mf):
    shapes = {}
    for name in ORDER:
        pen = TTGlyphPen(None)
        source = {"layer.a": "A", "layer.b": "B", "layer.c": "ghoti.alt"}.get(name, name)
        mf.OUTLINES[source](pen)
        shapes[name] = pen.glyph()
    return shapes


def _floats(palettes):
    return [[tuple(c / 255 for c in color) for color in palette]
            for palette in palettes]


def _font(mf, label, palettes, **cpal):
    advances = dict(mf.ADVANCES)
    for name in LAYERS:
        advances[name] = 600
    fb = mf.truetype(label, mf.unicode_cmap(), order=ORDER, glyphs=_glyphs(mf),
                     advances=advances)
    order = fb.font.getReverseGlyphMap()
    fb.font["COLR"] = buildCOLR(COLOR_LAYERS, version=0, glyphMap=order)
    fb.font["CPAL"] = buildCPAL(_floats(palettes), nameTable=fb.font["name"], **cpal)
    return fb


def build_color_v0(mf, out):
    fb = _font(mf, "Color v0", PALETTES,
               paletteTypes=[1, 2, 0], paletteLabels=["Light", "Dark", None],
               paletteEntryLabels=["Red", None, "Blue"])
    mf.pin(fb)
    fb.save(out)


def build_color_cpal0(mf, out):
    # Two palettes, the second equal to the first, so fontTools writes one run of
    # color records and both palettes point at it.
    fb = _font(mf, "Color CPAL0", [PALETTES[0], PALETTES[0]])
    mf.pin(fb)
    fb.save(out)
