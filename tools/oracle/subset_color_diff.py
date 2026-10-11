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
"""Subset every static color font and read both back through fontTools.

documentation/design.md sections 12.3 and 14. The subsetter carries `COLR` and
`CPAL`: the kept color glyphs bring the glyphs they paint with, the table is
rebuilt for the new numbering, and `CPAL` is copied. This holds the result to
fontTools, an independent reader of both the original and the subset: for every
kept base glyph, the subset's paint graph (and its version 0 layers and its clip
box) must be the original's with every glyph id sent through the subsetter's
own numbering, nothing else may have a record, and the palettes must be equal.

Three subsets of each font: the first few characters, every k-th of a few
dozen, and a few hundred spread across it. The test-glyph fonts have glyphs
that paint with other base glyphs, so their closure is exercised too.

Usage:
    subset_color_diff.py [--quiet] [-v] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import color_diff
import color_extents_diff
import corpus
import oracle_env

ROOT = oracle_env.ROOT
EXAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples")
SCRATCH = os.path.join(ROOT, "build", "oracle", "subset-color")

# Run in the fontTools image: for each pair, the graph the original has for every
# kept base glyph, carried through the numbering, against the subset's own.
COMPARE = r"""
import json, os, sys
sys.path.insert(0, os.path.join(sys.argv[3], "tools", "oracle"))
from fonttools_color import describe

def carry(node, new_of_old):
    if node.get("refused"):      # a graph that names itself: nothing to carry
        return node
    out = dict(node)
    if "g" in out:
        out["g"] = new_of_old[out["g"]]
    out["c"] = [carry(c, new_of_old) for c in node.get("c", [])]
    return out

def variation(path):
    from fontTools.ttLib import TTFont
    font = TTFont(path, lazy=True)
    if "COLR" not in font:
        return None
    table = getattr(font["COLR"], "table", None)
    if table is None:
        return None
    mapping = getattr(table, "VarIndexMap", None)
    store = getattr(table, "VarStore", None)
    sig = [None if mapping is None else
           (sorted(mapping.mapping.items()) if hasattr(mapping.mapping, "items")
            else list(mapping.mapping))]
    if store is not None:
        regions = [[(a.StartCoord, a.PeakCoord, a.EndCoord) for a in r.VarRegionAxis]
                   for r in store.VarRegionList.Region]
        data = [(list(d.VarRegionIndex), [list(row) for row in d.Item]) for d in store.VarData]
        sig.append((regions, data))
    else:
        sig.append(None)
    return json.dumps(sig, default=str)

def check(orig_path, sub_path, map_path):
    old_of_new = [int(l) for l in open(map_path).read().split()]
    new_of_old = {o: n for n, o in enumerate(old_of_new)}
    a, b = describe(orig_path), describe(sub_path)
    for key in ("palettes", "entry_labels", "version"):
        if a[key] != b[key]:
            return "%s differs" % key
    want_layers = {}
    for g, layers in (a["layers"] or {}).items():
        if int(g) in new_of_old:
            want_layers[str(new_of_old[int(g)])] = [[new_of_old[l], p] for l, p in layers]
    if (b["layers"] or {}) != want_layers:
        return "version 0 layers differ: %s vs %s" % (sorted(want_layers)[:5], sorted(b["layers"] or {})[:5])
    # The variation data is carried whole: the same index map and the same store rows.
    if variation(orig_path) != variation(sub_path):
        return "variation data differs"
    nodes = 0
    if a["paints"] is not None:
        want = {str(new_of_old[int(g)]): carry(n, new_of_old)
                for g, n in a["paints"].items() if int(g) in new_of_old}
        got = b["paints"] or {}
        if sorted(want, key=int) != sorted(got, key=int):
            return "base glyphs differ: %s vs %s" % (sorted(want, key=int)[:8], sorted(got, key=int)[:8])
        for g in want:
            if want[g] != got[g]:
                return "paint graph of new glyph %s differs" % g
            nodes += json.dumps(want[g]).count('"f"')
        want_clips = {str(new_of_old[int(g)]): box for g, box in (a["clips"] or {}).items()
                      if int(g) in new_of_old and g in a["paints"]}
        if (b["clips"] or {}) != want_clips:
            return "clips differ: %s vs %s" % (sorted(want_clips, key=int)[:5], sorted(b["clips"] or {}, key=int)[:5])
    return "ok %d %d" % (len(old_of_new), nodes)

pairs = json.load(open(sys.argv[1]))
json.dump([check(*p) for p in pairs], open(sys.argv[2], "w"))
"""


def subsets(cps):
    usable = [c for c in cps if c >= 0x20]
    if not usable:
        return []
    pick = [usable[:3]]
    step = max(1, len(usable) // 30)
    pick.append(usable[::step][:40])
    if len(usable) > 3:
        step = max(1, len(usable) // 250)
        pick.append(usable[::step][:260])
    return pick


def main(argv):
    quiet = "--quiet" in argv
    verbose = "-v" in argv
    rest = [a for a in argv[1:] if not a.startswith("-")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
    else:
        fixtures = [os.path.join(color_diff.FIXTURES, n)
                    for n in sorted(os.listdir(color_diff.FIXTURES))
                    if n.endswith((".ttf", ".otf"))]
        fonts = [f for f in fixtures + corpus.fonts("sfnt")
                 if color_diff.has_color_tables(f)]
    os.makedirs(SCRATCH, exist_ok=True)
    script = os.path.join(SCRATCH, "dump.py")
    info_path = os.path.join(SCRATCH, "info.json")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write(color_extents_diff.DUMP)
    ran = subprocess.run(oracle_env.command("fonttools",
        ["python3", script, info_path] + fonts, scratch=SCRATCH),
        capture_output=True, text=True)
    if ran.returncode != 0:
        raise oracle_env.OracleUnavailable("dump failed: %s" % ran.stderr[-300:])
    with open(info_path, encoding="utf-8") as handle:
        info = json.load(handle)

    pairs, names, refused, declined = [], [], 0, set()
    for number, font in enumerate(fonts):
        # Every code point the font maps, less the one the dump adds that it does not.
        cps = info[font]["cps"][:-1]
        for k, chosen in enumerate(subsets(cps)):
            tag = "%d_%d" % (number, k)
            out = os.path.join(SCRATCH, tag + ".ttf")
            mapf = os.path.join(SCRATCH, tag + ".map")
            ran = subprocess.run([os.path.join(EXAMPLES, "font-subset"), "--unicodes",
                ",".join("%X" % c for c in chosen), "--map", mapf, font, out],
                capture_output=True, text=True)
            if ran.returncode != 0 and "Unsupported feature" in ran.stderr:
                declined.add(font)   # the subsetter takes glyf outlines only
                continue
            if ran.returncode != 0:
                refused += 1
                print("REFUSED %s (%d characters): %s" % (os.path.relpath(font, ROOT),
                      len(chosen), ran.stderr.strip().splitlines()[0] if ran.stderr else ""))
                continue
            pairs.append([font, out, mapf])
            names.append((font, len(chosen), os.path.getsize(font), os.path.getsize(out)))
    with open(os.path.join(SCRATCH, "pairs.json"), "w", encoding="utf-8") as handle:
        json.dump(pairs, handle)
    with open(os.path.join(SCRATCH, "compare.py"), "w", encoding="utf-8") as handle:
        handle.write(COMPARE)
    ran = subprocess.run(oracle_env.command("fonttools", ["python3",
        os.path.join(SCRATCH, "compare.py"), os.path.join(SCRATCH, "pairs.json"),
        os.path.join(SCRATCH, "results.json"), ROOT], scratch=SCRATCH),
        capture_output=True, text=True)
    if ran.returncode != 0:
        raise oracle_env.OracleUnavailable("compare failed: %s" % ran.stderr[-400:])
    with open(os.path.join(SCRATCH, "results.json"), encoding="utf-8") as handle:
        results = json.load(handle)

    failed = glyphs = nodes = 0
    before = after = 0
    for (font, n, size_a, size_b), result in zip(names, results):
        before += size_a
        after += size_b
        if result.startswith("ok"):
            _, g, nn = result.split()
            glyphs += int(g)
            nodes += int(nn)
            if verbose:
                print("ok   %s %d characters: %d -> %d bytes" % (
                    os.path.relpath(font, ROOT), n, size_a, size_b))
        else:
            failed += 1
            print("DIFF %s (%d characters): %s" % (os.path.relpath(font, ROOT), n, result))
    print("subset_color_diff: %d fonts (%d declined: CFF outlines), %d subsets read back "
          "(%d refused), %d glyphs kept, %d paint nodes, %d differing; %d bytes in, %d out"
          % (len(fonts), len(declined), len(pairs), refused, glyphs, nodes, failed,
             before, after))
    return 1 if failed or refused or not pairs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
