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
"""Paragraph layout, this library against HarfBuzz, ICU and fontTools.

documentation/design.md section 10. Nothing here is a reference *layout engine*:
none is pinned in this repository's images, so the reference is built from the
three that are, each answering the question it is the authority on.

  * **ICU** (the image `libs/unicode` builds) answers the questions about text:
    the embedding level and script of every character, where a line may and must
    break, and the visual order of a line's runs with rule L1 applied
    (`icu_layout.cpp`).
  * **HarfBuzz** answers the questions about glyphs: what each item shapes to, and
    so what every cluster is called and how wide it is.
  * **fontTools** answers the questions about the font: the line metrics the
    font asks for and every glyph's outline bounds, which is the ink box.

What is left is the greedy line-breaking rule and the arithmetic of placing
runs, and *that is written a second time here* - structured differently from
`src/layout/layout.c` (chunks between break opportunities, not a scan with a
candidate) but from the same reading of `design.md` section 10 and CSS Text. So
the gate holds the library's use of Unicode, of the shaper and of the font to
three independent implementations, and holds its own rule only to a second
reading of it. It says so on its first line.

Per (font, text, direction, alignment) it lays the text out at the reference's
min-content and max-content widths and at fractions of them, and compares:

  1. **Lines** - byte range, width and offset, and every glyph in visual order
     with its cluster, pen position and advance;
  2. **Constraint widths** - min-content and max-content against the reference's;
  3. **Consistency of the hit-testing functions** with each other and with the
     glyphs: a caret sits on the edge of its cluster, a hit test at a caret
     returns it, and the selection rectangles of a line add up to the line's
     glyphs;
  4. **Metrics and ink** - each line's height against the font's own request, and
     each line's ink box against fontTools' bounds of the placed glyphs.

**What it cannot see**, stated so the absence is a statement: one face and one
size per paragraph (no span boundaries, no fallback), no justification,
truncation, tabs or letter-spacing (the rule being checked would be written
twice, from the same reading), and a font this library or HarfBuzz refuses is
counted and listed and never skipped.

Usage:
    layout_diff.py [--quiet] [--fonts N] [--all] [--list] [font...]
"""

import json
import gzip
import os
import subprocess
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hb_diff
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-layout")
SCRATCH = os.path.join(ROOT, "build", "oracle", "layout")
SIZE = 16          # pixels per em
SIZE26 = SIZE * 64

LATIN = [
    ("pangram", "The quick brown fox jumps over the lazy dog and keeps running "
                "through the forest until night falls."),
    ("kerning", "Wavy AVATAR office affluent difficult fluff — ligatures, "
                "kerning: 1,234.56 (parenthesised) well-known e-mail/slash/path."),
    ("hard", "First line\nSecond line is somewhat longer\n\nFourth after an "
             "empty line\n"),
    ("spaces", "  leading and  double   spaces  trailing  "),
    ("long", "Supercalifragilisticexpialidocious is a pneumonoultramicroscopic "
             "word"),
    ("marks", "café résumé naïve Ångström wide "
              "text goes on"),
    ("nbsp", "a b c d e f g h i"),
]
HEBREW = [
    ("hebrew", "שלום עולם זהו "
               "משפט ארוך בע"
               "ברית שצריך ל"
               "עבור שורות"),
    ("hebrew-hard", "שלום\nעולם "
                    "טוב מאוד\n"),
]
ARABIC = [
    ("arabic", "مرحبا بالعا"
               "لم هذا نص عرب"
               "ي طويل يجب أن "
               "ينتقل بين عد"
               "ة أسطر"),
]
MIXED_HE = [
    ("mixed-he", "The word שלום means peace, and 12 apples "
                 "cost דולר 3.50 today in the market."),
    ("numbers-he", "אבג 123 דהו 4.5 זח "
                   "טיכ (למנ) [סעפ]"),
]
MIXED_AR = [
    ("mixed-ar", "Say مرحبا (hello) to everyone in "
                 "2024, then شكرا twice."),
]
# (texts, directions, probes the font must map, alignments for the first text)
KINDS = [
    (LATIN, ("ltr",), (0x61, 0x57), ()),
    (HEBREW, ("auto", "rtl"), (0x5D0,), ("end", "center")),
    (ARABIC, ("auto", "rtl"), (0x645,), ("end", "center")),
    (MIXED_HE, ("ltr", "rtl", "auto"), (0x61, 0x5D0), ("end", "center")),
    (MIXED_AR, ("ltr", "rtl", "auto"), (0x61, 0x645), ()),
]
FRACTIONS = (0.2, 0.35, 0.5, 0.75, 1.0)
NEWLINES = {0x0A, 0x0B, 0x0C, 0x0D, 0x85, 0x2028, 0x2029}


class Refused(Exception):
    pass


def utf8_len(cp):
    return 1 if cp < 0x80 else 2 if cp < 0x800 else 3 if cp < 0x10000 else 4


def round_shift10(v):
    """gfnt_round_shift(v, 10): half away from zero."""
    sign = -1 if v < 0 else 1
    return sign * ((abs(v) + 512) >> 10)


def ink_px(units, scale):
    """scale_v() in layout.c: a bound in font units, as 26.6 of a unit, to 26.6 pixels."""
    t = int(round(units * 64)) * scale
    return (1 if t >= 0 else -1) * ((abs(t) + 32768) >> 16)


def scale_for(upem):
    return (SIZE26 * 1024 + upem // 2) // upem


def hangs(ch):
    if ch == " " or ord(ch) in NEWLINES:
        return True
    if ch in "   ":
        return False
    return unicodedata.category(ch) == "Zs"


# ------------------------------------------------------------------ containers

def run_icu(requests, tag):
    """Ask ICU. Returns one parsed JSON answer per request."""
    os.makedirs(SCRATCH, exist_ok=True)
    src = os.path.join(SCRATCH, "icu_layout.cpp")
    with open(os.path.join(ROOT, "tools", "oracle", "icu_layout.cpp"), "rb") as h:
        data = h.read()
    with open(src, "wb") as h:
        h.write(data)
    req = os.path.join(SCRATCH, "icu-%s.in" % tag)
    out = os.path.join(SCRATCH, "icu-%s.out" % tag)
    with open(req, "w", encoding="utf-8") as h:
        for r in requests:
            h.write(r + "\n")
    script = os.path.join(SCRATCH, "icu-run.sh")
    with open(script, "w", encoding="utf-8") as h:
        h.write("set -e\ncd '%s'\n" % SCRATCH)
        h.write("[ -x icu_layout ] || g++ -O1 -o icu_layout icu_layout.cpp "
                "-licuuc\n")
        h.write("./icu_layout < '%s' > '%s'\n" % (req, out))
    argv = oracle_env.command("icu", ["sh", script], scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "icu_layout failed: %s" % finished.stderr.strip()[-400:])
    with open(out, encoding="utf-8") as h:
        answers = [json.loads(line) for line in h]
    if len(answers) != len(requests):
        raise oracle_env.OracleUnavailable(
            "ICU answered %d of %d requests" % (len(answers), len(requests)))
    return answers


def run_hb(font_jobs):
    """font_jobs: list of (font, rtl, script, [cp...]). Returns glyph lists."""
    out_dir = os.path.join(SCRATCH, "hb")
    os.makedirs(out_dir, exist_ok=True)
    lines = []
    for index, (font, rtl, script, cps) in enumerate(font_jobs):
        parts = ["hb-shape", "--font-file=%s" % font, "--output-format=json",
                 "--no-glyph-names", "--direction=%s" % ("rtl" if rtl else "ltr")]
        if script and script not in ("Zyyy", "Zinh", "Zzzz"):
            parts.append("--script=%s" % script)
        parts.append("--unicodes=%s" % ",".join("U+%04X" % c for c in cps))
        lines.append(" ".join("'%s'" % p for p in parts)
                     + " > '%s/%d.json' 2>/dev/null\n" % (out_dir, index))
    script_path = os.path.join(SCRATCH, "hb-run.sh")
    with open(script_path, "w", encoding="utf-8") as h:
        h.write("".join(lines))
    argv = oracle_env.command("harfbuzz", ["sh", script_path], scratch=SCRATCH)
    subprocess.run(argv, capture_output=True, text=True)
    results = []
    for index in range(len(font_jobs)):
        path = os.path.join(out_dir, "%d.json" % index)
        try:
            with open(path, encoding="utf-8") as h:
                results.append(json.loads(h.read()))
        except (OSError, ValueError):
            results.append(None)
    return results


def run_fonttools(fonts_gids):
    """Metrics and glyph bounds for each font. {font: {...}} or None."""
    path = os.path.join(SCRATCH, "ft-in.json")
    out = os.path.join(SCRATCH, "ft-out.json")
    with open(path, "w", encoding="utf-8") as h:
        json.dump({f: sorted(g) for f, g in fonts_gids.items()}, h)
    argv = oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "fonttools_layout.py"),
        path, out], scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fonttools_layout failed: %s" % finished.stderr.strip()[-400:])
    with open(out, encoding="utf-8") as h:
        return json.load(h)


# ------------------------------------------------------------------- reference

class Unit:
    __slots__ = ("a", "b", "glyphs", "width", "space", "opp", "hard", "level")


def items_of(cps, info):
    """Split at level and script changes, as design.md section 10.5 does."""
    n = len(cps)
    levels = info["lv"]
    scripts = info["sc"]
    resolved = []
    current = None
    for i in range(n):
        sc = scripts[i]
        if sc in ("Zyyy", "Zinh"):
            resolved.append(current)
        else:
            current = sc
            resolved.append(sc)
    first = next((s for s in resolved if s), None)
    resolved = [s if s else first for s in resolved]
    cuts = [0]
    for i in range(1, n):
        if levels[i] != levels[i - 1] or resolved[i] != resolved[i - 1]:
            cuts.append(i)
    cuts.append(n)
    return [(cuts[k], cuts[k + 1], resolved[cuts[k]]) for k in range(len(cuts) - 1)]


def pieces_of(cps, a, b):
    """The item [a, b) cut at hard-break characters, which HarfBuzz's command
    line will not take; the breaking characters themselves are not shaped."""
    out = []
    start = a
    for i in range(a, b):
        if cps[i] in NEWLINES:
            if i > start:
                out.append((start, i))
            start = i + 1
    if b > start:
        out.append((start, b))
    return out


def snapshot_fields(layout):
    """Keep the public layout fields required for field-by-field regression."""
    top = {key: layout[key] for key in ("min", "max", "w", "h", "mode", "truncated", "ink")
           if key in layout}
    if "error" in layout:
        return {"error": layout["error"]}
    top["lines"] = []
    for line in layout.get("lines", []):
        item = {key: line[key] for key in ("s", "e", "x", "w", "t", "hh", "baseline",
                                           "ascent", "descent", "justified", "truncated", "box", "ink")
                if key in line}
        item["runs"] = []
        for run in line.get("runs", []):
            item["runs"].append({key: run[key] for key in (
                "span", "face_index", "fallback", "ellipsis", "hyphen", "l", "o", "start",
                "end", "pos", "advance", "g", "py", "b") if key in run})
        top["lines"].append(item)
    return top


def build_units(cps, info, shaped, upem):
    """The reference's units, in logical order. `shaped[(a,b)]` is hb's glyphs."""
    scale = scale_for(upem)
    units = []
    brk = set(info["brk"])
    hard = set(info["hard"])
    n = len(cps)
    for (a, b, script) in items_of(cps, info):
        rtl = info["lv"][a] & 1
        # Cover every character of the item: pieces, and a unit per newline.
        cursor = a
        todo = []
        for (pa, pb) in pieces_of(cps, a, b):
            for i in range(cursor, pa):
                todo.append(("nl", i, i + 1, None))
            todo.append(("text", pa, pb, shaped[(pa, pb, script, rtl)]))
            cursor = pb
        for i in range(cursor, b):
            todo.append(("nl", i, i + 1, None))
        for kind, pa, pb, glyphs in todo:
            if kind == "nl":
                u = Unit()
                u.a, u.b, u.glyphs, u.width = pa, pb, [], 0
                u.level = info["lv"][pa]
                units.append(u)
                continue
            if glyphs is None:
                raise Refused("HarfBuzz refused")
            logical = list(reversed(glyphs)) if rtl else list(glyphs)
            groups = []
            for g in logical:
                cl = pa + g["cl"]
                if groups and cl <= groups[-1][0]:
                    groups[-1][1].append(g)
                else:
                    groups.append((cl, [g]))
            if not groups:
                u = Unit()
                u.a, u.b, u.glyphs, u.width = pa, pb, [], 0
                u.level = info["lv"][pa]
                units.append(u)
                continue
            for k, (cl, gs) in enumerate(groups):
                u = Unit()
                u.a = pa if k == 0 else cl
                u.b = groups[k + 1][0] if k + 1 < len(groups) else pb
                # Glyphs left to right: hb gives an rtl run's already visually.
                ordered = list(reversed(gs)) if rtl else gs
                u.glyphs = [(g["g"], round_shift10(g["ax"] * scale)) for g in ordered]
                u.width = sum(w for _, w in u.glyphs)
                u.level = info["lv"][u.a]
                units.append(u)
    for u in units:
        chars = [chr(cps[i]) for i in range(u.a, u.b)]
        u.space = bool(chars) and all(hangs(c) for c in chars)
        u.opp = u.b in brk and u.b < n
        last = cps[u.b - 1]
        u.hard = (u.b in hard) or (u.b == n and last in NEWLINES)
        u.hard = u.hard and last in NEWLINES
    return units


def chunks_of(units):
    """Runs of units ending at a break opportunity or a hard break."""
    out, cur = [], []
    for u in units:
        cur.append(u)
        if u.opp or u.hard:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


def fit_of(chunk):
    """Width of the chunk without the spaces that hang off its end."""
    total, fit = 0, 0
    for u in chunk:
        total += u.width
        if not u.space:
            fit = total
    return fit


def reference_lines(units, width):
    """Greedy lines as lists of units, by chunks."""
    lines = [[]]
    used = 0
    for chunk in chunks_of(units):
        total = sum(u.width for u in chunk)
        if lines[-1] and width is not None and used + fit_of(chunk) > width:
            lines.append([])
            used = 0
        lines[-1].extend(chunk)
        used += total
        if chunk[-1].hard:
            lines.append([])
            used = 0
    if not lines[-1] and len(lines) > 1 and not (units and units[-1].hard):
        lines.pop()
    return lines


def content_widths(units):
    chunks = chunks_of(units)
    minimum = max((fit_of(c) for c in chunks), default=0)
    maximum, line = 0, []
    for c in chunks:
        line.extend(c)
        if c[-1].hard:
            maximum = max(maximum, fit_of(line))
            line = []
    maximum = max(maximum, fit_of(line))
    return minimum, maximum


def finish_line(line, runs, rtl_para):
    """Visual glyph order for one line given ICU's runs; widths and lead."""
    visual = []
    for (start, length, rtl) in runs:
        inside = [u for u in line if start <= u.a < start + length]
        if rtl:
            inside.reverse()
        visual.extend(inside)
    return visual


def reference_layout(cps, info, units, width, align, runs_for_line, upem):
    rtl_para = info["pl"] & 1
    byte_of = [0]
    for c in cps:
        byte_of.append(byte_of[-1] + utf8_len(c))
    lines = reference_lines(units, width)
    out = []
    widest = 0
    for k, line in enumerate(lines):
        hang_from = len(line)
        while hang_from > 0 and line[hang_from - 1].space:
            hang_from -= 1
        hang = set(id(u) for u in line[hang_from:])
        text_width = sum(u.width for u in line[:hang_from])
        widest = max(widest, text_width)
        if line:
            a, b = line[0].a, line[-1].b
            runs = runs_for_line(k, a, b)
            visual = finish_line(line, runs, rtl_para)
            start_byte, end_byte = byte_of[a], byte_of[b]
        else:
            visual = []
            start_byte = end_byte = byte_of[len(cps)]
        out.append((start_byte, end_byte, text_width, visual, hang))
    avail = width if width is not None else widest
    result = []
    for (s, e, w, visual, hang) in out:
        slack = avail - w
        eff = align
        if eff == "start":
            eff = "right" if rtl_para else "left"
        elif eff == "end":
            eff = "left" if rtl_para else "right"
        if slack < 0:
            off = slack if rtl_para else 0
        elif eff == "right":
            off = slack
        elif eff == "center":
            off = int(slack / 2)
        else:
            off = 0
        lead = 0
        for u in visual:
            if id(u) in hang:
                lead += u.width
            else:
                break
        x = off - lead
        glyphs = []
        for u in visual:
            for (gid, adv) in u.glyphs:
                glyphs.append((gid, byte_of[u.a], x, adv))
                x += adv
        result.append({"s": s, "e": e, "w": w, "x": off, "glyphs": glyphs})
    return result, content_widths(units), widest


# ------------------------------------------------------------------- comparing

def flatten(line):
    out = []
    for run in line["runs"]:
        for g in run["g"]:
            out.append((g[0], g[1], g[2], g[3]))
    return out


def compare_case(cps, text_bytes, mine, ref, ref_content, widest, newline_bytes):
    """Differences between the driver's JSON and the reference. List of strings."""
    diffs = []
    if "error" in mine:
        return ["library refused: %s" % mine["error"]]
    ref_lines, = (ref,)
    if (mine["min"], mine["max"]) != ref_content:
        diffs.append("content widths %s vs %s" % ((mine["min"], mine["max"]),
                                                  ref_content))
    if len(mine["lines"]) != len(ref_lines):
        diffs.append("line count %d vs %d" % (len(mine["lines"]), len(ref_lines)))
        return diffs
    for k, (m, r) in enumerate(zip(mine["lines"], ref_lines)):
        got = [g for g in flatten(m) if g[1] not in newline_bytes]
        want = [g for g in r["glyphs"]]
        if (m["s"], m["e"], m["w"], m["x"]) != (r["s"], r["e"], r["w"], r["x"]):
            diffs.append("line %d box %s vs %s" % (
                k, (m["s"], m["e"], m["w"], m["x"]), (r["s"], r["e"], r["w"], r["x"])))
        elif got != want:
            first = next((i for i, (p, q) in enumerate(zip(got, want)) if p != q),
                         min(len(got), len(want)))
            diffs.append("line %d glyph %d: %s vs %s (of %d, %d)" % (
                k, first, got[first] if first < len(got) else None,
                want[first] if first < len(want) else None, len(got), len(want)))
    return diffs


def consistency(mine, text_bytes, newline_bytes, boundaries):
    """The hit-testing functions against one another and against the glyphs."""
    diffs = []
    carets = {c[0]: c for c in mine["carets"]}
    for (off, hit_off, aff, x_back) in mine["hits"]:
        if off in boundaries and off in carets and carets[off][1] != x_back:
            diffs.append("hit test at the caret of %d returned %d whose caret is "
                         "at %d, not %d" % (off, hit_off, x_back, carets[off][1]))
    # A caret sits on the edge of its cluster.
    for line in mine["lines"]:
        for run in line["runs"]:
            groups = []
            for g in run["g"]:
                if groups and groups[-1][0] == g[1]:
                    groups[-1][2] = g[2] + g[3]
                else:
                    groups.append([g[1], g[2], g[2] + g[3]])
            for (cluster, left, right) in groups:
                if cluster in newline_bytes or cluster not in carets:
                    continue
                want = right if run["l"] & 1 else left
                got = carets[cluster][1]
                if got != want:
                    diffs.append("caret of %d at %d, cluster edge at %d" % (
                        cluster, got, want))
    # The rectangles of a line add up to its glyphs.
    for line in mine["lines"]:
        covered = sum(g[3] for run in line["runs"] for g in run["g"]
                      if g[1] not in newline_bytes)
        rects = [r for r in mine["rects"] if r[1] == line["t"]]
        total = sum(r[2] for r in rects)
        if line["runs"] and total != covered:
            diffs.append("selection of the line at %d is %d wide, glyphs %d" % (
                line["t"], total, covered))
        for r in rects:
            if r[3] != line["hh"]:
                diffs.append("selection rectangle height %d, line %d" % (
                    r[3], line["hh"]))
    return diffs


def metric_diffs(mine, metrics, fonts_bounds, font):
    """Line height and ink against fontTools."""
    diffs = []
    ft = metrics
    upem = ft["upem"]
    scale = scale_for(upem)
    asc, desc, gap = ft["asc"], ft["desc"], ft["gap"]
    height = (round_shift10(asc * scale) + round_shift10(-desc * scale)
              + round_shift10(gap * scale))
    for k, line in enumerate(mine["lines"]):
        if line["hh"] != height:
            diffs.append("line %d height %d, the font asks for %d" % (
                k, line["hh"], height))
        box = None
        for run in line["runs"]:
            for g in run["g"]:
                b = ft["bounds"].get(str(g[0]))
                if not b:
                    continue
                x, y = g[4], g[5]
                l = x + ink_px(b[0], scale)
                r = x + ink_px(b[2], scale)
                t = y - ink_px(b[3], scale)
                bt = y - ink_px(b[1], scale)
                box = [l, t, r, bt] if box is None else [
                    min(box[0], l), min(box[1], t), max(box[2], r), max(box[3], bt)]
        want = [0, 0, 0, 0] if box is None else [
            box[0], box[1], box[2] - box[0], box[3] - box[1]]
        got = line["ink"]
        # The library measures exact curve bounds in 26.6 of a font unit and
        # then scales; fontTools' bounds are floats. One 64th of a pixel of
        # slack covers the two roundings, and nothing more.
        if any(abs(a - b) > 1 for a, b in zip(got, want)):
            diffs.append("line %d ink %s, fontTools %s" % (k, got, want))
    return diffs


# ------------------------------------------------------------------------ main

def cases(font):
    have = hb_diff.covered(font, {p for _, _, probes, _ in KINDS for p in probes})
    for texts, dirs, probes, aligns in KINDS:
        if not all(p in have for p in probes):
            yield None, texts
            continue
        for index, (name, text) in enumerate(texts):
            for d in dirs:
                yield (name, text, d, "start"), None
                if index == 0:
                    for a in aligns:
                        yield (name, text, d, a), None


def main(argv):
    quiet = "--quiet" in argv
    listing = "--list" in argv
    everything = "--all" in argv
    limit = 40
    snapshot_write = None
    snapshot_check = None
    rest = []
    skip = False
    for i, a in enumerate(argv[1:], start=1):
        if skip:
            skip = False
        elif a == "--fonts":
            limit = int(argv[i + 1])
            skip = True
        elif a == "--snapshot-write":
            if i + 1 >= len(argv):
                print("layout_diff: --snapshot-write requires a path", file=sys.stderr)
                return 2
            snapshot_write = argv[i + 1]
            skip = True
        elif a == "--snapshot-check":
            if i + 1 >= len(argv):
                print("layout_diff: --snapshot-check requires a path", file=sys.stderr)
                return 2
            snapshot_check = argv[i + 1]
            skip = True
        elif not a.startswith("--"):
            rest.append(a)
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
    else:
        fonts = list(corpus.fonts("sfnt"))
        if not everything and limit < len(fonts):
            step = len(fonts) / limit
            fonts = [fonts[int(i * step)] for i in range(limit)]
    os.makedirs(SCRATCH, exist_ok=True)

    plan = []          # (font, case)
    uncovered = 0
    for font in fonts:
        for case, skipped in cases(font):
            if case is None:
                uncovered += len(skipped)
            else:
                plan.append((font, case))
    if not plan:
        print("layout_diff: nothing to compare")
        return 1

    # ICU: levels, scripts and break opportunities per (direction, text).
    texts = sorted({(c[2], c[1]) for _, c in plan})
    reqs = ["info %s %s" % (d, t.encode("utf-8").hex()) for d, t in texts]
    infos = dict(zip(texts, run_icu(reqs, "info")))
    for key, answer in infos.items():
        if "error" in answer:
            raise oracle_env.OracleUnavailable("ICU refused %r" % (key,))

    base_metrics = run_fonttools({f: [] for f in fonts})
    dropped = [f for f in fonts if not base_metrics.get(f)]
    fonts = [f for f in fonts if base_metrics.get(f)]
    plan = [(f, c) for f, c in plan if f in base_metrics and base_metrics[f]]

    # HarfBuzz: every distinct piece of every item.
    jobs, job_of = [], {}
    upem_of = {}
    metrics_of = {}
    for font, case in plan:
        name, text, d, align = case
        cps = [ord(c) for c in text]
        info = infos[(d, text)]
        for (a, b, script) in items_of(cps, info):
            rtl = info["lv"][a] & 1
            for (pa, pb) in pieces_of(cps, a, b):
                key = (font, tuple(cps[pa:pb]), script, rtl)
                if key not in job_of:
                    job_of[key] = len(jobs)
                    jobs.append((font, rtl, script, cps[pa:pb]))
    answers = run_hb(jobs)

    # fontTools: metrics and bounds of the glyphs we will place.
    refused = {}
    mine_of = {}
    for font in fonts:
        mine_of[font] = {}
    by_font = {}
    for font, case in plan:
        by_font.setdefault(font, []).append(case)
    gids = {}
    for font, cs in by_font.items():
        groups = {}
        for case in cs:
            groups.setdefault((case[2], case[3]), []).append(case)
        for (d, align), members in groups.items():
            requests = []
            plans = []
            for case in members:
                name, text, _, _ = case
                cps = [ord(c) for c in text]
                info = infos[(d, text)]
                shaped = {}
                try:
                    for (a, b, script) in items_of(cps, info):
                        rtl = info["lv"][a] & 1
                        for (pa, pb) in pieces_of(cps, a, b):
                            got = answers[job_of[(font, tuple(cps[pa:pb]), script, rtl)]]
                            shaped[(pa, pb, script, rtl)] = got
                    upem = base_metrics[font]["upem"]
                    units = build_units(cps, info, shaped, upem)
                except Refused as why:
                    refused[(font, name, d, align)] = str(why)
                    continue
                mn, mx = content_widths(units)
                widths = [None, mn] + sorted({int(mx * f) for f in FRACTIONS})
                plans.append((case, cps, info, units, widths, upem))
                for w in widths:
                    requests.append("%s\t%s" % ("u" if w is None else w,
                                                text.encode("utf-8").hex()))
            if not requests:
                continue
            argv_d = [DRIVER, "--size", str(SIZE), "--bidi", d, "--align", align, font]
            finished = subprocess.run(argv_d, input="\n".join(requests) + "\n",
                                      capture_output=True, text=True)
            lines = finished.stdout.splitlines()
            if finished.returncode != 0 or len(lines) != len(requests):
                for case, *_ in plans:
                    refused[(font, case[0], d, align)] = (
                        "driver: " + (finished.stderr.strip().splitlines() or ["no answer"])[0])
                continue
            at = 0
            for (case, cps, info, units, widths, upem) in plans:
                for w in widths:
                    mine = json.loads(lines[at])
                    at += 1
                    mine_of[font][(case[0], d, align, w)] = (mine, cps, info, units,
                                                              upem)
                    for line in mine.get("lines", []):
                        for run in line["runs"]:
                            for g in run["g"]:
                                gids.setdefault(font, set()).add(g[0])
    if snapshot_write or snapshot_check:
        records = []
        for font, table in mine_of.items():
            for (name, direction, align, width), (answer, *_rest) in table.items():
                records.append({
                    "font": os.path.relpath(font, ROOT), "case": name,
                    "direction": direction, "align": align, "width": width,
                    "layout": snapshot_fields(answer),
                })
        records.sort(key=lambda r: (r["font"], r["case"], r["direction"],
                                    r["align"], -1 if r["width"] is None else r["width"]))
        actual = "".join(json.dumps(r, ensure_ascii=False, sort_keys=True,
                                    separators=(",", ":")) + "\n" for r in records)
        if snapshot_write:
            opener = gzip.open if snapshot_write.endswith(".gz") else open
            with opener(snapshot_write, "wt", encoding="utf-8") as h:
                h.write(actual)
        if snapshot_check:
            try:
                opener = gzip.open if snapshot_check.endswith(".gz") else open
                with opener(snapshot_check, "rt", encoding="utf-8") as h:
                    expected = h.read()
            except OSError as why:
                print("layout snapshot unavailable: %s" % why, file=sys.stderr)
                return 1
            if actual != expected:
                print("layout snapshot differs: %s" % snapshot_check, file=sys.stderr)
                return 1
    # ICU: the visual runs of every reference line.
    run_reqs, run_keys = [], []
    layouts = {}
    for font, table in mine_of.items():
        for key, (mine, cps, info, units, upem) in table.items():
            if "error" in mine:
                continue
            name, d, align, w = key
            lines = reference_lines(units, w)
            layouts[(font, key)] = lines
            text = "".join(chr(c) for c in cps)
            for k, line in enumerate(lines):
                if line:
                    a, b = line[0].a, line[-1].b
                    run_reqs.append("runs %s %s %d %d" % (d, text.encode("utf-8").hex(), a, b))
                    run_keys.append((font, key, k))
    run_answers = {}
    if run_reqs:
        for k, ans in zip(run_keys, run_icu(run_reqs, "runs")):
            run_answers[k] = ans
    metrics = run_fonttools({f: g for f, g in gids.items()}) if gids else {}

    compared = failed = consistency_failed = metric_failed = 0
    seen = {"lines": 0, "wrapped": 0, "rtl lines": 0, "mixed-level lines": 0,
            "glyphs": 0, "ligature clusters": 0, "hard breaks": 0}
    reports = []
    for font, table in mine_of.items():
        for key, (mine, cps, info, units, upem) in table.items():
            name, d, align, w = key
            label = "%s %s %s %s width=%s" % (os.path.basename(font), name, d, align,
                                              "unbounded" if w is None else w)
            compared += 1
            for line in mine.get("lines", []):
                seen["lines"] += 1
                levels = {r["l"] for r in line["runs"]}
                seen["rtl lines"] += 1 if any(l & 1 for l in levels) else 0
                seen["mixed-level lines"] += 1 if len(levels) > 1 else 0
                for r in line["runs"]:
                    seen["glyphs"] += len(r["g"])
                    seen["ligature clusters"] += len(r["g"]) - len({g[1] for g in r["g"]})
            seen["wrapped"] += 1 if len(mine.get("lines", [])) > 1 else 0
            seen["hard breaks"] += sum(1 for c in cps if c in NEWLINES)
            if "error" in mine:
                failed += 1
                reports.append((label, ["library refused: %s" % mine["error"]]))
                continue

            def runs_for_line(k, a, b, font=font, key=key):
                ans = run_answers[(font, key, k)]
                return ans["runs"]

            ref, content, widest = reference_layout(cps, info, units, w, align,
                                                    runs_for_line, upem)
            text_bytes = "".join(chr(c) for c in cps).encode("utf-8")
            newline_bytes = set()
            at_byte = 0
            boundaries = {0}
            for c in cps:
                if c in NEWLINES:
                    newline_bytes.add(at_byte)
                at_byte += utf8_len(c)
                boundaries.add(at_byte)
            diffs = compare_case(cps, text_bytes, mine, ref, content, widest,
                                 newline_bytes)
            cons = consistency(mine, text_bytes, newline_bytes, boundaries)
            met = metric_diffs(mine, metrics[font], None, font) \
                if font in metrics and metrics[font] else []
            if diffs:
                failed += 1
                reports.append((label, diffs))
            if cons:
                consistency_failed += 1
                reports.append((label + " [consistency]", cons))
            if met:
                metric_failed += 1
                reports.append((label + " [metrics]", met))
    if not quiet or failed or consistency_failed or metric_failed or refused:
        for label, diffs in reports[:60]:
            print("DIFF %s" % label)
            for d in diffs[:4]:
                print("     %s" % d)
        if listing:
            for label, diffs in reports[60:]:
                print("DIFF %s: %s" % (label, diffs[0]))
    print("fonts %d, layouts compared %d (%d texts skipped for want of glyphs)"
          % (len(fonts), compared, uncovered))
    print("sample: " + ", ".join("%d %s" % (v, k) for k, v in seen.items()))
    print("declined, the reference cannot read the font: %s"
          % (", ".join(os.path.basename(f) for f in dropped) or "none"))
    print("refused by a reference or the driver: %d" % len(refused))
    for key, why in list(refused.items())[:10]:
        print("  %s: %s" % (key, why))
    print("lines/widths/glyphs differing: %d; hit-testing inconsistent: %d; "
          "metrics or ink differing: %d" % (failed, consistency_failed, metric_failed))
    return 1 if failed or consistency_failed or metric_failed or refused else 0


if __name__ == "__main__":
    for option in ("--snapshot-write", "--snapshot-check"):
        if option in sys.argv[1:] and sys.argv.index(option) == len(sys.argv) - 1:
            sys.stderr.write("layout_diff: %s requires a path\n" % option)
            sys.exit(2)
    try:
        oracle_env.provenance(["icu", "harfbuzz", "fonttools"])   # the pins, checked
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED layout_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
