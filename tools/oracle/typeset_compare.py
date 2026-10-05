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
"""The same line of text, drawn by HarfBuzz and by this library, one above the other.

This is a picture, not a gate. `hb_diff.py` is the gate: it compares the glyphs and
the positions, exactly. What it cannot show is what the two answers look like, and
the part of the pipeline it says nothing about at all - the rasteriser, and the
way a shaped run is laid on a baseline. So this draws each sample twice:

  * **HarfBuzz and `hb-view`**, in the harfbuzz image: HarfBuzz shapes, and cairo
    with FreeType (unhinted, `--ft-load-flags=2`) rasterises. Two libraries, none
    of them this one.
  * **This library**, `font-typeset`: `gfnt_face_shape()` and
    `gfnt_face_render_glyph()`, each glyph rendered at the sub-pixel offset the
    shaper gave it.

and writes one PNG per run: for each sample a label, the reference, this library,
and a third strip that is where they differ (white where they agree, darker the
more they disagree). The canvas follows `hb-view`'s own layout - margin, baseline,
size - so the rows are the same size and line up.

The pixel difference is reported too, as a number, because a picture that looks
the same can still be a pixel off everywhere: **the mean absolute difference per
pixel and the share of pixels differing by more than a quarter**. Two
rasterisers of the same outline differ at the anti-aliased edges, by rounding, so
a small mean is what agreement looks like; a large one is a line that was
shaped differently or placed somewhere else.

Usage:
    typeset_compare.py [--out file.png] [--size N]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-typeset")
SCRATCH = os.path.join(ROOT, "build", "oracle", "typeset")

# (label, font under the corpus, script ISO, OT script, features, text, location).
# What each sample is for is in its label: every one is a place the *shaper* decides
# what is drawn, which a plain advance-by-advance loop would get wrong. The location,
# when there is one, is a place in a variable font's design space.
SAMPLES = [
    ("kerning and the ffi ligature - DejaVu Serif",
     "truetype/dejavu/DejaVuSerif.ttf", "Latn", "latn", "",
     "AVATAR Tofu: office Wavy", ""),
    ("kerning pairs and ligatures - Liberation Serif",
     "truetype/liberation/LiberationSerif-Regular.ttf", "Latn", "latn", "",
     "To Wa Yo VAT fi fl ffi ffl", ""),
    ("a mark placed on a base - Cantarell",
     "opentype/cantarell/Cantarell-VF.otf", "Latn", "latn", "",
     "x\u0301 q\u0307 j\u0323 e\u0301 a\u0308", ""),
    ("figures and case - Inter, default instance",
     "truetype/inter-vf/InterVariable.ttf", "Latn", "latn",
     "+case,+zero,+tnum",
     "(Hello) 0123 [A-Z] 1/2", ""),
    ("kerning that moves with the weight - Inter at wght=800",
     "truetype/inter-vf/InterVariable.ttf", "Latn", "latn", "",
     "Variable Type AVATAR To Wa", "wght=800"),
    ("Greek and Cyrillic - DejaVu Sans",
     "truetype/dejavu/DejaVuSans.ttf", "Grek", "grek", "",
     "\u0391\u03bb\u03c6\u03ac\u03b2\u03b7\u03c4\u03bf \u03a4\u0391 "
     "\u041f\u0440\u0438\u0432\u0435\u0442 \u0422\u0410", ""),
]


def corpus_font(relative):
    for base in corpus.fonts("sfnt"):
        if base.endswith(relative):
            return base
    raise SystemExit("not in the corpus: %s" % relative)


def main(argv):
    from PIL import Image, ImageDraw, ImageFont

    out = os.path.join(SCRATCH, "compare.png")
    size = 56
    for i, a in enumerate(argv):
        if a == "--out":
            out = argv[i + 1]
        elif a == "--size":
            size = int(argv[i + 1])
    os.makedirs(SCRATCH, exist_ok=True)

    script = os.path.join(SCRATCH, "view.sh")
    with open(script, "w", encoding="utf-8") as handle:
        for i, (label, rel, iso, ot, features, text, location) in enumerate(SAMPLES):
            parts = ["hb-view", "--font-file=%s" % corpus_font(rel),
                     "--font-size=%d" % size, "--margin=16",
                     "--script=%s" % iso, "--output-format=png",
                     "--output-file=%s" % os.path.join(SCRATCH, "hb-%d.png" % i)]
            if features:
                parts.append("--features=%s" % features)
            if location:
                parts.append("--variations=%s" % location)
            parts.append("--text=%s" % text)
            handle.write(" ".join("'%s'" % p.replace("'", "'\\''")
                                  for p in parts) + "\n")
    argv_hb = oracle_env.command("harfbuzz", ["sh", script], scratch=SCRATCH)
    finished = subprocess.run(argv_hb, capture_output=True, text=True)
    if finished.returncode != 0:
        sys.exit("hb-view failed: %s" % finished.stderr.strip()[:300])

    font = ImageFont.load_default(size=14)
    rows = []
    report = []
    for i, (label, rel, iso, ot, features, text, location) in enumerate(SAMPLES):
        mine_pgm = os.path.join(SCRATCH, "ours-%d.pgm" % i)
        cmd = [DRIVER, "--script", ot]
        if features:
            cmd += ["--features", features]
        if location:
            cmd += ["--location", location]
        cmd += [corpus_font(rel), str(size), mine_pgm, text]
        finished = subprocess.run(cmd, capture_output=True, text=True)
        if finished.returncode != 0:
            sys.exit("font-typeset failed on %s: %s" % (label, finished.stderr))
        a = Image.open(os.path.join(SCRATCH, "hb-%d.png" % i)).convert("L")
        b = Image.open(mine_pgm).convert("L")
        width = max(a.width, b.width)
        height = max(a.height, b.height)
        canvas_a = Image.new("L", (width, height), 255)
        canvas_b = Image.new("L", (width, height), 255)
        canvas_a.paste(a, (0, 0))
        canvas_b.paste(b, (0, 0))
        pa = canvas_a.load()
        pb = canvas_b.load()
        diff = Image.new("L", (width, height), 255)
        pd = diff.load()
        total = 0
        over = 0
        for y in range(height):
            for x in range(width):
                d = abs(pa[x, y] - pb[x, y])
                total += d
                if d > 64:
                    over += 1
                pd[x, y] = 255 - min(255, d * 2)
        report.append((label, width, height, total / (width * height),
                       100.0 * over / (width * height)))
        rows.append((label, canvas_a, canvas_b, diff))

    gap = 6
    label_h = 22
    width = max(r[1].width for r in rows)
    height = sum(label_h + 3 * r[1].height + 4 * gap for r in rows)
    sheet = Image.new("L", (width, height), 235)
    draw = ImageDraw.Draw(sheet)
    y = 0
    for label, a, b, d in rows:
        draw.text((6, y + 3), label, fill=0, font=font)
        y += label_h
        for tag, image in (("HarfBuzz + cairo/FreeType", a),
                           ("this library", b), ("difference", d)):
            sheet.paste(image, (0, y))
            draw.text((width - 190, y + 4), tag, fill=110, font=font)
            y += image.height + gap
        y += gap
    sheet.save(out)
    print("wrote %s (%dx%d)" % (out, sheet.width, sheet.height))
    for label, w, h, mean, share in report:
        print("  %-52s %4dx%-3d mean |diff| %.2f, %.1f%% of pixels differ by >1/4"
              % (label, w, h, mean, share))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
