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
  * `svg-docs.ttf` - an `SVG ` table of four documents over ranges and gaps: a plain one for
    glyphs 2 to 3, a gzip one for glyph 4, a plain one for glyphs 6 to 7, and a gzip one for
    glyph 0 (the `.notdef`), with glyph 5 and everything past 7 in no document.
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


# Story 14c: a graph made of what the scene lowers (shape-scene's N14 and fixed.h),
# and one glyph of each kind it refuses that color-v1.ttf does not already hold.
SCENE_EXTRA = ["paint.d", "paint.e", "paint.f", "paint.g", "paint.h", "paint.i", "paint.j",
               "paint.k", "paint.l", "paint.m", "paint.n", "paint.o", "paint.p", "paint.q", "paint.r"]


def v1_scene_trees():
    return {
        # Layers of solids, one the foreground (0xFFFF) at half alpha.
        "A": {"Format": PF.PaintColrLayers, "Layers": [
            glyph("layer.a", solid(0, 1.0)), glyph("layer.b", solid(0xFFFF, 0.5)),
            glyph("layer.c", solid(2, 0.25))]},
        # A glyph over a transform over a solid (a fill), under two transforms.
        "B": {"Format": PF.PaintTranslate, "dx": -30, "dy": 40, "Paint": {
            "Format": PF.PaintRotateAroundCenter, "angle": 45.0, "centerX": 50, "centerY": -50,
            "Paint": glyph("layer.a", {"Format": PF.PaintScale, "scaleX": 1.5, "scaleY": 0.5,
                                       "Paint": solid(1, 0.75)})}},
        # A Porter-Duff composite.
        "ghoti.alt": {"Format": PF.PaintComposite, "CompositeMode": "src_in",
                      "SourcePaint": glyph("layer.b", solid(1)),
                      "BackdropPaint": glyph("layer.a", solid(0))},
        # A glyph over layers: a clip around them.
        "paint.d": glyph("layer.c", {"Format": PF.PaintColrLayers, "Layers": [
            glyph("layer.a", solid(2)), glyph("layer.b", solid(0, 0.5))]}),
        # Another base glyph's graph, moved.
        "paint.e": {"Format": PF.PaintTranslate, "dx": 100, "dy": 0, "Paint": {
            "Format": PF.PaintColrGlyph, "Glyph": "A"}},
        # Refused: a blend mode, and a solid no glyph bounds.
        "paint.f": {"Format": PF.PaintComposite, "CompositeMode": "multiply",
                    "SourcePaint": glyph("layer.a", solid(0)),
                    "BackdropPaint": glyph("layer.b", solid(1))},
        "paint.g": solid(1),
        # A graph that names itself: past every bound a walk has.
        "paint.h": {"Format": PF.PaintColrGlyph, "Glyph": "paint.h"},
        # Alphas past either end of [0, 1], which F2.14 can store and COLR clamps.
        "paint.i": {"Format": PF.PaintColrLayers, "Layers": [
            glyph("layer.a", solid(0, 1.5)), glyph("layer.b", solid(1, -0.5))]},
        # A clip under a transform, and a sibling after it that the transform does not reach.
        "paint.j": {"Format": PF.PaintColrLayers, "Layers": [
            {"Format": PF.PaintTranslate, "dx": 50, "dy": -20, "Paint": glyph("layer.c", {
                "Format": PF.PaintColrLayers, "Layers": [glyph("layer.a", solid(0)), glyph("layer.b", solid(1))]})},
            glyph("layer.b", solid(2))]},
        # Color-paint story P4 (L2 to L8): every gradient kind and extend under a glyph, transforms above the glyph
        # (the fill's own) and between the glyph and the gradient (the gradient's), the stops with the foreground,
        # an alpha, an offset outside [0, 1] and an unsorted pair.
        "paint.k": {"Format": PF.PaintTranslate, "dx": 20, "dy": -10, "Paint": glyph("layer.a", {
            "Format": PF.PaintScale, "scaleX": 1.5, "scaleY": 0.5, "Paint": {
                "Format": PF.PaintLinearGradient, "ColorLine": {"Extend": "pad", "ColorStop": [
                    {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
                    {"StopOffset": 0.75, "PaletteIndex": 0xFFFF, "Alpha": 0.5},
                    {"StopOffset": 1.25, "PaletteIndex": 2, "Alpha": 0.25}]},
                "x0": 0, "y0": 0, "x1": 600, "y1": 0, "x2": 0, "y2": 600}})},
        "paint.l": glyph("layer.b", {"Format": PF.PaintRotateAroundCenter, "angle": 30.0, "centerX": 100,
                                    "centerY": 200, "Paint": {
            "Format": PF.PaintRadialGradient, "ColorLine": {"Extend": "reflect", "ColorStop": [
                {"StopOffset": 0.5, "PaletteIndex": 1, "Alpha": 1.0},
                {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 0.75}]},
            "x0": 300, "y0": 300, "r0": 20, "x1": 350, "y1": 280, "r1": 400}}),
        "paint.m": glyph("layer.c", {
            "Format": PF.PaintSweepGradient, "ColorLine": {"Extend": "repeat", "ColorStop": [
                {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
                {"StopOffset": 1.0, "PaletteIndex": 2, "Alpha": 1.0}]},
            "centerX": 300, "centerY": 300, "startAngle": 10.0, "endAngle": 200.0}),
        # A gradient and a solid in a layers node under a glyph: paints the glyph bounds without being directly under
        # it (14c refused these), filling the plane under the glyph's clip, with the transform between them theirs.
        "paint.n": glyph("layer.c", {"Format": PF.PaintColrLayers, "Layers": [
            {"Format": PF.PaintTranslate, "dx": 40, "dy": 0, "Paint": {
                "Format": PF.PaintLinearGradient, "ColorLine": {"Extend": "repeat", "ColorStop": [
                    {"StopOffset": 0.0, "PaletteIndex": 1, "Alpha": 1.0},
                    {"StopOffset": 0.5, "PaletteIndex": 2, "Alpha": 1.0}]},
                "x0": 0, "y0": 0, "x1": 200, "y1": 0, "x2": 0, "y2": 200}},
            solid(1, 0.5),
            {"Format": PF.PaintScaleUniform, "scale": 1.75, "Paint": {
                "Format": PF.PaintRadialGradient, "ColorLine": {"Extend": "pad", "ColorStop": [
                    {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 0.5},
                    {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 0.5}]},
                "x0": 100, "y0": 100, "r0": 0, "x1": 100, "y1": 100, "r1": 150}}]}),
        # A gradient at the root: drawn where the glyph's clip box allows (the box cuts the glyph), refused where the
        # font states no box (paint.p).
        "paint.o": {"Format": PF.PaintLinearGradient, "ColorLine": {"Extend": "pad", "ColorStop": [
            {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
            {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 1.0}]},
            "x0": 0, "y0": 0, "x1": 300, "y1": 500, "x2": 100, "y2": 0},
        "paint.p": {"Format": PF.PaintRadialGradient, "ColorLine": {"Extend": "pad", "ColorStop": [
            {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
            {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 1.0}]},
            "x0": 0, "y0": 0, "r0": 0, "x1": 0, "y1": 0, "r1": 300},
        # A blend mode (hue, COLR's 24) whose source is a gradient.
        "paint.q": {"Format": PF.PaintComposite, "CompositeMode": "hsl_hue",
                    "SourcePaint": glyph("layer.a", {
                        "Format": PF.PaintLinearGradient, "ColorLine": {"Extend": "pad", "ColorStop": [
                            {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
                            {"StopOffset": 1.0, "PaletteIndex": 2, "Alpha": 1.0}]},
                        "x0": 0, "y0": 0, "x1": 600, "y1": 0, "x2": 0, "y2": 600}),
                    "BackdropPaint": glyph("layer.b", solid(1))},
        # Another glyph's graph, which has a clip box of its own, moved: both boxes clip (L5), the named glyph's under
        # the transform above the reference.
        "paint.r": {"Format": PF.PaintTranslate, "dx": 100, "dy": 0, "Paint": {
            "Format": PF.PaintColrGlyph, "Glyph": "paint.o"}},
    }


# The clip boxes of the scene fixture (xMin, yMin, xMax, yMax): paint.o's cuts its gradient; paint.r's is larger.
SCENE_CLIPS = {"paint.o": (0, 0, 300, 500), "paint.r": (-100, -100, 900, 900)}


def build_color_v1_scene(mf, out):
    order = ORDER + SCENE_EXTRA
    shapes = _glyphs_for(mf, ORDER)
    for name in SCENE_EXTRA:
        pen = TTGlyphPen(None)
        mf.OUTLINES["A"](pen)
        shapes[name] = pen.glyph()
    advances = dict(mf.ADVANCES)
    for name in LAYERS + SCENE_EXTRA:
        advances[name] = 600
    fb = mf.truetype("Color v1 scene", mf.unicode_cmap(), order=order, glyphs=shapes,
                     advances=advances)
    glyph_map = fb.font.getReverseGlyphMap()
    fb.font["COLR"] = buildCOLR(v1_scene_trees(), version=1, glyphMap=glyph_map, clipBoxes=SCENE_CLIPS)
    fb.font["CPAL"] = buildCPAL(_floats(PALETTES), nameTable=fb.font["name"])
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


def build_svg_docs(mf, out):
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables.S_V_G_ import SVGDocument

    fb = mf.truetype("SVG documents", mf.unicode_cmap(), order=ORDER, glyphs=_glyphs(mf),
                     advances=dict(mf.ADVANCES))

    def svg(ids, text):
        body = "".join('<g id="glyph%d"><rect width="10" height="10"/></g>' % i for i in ids)
        return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000">%s%s</svg>' % (
            body, text)
    table = newTable("SVG ")
    table.docList = [
        SVGDocument(svg([0], "<!-- notdef -->"), 0, 0, True),
        SVGDocument(svg([2, 3], ""), 2, 3, False),
        SVGDocument(svg([4], "<!-- compressed -->" * 20), 4, 4, True),
        SVGDocument(svg([6, 7], "<title>caf\u00e9</title>"), 6, 7, False),
    ]
    fb.font["SVG "] = table
    mf.pin(fb)
    fb.save(out)
