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
"""What fontTools says a font's `CPAL` and `COLR` hold, in `font-color`'s shape.

    fonttools_color.py <out.json> <font>...
    fonttools_color.py --location wght=700,wdth=80 --coords coords.json <out.json> <font>...

`coords.json` maps a font to the normalized coordinates (F2Dot14, in axis order)
this library computed for the location. When given they are used in place of
fontTools' own normalization, so the two sides are asked about the same point:
the 1/16384 by which they may differ moves a delta across a rounding tie on its
own, and what is being compared is the paint graph, not the normalization (which
`var_diff.py` holds to fontTools separately).

Writes {path: answer}, where an answer has the keys `font-color` prints. fontTools
decompiles the tables itself, so this is a second implementation of the format.
Version 1's paint graph is written as `font-color` writes it: a tree of nodes
`{"f": format, ..., "c": [children]}` in the file's own units (F2Dot14 and Fixed
values as the integers they are stored as), `PaintColrLayers` expanded into the
layers it names and `PaintColrGlyph` into the glyph's own graph. At a location the
variable forms have the item variation store's delta added, rounded half up, which
is the arithmetic of `fonttools_metrics.py`. Runs in the fonttools image.
"""

import json
import math
import sys

from fontTools.misc.fixedTools import otRound
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables as ot
from fontTools.varLib.varStore import VarStoreInstancer

from fonttools_variation import axes_of, normalised, parse_location


FMT_VALUES = {
    ot.PaintFormat.PaintLinearGradient: ("x0", "y0", "x1", "y1", "x2", "y2"),
    ot.PaintFormat.PaintRadialGradient: ("x0", "y0", "r0", "x1", "y1", "r1"),
    ot.PaintFormat.PaintSweepGradient: ("centerX", "centerY", "startAngle", "endAngle"),
    ot.PaintFormat.PaintTranslate: ("dx", "dy"),
    ot.PaintFormat.PaintScale: ("scaleX", "scaleY"),
    ot.PaintFormat.PaintScaleAroundCenter: ("scaleX", "scaleY", "centerX", "centerY"),
    ot.PaintFormat.PaintScaleUniform: ("scale",),
    ot.PaintFormat.PaintScaleUniformAroundCenter: ("scale", "centerX", "centerY"),
    ot.PaintFormat.PaintRotate: ("angle",),
    ot.PaintFormat.PaintRotateAroundCenter: ("angle", "centerX", "centerY"),
    ot.PaintFormat.PaintSkew: ("xSkewAngle", "ySkewAngle"),
    ot.PaintFormat.PaintSkewAroundCenter: ("xSkewAngle", "ySkewAngle", "centerX",
                                           "centerY"),
}
# Fields stored in F2Dot14 (raw = value * 16384) and in Fixed (value * 65536); the
# rest are FWORD or UFWORD.
F2DOT14 = {"scaleX", "scaleY", "scale"}
# fontTools shows an angle in degrees (F2Dot14 half turns times 180), and a sweep
# gradient's start and end with a bias of one half turn.
ANGLES = {"angle", "xSkewAngle", "ySkewAngle"}
SWEEP_ANGLES = {"startAngle", "endAngle"}


def half_up(value):
    return int(math.floor(round(value, 6) + 0.5))


class Graph:
    def __init__(self, font, location):
        self.font = font
        self.colr = font["COLR"].table
        self.location = location
        self.instancer = None
        self.map = None
        if location is not None and self.colr.VarStore is not None:
            self.instancer = VarStoreInstancer(self.colr.VarStore,
                                               font["fvar"].axes, location)
        if self.colr.VarIndexMap is not None:
            self.map = list(self.colr.VarIndexMap.mapping)
        self.order = font.getGlyphOrder()
        self.base = {r.BaseGlyph: r.Paint
                     for r in (self.colr.BaseGlyphList.BaseGlyphPaintRecord
                               if self.colr.BaseGlyphList else [])}

    def delta(self, base, slot):
        """The delta of variation index base+slot, 0 when there is none."""
        if self.instancer is None or base == 0xFFFFFFFF:
            return 0
        index = base + slot
        if self.map is not None:
            index = self.map[min(index, len(self.map) - 1)]
        return half_up(self.instancer[index])

    def native(self, name, value):
        if name in F2DOT14:
            return otRound(value * 16384)
        if name in ANGLES:
            return otRound(value / 180 * 16384)
        if name in SWEEP_ANGLES:
            return otRound(value / 180 * 16384) - 16384
        return int(value)

    def line(self, line):
        stops = []
        for stop in line.ColorStop:
            base = getattr(stop, "VarIndexBase", 0xFFFFFFFF)
            stops.append([otRound(stop.StopOffset * 16384) + self.delta(base, 0),
                          stop.PaletteIndex,
                          otRound(stop.Alpha * 16384) + self.delta(base, 1)])
        return {"ext": int(line.Extend), "stops": stops}

    def node(self, paint):
        fmt = int(paint.Format)
        out = {"f": fmt}
        base = getattr(paint, "VarIndexBase", 0xFFFFFFFF)
        if fmt == ot.PaintFormat.PaintColrLayers:
            first = paint.FirstLayerIndex
            layers = self.colr.LayerList.Paint[first:first + paint.NumLayers]
            out["c"] = [self.node(p) for p in layers]
        elif fmt in (ot.PaintFormat.PaintSolid, ot.PaintFormat.PaintVarSolid):
            out["pal"] = paint.PaletteIndex
            out["a"] = otRound(paint.Alpha * 16384) + self.delta(base, 0)
        elif fmt in (ot.PaintFormat.PaintGlyph,):
            out["g"] = self.font.getGlyphID(paint.Glyph)
            out["c"] = [self.node(paint.Paint)]
        elif fmt == ot.PaintFormat.PaintColrGlyph:
            out["g"] = self.font.getGlyphID(paint.Glyph)
            out["c"] = [self.node(self.base[paint.Glyph])]
        elif fmt in (ot.PaintFormat.PaintTransform, ot.PaintFormat.PaintVarTransform):
            t = paint.Transform
            tb = getattr(t, "VarIndexBase", 0xFFFFFFFF)
            out["v"] = [otRound(getattr(t, n) * 65536) + self.delta(tb, k)
                        for k, n in enumerate(("xx", "yx", "xy", "yy", "dx", "dy"))]
            out["c"] = [self.node(paint.Paint)]
        elif fmt == ot.PaintFormat.PaintComposite:
            out["mode"] = int(paint.CompositeMode)
            out["c"] = [self.node(paint.SourcePaint), self.node(paint.BackdropPaint)]
        else:
            # Everything else is a gradient or a simple transform; the Var form
            # shares the fixed form's field list, one format number up.
            fixed = fmt if fmt in FMT_VALUES else fmt - 1
            names = FMT_VALUES[ot.PaintFormat(fixed)]
            out["v"] = [self.native(n, getattr(paint, n)) + self.delta(base, k)
                        for k, n in enumerate(names)]
            if hasattr(paint, "ColorLine"):
                out["line"] = self.line(paint.ColorLine)
            if hasattr(paint, "Paint"):
                out["c"] = [self.node(paint.Paint)]
        out.setdefault("c", [])
        return out

    def clips(self):
        out = {}
        clips = self.colr.ClipList
        if clips is None:
            return out
        for box_set in clips.clips.values():
            pass
        for name, box in clips.clips.items():
            base = getattr(box, "VarIndexBase", 0xFFFFFFFF)
            values = [box.xMin + self.delta(base, 0), box.yMin + self.delta(base, 1),
                      box.xMax + self.delta(base, 2), box.yMax + self.delta(base, 3)]
            out[str(self.font.getGlyphID(name))] = values
        return out


def describe(path, location=None, coords=None):
    font = TTFont(path, fontNumber=0, lazy=True)
    out = {"palettes": None, "entry_labels": None, "version": None, "layers": None,
           "paints": None, "clips": None}
    if "CPAL" in font:
        cpal = font["CPAL"]
        types = cpal.paletteTypes
        labels = cpal.paletteLabels
        out["palettes"] = [
            {"flags": int(types[i]), "label": int(labels[i]),
             "colors": [[c.red, c.green, c.blue, c.alpha] for c in palette]}
            for i, palette in enumerate(cpal.palettes)]
        out["entry_labels"] = [int(x) for x in cpal.paletteEntryLabels]
    if "COLR" in font:
        colr = font["COLR"]
        out["version"] = int(colr.version)
        order = font.getReverseGlyphMap()
        layers = {}
        # Version 0 only: `ColorLayers` is fontTools' view of the base glyph
        # records. A version 1 table's paint graph is another comparison.
        if colr.version == 0:
            for name, records in colr.ColorLayers.items():
                layers[str(order[name])] = [[order[r.name], int(r.colorID)]
                                            for r in records]
        else:
            table = colr.table
            bases = table.BaseGlyphRecordArray
            records = table.LayerRecordArray.LayerRecord if table.LayerRecordArray else []
            for base in (bases.BaseGlyphRecord if bases else []):
                layers[str(order[base.BaseGlyph])] = [
                    [order[r.LayerGlyph], int(r.PaletteIndex)]
                    for r in records[base.FirstLayerIndex:
                                     base.FirstLayerIndex + base.NumLayers]]
        out["layers"] = layers
        if colr.version == 1:
            normal = None
            if coords is not None and "fvar" in font:
                normal = {axis.axisTag: value / 16384.0
                          for axis, value in zip(font["fvar"].axes, coords)}
            elif location is not None and "fvar" in font:
                normal = normalised(font, location)
            graph = Graph(font, normal)
            out["paints"] = {str(font.getGlyphID(name)): graph.node(paint)
                             for name, paint in graph.base.items()}
            out["clips"] = graph.clips()
    return out


def main(argv):
    out = {}
    location = None
    coords = {}
    args = argv[1:]
    while args and args[0] in ("--location", "--coords"):
        if args[0] == "--location":
            location = parse_location(args[1])
        else:
            with open(args[1], encoding="utf-8") as handle:
                coords = json.load(handle)
        args = args[2:]
    for path in args[1:]:
        try:
            out[path] = describe(path, location, coords.get(path))
        except Exception as why:
            out[path] = {"error": str(why)}
    with open(args[0], "w", encoding="utf-8") as handle:
        json.dump(out, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
