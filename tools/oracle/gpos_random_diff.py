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
"""Random GPOS lookups, shaped by this library and by HarfBuzz, and compared.

The companion of `gsub_random_diff.py`: a font per seed with a GDEF (base,
ligature and mark glyph classes) and a GPOS whose lookups are random in every
format (single, pair 1/2, cursive, mark-to-base, mark-to-ligature, mark-to-mark,
context and chain context with nested positioning lookups), shaped with a dozen
strings each, advances and offsets compared. Nothing is committed: every font is
a function of its seed.

Usage: gpos_random_diff.py [--seeds N] [--first K] [--reuse] [--driver PATH]
                           [--scratch DIR]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, cov1, classdef2, base_font, lookup_table

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "gpos-random")
GL = list(range(1, 9))                    # A..H
BASES, LIGS, MARKS = [1, 2, 7, 8], [3], [4, 5, 6]
FLAGS = (0, 0, 0, 1, 2, 4, 8, 0x10, 0x20, 0x100, 0x200, 0x11, 0x108)
# right-to-left, ignore base/lig/mark, a mark-filtering set, a mark attachment class


class PGen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def g(self): return self.r.choice(GL)

    def gset(self, pool=GL, k=None):
        k = k or self.r.randint(1, 4)
        return sorted(set(self.r.choice(pool) for _ in range(k)))

    def fmt(self):
        return self.r.choice((0x1, 0x2, 0x4, 0x8, 0x5, 0x4, 0x4, 0xF, 0x3, 0x6))

    def val(self, fmt):
        return b''.join(u16(self.r.randint(-150, 150)) for b in (1, 2, 4, 8) if fmt & b)

    def anchor(self):
        return u16(1, self.r.randint(-100, 300), self.r.randint(-100, 300))

    def tab(self, parts, hdr_fixed):
        """offsets (from the start) for parts placed after hdr_fixed bytes."""
        offs, pos = [], hdr_fixed
        for p in parts:
            offs.append(pos)
            pos += len(p)
        return offs

    def single(self):
        r = self.r
        cs = self.gset()
        cov = cov1(cs)
        fmt = self.fmt()
        n = bin(fmt).count('1')
        if r.random() < .5:
            return u16(1, 6 + 2 * n, fmt) + self.val(fmt) + cov
        recs = b''.join(self.val(fmt) for _ in cs)
        return u16(2, 8 + len(recs), fmt, len(cs)) + recs + cov

    def pair(self):
        r = self.r
        f1, f2 = self.fmt(), r.choice((0, 0, 0x4, 0x1, 0x2, 0x8))
        if r.random() < .5:
            cs = self.gset()
            cov = cov1(cs)
            sets = []
            for _ in cs:
                seconds = self.gset()
                sets.append(u16(len(seconds)) + b''.join(
                    u16(s) + self.val(f1) + self.val(f2) for s in seconds))
            hdr = 10 + 2 * len(cs)
            offs = self.tab(sets, hdr + len(cov))
            return u16(1, hdr, f1, f2, len(cs)) + u16(*offs)[:0] + u16(*offs) \
                if False else u16(1, hdr, f1, f2, len(cs), *offs) + cov + b''.join(sets)
        cs = self.gset()
        cov = cov1(cs)
        cd1 = classdef2({g: r.randint(1, 2) for g in GL if r.random() < .7})
        cd2 = classdef2({g: r.randint(1, 2) for g in GL if r.random() < .7})
        n1, n2 = 3, 3
        recs = b''.join(self.val(f1) + self.val(f2) for _ in range(n1 * n2))
        hdr = 16
        o_cov = hdr + len(recs)
        return u16(2, o_cov, f1, f2, o_cov + len(cov), o_cov + len(cov) + len(cd1),
                   n1, n2) + recs + cov + cd1 + cd2

    def cursive(self):
        cs = self.gset()
        cov = cov1(cs)
        recs, body = [], b''
        hdr = 6 + 4 * len(cs) + len(cov)
        for _ in cs:
            o = []
            for _ in range(2):
                if self.r.random() < .25:
                    o.append(0)
                else:
                    o.append(hdr + len(body))
                    body += self.anchor()
            recs.append(u16(*o))
        return u16(1, 6 + 4 * len(cs), len(cs)) + b''.join(recs) + cov + body

    def marks(self, kind):
        """3 mark-to-base, 4 mark-to-ligature, 5 mark-to-mark."""
        r = self.r
        nc = r.randint(1, 3)
        mpool = MARKS
        bpool = {3: BASES, 4: LIGS, 5: MARKS}[kind]
        mcs, bcs = self.gset(mpool), self.gset(bpool)
        mcov, bcov = cov1(mcs), cov1(bcs)
        marr_anchors = [self.anchor() for _ in mcs]
        mhdr = 2 + 4 * len(mcs)
        marr = u16(len(mcs)) + b''.join(
            u16(r.randrange(nc), mhdr + sum(len(a) for a in marr_anchors[:i]))
            for i in range(len(mcs))) + b''.join(marr_anchors)

        def anchors_row(hdr_len):
            row, body = [], b''
            for _ in range(nc):
                if r.random() < .2:
                    row.append(0)
                else:
                    row.append(hdr_len + len(body))
                    body += self.anchor()
            return row, body
        if kind in (3, 5):
            rows = []
            hdr = 2 + 2 * nc * len(bcs)
            allrow, body = [], b''
            for _ in bcs:
                row, b = anchors_row(hdr + len(body))
                # offsets are from the array start: anchors_row gives them directly
                allrow += row
                body += b
            barr = u16(len(bcs), *allrow) + body
        else:
            lats, body = [], b''
            hdr = 2 + 2 * len(bcs)
            for _ in bcs:
                ncomp = r.randint(1, 3)
                lhdr = 2 + 2 * nc * ncomp
                row, lbody = [], b''
                for _ in range(ncomp):
                    rr, b = anchors_row(lhdr + len(lbody))
                    row += rr
                    lbody += b
                lat = u16(ncomp, *row) + lbody
                lats.append(hdr + len(body))
                body += lat
            barr = u16(len(bcs), *lats) + body
        h = 12
        return u16(1, h, h + len(mcov), nc, h + len(mcov) + len(bcov),
                   h + len(mcov) + len(bcov) + len(marr)) + mcov + bcov + marr + barr


def gdef():
    cd = {g: 1 for g in BASES}
    cd.update({g: 2 for g in LIGS})
    cd.update({g: 3 for g in MARKS})
    c = u16(1, 1, 8, *[cd.get(g, 0) for g in range(1, 9)])
    attach = u16(1, 4, 3, 1, 2, 0)                 # marks D, E, F: classes 1, 2, 0
    sets = u16(1, 2) + u32(8) + u32(14) if False else None
    msets = u16(1, 2) + u32(8, 8 + len(cov1([4, 6]))) + cov1([4, 6]) + cov1([5])
    h = 14
    return (u32(0x10002) + u16(h, 0, 0, h + len(c), h + len(c) + len(attach))
            + c + attach + msets)


def lookup(typ, flag, subs, ext):
    if ext:
        subs = [u16(1, typ) + u32(8) + s for s in subs]
        typ = 9
    hdr = 6 + 2 * len(subs) + (2 if flag & 0x10 else 0)
    offs, body = [], b''
    for x in subs:
        offs.append(hdr + len(body))
        body += x
    return u16(typ, flag, len(subs), *offs) + (u16(random_set()) if flag & 0x10 else b'') + body


def random_set():
    return random_set.r.randrange(2)


def build_gpos(seed):
    gen = PGen(seed)
    r = gen.r
    lookups = []
    for _ in range(3):
        t = r.choice((1, 2, 2))
        lookups.append((t, r.choice(FLAGS),
                        [gen.single() if t == 1 else gen.pair() for _ in range(r.randint(1, 2))]))
    cg = G.Gen(seed * 3 + 1, GL)
    for _ in range(r.randint(2, 5)):
        k = r.choice((1, 2, 2, 3, 4, 4, 5, 5, 6, 7, 8))
        if k == 1: subs = [gen.single()]
        elif k == 2: subs = [gen.pair() for _ in range(r.randint(1, 2))]
        elif k == 3: subs = [gen.cursive()]
        elif k in (4, 5, 6): subs = [gen.marks({4: 3, 5: 4, 6: 5}[k])]
        else:
            subs = [cg.context(5 if k == 7 else 6, [0, 1, 2], 3) for _ in range(r.randint(1, 2))]
        lookups.append((k, r.choice(FLAGS), subs))
    n = len(lookups)
    feats = sorted((t, sorted(set(r.randrange(n) for _ in range(r.randint(1, 2)))))
                   for t in ('kern', 'mark', 'mkmk', 'curs', 'dist', 'abvm'))
    bodies = [u16(0, len(ls), *ls) for _, ls in feats]
    pos, frec = 2 + 6 * len(feats), b''
    for (t, _), b in zip(feats, bodies):
        frec += t.encode() + u16(pos)
        pos += len(b)
    flist = u16(len(feats)) + frec + b''.join(bodies)
    script = u16(4, 0) + u16(0, 0xffff, len(feats), *range(len(feats)))
    scripts = ['DFLT', 'latn']
    slhdr = 2 + 6 * len(scripts)
    sl, body = u16(len(scripts)), b''
    for sc in scripts:
        sl += sc.encode() + u16(slhdr + len(body))
        body += script
    sl += body
    random_set.r = r
    lbs = [lookup(t, f, s, r.random() < .2) for t, f, s in lookups]
    ll, off, offs = u16(n), 2 + 2 * n, []
    for l in lbs:
        offs.append(off)
        off += len(l)
    ll += u16(*offs) + b''.join(lbs)
    h = 10
    return u32(0x10000) + u16(h, h + len(sl), h + len(sl) + len(flist)) + sl + flist + ll


def main(argv):
    seeds, first, reuse, driver, scratch = 200, 0, False, G.DRIVER, SCRATCH
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--reuse": reuse = True
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
    os.makedirs(scratch, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    lines = []
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=30, cmap_map=cmap, extra={
                'GPOS': build_gpos(seed), 'GDEF': gdef()}))
        rr = random.Random(seed * 7)
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 7))) + "\n"
                            for _ in range(12)))
        lines.append("hb-shape --font-file='%s' --output-format=json "
                     "--no-glyph-names --script=Latn --text-file='%s' > '%s.hb'"
                     % (font, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    if not reuse:
        ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                             capture_output=True, text=True)
        if ref.returncode not in (0, 1):
            sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
            return 2
    bad = compared = 0
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    shown = 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        with open(font + ".hb", encoding="utf-8") as h:
            hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
        with open(text, "rb") as h:
            ours = subprocess.run([driver, "--batch", "--script", "latn", font],
                                  stdin=h, capture_output=True, text=True)
        mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        txt = open(text).read().split("\n")
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                if shown < 5 and "-v" in argv:
                    shown += 1
                    print("seed", seed, txt[i], "\n  hb ", norm(a), "\n  our", norm(b))
        if len(hb) != len(mine):
            bad += 1
    print("gpos_random_diff: %d seeds from %d, %d strings, %d differ" % (seeds, first, compared, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
