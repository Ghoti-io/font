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
"""Subset every corpus font, and hold the subset to the original.

documentation/design.md section 12.5. A subsetter is right when text shapes the
same in the subset as in the font it came from, and when every glyph that shaping
used still draws. This writes each corpus font's subset for the characters of a
sample text with the glyph ids retained (the mode that keeps the layout tables,
and so the only one HarfBuzz can shape both of) and then asks:

  * **HarfBuzz**, shaping the text with the original and with the subset: the two
    runs must agree in every glyph, cluster, advance and offset. This is the
    whole of what retaining the ids promises, and it is HarfBuzz's reading of the
    tables this library wrote or carried over, not this library's.
  * **fontTools**, which must open the subset; whose reading of each glyph the
    text shaped to must be the original's; and whose own subsetter, asked for the
    same characters with every layout feature, gives the glyph set this one is
    scored against. Glyphs it keeps that this library did not are a closure that
    missed something; glyphs this library kept that it did not are the
    over-approximation, reported as a count.

A font this library declines to subset (a CFF outline, a variable font) is counted
and named, never dropped from the denominator.

With --renumber the subsets are made the way a web font is, with the glyphs
renumbered and the layout tables rewritten for the new numbering. The two shapings
are then compared through the glyph map the library reports: each glyph of the
subset's run must be the original's glyph under the map, with the same cluster,
advance and offset, and fontTools must read the same outline for it.

Usage: subset_diff.py [--fonts N] [--groups a,b] [--renumber] [--quiet]
"""

import glob
import json
import os
import unicodedata
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hb_diff
import oracle_env

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "subset")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-subset")
DUMP = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-dump")
# Shaped by these and not by GSUB/GPOS: the subsetter does not carry them (design.md
# section 12.3), so HarfBuzz shapes the subset differently by construction.
AAT = ("morx", "mort", "kerx", "trak")


def aat_tables(font):
    run = subprocess.run([DUMP, font], capture_output=True, text=True)
    return [t for t in AAT if "table '%s'" % t in run.stdout]
FIXTURE_GROUP = ("fixture", "Latn", "latn", None, "", "", ["ABCD", "DCBA AB CD", "ABAB"])
KEYS = ("g", "cl", "ax", "ay", "dx", "dy")
# A variable font is also compared by the box each glyph's outline fills, which is where
# a wrong blend shows even when the advances (HVAR) agree.
VKEYS = KEYS + ("xb", "yb", "wb", "hb")


def norm(run, keys=KEYS):
    return [tuple(x.get(k) for k in keys) for x in run]


def axes_of(font):
    """The fvar axes of a font as (tag, min, default, max), from the library's own dump."""
    run = subprocess.run([DUMP, font], capture_output=True, text=True)
    out = []
    for line in run.stdout.splitlines():
        if line.startswith("fvar axis ") and " min " in line:
            tag = line.split("'")[1]
            words = line.split()
            vals = [int(words[words.index(w) + 1]) / 65536.0 for w in ("min", "default", "max")]
            out.append((tag, vals[0], vals[1], vals[2]))
    return out


def locations(axes):
    """A handful of places in the design space: every extreme, each axis alone, the middle."""
    if not axes:
        return []
    seen = []

    def add(loc):
        if loc not in seen:
            seen.append(loc)
    default = {t: d for t, lo, d, hi in axes}
    add({t: lo for t, lo, d, hi in axes})
    add({t: hi for t, lo, d, hi in axes})
    add({t: (d + hi) / 2 if hi != d else (lo + d) / 2 for t, lo, d, hi in axes})
    for t, lo, d, hi in axes:
        a = dict(default)
        a[t] = hi
        add(a)
        b = dict(default)
        b[t] = lo
        add(b)
    return seen[:8]


def var_string(loc):
    return ",".join("%s=%g" % kv for kv in loc.items())


def parse_hb(path):
    with open(path, encoding="utf-8") as handle:
        return [json.loads(l) if l.startswith("[") else [] for l in handle.read().split("\n")[:-1]]


def main(argv):
    quiet = "--quiet" in argv
    renumber = "--renumber" in argv
    limit = None
    only = None
    for i, a in enumerate(argv[1:], start=1):
        if a == "--fonts":
            limit = int(argv[i + 1])
        elif a == "--groups":
            only = set(argv[i + 1].split(","))
    os.makedirs(SCRATCH, exist_ok=True)
    fonts = corpus.fonts("sfnt")
    if limit:
        fonts = fonts[:limit]
    # The variable fixtures (CFF2 and gvar; HVAR, VVAR, MVAR, variable GPOS): few real fonts carry
    # them, so the committed ones are shaped too, with a text their few characters cover.
    fixtures = sorted(glob.glob(os.path.join(ROOT, "tests", "data", "fonts",
                                             "variable-cff2*.otf"))
                      + glob.glob(os.path.join(ROOT, "tests", "data", "fonts",
                                               "variable-*.ttf")))
    fonts = fonts + fixtures
    groups = [g for g in hb_diff.GROUPS if not g[5] and (only is None or g[0] in only)]

    tasks = []          # (font, group name, subset path, text file, iso, unicodes)
    declined = {}
    refused = []
    aat = []
    for fi, font in enumerate(fonts):
        if aat_tables(font):
            aat.append(font)
            continue
        axes = axes_of(font)
        locs = locations(axes)
        for gi, group in enumerate([FIXTURE_GROUP] if font in fixtures else groups):
            gi = 99 if font in fixtures else gi
            texts = group[6]
            # The shaper composes and decomposes before it looks at the font, a pair
            # at a time, so the characters it looks up include the intermediate
            # compositions: the NFC and NFD forms of the whole text and of every pair
            # of its characters. A caller who subsets by text has to ask for them,
            # as with fontTools.
            wanted = set()
            for t in texts:
                for form in (t, unicodedata.normalize("NFC", t), unicodedata.normalize("NFD", t)):
                    wanted.update(form)
                # A base composes with a mark that is not next to it (the marks between
                # them are not blocking), so every pair of the text's characters is
                # tried, not only the neighbours.
                chars = sorted(set(t) | set(unicodedata.normalize("NFD", t))
                               | set(unicodedata.normalize("NFC", t)))
                for x in chars:
                    for y in chars:
                        pair = x + y
                        wanted.update(unicodedata.normalize("NFC", pair))
                        wanted.update(unicodedata.normalize("NFD", pair))
            # And the characters whose glyphs the shaper borrows when text reaches
            # something the font lacks: the space, the digit and the full stop that
            # size the figure and punctuation spaces, the hyphen a non-breaking
            # hyphen falls back to, and the dotted circle a broken cluster is set on.
            cps = sorted({ord(c) for c in wanted if ord(c) >= 0x20}
                         | {0x20, 0x30, 0x2E, 0x2010, 0x25CC})
            out = os.path.join(SCRATCH, "s-%d-%d.ttf" % (fi, gi))
            args = [DRIVER, "--unicodes", ",".join("%X" % c for c in cps)]
            if renumber:
                args += ["--map", out + ".map"]
            else:
                args.append("--retain-gids")
            run = subprocess.run(args + [font, out], capture_output=True, text=True)
            if run.returncode != 0:
                why = run.stderr.strip().splitlines()[0] if run.stderr.strip() else "?"
                if "nsupported" in why:
                    declined[font] = why
                else:
                    refused.append((font, group[0], run.stderr.strip()[:200]))
                if "nsupported" in why:
                    break      # the same font fails the same way for every group
                continue
            text = os.path.join(SCRATCH, "t-%d.txt" % gi)
            if not os.path.exists(text):
                with open(text, "w", encoding="utf-8") as h:
                    for t in texts:
                        h.write(t + "\n")
            tasks.append((font, group, out, text, cps, locs))

    # HarfBuzz, original and subset, in one container.
    os.makedirs(os.path.join(SCRATCH, "out"), exist_ok=True)
    script = os.path.join(SCRATCH, "run.sh")
    with open(script, "w", encoding="utf-8") as h:
        for ti, (font, group, sub, text, _cps, locs) in enumerate(tasks):
            for vi, loc in enumerate([None] + locs):
                for tag, path in (("o", font), ("s", sub)):
                    out = os.path.join(SCRATCH, "out", "%d%s%s.json"
                                       % (ti, tag, "" if loc is None else "_%d" % vi))
                    parts = ["hb-shape", "--font-file=%s" % path, "--output-format=json",
                             "--no-glyph-names"]
                    if locs:
                        parts.append("--show-extents")
                    if loc:
                        parts.append("--variations=%s" % var_string(loc))
                    if group[1]:
                        parts.append("--script=%s" % group[1])
                    parts.append("--text-file=%s" % text)
                    h.write(" ".join("'%s'" % p for p in parts)
                            + " > '%s' 2>/dev/null\n" % out)
    argv_hb = oracle_env.command("harfbuzz", ["sh", script], scratch=SCRATCH)
    subprocess.run(argv_hb, capture_output=True, text=True)

    shaped = disagree = skipped = 0
    var_shaped = var_disagree = 0
    bad = []
    jobs = []
    meta = []

    def compare(a, b, gmap, keys):
        """Whether two shapings agree, the subset's through the glyph map when renumbered."""
        pairs = []
        if gmap is None:
            return [norm(r, keys) for r in a] == [norm(r, keys) for r in b], pairs
        same = len(a) == len(b) and all(len(x) == len(y) for x, y in zip(a, b))
        if same:
            for x, y in zip(a, b):
                for gx, gy in zip(norm(x, keys), norm(y, keys)):
                    if gy[0] is None or gy[0] >= len(gmap) or gmap[gy[0]] != gx[0] \
                            or gx[1:] != gy[1:]:
                        same = False
                    else:
                        pairs.append([gx[0], gy[0]])
        return same, pairs

    for ti, (font, group, sub, text, cps, locs) in enumerate(tasks):
        keys = VKEYS if locs else KEYS
        a = parse_hb(os.path.join(SCRATCH, "out", "%do.json" % ti))
        b = parse_hb(os.path.join(SCRATCH, "out", "%ds.json" % ti))
        flat = [g for run in a for g in run]
        mapped = [g for g in flat if g.get("g")]
        if not flat or len(mapped) * 2 < len(flat):
            skipped += 1      # the font lacks most of this group: nothing to compare
            continue
        shaped += 1
        gmap = None
        if renumber:
            with open(sub + ".map", encoding="utf-8") as h:
                gmap = [None if l.strip() == "-" else int(l) for l in h]
        same, pairs = compare(a, b, gmap, keys)
        if not same:
            disagree += 1
            bad.append((os.path.basename(font), group[0]))
        gids = {g["g"] for g in flat}
        all_pairs = {tuple(p) for p in pairs}
        for vi, loc in enumerate(locs, start=1):
            av = parse_hb(os.path.join(SCRATCH, "out", "%do_%d.json" % (ti, vi)))
            bv = parse_hb(os.path.join(SCRATCH, "out", "%ds_%d.json" % (ti, vi)))
            var_shaped += 1
            ok, vpairs = compare(av, bv, gmap, keys)
            all_pairs.update(tuple(p) for p in vpairs)
            gids.update(g["g"] for run in av for g in run)
            if not ok:
                var_disagree += 1
                bad.append((os.path.basename(font), "%s @ %s" % (group[0], var_string(loc))))
        job = {"original": font, "subset": sub, "unicodes": cps, "gids": sorted(gids),
               "locations": locs}
        if renumber:
            job["map"] = gmap
            job["pairs"] = sorted(list(p) for p in all_pairs)
        jobs.append(job)
        meta.append((os.path.basename(font), group[0]))

    jobs_path = os.path.join(SCRATCH, "jobs.json")
    results_path = os.path.join(SCRATCH, "results.json")
    with open(jobs_path, "w", encoding="utf-8") as h:
        json.dump(jobs, h)
    check = os.path.join(ROOT, "tools", "oracle", "fonttools_subset_check.py")
    argv_ft = oracle_env.command("fonttools", ["python3", check, jobs_path, results_path],
                                 scratch=SCRATCH)
    finished = subprocess.run(argv_ft, capture_output=True, text=True)
    unopened = drawn = missed = 0
    extra = theirs = ours = 0
    problems = []
    if finished.returncode == 0:
        with open(results_path, encoding="utf-8") as h:
            results = json.load(h)
        for (name, grp), r in zip(meta, results):
            if not r.get("opens") or "error" in r:
                unopened += 1
                problems.append((name, grp, r.get("error")))
                continue
            if r.get("different"):
                drawn += 1
                problems.append((name, grp, "glyphs differ: %s" % r["different"][:3]))
            if r.get("at_locations") or r.get("var_tables_differ"):
                drawn += 1
                problems.append((name, grp, "variable: outlines/widths differ at %s; tables differ %s"
                                 % (r.get("at_locations"), r.get("var_tables_differ"))))
            if r.get("subrs") or r.get("names_differ"):
                drawn += 1
                problems.append((name, grp, "CFF subset: %s subroutines left, names differ %s"
                                 % (r.get("subrs"), r.get("names_differ"))))
            if r.get("missing"):
                missed += 1
                problems.append((name, grp, "fontTools keeps %s, this does not" % r["missing"][:5]))
            extra += r.get("extra", 0)
            theirs += r.get("theirs", 0)
            ours += r.get("ours", 0)
    else:
        problems.append(("fontTools", "", finished.stderr[-300:]))

    print("fonts %d, groups %d: %d subsets compared (%d groups skipped for lack of glyphs)"
          % (len(fonts), len(groups), shaped, skipped))
    print("declined (unsupported): %d fonts; AAT shaping tables, not carried: %d fonts; "
          "refused otherwise: %d" % (len(declined), len(aat), len(refused)))
    print("HarfBuzz: %d of %d shaped differently in the subset" % (disagree, shaped))
    if var_shaped:
        print("HarfBuzz at %d locations of variable fonts (advances, offsets and glyph boxes): "
              "%d differ" % (var_shaped, var_disagree))
    print("fontTools: %d did not open, %d with a glyph read differently, "
          "%d with a glyph its closure keeps and this one lacks"
          % (unopened, drawn, missed))
    if theirs:
        print("glyphs with outlines kept: fontTools %d, this library %d (%d it did not)"
              % (theirs, ours, extra))
    by_group = {}
    for name, grp in bad:
        by_group[grp] = by_group.get(grp, 0) + 1
    if by_group:
        print("HarfBuzz differences by group:",
              ", ".join("%s %d" % kv for kv in sorted(by_group.items())))
    with open(os.path.join(SCRATCH, "bad.txt"), "w", encoding="utf-8") as h:
        for name, grp in bad:
            h.write("%s\t%s\n" % (name, grp))
    if not quiet:
        for item in bad[:20]:
            print("  hb  ", *item)
        for item in problems[:20]:
            print("  ft  ", *item)
        for item in refused[:10]:
            print("  refused", *item)
    return 1 if (disagree or var_disagree or unopened or drawn or missed or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
