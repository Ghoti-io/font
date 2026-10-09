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
"""Which layout tables HarfBuzz applies, for every combination of tables present.

The rules that choose between GSUB, morx/mort, GPOS, kerx and kern are not in any
specification. This builds, for each subset of {GSUB, GPOS, GPOS with a kern
feature, morx, mort, kerx, kern} and each direction, a font in which every table has
an effect that can be told from the others (GSUB maps glyph 1 to 20, morx 1 to 21 and
20 to 22, GSUB 21 to 23; GPOS widens by 1000; kern takes 20 off the pair, kerx 40),
shapes "AB" in HarfBuzz and here, and prints what each did. The truth table is what
`plan.c` has to reproduce; any row where this library differs is listed.

Usage: precedence_probe.py [--table] [--direction ltr|rtl|ttb|btt] [--script latn|arab]
                           [--gdef] [--vert] [--ltag latn|DFLT|native] [--only a,b] [--drop a,b]
"""

import itertools
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "precedence")
NG = 30
FIRSTS = (1, 20, 21, 22, 23)
# Per script: two base characters (glyphs 1, 2) and a mark (glyph 4).
CHARS = {"latn": "ABD\u0301", "arab": "\u0628\u062c\u064e", "hebr": "\u05d0\u05d1\u05b7",
         "thai": "\u0e01\u0e02\u0e31", "deva": "\u0915\u0916\u0941",
         "khmr": "\u1780\u1781\u17b7", "mymr": "\u1000\u1001\u102d", "sinh": "\u0d9a\u0d9b\u0dd2",
         "tibt": "\u0f40\u0f41\u0f71", "bali": "\u1b13\u1b14\u1b35", "laoo": "\u0e81\u0e82\u0eb1",
         "cyrl": "\u0410\u0411\u0301", "grek": "\u0391\u0392\u0301"}
OT_TAGS = {"deva": "dev2", "mymr": "mym2", "laoo": "lao "}
TABLES = ("gsub", "gpos", "gposk", "morx", "mort", "kerx", "kern")


def s16(v):
    return u16(v & 0xFFFF)


def layout(tag, feature, lookups, script="latn"):
    """A GSUB/GPOS with one script, one feature naming every lookup."""
    n = len(lookups)
    scr = u16(4, 0) + u16(0, 0xFFFF, 1, 0)
    scriptlist = u16(1) + script.encode() + u16(8) + scr
    feat = u16(0, n, *range(n))
    featlist = u16(1) + feature.encode() + u16(8) + feat
    body = b''
    offs = []
    base = 2 + 2 * n
    for typ, sub in lookups:
        lk = u16(typ, 0, 1, 8) + sub
        offs.append(base + len(body))
        body += lk
    lklist = u16(n, *offs) + body
    head = 10
    sl = head
    fl = sl + len(scriptlist)
    ll = fl + len(featlist)
    return u32(0x10000) + u16(sl, fl, ll) + scriptlist + featlist + lklist


def cov(gl):
    return u16(1, len(gl), *sorted(gl))


def gsub_table(feature="liga", ltag="latn"):
    # 1 -> 20 and 21 -> 23, in one lookup (a single substitution does one glyph).
    s1 = u16(2, 8, 1, 20) + cov([1])
    s2 = u16(2, 8, 1, 23) + cov([21])
    return layout("GSUB", feature, [(1, s1), (1, s2)], ltag)


def gpos_table(feature, ltag="latn"):
    cv = cov(list(FIRSTS))
    # coverage offset is from the start of the subtable: header 6 + one record 4.
    sub = u16(1, 10, 0x000C) + s16(1000) + s16(1000) + cv
    return layout("GPOS", feature, [(1, sub)], ltag)


def kern_pairs(value, lefts):
    ps = sorted((l, 2, value) for l in lefts)
    return b''.join(u16(a, b) + s16(v) for a, b, v in ps), len(ps)


def kern_table():
    body, n = kern_pairs(-20, FIRSTS)
    sub = u16(n, 0, 0, 0) + body
    return u16(0, 1) + u16(0, 6 + len(sub), 1) + sub


def kerx_table():
    body, n = kern_pairs(-40, FIRSTS)
    sub = u32(n, 0, 0, 0) + body
    sub += b'\0' * (-len(sub) % 4)
    return u16(2, 0) + u32(1) + u32(12 + len(sub), 0, 0) + sub


def lookup8(m, first, last):
    return u16(8, first, last - first + 1, *[m.get(g, g) for g in range(first, last + 1)])


def morx_table(old, vertical=False):
    m = {1: 21, 20: 22}
    body = lookup8(m, 1, 23)
    body += b'\0' * (-len(body) % 4)
    if old:
        sub = u16(8 + len(body), 4 | (0x8000 if vertical else 0)) + u32(1) + body
        return u32(0x00010000, 1) + u32(1, 12 + len(sub)) + u16(0, 1) + sub
    sub = u32(12 + len(body), 4 | (0x80000000 if vertical else 0), 1) + body
    chain = u32(1, 16 + len(sub), 0, 1) + sub
    return u32(0x00020000, 1) + chain


def build(have, gdef, vert=False, script="latn", glyf=False, ltag="latn"):
    extra = {}
    if glyf:
        import mixed_random_diff as X
        import random
        extra.update(X.glyf_tables(random.Random(7)))
    if "gsub" in have:
        extra['GSUB'] = gsub_table('vert' if vert else 'liga', ltag)
    if "gpos" in have:
        extra['GPOS'] = gpos_table("ccmp", ltag)
    if "gposk" in have:
        extra['GPOS'] = gpos_table("kern", ltag)
    if "morx" in have:
        extra['morx'] = morx_table(False, vert)
    if "mort" in have:
        extra['mort'] = morx_table(True, vert)
    if "kerx" in have:
        extra['kerx'] = kerx_table()
    if "kern" in have:
        extra['kern'] = kern_table()
    if gdef:
        import gpos_random_diff as P
        extra['GDEF'] = P.gdef()
    return base_font(nglyphs=NG, cmap_map={ord(CHARS[script][0]): 1, ord(CHARS[script][1]): 2, ord(CHARS[script][2]): 4,
                                      0x301: 4}, extra=extra)


def subsets(only, drop):
    pool = [t for t in TABLES if (not only or t in only) and t not in drop]
    for k in range(len(pool) + 1):
        for c in itertools.combinations(pool, k):
            if "gpos" in c and "gposk" in c:
                continue
            if "morx" in c and "mort" in c:
                continue
            yield frozenset(c)


def describe(out, direction):
    """What the output says was applied, from the first glyph and the pair."""
    if len(out) != 2:
        return "n=%d" % len(out)
    g0, g1 = out[0], out[1]
    sub = {1: "-", 20: "gsub", 21: "morx", 22: "gsub>morx", 23: "morx>gsub"}.get(g0["g"], "g%d" % g0["g"])
    adv = g0["ay"] if direction in ("ttb", "btt") else g0["ax"]
    base = 500 + 10 * g0["g"] if direction not in ("ttb", "btt") else None
    d0 = None if base is None else adv - base
    return "%s adv%+d off1=%d" % (sub, d0 if d0 is not None else adv, g1.get("dx", 0))


def ltag_for(sc, argv):
    t = argv[argv.index("--ltag") + 1] if "--ltag" in argv else "latn"
    return OT_TAGS.get(sc, sc) if t == "native" else t


def main(argv):
    only = tuple(argv[argv.index("--only") + 1].split(",")) if "--only" in argv else ()
    drop = tuple(argv[argv.index("--drop") + 1].split(",")) if "--drop" in argv else ()
    dirs = [argv[argv.index("--direction") + 1]] if "--direction" in argv else ["ltr", "rtl", "ttb", "btt"]
    gdef = "--gdef" in argv
    scripts = argv[argv.index("--scripts") + 1].split(",") if "--scripts" in argv else ["latn"]
    OT = {"latn": "latn", "arab": "arab", "hebr": "hebr", "thai": "thai", "deva": "dev2", "khmr": "khmr", "mymr": "mym2", "sinh": "sinh", "tibt": "tibt",
          "bali": "bali", "laoo": "lao ", "cyrl": "cyrl", "grek": "grek"}
    driver = G.DRIVER
    os.makedirs(SCRATCH, exist_ok=True)
    cases, lines = [], []
    for i, have in enumerate(subsets(only, drop)):
        for sc in scripts:
          font = os.path.join(SCRATCH, "p%d%s.ttf" % (i, sc))
          with open(font, "wb") as h:
            h.write(build(have, gdef, '--vert' in argv, sc, '--glyf' in argv, ltag_for(sc, argv)))
          x, y, m = CHARS[sc][0], CHARS[sc][1], CHARS[sc][2]
          text = os.path.join(SCRATCH, "p-%s.txt" % sc)
          with open(text, "w", encoding="utf-8") as h:
            h.write("\n".join((x + y, x + m, m + x, x + m + y)) + "\n")
          for d in dirs:
            if not (sc in ("arab", "hebr") and d == "ltr"):
                cases.append((have, d, font, sc))
                lines.append("hb-shape --font-file='%s' --output-format=json --direction=%s "
                             "--no-glyph-names --script=%s --text-file='%s' > '%s.%s.%s.hb' 2>/dev/null"
                             % (font, d, sc.capitalize(), text, font, d, sc))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "ax", "ay", "dx", "dy")
    bad = 0
    for have, d, font, sc in cases:
        with open("%s.%s.%s.hb" % (font, d, sc)) as h:
            hbs = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
        flags = {"rtl": ["--rtl"], "ttb": ["--ttb"], "btt": ["--btt"], "ltr": []}[d]
        with open(os.path.join(SCRATCH, "p-%s.txt" % sc), "rb") as h:
            ours = subprocess.run([driver, "--batch", "--script", OT[sc]] + flags + [font],
                                  stdin=h, capture_output=True, text=True, timeout=20)
        mines = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        txt = ["XY", "XM", "MX", "XMY"]
        wrong = [i for i, (a, b) in enumerate(zip(hbs, mines))
                 if [tuple(x.get(k) for k in keys) for x in a] != [tuple(x.get(k) for k in keys) for x in b]]
        if wrong:
            bad += 1
        if "--table" in argv or wrong:
            print("%-3s %-5s %-28s HB: %-34s %s" % (d, sc, '+'.join(sorted(have)) or "(none)",
                  describe(hbs[0], d), "" if not wrong else "  DIFFERS on " + ','.join(txt[i] for i in wrong)))
            for i in wrong[:1]:
                if "-v" in argv:
                    print("    hb ", [tuple(x.get(k) for k in keys) for x in hbs[i]])
                    print("    our", [tuple(x.get(k) for k in keys) for x in mines[i]])
    print("precedence_probe: %d cases, %d differ" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
