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
  * `color-v0-edges.ttf` - version 0 layers whose boxes HarfBuzz measures in
    unexpected ways: empty layers, a scaled composite, and a side bearing that is
    not the outline's.
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


def build_color_v0_edges(mf, out):
    """COLR version 0 layers HarfBuzz measures by what they draw.

    `A` is a drawn layer and an empty one, `B` has only empty layers, and `space`
    is one layer that is a composite scaled by 0.719970703125 (a factor `glyf` stores
    exactly, so the corner it draws is fractional and its header box is not). Every
    glyph's left side bearing in `hmtx` is 0, which is not its outline's xMin, so each
    layer is also drawn shifted.
    """
    order = ORDER + ["layer.empty", "layer.scaled"]
    shapes = _glyphs_for(mf, ORDER)
    shapes["layer.empty"] = TTGlyphPen(None).glyph()
    pen = TTGlyphPen(shapes)
    pen.addComponent("layer.a", (0.719970703125, 0, 0, 0.719970703125, 16, 202))
    shapes["layer.scaled"] = pen.glyph()
    advances = dict(mf.ADVANCES)
    for name in LAYERS + ["layer.empty", "layer.scaled"]:
        advances[name] = 600
    fb = mf.truetype("Color v0 edges", mf.unicode_cmap(), order=order, glyphs=shapes,
                     advances=advances)
    glyph_map = fb.font.getReverseGlyphMap()
    layers = {
        "A": [("layer.a", 0), ("layer.empty", 1)],
        "B": [("layer.empty", 0), ("layer.empty", 1)],
        "space": [("layer.scaled", 0)],
    }
    fb.font["COLR"] = buildCOLR(layers, version=0, glyphMap=glyph_map)
    fb.font["CPAL"] = buildCPAL(_floats(PALETTES[:1]), nameTable=fb.font["name"])
    # Vertical metrics, so a test can place the vertical origin: HarfBuzz derives it
    # from the glyph's own outline box, not from the color box above.
    fb.setupVerticalMetrics({n: (1000, 30) for n in order})
    fb.setupVerticalHeader(ascent=500, descent=-500)
    mf.pin(fb)
    fb.save(out)


def _glyphs_for(mf, names):
    shapes = {}
    for name in names:
        pen = TTGlyphPen(None)
        source = {"layer.a": "A", "layer.b": "B", "layer.c": "ghoti.alt"}.get(name, name)
        mf.OUTLINES[source](pen)
        shapes[name] = pen.glyph()
    return shapes


# --------------------------------------------------------------- COLR version 1

from fontTools.ttLib.tables import otTables as ot

PF = ot.PaintFormat
LINE3 = {"Extend": "pad", "ColorStop": [
    {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
    {"StopOffset": 0.5, "PaletteIndex": 1, "Alpha": 0.5},
    {"StopOffset": 1.0, "PaletteIndex": 2, "Alpha": 0.25}]}
LINE2 = {"Extend": "repeat", "ColorStop": [
    {"StopOffset": 0.25, "PaletteIndex": 2, "Alpha": 1.0},
    {"StopOffset": 0.75, "PaletteIndex": 0xFFFF, "Alpha": 0.75}]}
LINE_REFLECT = {"Extend": "reflect", "ColorStop": [
    {"StopOffset": 0.0, "PaletteIndex": 1, "Alpha": 1.0},
    {"StopOffset": 1.0, "PaletteIndex": 0, "Alpha": 1.0}]}


def solid(index=0, alpha=1.0):
    return {"Format": PF.PaintSolid, "PaletteIndex": index, "Alpha": alpha}


def glyph(name, paint):
    return {"Format": PF.PaintGlyph, "Glyph": name, "Paint": paint}


def v1_trees():
    """The static graph: every non-variable paint format once."""
    gradients = [
        glyph("layer.a", solid(0, 0.5)),
        glyph("layer.b", {"Format": PF.PaintLinearGradient, "ColorLine": LINE3,
                          "x0": 0, "y0": 0, "x1": 600, "y1": 0, "x2": 0, "y2": 600}),
        glyph("layer.c", {"Format": PF.PaintRadialGradient, "ColorLine": LINE2,
                          "x0": 300, "y0": 300, "r0": 0, "x1": 300, "y1": 300,
                          "r1": 400}),
        glyph("layer.a", {"Format": PF.PaintSweepGradient, "ColorLine": LINE_REFLECT,
                          "centerX": 300, "centerY": 300, "startAngle": 0.0,
                          "endAngle": 90.0}),
    ]
    inner = glyph("layer.a", solid(1))
    chain = inner
    wrappers = [
        lambda p: {"Format": PF.PaintSkewAroundCenter, "Paint": p, "xSkewAngle": 22.5,
                   "ySkewAngle": -11.25, "centerX": 100, "centerY": 200},
        lambda p: {"Format": PF.PaintSkew, "Paint": p, "xSkewAngle": 45.0,
                   "ySkewAngle": 0.0},
        lambda p: {"Format": PF.PaintRotateAroundCenter, "Paint": p, "angle": 45.0,
                   "centerX": 50, "centerY": -50},
        lambda p: {"Format": PF.PaintRotate, "Paint": p, "angle": -90.0},
        lambda p: {"Format": PF.PaintScaleUniformAroundCenter, "Paint": p,
                   "scale": 0.5, "centerX": 10, "centerY": 20},
        lambda p: {"Format": PF.PaintScaleUniform, "Paint": p, "scale": 1.5},
        lambda p: {"Format": PF.PaintScaleAroundCenter, "Paint": p, "scaleX": 0.75,
                   "scaleY": 1.25, "centerX": 300, "centerY": 0},
        lambda p: {"Format": PF.PaintScale, "Paint": p, "scaleX": 1.75, "scaleY": 0.5},
        lambda p: {"Format": PF.PaintTranslate, "Paint": p, "dx": -30, "dy": 40},
        lambda p: {"Format": PF.PaintTransform, "Paint": p,
                   "Transform": (1.0, 0.5, -0.25, 1.0, 100.0, -200.0)},
    ]
    for wrap in wrappers:
        chain = wrap(chain)
    composite = {"Format": PF.PaintComposite, "SourcePaint": {
                     "Format": PF.PaintColrGlyph, "Glyph": "A"},
                 "CompositeMode": "multiply",
                 "BackdropPaint": glyph("layer.b", solid(2, 0.75))}
    return {"A": {"Format": PF.PaintColrLayers, "Layers": gradients},
            "B": chain, "ghoti.alt": composite}


def build_color_v1(mf, out):
    fb = _font(mf, "Color v1", PALETTES)
    order = fb.font.getReverseGlyphMap()
    clip = (0, -100, 700, 900)
    fb.font["COLR"] = buildCOLR(v1_trees(), version=1, glyphMap=order,
                                clipBoxes={"A": clip, "B": clip, "ghoti.alt": clip})
    mf.pin(fb)
    fb.save(out)


# The variable graph: the same shapes in their Var forms, each with a
# VarIndexBase into a store of fifty rows over the weight and width regions.
VAR_ROWS = [((i * 7) % 23 - 11, (i * 5) % 17 - 8) for i in range(64)]


def v1_var_trees():
    base = {"n": 0}

    def at(count=1):
        index = base["n"]
        base["n"] += count
        return index

    line = {"Extend": "pad", "ColorStop": [
        {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0, "VarIndexBase": at(2)},
        {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 0.5, "VarIndexBase": at(2)}]}
    solid_var = {"Format": PF.PaintVarSolid, "PaletteIndex": 2, "Alpha": 0.5,
                 "VarIndexBase": at(1)}
    paints = [
        glyph("ring", {"Format": PF.PaintVarLinearGradient, "ColorLine": line,
                       "x0": 0, "y0": 0, "x1": 600, "y1": 0, "x2": 0, "y2": 600,
                       "VarIndexBase": at(6)}),
        glyph("dot", {"Format": PF.PaintVarRadialGradient, "ColorLine": line,
                      "x0": 300, "y0": 300, "r0": 10, "x1": 300, "y1": 300, "r1": 400,
                      "VarIndexBase": at(6)}),
        glyph("bar", {"Format": PF.PaintVarSweepGradient, "ColorLine": line,
                      "centerX": 300, "centerY": 300, "startAngle": 0.0,
                      "endAngle": 90.0, "VarIndexBase": at(4)}),
    ]
    chain = glyph("tri", solid_var)
    for fmt, extra, count in [
            (PF.PaintVarSkewAroundCenter, {"xSkewAngle": 22.5, "ySkewAngle": 11.25,
                                           "centerX": 100, "centerY": 200}, 4),
            (PF.PaintVarSkew, {"xSkewAngle": 45.0, "ySkewAngle": 0.0}, 2),
            (PF.PaintVarRotateAroundCenter, {"angle": 45.0, "centerX": 50,
                                             "centerY": -50}, 3),
            (PF.PaintVarRotate, {"angle": -90.0}, 1),
            (PF.PaintVarScaleUniformAroundCenter, {"scale": 0.5, "centerX": 10,
                                                   "centerY": 20}, 3),
            (PF.PaintVarScaleUniform, {"scale": 1.5}, 1),
            (PF.PaintVarScaleAroundCenter, {"scaleX": 0.75, "scaleY": 1.25,
                                            "centerX": 300, "centerY": 0}, 4),
            (PF.PaintVarScale, {"scaleX": 1.75, "scaleY": 0.5}, 2),
            (PF.PaintVarTranslate, {"dx": -30, "dy": 40}, 2)]:
        chain = dict(Format=fmt, Paint=chain, VarIndexBase=at(count), **extra)
    # A variable matrix keeps its base in the matrix, not in the paint.
    chain = {"Format": PF.PaintVarTransform, "Paint": chain,
             "Transform": {"xx": 1.0, "yx": 0.5, "xy": -0.25, "yy": 1.0,
                           "dx": 100.0, "dy": -200.0, "VarIndexBase": at(6)}}
    assert base["n"] <= len(VAR_ROWS), base
    return {"tri": {"Format": PF.PaintColrLayers, "Layers": paints},
            "bar": chain}, base["n"]


def build_color_v1_var(mf, out, mapped=False):
    from fontTools.varLib import builder as vb
    fb = mf._variable_font("Color v1 var")
    font = fb.font
    trees, used = v1_var_trees()
    store = mf._var_store([([0, 1], [list(r) for r in VAR_ROWS])])
    order = font.getReverseGlyphMap()
    index_map = None
    if mapped == "none":
        # Every entry is 0xFFFF:0xFFFF, the map's own "no variation": all nine
        # entries fixed, so a moved location must read the stored numbers.
        index_map = vb.buildVarIdxMap([0xFFFFFFFF] * len(VAR_ROWS), order)
    elif mapped:
        # Indexes into the map are the VarIndexBases; the map sends each to the
        # row in reverse, so a reader that ignored it would read the wrong delta.
        index_map = vb.buildVarIdxMap([(0 << 16) | (len(VAR_ROWS) - 1 - i)
                                       for i in range(len(VAR_ROWS))], order)
    clip_a = (0, -100, 700, 900, 5) if not mapped else (0, -100, 700, 900, 5)
    font["COLR"] = buildCOLR(trees, version=1, glyphMap=order, varStore=store,
                             varIndexMap=index_map,
                             clipBoxes={"tri": clip_a, "bar": (0, 0, 500, 500)})
    font["CPAL"] = buildCPAL(_floats(PALETTES), nameTable=font["name"])
    mf.pin(fb)
    fb.save(out)
