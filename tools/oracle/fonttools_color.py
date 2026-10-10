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

Writes {path: answer}, where an answer has the keys `font-color` prints. fontTools
decompiles the tables itself, so this is a second implementation of the format.
Runs in the fonttools image.
"""

import json
import sys

from fontTools.ttLib import TTFont


def describe(path):
    font = TTFont(path, fontNumber=0, lazy=True)
    out = {"palettes": None, "entry_labels": None, "version": None, "layers": None}
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
        for name, records in colr.ColorLayers.items():
            layers[str(order[name])] = [[order[r.name], int(r.colorID)]
                                        for r in records]
        out["layers"] = layers
    return out


def main(argv):
    out = {}
    for path in argv[2:]:
        try:
            out[path] = describe(path)
        except Exception as why:
            out[path] = {"error": str(why)}
    with open(argv[1], "w", encoding="utf-8") as handle:
        json.dump(out, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
