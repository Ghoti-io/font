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
"""What fontTools says of a subset this library wrote, and what it would have kept.

    fonttools_subset_check.py <jobs.json> <results.json>

Runs **inside the fonttools image**. Each job names an original, this library's
subset of it (written with the glyph ids retained), the characters asked for and
the glyph ids HarfBuzz shaped the sample text to. For each it reports:

  * whether fontTools opens the subset at all;
  * the glyphs among those shaped that fontTools reads differently in the subset
    and the original (their coordinates, contour ends and flags) - a glyph the
    closure missed, or one whose composite was rewritten wrongly;
  * the glyphs fontTools' own subsetter keeps for the same request (retained ids,
    every layout feature) that have an outline in the original and none in this
    library's subset - what its closure found and ours did not - and the ones
    ours kept that it did not, which are the over-approximation made visible.
"""

import json
import sys

from fontTools import subset
from fontTools.ttLib import TTFont


def outline(font, gid):
    """What a glyph draws, as something comparable; None when it draws nothing."""
    glyf = font["glyf"]
    name = font.getGlyphOrder()[gid]
    glyph = glyf[name]
    if glyph.numberOfContours == 0:
        return None
    coords, ends, flags = glyph.getCoordinates(glyf)
    return ([tuple(c) for c in coords], list(ends), list(flags))


def nonempty(font):
    glyf = font["glyf"]
    return {i for i, n in enumerate(font.getGlyphOrder())
            if glyf[n].numberOfContours != 0}


def check(job):
    out = {"opens": False}
    try:
        number = 0 if job["original"].endswith((".ttc", ".otc")) else -1
        original = TTFont(job["original"], lazy=True, fontNumber=number)
        mine = TTFont(job["subset"], lazy=True)
    except Exception as why:
        out["error"] = "%s: %s" % (type(why).__name__, why)
        return out
    out["opens"] = True
    count = len(original.getGlyphOrder())
    if len(mine.getGlyphOrder()) > count:
        out["error"] = "the subset has more glyphs than the original"
        return out
    different = []
    for gid in job["gids"]:
        if gid >= count:
            continue
        try:
            a = outline(original, gid)
            b = outline(mine, gid) if gid < len(mine.getGlyphOrder()) else None
        except Exception as why:
            different.append([gid, "unreadable: %s" % why])
            continue
        if a != b:
            different.append([gid, "differs"])
    out["different"] = different
    # fontTools' closure for the same request.
    options = subset.Options()
    options.retain_gids = True
    options.layout_features = ["*"]
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.drop_tables = ["MATH"]
    try:
        reference = TTFont(job["original"], fontNumber=number)
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=job["unicodes"])
        subsetter.subset(reference)
        theirs = nonempty(reference)
        ours = nonempty(mine)
        out["theirs"] = len(theirs)
        out["ours"] = len(ours)
        out["missing"] = sorted(theirs - ours)[:20]
        out["extra"] = len(ours - theirs)
    except Exception as why:
        out["reference_error"] = "%s: %s" % (type(why).__name__, why)
    return out


def main(argv):
    with open(argv[1], encoding="utf-8") as handle:
        jobs = json.load(handle)
    results = [check(j) for j in jobs]
    with open(argv[2], "w", encoding="utf-8") as handle:
        json.dump(results, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
