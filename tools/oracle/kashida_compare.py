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
"""Kashida justification against the screenshots of the Chromium kashida patch.

notes/font/KASHIDA.md. No build of the patch (CL 7957320) runs here, so its oracle is the set of
screenshots its published page shows ("expected": Amiri, `text-justify: kashida`, one line each,
`text-align-last: justify`). `--fetch` downloads them and the Amiri font the page uses into
build/oracle/kashida/ and checks them against the hashes below (the Google Fonts Arabic subset of
Amiri, so a change at the host is seen); the comparison then lays each string out here, in the
same box, with `GFNT_JUSTIFY_KASHIDA`, and compares ink.

The screenshots are 2.31 times the page's CSS pixels (found by matching an unstretched word:
Amiri 26 px is 60 here) and the box is the image's width less 59 pixels of padding. Compared per
case: where the ink starts and ends along the line, and the column profile of the ink (how much
ink there is in each pixel column), as the sum of absolute differences over the sum of both.
That is blind to height and to which glyph is which, and sees exactly what a kashida is: where a
stroke is, how long, and whether the letters around it have moved.

Usage: kashida_compare.py [--fetch] [-v]
"""

import hashlib
import os
import subprocess
import sys
import urllib.request

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "build", "oracle", "kashida")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-paragraph")
BASE = "https://static.januschka.com/i-41431127/img/%s.png"
FONT_URL = "https://fonts.gstatic.com/s/amiri/v30/J7aRnpd8CGxBHpUrtLMA7w.woff2"
FONT_SHA = "9abf8a10b4a2f27b698740522c9beec9ded7728aeef0738ac0d6e175bf6249d7"
LATIN_URL = "https://fonts.gstatic.com/s/amiri/v30/J7aRnpd8CGxBHpUutLM.woff2"   # for the Latin and digits
SIZE = 60                 # the screenshots' scale: Amiri 26 px
PADDING = 59              # image width less the box's content width
MARGIN = 29               # from the image's left edge to the box's content (border, padding)

# id (its path under the page's img/), text, direction, space_share (the page's own list)
CASES = [
    ("salsabil", "سلسبيل", "rtl"), ("kataba", "كتب", "rtl"), ("muslim", "مسلم", "rtl"),
    ("madrasa", "مدرسة", "rtl"), ("barid", "بريد", "rtl"), ("zir", "زر", "rtl"),
    ("ward", "ورد", "rtl"), ("hilal", "هلال", "rtl"), ("allah", "الله", "rtl"),
    ("istiqlal", "استقلال", "rtl"),
    ("vowelled", "نَصٌّ حَكِيمٌ", "rtl"), ("kataba_vow", "كَتَبَ", "rtl"),
    ("persian", "چند کلمه", "rtl"), ("punjab", "پنجاب", "rtl"), ("zhe", "ژژژ", "rtl"),
    ("lamalef", "لا", "rtl"), ("bidi", "النسخة 2.4 جاهزة", "rtl"),
    ("digits", "١٢٣٤٥", "rtl"), ("digits_ext", "۱۲۳۴۵", "rtl"),
    ("tatweel", "كــتاب", "rtl"), ("ltrpara", "with سلسبيل inside", "ltr"),
    ("pangram", "نص حكيم له سر قاطع وذو شأن عظيم", "rtl"),
    ("basmala", "بسم الله الرحمن الرحيم", "rtl"),
]
RATIOS = [("kspace100", "نص حكيم", "rtl", 0.0), ("kspace50", "نص حكيم", "rtl", 0.5),
          ("kspace0", "نص حكيم", "rtl", 1.0)]
# The judgement: the screenshot and this layout agree to within these.
EDGE_TOLERANCE = 12       # where the ink starts and ends, pixels at the screenshots' scale (a tatweel is 11)
                          # draws whole tatweels (11 of them here) and leaves the remainder at the line's
                          # end, this library shortens them to fill it
PROFILE_TOLERANCE = 0.25  # of the ink in the two, for what is where in between


def fetch():
    os.makedirs(os.path.join(DATA, "img"), exist_ok=True)
    targets = [(FONT_URL, os.path.join(DATA, "amiri-arabic.woff2"), FONT_SHA),
               (LATIN_URL, os.path.join(DATA, "amiri-latin.woff2"), None)]
    targets += [(BASE % ("cases/" + c[0]), os.path.join(DATA, "img", c[0] + ".png"), None)
                for c in CASES]
    targets += [(BASE % c[0], os.path.join(DATA, "img", c[0] + ".png"), None) for c in RATIOS]
    for url, path, sha in targets:
        with urllib.request.urlopen(url) as r:
            data = r.read()
        if sha and hashlib.sha256(data).hexdigest() != sha:
            sys.exit("%s is not the font the comparison was made with" % url)
        with open(path, "wb") as h:
            h.write(data)
    with open(os.path.join(DATA, "SHA256"), "w") as h:
        for url, path, sha in targets:
            h.write("%s  %s\n" % (hashlib.sha256(open(path, "rb").read()).hexdigest(),
                                  os.path.relpath(path, DATA)))


def ink(a):
    return a < 128


def render(text, width, direction, share):
    out = os.path.join(DATA, "ours.pgm")
    argv = [DRIVER, "--size", str(SIZE), "--width", str(width), "--align", "justify",
            "--justify-last", "--justify", "kashida", "--space-share", str(share),
            "--bidi", direction, "--margin", str(MARGIN),
            "--fallback", os.path.join(DATA, "amiri-latin.woff2"),
            os.path.join(DATA, "amiri-arabic.woff2"), out, text]
    subprocess.run(argv, check=True, capture_output=True)
    return np.array(Image.open(out).convert("L"))


def profile(a):
    return ink(a).sum(axis=0).astype(float)


def shifted_score(pt, pm, reach):
    """The best profile score over shifts of one against the other by up to `reach` pixels: a
    remainder of less than a tatweel is left at one end or the other, and a stroke one tatweel
    longer or shorter is not a stroke in another place."""
    best = 1.0
    for d in range(-reach, reach + 1):
        a = pt[max(0, d):len(pt) + min(0, d)]
        b = pm[max(0, -d):len(pm) + min(0, -d)]
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        best = min(best, np.abs(a - b).sum() / (a.sum() + b.sum()))
    return best


def main():
    if "--fetch" in sys.argv:
        fetch()
    if not os.path.exists(os.path.join(DATA, "amiri-arabic.woff2")):
        sys.exit("kashida_compare: run with --fetch first (the screenshots and the font are not "
                 "in the repository)")
    worst = 0
    differ = 0
    cases = [(i, t, d, 0.0) for i, t, d in CASES] + RATIOS
    for ident, text, direction, share in cases:
        theirs = np.array(Image.open(os.path.join(DATA, "img", ident + ".png")).convert("L"))
        w = theirs.shape[1]
        mine = render(text, w - PADDING, direction, share)
        # The same window: the box's row range does not matter, the columns do.
        pt = profile(theirs[8:-8, :])
        pm = profile(mine)
        pm = np.pad(pm, (0, max(0, w - len(pm))))[:w]
        ct = np.where(pt > 0)[0]
        cm = np.where(pm > 0)[0]
        dl = int(cm.min() - ct.min())
        dr = int(cm.max() - ct.max())
        score = shifted_score(pt, pm, EDGE_TOLERANCE)
        ok = abs(dl) <= EDGE_TOLERANCE and abs(dr) <= EDGE_TOLERANCE and score <= PROFILE_TOLERANCE
        differ += not ok
        worst = max(worst, score)
        if not ok or "-v" in sys.argv:
            print("%-11s %s  edges %+3d %+3d  profile %.3f" % (
                ident, "agrees" if ok else "DIFFERS", dl, dr, score))
    print("kashida_compare: %d cases, %d differ (worst profile %.3f)" % (len(cases), differ, worst))
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
