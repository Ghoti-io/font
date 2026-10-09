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
"""Indic syllables through lookups shaped like the ones real Indic fonts have.

`gsub_random_diff.py` draws lookups over arbitrary glyphs, so a lookup that matches
a reph pair, a halant-consonant pair or a consonant-halant-consonant conjunct comes
up rarely. This one draws syllables (a consonant, an optional nukta, halant-consonant
runs with joiners, matras, a nasal or visarga) and lookups built to fit them: reph
(RA + halant), half (C + halant), below-base and post-base forms (halant + C), conjuncts
(C + halant + C), nukta forms, split matras, chains with a backtrack or lookahead that
call a single substitution, and the outputs of earlier lookups used as inputs of later
ones. Lookups are shared between features and a feature holds several lookups. Each
seed picks a script, a new or old script tag, and whether there is a GDEF.

Usage: indic_random_diff.py [--seeds N] [--first K] [--scripts a,b] [--old] [--gdef]
                            [--userfeatures] [-v] [--driver PATH]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, cov1, base_font, lookup_table

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "indic-random")
NG = 300
DESC = []
ROLES_TEXT = ()
PAIRS = False   # --pairs (with --reveal): a ligature for every ordered pair of glyphs
REVEAL = False   # --reveal --feat f: a single substitution of every glyph under f shows which glyphs have its mask
KEEP = ()   # --feat a,b: keep only these features (the random stream is unchanged)

# script -> (ISO 15924, new OT tag, old OT tag, block base, has nukta)
SCRIPTS = {
    "deva": ("Deva", "dev2", "deva", 0x900, True),
    "beng": ("Beng", "bng2", "beng", 0x980, True),
    "guru": ("Guru", "gur2", "guru", 0xA00, True),
    "gujr": ("Gujr", "gjr2", "gujr", 0xA80, True),
    "orya": ("Orya", "ory2", "orya", 0xB00, True),
    "taml": ("Taml", "tml2", "taml", 0xB80, False),
    "telu": ("Telu", "tel2", "telu", 0xC00, False),
    "knda": ("Knda", "knd2", "knda", 0xC80, True),
    "mlym": ("Mlym", "mlm2", "mlym", 0xD00, False),
}

# The feature tags of an Indic font, in the order the shaper runs them.
FEATURES = ["locl", "ccmp", "nukt", "akhn", "rphf", "rkrf", "pref", "blwf", "abvf",
            "half", "pstf", "vatu", "cjct", "init", "pres", "abvs", "blws", "psts",
            "haln", "calt", "clig"]


def chars(script):
    """Role -> code point, for the roles a syllable is made of."""
    base = SCRIPTS[script][3]
    c = {"KA": base + 0x15, "TA": base + 0x1F, "YA": base + 0x2F, "RA": base + 0x30,
         "H": base + 0x4D, "V": base + 0x05, "AA": base + 0x3E, "I": base + 0x3F,
         "U": base + 0x41, "E": base + 0x47, "AN": base + 0x02, "VI": base + 0x03,
         "ZWJ": 0x200D, "ZWNJ": 0x200C}
    if SCRIPTS[script][4]:
        c["NUKTA"] = base + 0x3C
    return c


ROLES = ["KA", "TA", "YA", "RA", "NUKTA", "H", "V", "AA", "I", "U", "E", "AN", "VI",
         "ZWJ", "ZWNJ"]


def glyph_map(script):
    c = chars(script)
    roles = [r for r in ROLES if r in c]
    return {r: i + 1 for i, r in enumerate(roles)}, c


def cov(gs):
    return cov1(sorted(set(gs)))


class Look:
    """Builds the subtables of one lookup from a seeded generator."""

    def __init__(self, r, glyphs, g, outs):
        self.r, self.g, self.outs = r, g, outs
        self.cons = [g[k] for k in ("KA", "TA", "YA", "RA") if k in g]
        self.marks = [g[k] for k in ("AA", "I", "U", "E", "AN", "VI") if k in g]
        self.glue = [g[k] for k in ("ZWJ", "ZWNJ") if k in g]

    def any(self):
        pool = list(self.cons) * 3 + [self.g["H"]] * 2 + self.marks + self.outs * 2
        if "NUKTA" in self.g:
            pool.append(self.g["NUKTA"])
        return self.r.choice(pool)

    def new_out(self):
        return self.r.choice(self.outs)

    def single(self):
        n = self.r.randint(1, 4)
        m = {self.any(): self.new_out() for _ in range(n)}
        keys = sorted(m)
        self.last = "single %s" % {k: m[k] for k in keys}
        return 1, u16(2, 6 + 2 * len(keys), len(keys), *[m[k] for k in keys]) + cov(keys)

    def multiple(self):
        m = {}
        for _ in range(self.r.randint(1, 3)):
            m[self.any()] = [self.any() if self.r.random() < .6 else self.new_out()
                             for _ in range(self.r.randint(1, 3))]
        keys = sorted(m)
        self.last = "multiple %s" % {k: m[k] for k in keys}
        seqs = [u16(len(m[k]), *m[k]) for k in keys]
        hdr = 6 + 2 * len(keys)
        c = cov(keys)
        offs, body = [], b''
        for s in seqs:
            offs.append(hdr + len(c) + len(body))
            body += s
        return 2, u16(1, hdr, len(keys), *offs) + c + body

    def ligature(self, shape=None):
        r, g = self.r, self.g
        cons = lambda: r.choice(self.cons)
        shapes = {
            "reph": lambda: [g.get("RA", cons()), g["H"]],
            "half": lambda: [cons(), g["H"]],
            "below": lambda: [g["H"], cons()],
            "conj": lambda: [cons(), g["H"], cons()],
            "nukta": lambda: [cons(), g.get("NUKTA", g["H"])],
            "matra": lambda: [self.any(), r.choice(self.marks)],
            "any": lambda: [self.any() for _ in range(r.randint(2, 3))],
            "zwj": lambda: [cons(), g["H"], g.get("ZWJ", g["H"])],
        }
        entries = []
        for _ in range(r.randint(1, 3)):
            s = shape or r.choice(list(shapes))
            entries.append((shapes[s](), self.new_out()))
        self.last = "ligature %s" % [(c, o) for c, o in entries]
        sets = {}
        for comps, out in entries:
            sets.setdefault(comps[0], []).append(u16(out, len(comps), *comps[1:]))
        keys = sorted(sets)
        c = cov(keys)
        hdr = 6 + 2 * len(keys)
        bodies = []
        for k in keys:
            ls = sets[k]
            h = 2 + 2 * len(ls)
            offs, body = [], b''
            for l in ls:
                offs.append(h + len(body))
                body += l
            bodies.append(u16(len(ls), *offs) + body)
        offs, body = [], b''
        for b in bodies:
            offs.append(hdr + len(c) + len(body))
            body += b
        return 4, u16(1, hdr, len(keys), *offs) + c + body

    def chain(self, nested):
        r = self.r
        bt = [cov([self.any() for _ in range(r.randint(1, 2))]) for _ in range(r.randint(0, 2))]
        ins = [cov([self.any() for _ in range(r.randint(1, 3))]) for _ in range(r.randint(1, 3))]
        la = [cov([self.any() for _ in range(r.randint(1, 2))]) for _ in range(r.randint(0, 2))]
        recs = [(r.randrange(len(ins)), r.choice(nested))] if nested and r.random() < .9 else []
        self.last = "chain bt=%d in=%d la=%d recs=%s" % (len(bt), len(ins), len(la), recs)
        hdr = 2 + 2 + 2 * len(bt) + 2 + 2 * len(ins) + 2 + 2 * len(la) + 2 + 4 * len(recs)
        pos = hdr
        placed = []
        for group in (bt, ins, la):
            o = []
            for cv in group:
                o.append(pos)
                pos += len(cv)
            placed.append(o)
        body = (u16(3, len(bt), *placed[0], len(ins), *placed[1], len(la), *placed[2],
                    len(recs)) + b''.join(u16(a, b) for a, b in recs)
                + b''.join(bt + ins + la))
        return 6, body


def build_gsub(seed, g, outs, tag, old):
    r = random.Random(seed * 101 + 3)
    L = Look(r, None, g, outs)
    lookups = []

    def add(kind, sub, flag):
        DESC.append((L.last, flag))
        lookups.append(lookup_table(kind, flag, [sub]))

    # Single substitutions first: the chains below call them.
    for _ in range(r.randint(1, 3)):
        add(*L.single(), 0)
    nested = list(range(len(lookups)))
    flags = (0, 0, 0, 4, 8, 0x0C)
    for _ in range(r.randint(5, 12)):
        k = r.random()
        if k < .12:
            add(*L.single(), r.choice(flags))
        elif k < .22:
            add(*L.multiple(), r.choice(flags))
        elif k < .34:
            add(*L.chain(nested), r.choice(flags))
        else:
            kind, sub = L.ligature()
            subs = [sub]
            d = L.last
            if r.random() < .3:
                subs.append(L.ligature()[1])
                d += " + " + L.last
            DESC.append((d, 0))
            lookups.append(lookup_table(4, DESC[-1][1] if False else r.choice(flags), subs))
    n = len(lookups)
    # Features: each holds one to three lookups; a lookup may sit under several.
    feats = []
    for t in FEATURES:
        if r.random() < .55:
            feats.append((t, sorted(set(r.randrange(n) for _ in range(r.randint(1, 3))))))
    if KEEP:
        feats = [f for f in feats if f[0] in KEEP]
    if not feats:
        feats = [("pres", [0])]
    feats.sort(key=lambda f: f[0])
    build_gsub.feats = feats
    pos = 2 + 6 * len(feats)
    frec, fbody = b'', b''
    for t, ls in feats:
        b = u16(0, len(ls), *ls)
        frec += t.encode() + u16(pos + len(fbody))
        fbody += b
    flist = u16(len(feats)) + frec + fbody
    langsys = u16(0, 0xFFFF, len(feats), *range(len(feats)))
    script = u16(4, 0) + langsys
    tags = sorted({"DFLT", tag, "latn"})
    slhdr = 2 + 6 * len(tags)
    sl = u16(len(tags)) + b''.join(t.encode() + u16(slhdr + k * len(script))
                                   for k, t in enumerate(tags)) + script * len(tags)
    ll = u16(n) + u16(*[2 + 2 * n + sum(len(x) for x in lookups[:k]) for k in range(n)]) \
        + b''.join(lookups)
    h = 10
    return u32(0x10000) + u16(h, h + len(sl), h + len(sl) + len(flist)) + sl + flist + ll


def build_reveal(g, tag, feats):
    """One single substitution mapping every role glyph to its own marker, under each feature."""
    keys = sorted(g.values())
    if PAIRS:
        # A ligature for every ordered pair, each to a glyph of its own.
        bodies = []
        for a in keys:
            ls = [u16(40 + a * 16 + b, 2, b) for b in keys]
            h = 2 + 2 * len(ls)
            offs, body = [], b''
            for l in ls:
                offs.append(h + len(body))
                body += l
            bodies.append(u16(len(ls), *offs) + body)
        hdr = 6 + 2 * len(keys)
        c = cov(keys)
        offs, body = [], b''
        for b in bodies:
            offs.append(hdr + len(c) + len(body))
            body += b
        sub = u16(1, hdr, len(keys), *offs) + c + body
        lk = lookup_table(4, 0, [sub])
    else:
        sub = u16(2, 6 + 2 * len(keys), len(keys), *[20 + k for k in keys]) + cov(keys)
        lk = lookup_table(1, 0, [sub])
    feats = sorted(feats)
    pos = 2 + 6 * len(feats)
    frec, fbody = b'', b''
    for t in feats:
        frec += t.encode() + u16(pos + len(fbody))
        fbody += u16(0, 1, 0)
    flist = u16(len(feats)) + frec + fbody
    script = u16(4, 0) + u16(0, 0xFFFF, len(feats), *range(len(feats)))
    tags = sorted({"DFLT", tag, "latn"})
    slhdr = 2 + 6 * len(tags)
    sl = u16(len(tags)) + b''.join(t.encode() + u16(slhdr + k * len(script))
                                   for k, t in enumerate(tags)) + script * len(tags)
    ll = u16(1, 4) + lk
    h = 10
    return u32(0x10000) + u16(h, h + len(sl), h + len(sl) + len(flist)) + sl + flist + ll


def gdef_table(g, outs, r):
    marks = [g[k] for k in ("AA", "I", "U", "E", "AN", "VI", "NUKTA") if k in g]
    cd = {}
    for v in g.values():
        cd[v] = 1
    for m in marks:
        cd[m] = 3 if r.random() < .8 else 1
    cd[g["H"]] = 3 if r.random() < .5 else 1
    for o in outs:
        cd[o] = r.choice((1, 1, 2, 3))
    lo, hi = min(cd), max(cd)
    c = u16(1, lo, hi - lo + 1, *[cd.get(x, 0) for x in range(lo, hi + 1)])
    return u32(0x10000) + u16(12, 0, 0, 0) + c


def syllable(r, g):
    out = []
    pick = lambda *ks: g[r.choice([k for k in ks if k in g])]
    if r.random() < .08:
        out.append(g["V"])
    else:
        out.append(pick("KA", "TA", "YA", "RA"))
    if "NUKTA" in g and r.random() < .15:
        out.append(g["NUKTA"])
    for _ in range(r.choice((0, 0, 1, 1, 2, 3))):
        out.append(g["H"])
        if r.random() < .2:
            out.append(pick("ZWJ", "ZWNJ"))
        if r.random() < .93:
            out.append(pick("KA", "TA", "YA", "RA"))
            if "NUKTA" in g and r.random() < .1:
                out.append(g["NUKTA"])
    if r.random() < .15:
        out.append(g["H"])
    for _ in range(r.choice((0, 0, 1, 1, 2))):
        out.append(pick("AA", "I", "U", "E"))
    if r.random() < .25:
        out.append(pick("AN", "VI"))
    return out


def text_for(r, g, inv):
    syl = [syllable(r, g) for _ in range(r.randint(1, 3))]
    flat = [c for s in syl for c in s]
    if r.random() < .06:
        flat.insert(r.randrange(len(flat) + 1), g["H"])
    return ''.join(inv[x] for x in flat)


def shape_both(driver, cases, jobs):
    """jobs: [(seed, string)] -> [(hb glyph tuples, our glyph tuples)] in one container call."""
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda gl: [tuple(x.get(k) for k in keys) for x in gl]
    lines = []
    for n, (seed, string) in enumerate(jobs):
        script, tag, feat, font, _ = cases[seed]
        t = os.path.join(SCRATCH, "m%d.txt" % n)
        with open(t, "w", encoding="utf-8") as h:
            h.write(string + "\n")
        lines.append("hb-shape --font-file='%s' --output-format=json --no-glyph-names "
                     "--script=%s%s --text-file='%s' > '%s.mhb' 2>/dev/null"
                     % (font, SCRIPTS[script][0], feat, t, t))
    runner = os.path.join(SCRATCH, "mrun.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                   capture_output=True, text=True)
    out = []
    for n, (seed, string) in enumerate(jobs):
        script, tag, feat, font, _ = cases[seed]
        t = os.path.join(SCRATCH, "m%d.txt" % n)
        try:
            with open(t + ".mhb", encoding="utf-8") as h:
                hb = norm(json.loads(h.readline()))
            extra = ["--features", feat.split("'")[1]] if feat else []
            ours = subprocess.run([driver, "--batch", "--script", tag] + extra + [font],
                                  input=string + "\n", capture_output=True, text=True, timeout=20)
            out.append((hb, norm(json.loads(ours.stdout.split("\n")[0]))))
        except (OSError, ValueError, subprocess.TimeoutExpired):
            out.append((None, None))
    return out


def minimize(driver, cases, failing, limit=8):
    """Delete characters from each failing string while the two shapers still differ."""
    cur = failing[:limit]
    while True:
        jobs, owner = [], []
        for k, (seed, string) in enumerate(cur):
            for i in range(len(string)):
                jobs.append((seed, string[:i] + string[i + 1:]))
                owner.append(k)
        res = shape_both(driver, cases, jobs)
        nxt = list(cur)
        moved = False
        for (seed, cand), k, (a, b) in zip(jobs, owner, res):
            if a is not None and a != b and cand and len(cand) < len(nxt[k][1]) and nxt[k] == cur[k]:
                nxt[k] = (seed, cand)
                moved = True
        if not moved:
            return cur
        cur = nxt


def main(argv):
    global KEEP, REVEAL, ROLES_TEXT, PAIRS
    PAIRS = "--pairs" in argv
    if "--roles" in argv:
        ROLES_TEXT = [x.strip() for x in argv[argv.index("--roles") + 1].split(";")]
    REVEAL = "--reveal" in argv
    seeds, first, driver = 300, 0, G.DRIVER
    scripts = list(SCRIPTS)
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scripts": scripts = argv[i + 1].split(",")
        elif a == "--feat": KEEP = tuple(argv[i + 1].split(","))
    old_all = "--old" in argv
    os.makedirs(SCRATCH, exist_ok=True)
    cases, lines = {}, []
    for seed in range(first, first + seeds):
        r = random.Random(seed * 7 + 1)
        script = scripts[seed % len(scripts)]
        iso, new, oldtag, _, _ = SCRIPTS[script]
        old = old_all or (r.random() < .25 and "--new" not in argv)
        tag = oldtag if old else new
        g, cp = glyph_map(script)
        inv = {v: chr(cp[k]) for k, v in g.items()}
        outs = list(range(20, 36))
        font = os.path.join(SCRATCH, "s%d.ttf" % seed)
        text = os.path.join(SCRATCH, "s%d.txt" % seed)
        extra = {'GSUB': build_reveal(g, tag, KEEP) if REVEAL else build_gsub(seed, g, outs, tag, old)}
        if "--gdef" in argv or r.random() < .3:
            extra['GDEF'] = gdef_table(g, outs, r)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=NG, cmap_map={cp[k]: v for k, v in g.items()}, extra=extra))
        with open(text, "w", encoding="utf-8") as h:
            if ROLES_TEXT:
                h.write(''.join(''.join(chr(cp[x]) for x in line.split()) + "\n" for line in ROLES_TEXT))
            else:
                h.write(''.join(text_for(r, g, inv) + "\n" for _ in range(12)))
        feat = ""
        if "--userfeatures" in argv:
            fs = G.features_for(seed, FEATURES)
            feat = " --features='%s'" % fs
        cases[seed] = (script, tag, feat, font, text)
        lines.append("hb-shape --font-file='%s' --output-format=json --no-glyph-names "
                     "--script=%s%s --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, iso, feat, text, font))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda gl: [tuple(x.get(k) for k in keys) for x in gl]
    bad = compared = skipped = shown = 0
    badseeds = []
    failing = []
    for seed, (script, tag, feat, font, text) in cases.items():
        try:
            with open(font + ".hb", encoding="utf-8") as h:
                hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
            with open(text, "rb") as h:
                extra = ["--features", feat.split("'")[1]] if feat else []
                ours = subprocess.run([driver, "--batch", "--script", tag] + extra + [font],
                                      stdin=h, capture_output=True, text=True, timeout=20)
            mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        except (OSError, ValueError, subprocess.TimeoutExpired):
            skipped += 1
            continue
        if len(hb) != len(mine):
            skipped += 1
            continue
        txt = open(text, encoding="utf-8").read().split("\n")
        seed_bad = False
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if "--show" in argv and seed == first:
                inv2 = {v: k for k, v in chars(script).items()}
                gi = {v: k for k, v in glyph_map(script)[0].items()}

                def dec(gl):
                    out = []
                    for t in gl:
                        x = t[0]
                        if PAIRS and x >= 40:
                            x -= 40
                            out.append("(%s %s)" % (gi.get(x // 16, '?'), gi.get(x % 16, '?')))
                        elif REVEAL and 20 < x < 40:
                            out.append(gi.get(x - 20, '?') + "'")
                        else:
                            out.append(gi.get(x, str(x)))
                    return ' '.join(out)
                print("%-22s hb: %-38s our: %-38s %s" % (' '.join(inv2.get(ord(c), '?') for c in txt[i]), dec(norm(a)), dec(norm(b)), "" if norm(a) == norm(b) else "<<<"))
            if norm(a) != norm(b):
                bad += 1
                seed_bad = True
                failing.append((seed, txt[i]))
                if (shown < 6 or ROLES_TEXT) and "-v" in argv:
                    shown += 1
                    print("seed", seed, script, tag, feat, ' '.join('%X' % ord(c) for c in txt[i]),
                          "\n  hb ", [(t[0], t[1]) + tuple(t[2:]) for t in norm(a)], "\n  our", [(t[0], t[1]) + tuple(t[2:]) for t in norm(b)])
        if seed_bad:
            badseeds.append(seed)
    print("indic_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    if "--minimize" in argv and failing:
        for (seed, string), (a, b) in zip(*(lambda m: (m, shape_both(driver, cases, m)))(minimize(driver, cases, failing, int(argv[argv.index("--minimize") + 1])))):
            inv = {v: k for k, v in chars(cases[seed][0]).items()}
            print("min seed %d %s %s: %s  hb %s  our %s" % (seed, cases[seed][0], cases[seed][1], ' '.join(inv.get(ord(c), '?') for c in string), [t[0] for t in a], [t[0] for t in b]))
    if "--describe" in argv:
        del DESC[:]
        sd = int(argv[argv.index("--describe") + 1])
        sc = cases[sd]
        g2, _ = glyph_map(sc[0])
        build_gsub(sd, g2, list(range(20, 36)), sc[1], False)
        print("glyphs", g2)
        for i, d in enumerate(DESC):
            print(i, d)
        print("features", build_gsub.feats)
    if badseeds and "-s" in argv:
        print("bad seeds:", ' '.join(str(s) for s in badseeds[:40]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
