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
"""What fontTools says of the `MATH` table of a subset this library wrote.

    fonttools_math_check.py pick <fonts.json> <samples.json>
    fonttools_math_check.py compare <jobs.json> <results.json>

Runs **inside the fonttools image**.

`pick` chooses, for each font, the characters to subset by: a formula's worth of the usual ones,
and seeded random draws from the characters whose glyphs the font's `MATH` table says something
about (a size a glyph grows to, an italics correction, a top accent, a kern, an extended shape),
so that every part of the table is asked about.

`compare` takes, for each job, the original, this library's subset with the ids retained and the
one renumbered (with its glyph map), and asks fontTools to subset the original **to the very
glyph set this library kept**, closure off: the two `MATH` tables are then answers to the same
question, and every difference is in what was written, not in which glyphs were chosen. Every
glyph name in either table is turned into an id in the original's numbering before the two trees
are compared. A second question, whether fontTools' own closure for the same characters keeps a
glyph this library's did not, is reported as the glyphs missed.
"""

import io
import json
import os
import random
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

FORMULA = "x+y=(a)[b]{c}∑∫√∏∮⌊⌋‖|ΣΠαβ"


def math_glyphs(font):
    """The glyph names the MATH table of a font says something about, by kind."""
    m = font["MATH"].table
    kinds = {"variants": set(), "italic": set(), "accent": set(), "kern": set(), "extended": set()}
    v = m.MathVariants
    if v:
        for cov in (v.VertGlyphCoverage, v.HorizGlyphCoverage):
            if cov:
                kinds["variants"].update(cov.glyphs)
    gi = m.MathGlyphInfo
    if gi:
        if gi.MathItalicsCorrectionInfo:
            kinds["italic"].update(gi.MathItalicsCorrectionInfo.Coverage.glyphs)
        if gi.MathTopAccentAttachment:
            kinds["accent"].update(gi.MathTopAccentAttachment.TopAccentCoverage.glyphs)
        if gi.MathKernInfo:
            kinds["kern"].update(gi.MathKernInfo.MathKernCoverage.glyphs)
        if gi.ExtendedShapeCoverage:
            kinds["extended"].update(gi.ExtendedShapeCoverage.glyphs)
    return kinds


def pick(fonts):
    out = {}
    for path in fonts:
        font = TTFont(path, fontNumber=0, lazy=True)
        cmap = font.getBestCmap()
        by_glyph = {}
        for cp, name in cmap.items():
            by_glyph.setdefault(name, cp)
        rng = random.Random(os.path.basename(path))
        samples = [sorted({ord(c) for c in FORMULA if ord(c) in cmap})]
        kinds = math_glyphs(font)
        every = set()
        for kind in ("variants", "italic", "accent", "kern", "extended"):
            cps = sorted(by_glyph[g] for g in kinds[kind] if g in by_glyph)
            every.update(cps)
            for n in (6, 24):
                if cps:
                    samples.append(sorted(rng.sample(cps, min(n, len(cps)))))
        samples.append(sorted(every)[:400])
        plain = sorted(cmap)
        samples.append(sorted(rng.sample(plain, min(40, len(plain)))))
        out[path] = [s for s in samples if s]
    return out


def to_tree(obj, gid, store=None):
    """A comparable tree of a MATH object, every glyph name an id in the original's numbering and
    every variation index the row of deltas (with its regions) that it names in GDEF's store:
    the two writers may number the rows of a pruned store differently and still say the same."""
    if isinstance(obj, str):
        return gid(obj)
    if isinstance(obj, list):
        return [to_tree(x, gid, store) for x in obj]
    if hasattr(obj, "__dict__"):
        if getattr(obj, "DeltaFormat", None) == 0x8000:
            outer, inner = obj.StartSize, obj.EndSize
            if store is None or outer >= len(store.VarData) \
                    or inner >= len(store.VarData[outer].Item):
                return ["unresolved variation index", outer, inner]
            data = store.VarData[outer]
            return ["variation", list(data.VarRegionIndex), list(data.Item[inner])]
        tree = {}
        for key, value in sorted(vars(obj).items()):
            if key == "Format":
                continue
            tree[key] = to_tree(value, gid, store)
        return tree
    return obj


def prune(tree):
    """The tree with what says nothing taken out: a structure with no entries, or a count of
    none, is the same as the structure being absent, and a writer may choose either."""
    if isinstance(tree, dict):
        kept = {}
        for key, value in tree.items():
            value = prune(value)
            if value not in (None, 0, [], {}):
                kept[key] = value
        return kept or None
    if isinstance(tree, list):
        return [prune(x) for x in tree] or None
    return tree


def diff(a, b, path=""):
    """The first place two trees differ, as text; None when they are the same."""
    if type(a) is not type(b):
        return "%s: %r vs %r" % (path, str(a)[:60], str(b)[:60])
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return "%s/%s present in only one (%s)" % (
                    path, key, "ours" if key in a else "fontTools")
            d = diff(a[key], b[key], path + "/" + key)
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return "%s: %d entries vs %d" % (path, len(a), len(b))
        for i, (x, y) in enumerate(zip(a, b)):
            d = diff(x, y, "%s[%d]" % (path, i))
            if d:
                return d
        return None
    return None if a == b else "%s: %r vs %r" % (path, a, b)


def math_of(path, ids_of_names, store_from=None):
    """The MATH tree of a font. Its variation indices are read through the GDEF store of
    @p store_from (a path) when given, else through its own."""
    font = TTFont(path)
    if "MATH" not in font:
        return None
    order = font.getGlyphOrder()
    by_name = {n: i for i, n in enumerate(order)}
    store = None
    owner = TTFont(store_from) if store_from else font
    if "GDEF" in owner and getattr(owner["GDEF"].table, "VarStore", None):
        store = owner["GDEF"].table.VarStore
    return prune(to_tree(font["MATH"].table, lambda n: ids_of_names(by_name[n]), store))


def reference(original, kept, retain_gids=True):
    """fontTools' subset of the original to exactly these glyph ids, closure off."""
    options = subset.Options()
    options.retain_gids = retain_gids
    options.layout_closure = False
    options.layout_features = ["*"]
    options.notdef_outline = True
    options.glyph_names = True
    font = TTFont(original)
    subsetter = subset.Subsetter(options)
    subsetter.populate(gids=sorted(kept))
    subsetter.subset(font)
    data = io.BytesIO()
    font.save(data)
    return data.getvalue()


def theirs_full(original, cps):
    """The glyph ids fontTools keeps for the characters, with its own closure."""
    options = subset.Options()
    options.retain_gids = True
    options.layout_features = ["*"]
    options.notdef_outline = True
    options.glyph_names = True
    # Its closure also adds the character a mirrored one stands for (a bracket's other half).
    # This library leaves that to the caller, as it does the other characters the shaper borrows
    # (write.h), so it is not asked for here.
    options.bidi_closure = False
    font = TTFont(original)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=cps)
    subsetter.subset(font)
    return set(font.getGlyphID(n) for n in subsetter.glyphs_retained)


def compare(jobs):
    results = []
    for job in jobs:
        r = {"font": job["font"], "sample": job["sample"]}
        try:
            gmap = job["map"]
            kept = set(g for g in gmap if g is not None)
            ref_bytes = reference(job["font"], kept)
            ref_path = job["retain"] + ".ref"
            with open(ref_path, "wb") as h:
                h.write(ref_bytes)
            # fontTools prunes GDEF's store without looking at MATH, whose variation indices are
            # then left naming rows of the unpruned store; they are read through the original's.
            ref = math_of(ref_path, lambda i: i, store_from=job["font"])
            ours_retain = math_of(job["retain"], lambda i: i)
            ours_ren = math_of(job["renumber"], lambda i: gmap[i])
            r["has_ref"] = ref is not None
            r["has_ours"] = ours_retain is not None and ours_ren is not None
            r["retain"] = diff(ours_retain, ref) if ref is not None and ours_retain is not None \
                else ("missing" if (ref is None) != (ours_retain is None) else None)
            r["renumber"] = diff(ours_ren, ref) if ref is not None and ours_ren is not None \
                else ("missing" if (ref is None) != (ours_ren is None) else None)
            full = theirs_full(job["font"], job["cps"])
            r["missed"] = sorted(full - kept)[:10]
            r["extra"] = len(kept - full)
            r["kept"] = len(kept)
        except Exception as e:     # a font that does not open at all is a result, not a crash
            r["error"] = "%s: %s" % (type(e).__name__, e)
        results.append(r)
    return results


def main(argv):
    mode = argv[1]
    with open(argv[2], encoding="utf-8") as h:
        data = json.load(h)
    out = pick(data) if mode == "pick" else compare(data)
    with open(argv[3], "w", encoding="utf-8") as h:
        json.dump(out, h)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
