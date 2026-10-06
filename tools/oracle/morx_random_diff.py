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
"""Random AAT `morx` chains, shaped by this library and by HarfBuzz, and compared.

A font per seed with one chain of one to three random subtables - rearrangement,
contextual substitution, ligature, non-contextual and insertion - each with a
random state table over the glyphs A..H and their neighbours, shaped with a dozen
strings, glyph ids and clusters compared. Nothing is committed: every font is a
function of its seed. A string that runs past the number of steps HarfBuzz allows,
or hangs this library, is left out of the count.

Usage: morx_random_diff.py [--seeds N] [--first K] [--reuse] [-v] [--driver PATH]
                           [--scratch DIR] [--types 0,1,2,4,5]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "morx-random")
GLYPHS = list(range(1, 15))
DONT = 0.08
COVER = False
DONT_INS = 0.0   # an insertion that does not advance may never stop


def lookup8(m, first=1, last=14, missing=0):
    """A lookup table of format 8 (a trimmed array) over glyphs first..last."""
    return u16(8, first, last - first + 1, *[m.get(g, g if missing is None else missing) for g in range(first, last + 1)])


def pad(b, n=4):
    return b + b'\0' * (-len(b) % n)


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def g(self):
        return self.r.choice(GLYPHS)

    def classes(self, nclasses):
        # Every glyph in 1..14 has a class; 0 and 1 are for the end of text and glyphs
        # outside the table, 2 for deleted and 3 for the end of the line.
        return {g: self.r.choice((1, 2, 3) + tuple(range(4, nclasses)) * 3)
                if self.r.random() < .12 else self.r.choice(tuple(range(4, nclasses)))
                for g in GLYPHS}

    def stx(self, kind):
        r = self.r
        nclasses = r.randint(5, 8)
        nstates = r.randint(2, 5)
        nentries = r.randint(2, 7)
        classes = self.classes(nclasses)
        states = [[r.randrange(nentries) for _ in range(nclasses)] for _ in range(nstates)]
        # Both start states have to lead somewhere.
        return nclasses, nstates, nentries, classes, states

    def subtable(self, kind):
        r = self.r
        if kind == 4:
            m = {g: r.choice(GLYPHS) for g in GLYPHS if r.random() < .6}
            return pad(lookup8(m, missing=None))
        nclasses, nstates, nentries, classes, states = self.stx(kind)
        clstab = pad(lookup8(classes))
        statetab = pad(b''.join(u16(*row) for row in states))
        extras = b''
        entries = []
        if kind == 0:
            for _ in range(nentries):
                flags = r.choice((0, 0x8000, 0x2000, 0xA000)) | r.randint(0, 15)
                if r.random() < DONT:
                    flags |= 0x4000
                entries.append(u16(r.randrange(nstates), flags))
        elif kind == 1:
            nsub = r.randint(1, 3)
            subs = [{g: r.choice(GLYPHS) for g in GLYPHS if r.random() < .5}
                    for _ in range(nsub)]
            for _ in range(nentries):
                flags = r.choice((0, 0, 0x8000))
                if r.random() < DONT:
                    flags |= 0x4000
                mark = r.choice((0xFFFF, 0xFFFF) + tuple(range(nsub)))
                cur = r.choice((0xFFFF, 0xFFFF) + tuple(range(nsub)))
                entries.append(u16(r.randrange(nstates), flags, mark, cur))
            body = b''
            offs = []
            head = 4 * nsub
            for m in subs:
                offs.append(head + len(body))
                body += pad(lookup8(m, missing=None))
            extras = pad(u32(*offs) + body)
        elif kind == 2:
            ncomp, nlig = 24, 40
            comps = [r.randrange(3) for _ in range(ncomp)]
            ligs = [r.choice(GLYPHS) for _ in range(nlig)]
            nact = r.randint(2, 6)
            actions = []
            for i in range(nact):
                v = r.randint(0, ncomp - 16)
                last = 0x80000000 if r.random() < .4 or i == nact - 1 else 0
                store = 0x40000000 if r.random() < .5 else 0
                actions.append(last | store | v)
            for _ in range(nentries):
                flags = r.choice((0, 0x8000, 0x2000, 0xA000, 0x2000))
                if r.random() < DONT:
                    flags |= 0x4000
                entries.append(u16(r.randrange(nstates), flags, r.randrange(nact)))
            extras = (pad(u32(*actions)), pad(u16(*comps)), pad(u16(*ligs)))
        elif kind == 5:
            nins = 16
            glyphs = [r.choice(GLYPHS) for _ in range(nins)]
            for _ in range(nentries):
                flags = r.choice((0, 0x8000)) | (0x4000 if r.random() < DONT_INS else 0)
                cc, mc = r.randint(0, 3), r.randint(0, 3)
                flags |= (cc << 5) | mc
                flags |= r.choice((0, 0x800)) | r.choice((0, 0x400))
                entries.append(u16(r.randrange(nstates),
                                   flags, r.choice((0xFFFF, r.randrange(nins - 3))),
                                   r.choice((0xFFFF, r.randrange(nins - 3)))))
            extras = pad(u16(*glyphs))
        enttab = pad(b''.join(entries))
        nextra = {0: 0, 1: 1, 2: 3, 5: 1}[kind]
        head_len = 16 + 4 * nextra
        off_cls = head_len
        off_state = off_cls + len(clstab)
        off_ent = off_state + len(statetab)
        off_extra = off_ent + len(enttab)
        header = u32(nclasses, off_cls, off_state, off_ent)
        if kind in (1, 5):
            header += u32(off_extra)
            tail = extras
        elif kind == 2:
            a, c, l = extras
            header += u32(off_extra, off_extra + len(a), off_extra + len(a) + len(c))
            tail = a + c + l
        else:
            tail = b''
        return header + clstab + statetab + enttab + tail

    def morx(self, types, features=False):
        n = self.r.randint(1, 3)
        subs = b''
        bits = (1, 2, 4, 8, 3, 6, 0xFFFFFFFF)
        for _ in range(n):
            kind = self.r.choice(types)
            body = self.subtable(kind)
            flags = self.r.choice(bits) if features else 1
            cov = kind | (self.r.choice((0, 0, 0x40000000, 0x80000000, 0x20000000, 0x10000000 | 0x40000000)) if COVER else 0)
            subs += u32(12 + len(body), cov, flags) + body
        feats = b''
        nf = 0
        if features:
            pool = [(1, 2), (1, 3), (1, 4), (1, 5), (11, 1), (11, 0), (37, 1), (37, 0), (0, 0), (0, 1)]
            required = [(1, 2), (1, 4), (37, 1), (11, 1)]
            for t, sel in required + self.r.sample(pool, self.r.randint(0, 3)):
                enable = self.r.choice((0, 2, 4, 8, 6, 3, 0xE))
                disable = self.r.choice((0xFFFFFFFF, 0xFFFFFFFD, 0xFFFFFFFB, 0xFFFFFFF1, 1))
                feats += u16(t, sel) + u32(enable, disable)
                nf += 1
        chain = u32(1, 16 + len(feats) + len(subs), nf, n) + feats + subs
        return u32(0x00020000, 1) + chain


AAT_TAGS = ('liga', 'dlig', 'smcp', 'frac', 'liga', 'smcp')


def features_for(seed):
    r = random.Random(seed * 17 + 3)
    out = []
    for _ in range(r.randint(0, 3)):
        t = r.choice(AAT_TAGS)
        k = r.randrange(5)
        if k == 4:
            a = r.randrange(0, 4)
            out.append("%s[%d:%d]" % (t, a, a + r.randint(1, 4)))
        else:
            out.append(r.choice(("+", "-", "")) + t)
    return ",".join(out) or "kern"


def feat_table():
    """A `feat` table that lets the features the chains name be asked for."""
    feats = [(0, [0, 1]), (1, [2, 3, 4, 5]), (11, [0, 1]), (37, [0, 1])]
    head = 12 + 12 * len(feats)
    names, settings = b'', b''
    for t, sels in feats:
        names += u16(t, len(sels)) + u32(head + len(settings)) + u16(0x8000 if t in (1,) else 0, 0)
        settings += b''.join(u16(x, 0) for x in sels)
    return u32(0x00010000) + u16(len(feats), 0) + u32(0) + names + settings


def main(argv):
    global DONT, COVER, DONT_INS
    COVER = '--coverage' in argv
    direction = argv[argv.index('--direction') + 1] if '--direction' in argv else ''
    aatfeat = "--aatfeatures" in argv
    seeds, first, reuse, driver, scratch = 200, 0, False, G.DRIVER, SCRATCH
    types = (0, 1, 2, 4, 5)
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--reuse": reuse = True
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--dont": DONT = float(argv[i + 1])
        elif a == "--dontins": DONT_INS = float(argv[i + 1])
        elif a == "--types": types = tuple(int(x) for x in argv[i + 1].split(","))
    os.makedirs(scratch, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    lines = []
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=30, cmap_map=cmap,
                              extra=dict({'morx': Gen(seed).morx(types, aatfeat)}, **({'feat': feat_table()} if aatfeat else {}))))
        rr = random.Random(seed * 7)
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 8)))
                            + "\n" for _ in range(12)))
        opt = " --features='%s'" % features_for(seed) if aatfeat else ""
        if direction:
            opt += " --direction=%s" % direction
        lines.append("hb-shape --font-file='%s' --output-format=json%s "
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, opt, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    if not reuse:
        ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                             capture_output=True, text=True)
        if ref.returncode not in (0, 1):
            sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
            return 2
    bad = compared = skipped = 0
    keys = ("g", "cl")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    shown = 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        try:
            with open(font + ".hb", encoding="utf-8") as h:
                hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
        except (OSError, ValueError):
            skipped += 1
            continue
        try:
            with open(text, "rb") as h:
                extra = ["--features", features_for(seed)] if aatfeat else []
                if direction:
                    extra += {"rtl": ["--rtl"], "ttb": ["--ttb"], "btt": ["--btt"], "ltr": []}[direction]
                ours = subprocess.run([driver, "--batch", "--script", "latn"] + extra + [font],
                                      stdin=h, capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            skipped += 1
            continue
        mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        txt = open(text).read().split("\n")
        if len(hb) != len(mine):
            skipped += 1
            continue
        try:
            pairs = [(norm(a), norm(b)) for a, b in zip(hb, mine)]
        except AttributeError:
            skipped += 1
            continue
        for i, (a, b) in enumerate(pairs):
            compared += 1
            if a != b:
                bad += 1
                if shown < 8 and "-v" in argv:
                    shown += 1
                    print("seed", seed, txt[i], "\n  hb ", a, "\n  our", b)
    print("morx_random_diff: %d seeds from %d, %d strings, %d differ, %d seeds skipped"
          % (seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
