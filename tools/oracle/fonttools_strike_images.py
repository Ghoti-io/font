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
"""Hold `font-strike-images`' output for one font to fontTools and Pillow.

    fonttools_strike_images.py <font> <dir> <ours.jsonl> <out.json>

`<ours.jsonl>` is what `font-strike-images --decode` printed and `<dir>` is where it
wrote the font's bytes (`S-G.img`) and its decoded pixels (`S-G.rgba`). For every
glyph of every color strike this checks, against fontTools' own reading of `CBDT` or
`sbix`:

  * the same glyphs are present, and the same are absent;
  * the encoded bytes are identical to the byte;
  * the format, the box and the advance the font states are equal;
  * the decoded pixels are what Pillow decodes the same bytes to, converted to
    RGBA (straight alpha), compared byte for byte.

The last is the only comparison between two *decoders*, so it is what holds the
`image` library's PNG reader, as this library calls it, to a second one.

Writes {"compared": n, "pixels": n, "differences": [first few]} and exits 0. Runs in
the fonttools image, where Pillow is.
"""

import io
import json
import os
import sys

from fontTools.ttLib import TTFont
from PIL import Image

KINDS = {"png ": "png", "jpg ": "jpeg", "tiff": "tiff"}


def reference(font):
    """{strike index: {glyph id: (format, bytes, w, h, bx, by, adv or None)}} and
    the strike's identity, from fontTools."""
    order = font.getGlyphOrder()
    ids = {name: i for i, name in enumerate(order)}
    out = {}
    if "CBLC" in font:
        cblc, cbdt = font["CBLC"], font["CBDT"]
        for index, strike in enumerate(cblc.strikes):
            size = strike.bitmapSizeTable
            glyphs = {}
            for name, g in cbdt.strikeData[index].items():
                data = bytes(g.imageData)
                if g.getFormat() == 19:
                    # Image format 19 has no metrics of its own: they are the
                    # index subtable's, and fontTools leaves them there. A read of
                    # `metrics` on the glyph would decompile it a second time.
                    m = next(st.metrics for st in strike.indexSubTables
                             if name in st.names)
                else:
                    m = g.metrics
                # SmallGlyphMetrics and BigGlyphMetrics name the same five fields
                # differently; the big one's are the horizontal set.
                if hasattr(m, "BearingX"):
                    bx, by, adv = m.BearingX, m.BearingY, m.Advance
                else:
                    bx, by, adv = m.horiBearingX, m.horiBearingY, m.horiAdvance
                glyphs[ids[name]] = ("png", data, m.width, m.height,
                                     bx, by, adv)
            out[index] = ((size.ppemX, size.ppemY), glyphs)
    elif "sbix" in font:
        sbix = font["sbix"]
        for index, strike in enumerate(sbix.strikes.values()):
            glyphs = {}
            for name, g in strike.glyphs.items():
                target = g
                seen = 0
                while target.graphicType == "dupe" and seen < 8:
                    target = strike.glyphs[target.referenceGlyphName]
                    seen += 1
                if target.graphicType not in KINDS or not target.imageData:
                    continue
                glyphs[ids[name]] = (KINDS[target.graphicType],
                                     bytes(target.imageData), None, None,
                                     target.originOffsetX, target.originOffsetY,
                                     None)
            # fontTools lists sbix strikes by size, not in file order, so they are
            # keyed by size and matched to this library's by it.
            out[strike.ppem] = ((strike.ppem, strike.ppem), glyphs)
    return out


def main(argv):
    path, folder, lines, out = argv[1:5]
    font = TTFont(path, lazy=True)
    want = reference(font)
    ours = {}
    strikes = {}
    with open(lines, encoding="utf-8") as handle:
        for n, line in enumerate(handle):
            row = json.loads(line)
            if n == 0:
                strikes = {s["index"]: s for s in row["strikes"] if s["color"]}
                continue
            ours.setdefault(row["strike"], {})[row["glyph"]] = row
    diffs = []
    compared = pixels = 0

    def note(text):
        if len(diffs) < 20:
            diffs.append(text)

    keyed = {}
    for index, s in strikes.items():
        keyed[index if "CBLC" in font else s["ppem_x"]] = index
    if set(keyed) != set(want):
        note("color strikes: this library %s, fontTools %s"
             % (sorted(keyed), sorted(want)))
    for key in sorted(set(keyed) & set(want)):
        index = keyed[key]
        (ppem, theirs) = want[key]
        if (strikes[index]["ppem_x"], strikes[index]["ppem_y"]) != ppem:
            note("strike %d: ppem %s vs %s" % (index, (strikes[index]["ppem_x"],
                 strikes[index]["ppem_y"]), ppem))
        mine = {g: r for g, r in ours.get(index, {}).items() if not r.get("absent")}
        # A glyph this library declines (a mask record, a dupe chain that never ends)
        # is one fontTools has no picture for either; one it declines that fontTools
        # has a picture for is a difference.
        errors = {g: r for g, r in mine.items() if "error" in r}
        for g, r in sorted(errors.items()):
            if g in theirs:
                note("strike %d glyph %d: %s" % (index, g, r["error"]))
        present = {g for g, r in mine.items() if "error" not in r}
        if present != set(theirs) - set(errors):
            only_mine = sorted(present - set(theirs))[:5]
            only_theirs = sorted(set(theirs) - present - set(errors))[:5]
            note("strike %d glyphs: only this library %s, only fontTools %s"
                 % (index, only_mine, only_theirs))
        for g in sorted(present & set(theirs)):
            r = mine[g]
            fmt, data, w, h, bx, by, adv = theirs[g]
            compared += 1
            with open(os.path.join(folder, "%d-%d.img" % (index, g)), "rb") as f:
                got = f.read()
            if r["format"] != fmt or got != data:
                note("strike %d glyph %d: bytes or format differ (%s %d vs %s %d)"
                     % (index, g, r["format"], len(got), fmt, len(data)))
                continue
            # CBDT states the top edge and sbix the bottom; `oy` is the bottom for both.
            sbix = "sbix" in font
            ours_y = r["oy"] if sbix else r["by"]
            if (r["bx"], ours_y) != (bx, by):
                note("strike %d glyph %d: bearing %s vs %s"
                     % (index, g, (r["bx"], ours_y), (bx, by)))
            if w is not None and (r["w"], r["h"]) != (w, h):
                note("strike %d glyph %d: size %s vs %s"
                     % (index, g, (r["w"], r["h"]), (w, h)))
            if adv is not None and r["adv"] != adv:
                note("strike %d glyph %d: advance %s vs %s"
                     % (index, g, r["adv"], adv))
            if "dw" in r:
                pixels += 1
                img = Image.open(io.BytesIO(data)).convert("RGBA")
                with open(os.path.join(folder, "%d-%d.rgba" % (index, g)), "rb") as f:
                    mine_px = f.read()
                if (r["dw"], r["dh"]) != img.size or mine_px != img.tobytes():
                    note("strike %d glyph %d: pixels differ (%s vs %s)"
                         % (index, g, (r["dw"], r["dh"]), img.size))
            elif "decode_error" in r:
                note("strike %d glyph %d: %s" % (index, g, r["decode_error"]))
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"compared": compared, "pixels": pixels, "differences": diffs,
                   "strikes": len(want)}, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
