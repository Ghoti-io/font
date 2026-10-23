# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Where a `glyf` glyph's origin is, computed explicitly from fontTools' data.

Runs **inside** the pinned image, imported by `fonttools_outlines.py` and
`fonttools_variation.py`.

**Why this exists rather than asking fontTools to draw.** A glyph's points are
stored in its own design coordinates, whose origin is the first phantom point,
`xMin - lsb`, and a renderer moves the outline by the negative of it. fontTools'
glyph set does that for a *simple* glyph (`_TTGlyphGlyf._getGlyphAndOffset`,
`lsb - xMin`) and not for a composite: it passes the offset to `Glyph.draw()`,
which for a composite only adds components, and a component is drawn at depth
one, where the offset is zero. It does not implement USE_MY_METRICS in that path
either, and it applies `gvar`'s delta to the first phantom point even when the
font has an `HVAR`. FreeType and HarfBuzz move a composite by its own phantom
point (or the metrics-lending component's), and FreeType leaves the stored point
alone when the font has an `HVAR`. design.md section 7.3 has the measured table.

So this module holds fontTools' *data* (header `xMin`, `hmtx` bearing, component
flags, `gvar` phantom deltas) to the rules this library implements, which are
FreeType's: where fontTools itself speaks (a simple glyph, no `HVAR`), the caller
checks the rule against fontTools' own offset, and that check is what keeps this
from being the library's rule restated; where it does not, the FreeType leg of the
same gate is the independent witness.
"""

from fontTools.varLib.models import supportScalar

USE_MY_METRICS = 0x0200
MAX_DEPTH = 64


def stored(font, name):
    """`xMin - lsb` as the file states it (the default instance), in font units."""
    glyph = font["glyf"][name]
    x_min = glyph.xMin if hasattr(glyph, "xMin") else 0
    return x_min - font["hmtx"][name][1]


def phantom_delta(font, name, location):
    """`gvar`'s delta to the first phantom point at a normalised location."""
    if not location or "gvar" not in font:
        return 0.0
    total = 0.0
    for tuple_ in font["gvar"].variations.get(name, []):
        scalar = supportScalar(location, tuple_.axes)
        if not scalar:
            continue
        delta = tuple_.coordinates[-4] if len(tuple_.coordinates) >= 4 else None
        if delta is not None:
            total += delta[0] * scalar
    return total


def origin(font, name, location=None, depth=0):
    """The x of the glyph's first phantom point, in font units (a float).

    Simple glyph: stored, plus `gvar`'s delta unless the font has an `HVAR`.
    Composite: the last USE_MY_METRICS component's final point if there is one,
    else its own, as a simple glyph's. An empty glyph has no `xMin`: zero.
    """
    if depth > MAX_DEPTH:
        raise RecursionError("composite nesting")
    glyph = font["glyf"][name]
    if glyph.isComposite():
        borrowed = None
        for part in glyph.components:
            if part.flags & USE_MY_METRICS:
                borrowed = origin(font, part.glyphName, location, depth + 1)
        if borrowed is not None:
            return borrowed
    point = stored(font, name)
    if location and "HVAR" not in font:
        point += phantom_delta(font, name, location)
    return point
