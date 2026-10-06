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
"""Random variable fonts, shaped here and by HarfBuzz at random locations.

A font per seed with an `fvar` of one or two axes (and, in some, an `avar` of
random segment maps), an `HVAR` and a GDEF each holding an item variation store of
random regions and deltas, and a GPOS of random single and pair adjustments whose value records
carry variation-index device tables into the store. Twelve random strings shaped at
a random location each; advances and offsets compared. Nothing is committed.

Usage: var_random_diff.py [--seeds N] [--first K] [--avar] [-v] [--driver PATH]
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
from gsub_random_diff import u16, u32, cov1, base_font

TIESIGN = (-1, 1)
SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "var-random")
GL = list(range(1, 9))


def s16(v):
    return u16(v & 0xFFFF)


def fixed(v):
    return u32(int(round(v * 65536)) & 0xFFFFFFFF)


def f2dot14(v):
    return s16(int(round(v * 16384)))


class VarGen:
    def __init__(self, seed, axes):
        self.r = random.Random(seed)
        self.axes = axes

    def store(self, nitems):
        r = self.r
        nreg = r.randint(1, 3)
        regions = b''
        for _ in range(nreg):
            # A region with every peak zero is left out: it scales to one anywhere,
            # and HarfBuzz applies it at an explicit default location where this
            # library, which treats all-zero coordinates as the default instance,
            # does not. No real font has one.
            live = r.randrange(self.axes)
            for ax in range(self.axes):
                if ax == live or r.random() < .75:
                    peak = r.choice((0.5, 1.0, -1.0, -0.5, 0.25))
                    lo, hi = (0.0, 1.0) if peak > 0 else (-1.0, 0.0)
                    start = r.choice((lo, 0.0 if peak > 0 else peak - 0.5 if peak > -1 else -1.0))
                    end = r.choice((hi, peak if peak > 0 else 0.0))
                    start = min(start, peak)
                    end = max(end, peak)
                    if peak > 0:
                        start = max(start, 0.0)
                    else:
                        end = min(end, 0.0)
                else:
                    start = peak = end = 0.0
                regions += f2dot14(start) + f2dot14(peak) + f2dot14(end)
        rl = u16(self.axes, nreg) + regions
        ndata = r.randint(1, 2)
        datas = []
        for d in range(ndata):
            cnt = nitems if d == 0 else r.randint(2, 6)
            ri = sorted(r.sample(range(nreg), r.randint(1, nreg)))
            body = u16(cnt, len(ri), len(ri), *ri)
            for _ in range(cnt):
                body += b''.join(s16(r.randint(-90, 90)) for _ in ri)
            datas.append((body, cnt))
        head = 8 + 4 * ndata
        off = head + len(rl)
        out = u16(1) + u32(head) + u16(ndata)
        for body, _ in datas:
            out += u32(off)
            off += len(body)
        out += rl + b''.join(b for b, _ in datas)
        return out, [c for _, c in datas]


def device(outer, inner):
    return u16(inner, 0, 0x8000) if False else u16(outer, inner, 0x8000)


def value_record(r, fmt, devs, base_offset_holder):
    """A value record for fmt, its device tables appended through devs (a list)."""
    out = b''
    for bit in (0x1, 0x2, 0x4, 0x8):
        if fmt & bit:
            out += s16(r.randint(-60, 60))
    for bit in (0x10, 0x20, 0x40, 0x80):
        if fmt & bit:
            out += ('DEV', len(devs)) and b'\0\0'
            devs.append(None)
    return out


def main(argv):
    seeds, first, driver, scratch = 200, 0, G.DRIVER, SCRATCH
    use_avar = "--avar" in argv
    ties = "--ties" in argv     # locations that sit exactly between two 2.14 steps
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
    os.makedirs(scratch, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    lines = []
    locs = {}
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        r = random.Random(seed)
        naxes = r.choice((1, 1, 2))
        tags = ['wght', 'wdth'][:naxes]
        ranges = []
        for _ in tags:
            lo = r.choice((100, 200, 0, 50))
            hi = lo + r.choice((400, 600, 800))
            ranges.append((lo, lo + (hi - lo) // 2, hi))
            if ties:
                ranges[-1] = (0, 1000, 2000)
        fvar = (u16(1, 0, 16, 2, naxes, 20, 0, 4 + 4 * naxes)
                + b''.join(t.encode() + fixed(lo) + fixed(d) + fixed(hi) + u16(0, 256 + i)
                           for i, (t, (lo, d, hi)) in enumerate(zip(tags, ranges))))
        vg = VarGen(seed * 3 + 1, naxes)
        nglyphs = 30
        store, counts = vg.store(nglyphs)
        # GPOS: single and pair adjustments with device tables
        def glyph_dev():
            o = r.randrange(len(counts))
            return o, r.randrange(counts[o])
        lookups = []
        def anchor():
            if r.random() < .15:
                return u16(1, r.randint(-80, 80) & 0xFFFF, r.randint(-80, 80) & 0xFFFF)
            d = []
            for _ in range(2):
                o, i2 = glyph_dev()
                d.append(u16(o, i2, 0x8000))
            if r.random() < .15:
                d[0] = b''
            offs = [10 if d[0] else 0, (10 + len(d[0])) if d[1] else 0]
            return (u16(3, r.randint(-80, 80) & 0xFFFF, r.randint(-80, 80) & 0xFFFF, *offs)
                    + d[0] + d[1])

        def with_offsets(parts, head):
            # parts: byte strings to place after a head of that size; offsets from the start
            offs, body = [], b''
            for part in parts:
                offs.append(head + len(body))
                body += part
            return offs, body

        for _ in range(r.randint(1, 3)):
            kind = r.random()
            if kind < .25:
                bases, marks = [1, 2, 3, 4], [5, 6, 7, 8]
                mk = sorted(set(r.sample(marks, r.randint(1, 3))))
                bs = sorted(set(r.sample(bases, r.randint(1, 3))))
                manchors = [anchor() for _ in mk]
                moffs, mbody = with_offsets(manchors, 2 + 4 * len(mk))
                marray = u16(len(mk)) + b''.join(u16(0, o) for o in moffs) + mbody
                banchors = [anchor() for _ in bs]
                boffs, bbody = with_offsets(banchors, 2 + 2 * len(bs))
                barray = u16(len(bs)) + b''.join(u16(o) for o in boffs) + bbody
                mcov, bcov = cov1(mk), cov1(bs)
                head = 12
                sub = (u16(1, head, head + len(mcov), 1, head + len(mcov) + len(bcov),
                           head + len(mcov) + len(bcov) + len(marray))
                       + mcov + bcov + marray + barray)
                lookups.append((4, sub))
            elif kind < .6:
                fmt = r.choice((0x40, 0x44, 0x10, 0x50, 0x41, 0xC0 | 0x04, 0x20 | 0x01))
                cov = cov1(sorted(set(r.choice(GL) for _ in range(r.randint(1, 5)))))
                devs = []
                head = 6 + 2 * bin(fmt).count('1')
                rec = b''
                tail = b''
                dev_base = head + len(cov)
                for bit in (0x1, 0x2, 0x4, 0x8):
                    if fmt & bit:
                        rec += s16(r.randint(-60, 60))
                for bit in (0x10, 0x20, 0x40, 0x80):
                    if fmt & bit:
                        if r.random() < .85:
                            o, i2 = glyph_dev()
                            rec += u16(dev_base + len(tail))
                            tail += u16(i2, 0, 0x8000) if False else u16(o, i2, 0x8000)
                        else:
                            rec += u16(0)
                sub = u16(1, head, fmt) + rec + cov + tail
                # coverage offset: fix up
                sub = u16(1, 6 + len(rec), fmt) + rec + cov + tail
                # device offsets were computed for cov following rec at head: recompute
                dev_base = 6 + len(rec) + len(cov)
                rec2 = b''
                tail2 = b''
                for bit in (0x1, 0x2, 0x4, 0x8):
                    if fmt & bit:
                        rec2 += rec[len(rec2):len(rec2) + 2]
                idx = 0
                rec3 = b''
                pos = 0
                for bit in (0x1, 0x2, 0x4, 0x8):
                    if fmt & bit:
                        rec3 += rec[pos:pos + 2]
                        pos += 2
                devlist = []
                for bit in (0x10, 0x20, 0x40, 0x80):
                    if fmt & bit:
                        v = int.from_bytes(rec[pos:pos + 2], 'big')
                        pos += 2
                        devlist.append(v != 0)
                tail3 = b''
                for present in devlist:
                    if present:
                        o, i2 = glyph_dev()
                        rec3 += u16(6 + len(rec) + len(cov) + len(tail3))
                        tail3 += u16(o, i2, 0x8000)
                    else:
                        rec3 += u16(0)
                sub = u16(1, 6 + len(rec3), fmt) + rec3 + cov + tail3
                lookups.append((1, sub))
            else:
                f1 = r.choice((0x4, 0x44, 0x40, 0x1, 0x5))
                f2 = r.choice((0, 0, 0x4, 0x40, 0x1))
                firsts = sorted(set(r.choice(GL) for _ in range(r.randint(1, 4))))
                nsets = len(firsts)
                cov = cov1(firsts)
                n1 = bin(f1).count('1')
                n2 = bin(f2).count('1')
                size = 2 * (n1 + n2)
                head = 10 + 2 * nsets
                sets = []
                body_len = 0
                psdata = []
                for _ in firsts:
                    pairs = sorted(set(r.choice(GL) for _ in range(r.randint(1, 4))))
                    psdata.append(pairs)
                # layout: head, pairsets, coverage, devices
                set_sizes = [2 + (2 + size) * len(p) for p in psdata]
                cov_off = head + sum(set_sizes)
                dev_off = cov_off + len(cov)
                tail = b''
                psets = []
                setpos = head
                for pairs in psdata:
                    ps = u16(len(pairs))
                    for g2 in pairs:
                        ps += u16(g2)
                        for fmt in (f1, f2):
                            for bit in (0x1, 0x2, 0x4, 0x8):
                                if fmt & bit:
                                    ps += s16(r.randint(-60, 60))
                            for bit in (0x10, 0x20, 0x40, 0x80):
                                if fmt & bit:
                                    if r.random() < .85:
                                        o, i2 = glyph_dev()
                                        ps += u16(dev_off + len(tail) - setpos)   # from the PairSet
                                        tail += u16(o, i2, 0x8000)
                                    else:
                                        ps += u16(0)
                    psets.append(ps)
                    setpos += len(ps)
                offs = []
                pos = head
                for ps in psets:
                    offs.append(pos)
                    pos += len(ps)
                sub = (u16(1, cov_off, f1, f2, nsets, *offs) + b''.join(psets) + cov + tail)
                lookups.append((2, sub))
        nl = len(lookups)
        lk = [u16(t, 0, 1, 8) + s for t, s in lookups]
        ll = u16(nl)
        off = 2 + 2 * nl
        offs = []
        for l in lk:
            offs.append(off)
            off += len(l)
        ll += u16(*offs) + b''.join(lk)
        feat = u16(0, nl, *range(nl))
        flist = u16(1) + b'kern' + u16(8) + feat
        script = u16(4, 0) + u16(0, 0xFFFF, 1, 0)
        sl = u16(2) + b'DFLT' + u16(14) + b'latn' + u16(14) + script + script
        sl = u16(2) + b'DFLT' + u16(14) + b'latn' + u16(14) + script
        h = 10
        gpos = u32(0x10000) + u16(h, h + len(sl), h + len(sl) + len(flist)) + sl + flist + ll
        # fix lookup subtable offsets (each lookup has its one subtable right after)
        lk = [u16(t, 0, 1, 8) + s for t, s in lookups]
        gdef = u32(0x00010003) + u16(0, 0, 0, 0, 0) + u32(14 + 4 * 0 + 0 + 4) * 0
        gdef = u16(1, 3, 0, 0, 0, 0, 0) + u32(0) * 0
        gdef = u16(1, 3) + u16(0, 0, 0, 0, 0) + u32(18)
        gdef += store
        classes = u16(2, 1, 8) + u16(*([1] * 4 + [3] * 4))
        gdef = gdef[:4] + u16(18 + len(store)) + gdef[6:] + classes
        hvar = u16(1, 0) + u32(20, 0, 0, 0) + store
        tables = {'fvar': fvar, 'GPOS': gpos, 'GDEF': gdef, 'HVAR': hvar}
        if use_avar:
            segs = u16(1, 0, 0, naxes) if False else u16(1, 0) + u16(0, naxes)
            for _ in tags:
                pts = sorted(set(round(r.uniform(-1, 1), 3) for _ in range(2)) | {-1.0, 0.0, 1.0})
                segs += u16(len(pts)) + b''.join(
                    f2dot14(p) + f2dot14(max(-1, min(1, p + r.uniform(-.2, .2)))) for p in pts)
            tables['avar'] = segs
        with open(font, "wb") as hh:
            hh.write(base_font(nglyphs=nglyphs, cmap_map=cmap, extra=tables))
        rr = random.Random(seed * 7 + 1)
        if ties:
            # 125/4096 user units is half a 2.14 step on a span of 1000
            loc = ','.join('%s=%s' % (t, ('%.12f' % (d + rr.choice(TIESIGN) * 125
                                                      * (2 * rr.randrange(0, 40) + 1) / 4096.0)).rstrip('0'))
                           for t, (lo, d, hi) in zip(tags, ranges))
        else:
            loc = ','.join('%s=%d' % (t, rr.randint(lo - 20, hi + 20)) for t, (lo, d, hi) in zip(tags, ranges))
        locs[seed] = loc
        with open(text, "w") as hh:
            for _ in range(12):
                hh.write(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 6))) + "\n")
        lines.append("hb-shape --font-file='%s' --output-format=json --variations='%s' "
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, loc, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as hh:
        hh.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    bad = compared = skipped = 0
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    shown = 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        try:
            with open(font + ".hb", encoding="utf-8") as hh:
                hb = [json.loads(l) if l.startswith("[") else [] for l in hh.read().split("\n")[:-1]]
            with open(text, "rb") as hh:
                ours = subprocess.run([driver, "--batch", "--script", "latn", "--location",
                                       locs[seed], font], stdin=hh, capture_output=True,
                                      text=True, timeout=20)
            mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        except (OSError, ValueError, subprocess.TimeoutExpired) as e:
            skipped += 1
            if "-v" in argv:
                print("skipped seed", seed, locs[seed], repr(e)[:100])
            continue
        if len(hb) != len(mine):
            skipped += 1
            if "-v" in argv:
                print("skipped seed", seed, locs[seed], "line counts", len(hb), len(mine),
                      ours.stderr[:80])
            continue
        txt = open(text).read().split("\n")
        try:
            pairs = [(norm(a), norm(b)) for a, b in zip(hb, mine)]
        except AttributeError:
            skipped += 1
            if "-v" in argv:
                print("skipped seed", seed, locs[seed], "unreadable", str(hb[0])[:100], str(mine[0])[:100])
            continue
        for i, (a, b) in enumerate(pairs):
            compared += 1
            if a != b:
                bad += 1
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, locs[seed], txt[i], "\n  hb ", a, "\n  our", b)
    print("var_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
