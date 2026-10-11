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
"""Bitmap strikes through the subsetter: the pictures a subset keeps are the original's.

documentation/design.md sections 7.5 and 12.3. Every font with `EBLC`, `CBLC` or `sbix`
is subset five ways (the first few characters, about thirty spread, about 250 spread, the upper half, every second)
by `font-subset --map`, and then fontTools reads the original and the subset and checks
that every kept glyph has, in every strike, the same picture, bytes and metrics, it had
before; that a glyph the original strike lacked is still lacking; and that a strike
with a kept glyph survived and the others did not. HarfBuzz then shapes the same text in
both and the boxes must be the same, which holds the index rewrite to a second reader
of the metrics and the strike choice.

Noto Color Emoji has no outlines, so it is subset as a bitmap-only font; the fixtures
carry the formats Noto does not.

Usage:
    subset_strike_diff.py [--quiet] [-v] [font...]
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
import strike_image_diff
import subset_color_diff

ROOT = oracle_env.ROOT
EXAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples")
SCRATCH = os.path.join(ROOT, "build", "oracle", "subset-strikes")

COMPARE = r"""
import json, os, sys
sys.path.insert(0, os.path.join(sys.argv[3], "tools", "oracle"))
from fontTools.ttLib import TTFont
from fonttools_strike_images import reference

def eblc_reference(font):
    # Monochrome strikes: the image bytes and metrics per glyph, by ppem.
    out = {}
    order = font.getGlyphOrder()
    ids = {n: i for i, n in enumerate(order)}
    for index, strike in enumerate(font["EBLC"].strikes):
        size = strike.bitmapSizeTable
        glyphs = {}
        for name, g in font["EBDT"].strikeData[index].items():
            if g.getFormat() in (8, 9):
                continue
            data = bytes(g.imageData)
            if g.getFormat() == 5:
                m = next(st.metrics for st in strike.indexSubTables if name in st.names)
            else:
                m = g.metrics
            if hasattr(m, "BearingX"):
                bx, by, adv = m.BearingX, m.BearingY, m.Advance
            else:
                bx, by, adv = m.horiBearingX, m.horiBearingY, m.horiAdvance
            if name not in ids:
                continue      # a name fontTools made up for a glyph past the glyph order
            glyphs[ids[name]] = ("mono", data, m.width, m.height, bx, by, adv)
        out[index] = ((size.ppemX, size.ppemY), glyphs)
    return out

def read(path):
    font = TTFont(path)
    if "CBLC" in font or "sbix" in font:
        return reference(font), ranges(font), heads(font)
    return eblc_reference(font), ranges(font), heads(font)

def ranges(font):
    # Each strike's stated glyph range, by size: what the BitmapSize record says.
    out = {}
    for tag in ("CBLC", "EBLC"):
        if tag in font:
            for index, strike in enumerate(font[tag].strikes):
                size = strike.bitmapSizeTable
                out[index] = (size.startGlyphIndex, size.endGlyphIndex)
    return out

def heads(font):
    # The first four bytes of each strike table: the version, and for sbix the flags.
    return {tag: bytes(font.reader[tag][:4]) for tag in ("EBLC", "EBDT", "CBLC", "CBDT", "sbix")
            if tag in font.reader.tables}

def check(orig_path, sub_path, map_path):
    old_of_new = [int(l) for l in open(map_path).read().split()]
    new_of_old = {o: n for n, o in enumerate(old_of_new)}
    try:
        a = read(orig_path)
    except Exception as why:
        # fontTools refuses some real fonts that every renderer shows (mona.ttf states an
        # index size that overlaps its neighbour); there is no second reader of those.
        return "skip %s" % str(why)[:60]
    (a, a_ranges, a_heads), (b, b_ranges, b_heads) = a, read(sub_path)
    if a_heads != b_heads:
        return "table versions differ: %s vs %s" % (a_heads, b_heads)
    pictures = 0
    for key, (size, glyphs) in a.items():
        kept = {g: v for g, v in glyphs.items() if g in new_of_old}
        # Every strike stays, an empty one with no glyph: a strike is chosen by size alone.
        if key not in b:
            return "strike %s was lost" % (key,)
        got = b[key][1]
        if key in b_ranges and got and b_ranges[key] != (min(got), max(got)):
            return "strike %s states glyphs %s but holds %s" % (
                key, b_ranges[key], (min(got), max(got)))
        want = {new_of_old[g]: v for g, v in kept.items()}
        if set(got) != set(want):
            return "strike %s glyphs: %s vs %s" % (key, sorted(got)[:6], sorted(want)[:6])
        for g in want:
            if got[g] != want[g]:
                return "strike %s new glyph %d differs: %s vs %s" % (
                    key, g, [str(x)[:30] for x in got[g]], [str(x)[:30] for x in want[g]])
            pictures += 1
    for key in b:
        if key not in a:
            return "a strike %s appeared" % (key,)
    return "ok %d %d" % (len(old_of_new), pictures)

pairs = json.load(open(sys.argv[1]))
json.dump([check(*p) for p in pairs], open(sys.argv[2], "w"))
"""


def has_strikes(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return bool(tags & {b"CBLC", b"sbix", b"EBLC"})


def main(argv):
    verbose = "-v" in argv
    rest = [a for a in argv[1:] if not a.startswith("-")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
    else:
        fixtures = [os.path.join(color_diff.FIXTURES, n)
                    for n in sorted(os.listdir(color_diff.FIXTURES))
                    if n.endswith((".ttf", ".otf"))]
        fonts = [f for f in fixtures + corpus.fonts("sfnt") if has_strikes(f)
                 and not color_diff.axes_of(f)]
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
        cps = info[font]["cps"][:-1]
        usable = [c for c in cps if c >= 0x20]
        # The three of the color gate, and two that move the glyph numbers a long way: the
        # upper half, which drops the low glyphs a dupe may name, and every second one.
        picks = subset_color_diff.subsets(cps)
        if len(usable) > 3:
            picks += [usable[len(usable) // 2:][:260], usable[1::2][:260]]
        for k, chosen in enumerate(picks):
            tag = "%d_%d" % (number, k)
            out = os.path.join(SCRATCH, tag + ".ttf")
            mapf = os.path.join(SCRATCH, tag + ".map")
            ran = subprocess.run([os.path.join(EXAMPLES, "font-subset"), "--unicodes",
                ",".join("%X" % c for c in chosen), "--map", mapf, font, out],
                capture_output=True, text=True)
            if ran.returncode != 0 and "composite bitmap" in ran.stderr:
                declined.add(font)
                continue
            if ran.returncode != 0:
                refused += 1
                print("REFUSED %s (%d characters): %s" % (os.path.relpath(font, ROOT),
                      len(chosen), ran.stderr.strip().splitlines()[-1] if ran.stderr else ""))
                continue
            pairs.append([font, out, mapf])
            names.append((font, chosen, os.path.getsize(font), os.path.getsize(out), tag,
                          info[font]["upem"]))
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

    # HarfBuzz: the same text in the original and the subset, one character a line.
    lines = []
    for (font, chosen, _, _, tag, upem), (_, out, _) in zip(names, pairs):
        with open(os.path.join(SCRATCH, tag + ".txt"), "w", encoding="utf-8") as handle:
            handle.write("".join(chr(c) + "\n" for c in chosen))
        for which, path in (("a", font), ("b", out)):
            lines.append("hb-shape --font-file='%s' --font-size=%d --show-extents "
                         "--no-glyph-names --output-format=json --text-file='%s/%s.txt' "
                         "> '%s/%s.%s.hb' 2>/dev/null" % (path, upem, SCRATCH, tag,
                                                          SCRATCH, tag, which))
    with open(os.path.join(SCRATCH, "run.sh"), "w") as handle:
        handle.write("\n".join(lines) + "\n")
    ran = subprocess.run(oracle_env.command("harfbuzz",
        ["sh", os.path.join(SCRATCH, "run.sh")], scratch=SCRATCH),
        capture_output=True, text=True)
    if ran.returncode not in (0, 1):
        raise oracle_env.OracleUnavailable("hb-shape failed: %s" % ran.stderr[-300:])

    failed = glyphs = pictures = boxes = skipped = gained = 0
    before = after = 0
    for (font, chosen, size_a, size_b, tag, _), result in zip(names, results):
        before += size_a
        after += size_b
        if result.startswith("skip"):
            skipped += 1
            continue
        if not result.startswith("ok"):
            failed += 1
            print("DIFF %s (%d characters): %s" % (os.path.relpath(font, ROOT),
                                                   len(chosen), result))
            continue
        _, g, p = result.split()
        glyphs += int(g)
        pictures += int(p)
        boxed = []
        for which in ("a", "b"):
            with open(os.path.join(SCRATCH, "%s.%s.hb" % (tag, which)),
                      encoding="utf-8") as handle:
                boxed.append([[(x["xb"], x["yb"], x["w"], x["h"]) for x in json.loads(l)]
                              for l in handle.read().split("\n")[:-1]])
        # A box the original has must survive. One it did not have may appear: image
        # formats 5 and 19, which HarfBuzz cannot read, become 7 and 18, which it can.
        bad = next((i for i, (x, y) in enumerate(zip(boxed[0], boxed[1]))
                    if x != y and any(any(v) for v in x)), None)
        gained += sum(1 for x, y in zip(boxed[0], boxed[1])
                      if x != y and not any(any(v) for v in x))
        if bad is not None or len(boxed[0]) != len(boxed[1]):
            failed += 1
            print("DIFF %s: HarfBuzz measures U+%04X as %s in the original and %s in "
                  "the subset" % (os.path.relpath(font, ROOT), chosen[bad or 0],
                                  boxed[0][bad or 0], boxed[1][bad or 0]))
        boxes += len(boxed[0])
        if verbose:
            print("ok   %s %d characters: %d -> %d bytes" % (
                os.path.relpath(font, ROOT), len(chosen), size_a, size_b))
    print("subset_strike_diff: %d fonts (%d declined: composite bitmaps), %d subsets read "
          "back (%d refused, %d fontTools could not read), %d glyphs kept, %d pictures, %d "
          "boxes shaped (%d of them new to HarfBuzz: formats 5 and 19 became 7 and 18), %d "
          "differing; %d bytes in, %d out"
          % (len(fonts), len(declined), len(pairs), refused, skipped, glyphs, pictures,
             boxes, gained, failed, before, after))
    return 1 if failed or refused or not pairs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
