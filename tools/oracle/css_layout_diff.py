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
"""Paragraph layout against Chromium: CSS Writing Modes, CSS Text and the browser's line breaking.

documentation/design.md section 10 and notes/font/VERTICAL.md. Chromium (the pinned image,
`chrome` in tools/oracle/containers/IMAGES) is asked to lay each case out in a page whose only
font is the test font, and to say where every character is: the rectangle of each code point
(`Range.getClientRects`, united). This library lays out the same text with the same
options through `font-layout`, and the rectangles are compared.

What is held equal on purpose: the font is `vertical-text.ttf`, whose ascent and descent come to
whole pixels at the sizes used, so that Chromium's rounding of font metrics to pixels cannot move
a line; the text is set with `white-space: pre-wrap`, which is what this library's paragraph
does with its spaces (they are kept, and a space at the end of a line hangs); and the room along
the lines is the same number of pixels.

A second phase compares *ink*: Chromium draws a few cases at a large size, the screenshot is
read back (a PNG decoded here, no imaging library), and the box of each glyph's ink is compared
with the one predicted from this library's placed glyphs and their orientation. The rectangles
of the first phase are line boxes and advances; they cannot show a glyph drawn off its axis or
turned the wrong way, and the ink can.

**What it cannot see**, stated so the absence is a statement: one face and one size, no fallback
fonts, and a character Chromium gives no rectangle (a collapsed or hung space) is compared only
when it has one. The rectangle matrix includes horizontal, vertical and sideways writing,
wrapping, bidi, line-height, inline-object and flow cases, plus inter-word and inter-character
justification in both directions. It does not cover kashida shaping; that has its own oracle.

Usage: css_layout_diff.py [-v] [case-name...]
"""

import html
import json
import math
import os
import re
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import layout_snapshot

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-layout")
SCRATCH = os.path.join(ROOT, "build", "oracle", "css-layout")
FONT = os.path.join(ROOT, "tests", "data", "fonts", "vertical-text.ttf")

# The most a coordinate may differ, in pixels: two units of the 1/64 pixel both sides compute in.
TOLERANCE = 0.05
# A justified line spreads its slack over many gaps, each rounded to 1/64 pixel, and the two sides
# round differently (Chromium may overshoot the room by a tenth of a pixel in all); the error
# builds along the line, so a position may be off by a few of those.
TOLERANCE_JUSTIFIED = 0.25

SIZE = 50   # ascent 880 and descent 120 per 1000 are 44 and 6 pixels: whole, so nothing rounds

J = "日本語"          # 日本語
H = "\ud55c"                         # a Hangul syllable: justified by its spaces, not between characters
JUSTIFY_TEXTS = [
    ("cjk", J * 4),
    ("mixed", "\u65e5AB\u672cab \u8a9eAB\u65e5\u672c ab\u8a9eAB\u65e5\u672c"),
    ("latin", "AB ab AB ab AB ab AB ab AB ab AB ab"),
    ("punct", "\u65e5\u3001\u672c\u3002\u300c\u8a9e\u300d\u65e5\u672c\u8a9e\u65e5\u672c\u8a9e\u65e5"),
    ("dash", "\u65e5\u2014\u672c\u2026\u8a9e\u65e5\u672c\u8a9e\u65e5\u672c\u8a9e\u65e5"),
    ("hangul", "a" + H + "a" + H + "a" + H + "a" + H + "a" + H + "a" + H + "a" + H + "a" + H + "a"),
    ("digits", "\u65e5" "0124" "\u672c\u8a9e\u65e5\u672c\u8a9e\u65e5\u672c\u8a9e\u65e5"),
    ("forms", "a\uff21a\uff71a\u3105a\u30fba\u3005a\uff01a\u3231a\u3400a\u337fa\u2e80a"),
]
CASES = [
    # name, text, mode, orientation, room (px, None = unbounded), align, direction
    ("horizontal-control", "AB ab 日 AB ab", "horizontal", "mixed", 330, "start", "ltr"),
    ("vertical-rl-cjk-wrap", J * 3, "vertical-rl", "mixed", 250, "start", "ltr"),
    ("vertical-lr-cjk-wrap", J * 3, "vertical-lr", "mixed", 250, "start", "ltr"),
    ("sideways-rl-cjk-wrap", J * 3, "sideways-rl", "mixed", 250, "start", "ltr"),
    ("sideways-lr-cjk-wrap", J * 3, "sideways-lr", "mixed", 250, "start", "ltr"),
    ("mixed-latin", "日AB本ab語", "vertical-rl", "mixed", 600, "start", "ltr"),
    ("mixed-digits", "日" "2014" "語", "vertical-rl", "mixed", 600, "start", "ltr"),
    ("upright-latin", "日AB本ab", "vertical-rl", "upright", 600, "start", "ltr"),
    ("sideways-text", "日本AB語", "vertical-rl", "sideways", 600, "start", "ltr"),
    ("punctuation-vert-forms", "日、本。「語」ー", "vertical-rl",
     "mixed", 700, "start", "ltr"),
    ("dash-and-ellipsis", "日—本…語", "vertical-rl", "mixed", 700,
     "start", "ltr"),
    ("brackets", "日(a)本", "vertical-rl", "mixed", 700, "start", "ltr"),
    ("spaces", "日 本  語", "vertical-rl", "mixed", 700, "start", "ltr"),
    ("words-wrap", "日本 AB ab 語日 ABC", "vertical-rl", "mixed", 260,
     "start", "ltr"),
    ("newlines", "日本\n語\n\n日", "vertical-rl", "mixed", 700, "start", "ltr"),
    ("unbounded", J * 2 + "AB" + J, "vertical-rl", "mixed", None, "start", "ltr"),
    ("align-center", J, "vertical-rl", "mixed", 400, "center", "ltr"),
    ("align-end", J, "vertical-rl", "mixed", 400, "end", "ltr"),
    ("rtl-cjk", J * 2, "vertical-rl", "mixed", 400, "start", "rtl"),
    ("rtl-wrap", J * 3, "vertical-rl", "mixed", 250, "start", "rtl"),
    ("vertical-lr-mixed", "日AB本ab語" * 2, "vertical-lr", "mixed", 400, "start",
     "ltr"),
    ("sideways-lr-mixed", "日AB本ab語", "sideways-lr", "mixed", 600, "start", "ltr"),
]

for _name, _text in JUSTIFY_TEXTS:
    for _mode, _room in (("horizontal", 330), ("vertical-rl", 330)):
        for _j in ("auto", "inter-word", "inter-character", "none"):
            CASES.append(("justify-%s-%s-%s" % (_name, _mode, _j), _text, _mode, "mixed", _room,
                          "justify", "ltr", _j))

for _name, _text, _spacing, _room, _mode in (
    ("word-spacing-positive", "AB ab  ca", "8px", 330, "horizontal"),
    ("word-spacing-negative", "AB ab  ca", "-3px", 330, "horizontal"),
    ("word-spacing-wrap", "AB ab ca AB", "12px", 200, "horizontal"),
    ("word-spacing-cjk", "日\u3000本\u3000語", "7px", 330, "horizontal"),
    ("word-spacing-vertical", "AB ab \u3000語", "6px", 330, "vertical-rl"),
):
    CASES.append((_name, _text, _mode, "mixed", _room, "start", "ltr", "auto",
                  {"wordSpacing": _spacing,
                   "driver": ("--word-spacing", _spacing[:-2])}))
for _align_last in ("start", "end", "left", "right", "center", "justify"):
    CASES.append(("align-last-" + _align_last, "AB ab ca AB", "horizontal", "mixed", 200,
                  "start", "ltr", "auto",
                  {"alignLast": _align_last, "driver": ("--align-last", _align_last)}))
for _wrap in ("anywhere", "break-word"):
    CASES.append(("overflow-wrap-" + _wrap, "ABABABABABAB", "horizontal", "mixed", 140,
                  "start", "ltr", "auto",
                  {"overflowWrap": _wrap, "driver": ("--overflow-wrap", _wrap)}))
CASES.append(("word-break-break-all", "ABABABABABAB", "horizontal", "mixed", 140,
              "start", "ltr", "auto",
              {"wordBreak": "break-all", "driver": ("--word-break", "break-all")}))
CASES.append(("word-break-break-word", "ABABABABABAB", "horizontal", "mixed", 140,
              "start", "ltr", "auto",
              {"wordBreak": "break-word", "driver": ("--word-break", "break-word")}))
CASES.append(("word-break-break-word-overrides-overflow-wrap", "ABABABABABAB", "horizontal",
              "mixed", 140, "start", "ltr", "auto",
              {"wordBreak": "break-word", "overflowWrap": "break-word",
               "driver": ("--word-break", "break-word", "--overflow-wrap", "break-word")}))
CASES.append(("word-break-keep-all-korean", "가나다라마바사", "horizontal", "mixed", 70,
              "start", "ltr", "auto",
              {"wordBreak": "keep-all", "lineStructureOnly": True,
               "driver": ("--word-break", "keep-all")}))
CASES.append(("word-break-keep-all-cjk", "日本語日本語", "horizontal", "mixed", 70,
              "start", "ltr", "auto",
              {"wordBreak": "keep-all", "lineStructureOnly": True,
               "driver": ("--word-break", "keep-all")}))
CASES.append(("line-break-anywhere-punctuation", "日本語（AB）日本語", "horizontal", "mixed", 50,
              "start", "ltr", "auto",
              {"lineBreak": "anywhere", "driver": ("--line-break", "anywhere")}))
CASES.append(("line-break-anywhere-before-closing-punctuation", "AB）", "horizontal", "mixed", 70,
              "start", "ltr", "auto",
              {"lineBreak": "anywhere", "lang": "ja",
               "driver": ("--line-break", "anywhere")}))
CASES.append(("line-break-anywhere-overrides-keep-all", "日本語日本語", "horizontal", "mixed", 50,
              "start", "ltr", "auto",
              {"lineBreak": "anywhere", "wordBreak": "keep-all",
               "driver": ("--line-break", "anywhere", "--word-break", "keep-all")}))
for _line_break in ("strict", "loose"):
    CASES.append(("line-break-%s-small-kana" % _line_break, "あぁあぁあぁあぁ", "horizontal",
                  "mixed", 55, "start", "ltr", "auto",
                  {"lineBreak": _line_break, "lang": "ja",
                   "driver": ("--line-break", _line_break)}))
CASES.append(("white-space-pre", "AB AB  \nAB AB", "horizontal", "mixed", 100,
              "start", "ltr", "auto",
              {"whiteSpace": "pre", "driver": ("--white-space", "pre")}))
# Not here, on purpose: mixed Latin, spaces and ideographs in a right-to-left paragraph. Chromium
# 154 does not fill some of those lines (two opportunities of a line of "ab \u65e5" repeated go
# nowhere), and counts one more on others, so it is not a reference for them; this library fills
# the line and spreads the slack over the opportunities it has. See notes/font/VERTICAL.md.
# Soft hyphens (hyphens: manual, Chromium's default) in both axes, with the hyphen that ends a line.
for _mode, _room in (("horizontal", 170), ("vertical-rl", 170), ("vertical-lr", 170),
                     ("sideways-lr", 170)):
    CASES.append(("soft-hyphens-" + _mode, "AB\u00adAB\u00adAB\u00adAB\u00adAB", _mode, "mixed",
                  _room, "start", "ltr"))
    CASES.append(("soft-hyphens-cjk-" + _mode, "\u65e5\u672c\u00ad\u8a9e\u65e5\u00ad\u672c\u8a9e",
                  _mode, "mixed", 150, "start", "ltr"))
CASES.append(("justify-rtl-cjk", J * 4, "horizontal", "mixed", 330, "justify", "rtl", "auto"))
CASES.append(("justify-rtl-vertical", J * 4, "vertical-rl", "mixed", 330, "justify", "rtl", "auto"))
CASES.append(("justify-lr", J * 4, "vertical-lr", "mixed", 330, "justify", "ltr", "auto"))
CASES.append(("justify-sideways-lr", "\u65e5AB\u672cab \u8a9eAB\u65e5\u672c ab\u8a9eAB\u65e5\u672c",
              "sideways-lr", "mixed", 330, "justify", "ltr", "auto"))

# Rectangle, circle and polygon floats on both sides. The height-zero query sees the full width,
# then the measured-height query intersects the float and narrows the band.
FLOW_CASES = []
for _shape in ("rect", "circle", "polygon"):
    for _side in ("left", "right"):
        for _index in range(5):
            _inset = 80 + _index * 20
            _height = _inset if _shape == "circle" else 70 + _index * 20
            _name = "flow-%s-%s-%d" % (_shape, _side, _index)
            _flow = {"left": _inset, "top": 0, "height": _height,
                     "shape": _shape, "side": _side}
            _driver = ("--flow-left", str(_inset), "--flow-top", "0",
                       "--flow-depth", str(_height), "--flow-shape", _shape,
                       "--flow-side", _side)
            FLOW_CASES.append(_name)
            # Short words fit the narrowed bands; the repeated spaces force wrapping while
            # the float is active, unlike an unbreakable word that simply clears the float.
            CASES.append((_name, "AA " * 40, "horizontal", "mixed", 300,
                          "start", "ltr", "auto", {"flow": _flow, "driver": _driver}))

# A circle is already partway through its lower half at paragraph top. This
# unbreakable word cannot fit in the current band; Chromium clears it to the
# float's bottom rather than searching for a lower, wider slice within the shape.
CASES.append(("flow-circle-tapered-clear", "AAAAA", "horizontal", "mixed", 300,
              "start", "ltr", "auto", {"flow": {"left": 160, "top": -80, "height": 160,
              "shape": "circle", "side": "left"},
              "driver": ("--flow-left", "160", "--flow-top", "-80", "--flow-depth", "160",
                         "--flow-shape", "circle", "--flow-side", "left")}))
FLOW_CASES.append("flow-circle-tapered-clear")

# Explicit line-height rectangles, including a requested height below the font metrics.
for _name, _text, _css, _driver in (
    ("line-height-short", "AB ab", "30px", ("--line-height-length", "30")),
    ("line-height-tall", "AB ab", "80px", ("--line-height-length", "80")),
    ("line-height-multiple", "AB ab", "1.5", ("--line-height-multiple", "1.5")),
):
    CASES.append((_name, _text, "horizontal", "mixed", None, "start", "ltr", "auto",
                  {"lineHeight": _css, "driver": _driver}))

for _name, _css, _driver in (
    ("vertical-align-sub", "sub", "sub"),
    ("vertical-align-super", "super", "super"),
    ("vertical-align-length", "10px", "length:10"),
    ("vertical-align-negative-length", "-8px", "length:-8"),
    ("vertical-align-percentage", "50%", "percentage:0.5"),
    ("vertical-align-text-top", "text-top", "text-top"),
    ("vertical-align-text-bottom", "text-bottom", "text-bottom"),
    ("vertical-align-middle", "middle", "middle"),
    ("vertical-align-top", "top", "top"),
    ("vertical-align-bottom", "bottom", "bottom"),
):
    CASES.append((_name, "AB", "horizontal", "mixed", None, "start", "ltr", "auto",
                  {"verticalAlign": _css, "driver": ("--vertical-align", _driver)}))

# One paragraph with two genuinely different text styles. The second character is smaller and
# superscripted; compare each character's Range rectangle against its corresponding span run.
CASES.append(("mixed-size-length", "AB", "horizontal", "mixed", None, "start", "ltr", "auto",
              {"secondSpan": {"start": 1, "size": 30, "verticalAlign": "10px"},
               "driver": ("--split-at", "1", "--second-size", "30",
                          "--second-vertical-align", "length:10")}))

PAGE = """<!doctype html><meta charset=utf-8>
<style>
@font-face { font-family: T; src: url("file://%(font)s"); }
body { margin: 0 }
.t { font-family: T; font-size: %(size)dpx; line-height: normal; position: absolute;
     left: 0; top: 0; margin: 0; padding: 0; white-space: pre-wrap; overflow-wrap: normal;
     word-break: normal; line-break: auto; font-kerning: normal }
</style>
<pre id=out></pre>
<script>
const cases = %(cases)s;
function measure(c) {
  const d = document.createElement('div');
  d.className = 't';
  if (c.lang) d.lang = c.lang;
  d.style.writingMode = c.mode === 'horizontal' ? 'horizontal-tb' : c.mode;
  d.style.textOrientation = c.orient;
  d.style.textAlign = c.align;
  if (c.alignLast) d.style.textAlignLast = c.alignLast;
  if (c.overflowWrap) d.style.overflowWrap = c.overflowWrap;
  if (c.wordBreak) d.style.wordBreak = c.wordBreak;
  if (c.lineBreak) d.style.lineBreak = c.lineBreak;
  if (c.whiteSpace) d.style.whiteSpace = c.whiteSpace;
  d.style.textJustify = c.justify;
  d.style.direction = c.dir;
  d.style.lineHeight = c.verticalAlign ? '0' : (c.lineHeight || 'normal');
  if (c.room === null) {
    d.style.whiteSpace = 'pre';
  } else if (c.mode === 'horizontal') {
    d.style.width = c.room + 'px';
  } else {
    d.style.height = c.room + 'px';
  }
  if (c.flow) {
    const f = document.createElement('div');
    f.style.cssFloat = c.flow.side;
    f.style.width = c.flow.left + 'px';
    f.style.marginTop = c.flow.top + 'px';
    f.style.height = c.flow.height + 'px';
    if (c.flow.shape === 'circle') f.style.shapeOutside = 'circle(50%%)';
    if (c.flow.shape === 'polygon') {
      f.style.shapeOutside = c.flow.side === 'left'
        ? 'polygon(0 0, 100%% 0, 60%% 100%%, 0 100%%)'
        : 'polygon(0 0, 100%% 0, 100%% 100%%, 40%% 100%%)';
    }
    d.append(f, document.createTextNode(c.text));
  } else if (c.secondSpan) {
    const first = document.createTextNode(c.text.slice(0, c.secondSpan.start));
    const span = document.createElement('span');
    span.style.fontSize = c.secondSpan.size + 'px';
    span.style.lineHeight = 'normal';
    span.style.verticalAlign = c.secondSpan.verticalAlign;
    span.textContent = c.text.slice(c.secondSpan.start);
    d.append(first, span);
  } else if (c.verticalAlign) {
    const span = document.createElement('span');
    span.style.lineHeight = 'normal';
    span.style.verticalAlign = c.verticalAlign;
    span.textContent = c.text;
    d.appendChild(span);
  } else {
    d.textContent = c.text;
  }
  if (c.wordSpacing) d.style.wordSpacing = c.wordSpacing;
  document.body.appendChild(d);
  const tn = c.secondSpan ? null : (c.flow ? d.childNodes[1]
      : c.verticalAlign ? d.firstChild.firstChild : d.firstChild);
  const chars = [];
  let i = 0;
  for (const ch of c.text) {
    const node = c.secondSpan ? (i < c.secondSpan.start ? d.childNodes[0] : d.childNodes[1].firstChild) : tn;
    const start = c.secondSpan && i >= c.secondSpan.start ? i - c.secondSpan.start : i;
    const r = document.createRange();
    r.setStart(node, start);
    r.setEnd(node, start + ch.length);
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, n = 0;
    for (const q of r.getClientRects()) {
      n++;
      x0 = Math.min(x0, q.x); y0 = Math.min(y0, q.y);
      x1 = Math.max(x1, q.x + q.width); y1 = Math.max(y1, q.y + q.height);
    }
    chars.push(x0 === Infinity ? null : [x0, y0, x1 - x0, y1 - y0, n]);
    i += ch.length;
  }
  const b = d.getBoundingClientRect();
  let minContent = null;
  if (c.overflowWrap || c.wordBreak === 'break-all' || c.wordBreak === 'break-word'
      || c.lineBreak === 'anywhere'
      || c.whiteSpace === 'pre') {
    const intrinsic = d.cloneNode(true);
    intrinsic.style.width = 'min-content';
    if (c.whiteSpace !== 'pre') intrinsic.style.whiteSpace = 'normal';
    document.body.appendChild(intrinsic);
    minContent = intrinsic.getBoundingClientRect().width;
    intrinsic.remove();
  }
  const out = {name: c.name, box: [b.x, b.y, b.width, b.height], chars: chars,
               minContent: minContent};
  d.remove();
  return out;
}
Promise.all(cases.flatMap(c => [document.fonts.load(%(size)d + 'px T', c.text),
  ...(c.secondSpan ? [document.fonts.load(c.secondSpan.size + 'px T', c.text.slice(c.secondSpan.start))] : [])])).then(() => {
  document.getElementById('out').textContent = JSON.stringify(cases.map(measure));
});
</script>
"""


def chrome_rects(cases):
    """Ask Chromium, in its pinned image, where every character of every case is."""
    os.makedirs(SCRATCH, exist_ok=True)
    spec = [dict(name=c[0], text=c[1], mode=c[2], orient=c[3], room=c[4], align=c[5], dir=c[6],
                 justify=c[7] if len(c) > 7 else "auto",
                 lineHeight=(c[8].get("lineHeight", "normal") if len(c) > 8 else "normal"),
                 verticalAlign=(c[8].get("verticalAlign") if len(c) > 8 else None),
                 secondSpan=(c[8].get("secondSpan") if len(c) > 8 else None),
                 flow=(c[8].get("flow") if len(c) > 8 else None),
                 wordSpacing=(c[8].get("wordSpacing") if len(c) > 8 else None),
                 alignLast=(c[8].get("alignLast") if len(c) > 8 else None),
                 overflowWrap=(c[8].get("overflowWrap") if len(c) > 8 else None),
                 wordBreak=(c[8].get("wordBreak") if len(c) > 8 else None),
                 lineBreak=(c[8].get("lineBreak") if len(c) > 8 else None),
                 lang=(c[8].get("lang") if len(c) > 8 else None),
                 lineStructureOnly=(c[8].get("lineStructureOnly", False) if len(c) > 8 else False),
                 whiteSpace=(c[8].get("whiteSpace") if len(c) > 8 else None))
            for c in cases]
    page = os.path.join(SCRATCH, "page.html")
    with open(page, "w", encoding="utf-8") as h:
        h.write(PAGE % dict(font=FONT, size=SIZE, cases=json.dumps(spec)))
    argv = oracle_env.command("chrome", [
        "chromium", "--headless=new", "--no-sandbox", "--disable-gpu",
        "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--user-data-dir=" + os.path.join(SCRATCH, "profile"),
        "--virtual-time-budget=5000", "--dump-dom", "file://" + page], scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    found = re.search(r'<pre id="out">(.*?)</pre>', finished.stdout, re.S)
    if not found or not found.group(1).strip():
        raise RuntimeError("Chromium gave no measurements:\n" + finished.stderr[-600:])
    return json.loads(html.unescape(found.group(1)))


def ours(case):
    """This library's rectangles for one case, through the same driver the other gates use."""
    name, text, mode, orient, room, align, direction = case[:7]
    justify = case[7] if len(case) > 7 else "auto"
    argv = [DRIVER, "--size", str(SIZE), "--mode", mode if mode != "horizontal" else "horizontal",
            "--orient", orient, "--align", align, "--bidi", direction,
            "--justify", justify, FONT]
    if len(case) > 8:
        argv[-1:-1] = list(case[8]["driver"])
    width = "u" if room is None else str(room * 64)
    request = "%s\t%s\n" % (width, text.encode("utf-8").hex())
    finished = subprocess.run(argv, input=request, capture_output=True, text=True)
    answer = json.loads(finished.stdout.splitlines()[0])
    if "error" in answer:
        raise RuntimeError("%s: %s" % (name, answer["error"]))
    if len(case) <= 8:
        layout_snapshot.record("css", name, answer)
    return answer


def compare(case, theirs, mine, verbose):
    """The differences for one case: lines of text, empty when the case agrees."""
    name, text = case[0], case[1]
    tolerance = TOLERANCE_JUSTIFIED if case[5] == "justify" else TOLERANCE
    problems = []
    chars = list(text)
    if len(case) > 8 and "overflowWrap" in case[8]:
        css_min = theirs.get("minContent")
        ours_min = mine["min"] / 64.0
        if css_min is None or abs(css_min - ours_min) > TOLERANCE:
            problems.append("min-content width: chromium %s, this %.2f"
                            % (css_min, ours_min))
    if len(case) > 8 and case[8].get("wordBreak") == "break-all":
        css_min = theirs.get("minContent")
        ours_min = mine["min"] / 64.0
        if css_min is None or abs(css_min - ours_min) > TOLERANCE:
            problems.append("min-content width: chromium %s, this %.2f"
                            % (css_min, ours_min))
    if len(case) > 8 and case[8].get("wordBreak") == "break-word":
        css_min = theirs.get("minContent")
        ours_min = mine["min"] / 64.0
        if css_min is None or abs(css_min - ours_min) > TOLERANCE:
            problems.append("min-content width: chromium %s, this %.2f"
                            % (css_min, ours_min))
    if len(case) > 8 and case[8].get("lineBreak") == "anywhere":
        css_min = theirs.get("minContent")
        ours_min = mine["min"] / 64.0
        if css_min is None or abs(css_min - ours_min) > TOLERANCE:
            problems.append("min-content width: chromium %s, this %.2f"
                            % (css_min, ours_min))
    if len(case) > 8 and case[8].get("whiteSpace") == "pre":
        css_min = theirs.get("minContent")
        ours_min = mine["min"] / 64.0
        if css_min is None or abs(css_min - ours_min) > TOLERANCE:
            problems.append("min-content width: chromium %s, this %.2f"
                            % (css_min, ours_min))
    if len(case) > 8 and case[8].get("lineStructureOnly"):
        css_lines, ours_lines = {}, {}
        for i, ch in enumerate(chars):
            a, b = theirs["chars"][i], mine["chars"][i]
            if a is None or b is None:
                continue
            css_cross, ours_cross = round(a[1], 3), b[1]
            css_lines.setdefault(css_cross, len(css_lines))
            ours_lines.setdefault(ours_cross, len(ours_lines))
            if css_lines[css_cross] != ours_lines[ours_cross]:
                problems.append("char %d %r is on a different line" % (i, ch))
        if verbose and not problems:
            print("  %s: agrees" % name)
        return problems
    word_spacing_case = len(case) > 8 and "wordSpacing" in case[8]
    if word_spacing_case:
        vertical = case[2] != "horizontal"
        css_lines, ours_lines = {}, {}
        for i, ch in enumerate(chars):
            a, b = theirs["chars"][i], [v / 64.0 for v in mine["chars"][i]]
            if a is None:
                continue
            css_cross = round(a[0 if vertical else 1], 3)
            ours_cross = round(b[0 if vertical else 1], 3)
            css_lines.setdefault(css_cross, len(css_lines))
            ours_lines.setdefault(ours_cross, len(ours_lines))
            if css_lines[css_cross] != ours_lines[ours_cross]:
                problems.append("char %d %r is on a different line" % (i, ch))
                continue
            inline_css = (a[1], a[3]) if vertical else (a[0], a[2])
            inline_ours = (b[1], b[3]) if vertical else (b[0], b[2])
            if not all(abs(x - y) <= tolerance for x, y in zip(inline_css, inline_ours)):
                problems.append("char %d %r inline geometry: chromium %s, this %s"
                                % (i, ch, [round(v, 2) for v in inline_css],
                                   [round(v, 2) for v in inline_ours]))
        if verbose and not problems:
            print("  %s: agrees" % name)
        return problems
    mixed_span_case = len(case) > 8 and "secondSpan" in case[8]
    if mixed_span_case:
        # Range rectangles for inline text use each run's own font box, not the paragraph's
        # union line box. Reconstruct that box from the placed baseline, advance and the oracle
        # font's OS/2 typo ascent (880/1000 em, rounded to Chromium's whole-pixel metrics).
        glyph_rects = {}
        second = case[8]["secondSpan"]
        for line in mine["lines"]:
            for run in line["runs"]:
                size = second["size"] if run["span"] == 1 else SIZE
                ascent = math.floor(size * 0.88 + 0.5)
                for glyph, baseline in zip(run["g"], run["py"]):
                    glyph_rects[glyph[1]] = [glyph[4] / 64.0, baseline / 64.0 - ascent,
                                             glyph[3] / 64.0, float(size)]
        for i, ch in enumerate(chars):
            a = theirs["chars"][i]
            b = glyph_rects.get(i)
            if a is None or b is None:
                problems.append("char %d %r has no corresponding rectangle" % (i, ch))
                continue
            if not all(abs(x - y) <= tolerance for x, y in zip(a[:4], b)):
                problems.append("char %d %r: chromium %s, this %s"
                                % (i, ch, [round(v, 2) for v in a[:4]],
                                   [round(v, 2) for v in b]))
        if verbose and not problems:
            print("  %s: agrees" % name)
        return problems
    line_height_case = len(case) > 8 and "lineHeight" in case[8]
    glyph_y = {}
    if line_height_case:
        byte_at = 0
        for line in mine["lines"]:
            for run in line["runs"]:
                for glyph in run["g"]:
                    glyph_y[glyph[1]] = glyph[5] / 64.0
        for i, ch in enumerate(chars):
            if ch == "\n":
                byte_at += len(ch.encode("utf-8"))
                continue
            a = theirs["chars"][i]
            b = [v / 64.0 for v in mine["chars"][i]]
            if a is None:
                byte_at += len(ch.encode("utf-8"))
                continue
            gy = glyph_y.get(byte_at)
            if gy is None or abs(a[1] + 44.0 - gy) > tolerance:
                problems.append("char %d %r baseline: chromium %.2f, this %s"
                                % (i, ch, a[1] + 44.0, "missing" if gy is None else "%.2f" % gy))
            if abs(a[0] - b[0]) > tolerance or abs(a[2] - b[2]) > tolerance:
                problems.append("char %d %r: chromium x/width %s, this %s"
                                % (i, ch, [round(a[0], 2), round(a[2], 2)],
                                   [round(b[0], 2), round(b[2], 2)]))
            byte_at += len(ch.encode("utf-8"))
        # Chromium's Range rectangle keeps the font's ink-independent 50px character box,
        # whereas this API's selection geometry is the complete line box. Baseline and block
        # line-box height are the comparable quantities for explicit line-height.
        if verbose and not problems:
            print("  %s: agrees" % name)
        box = [mine["w"] / 64.0, mine["h"] / 64.0]
        if abs(theirs["box"][2] - box[0]) > TOLERANCE or abs(theirs["box"][3] - box[1]) > TOLERANCE:
            problems.append("box: chromium %s, this %s" % (
                [round(v, 2) for v in theirs["box"][2:]], [round(v, 2) for v in box]))
        return problems
    for i, ch in enumerate(chars):
        a = theirs["chars"][i]
        b = [v / 64.0 for v in mine["chars"][i]]
        if a is None:
            continue                       # Chromium gave this character no box
        if ch in "\n\u00ad":
            continue                       # a newline has no place in either; a soft hyphen's
                                           # rectangle holds the hyphen Chromium adds, which is no
                                           # character of the text
        if len(a) > 4 and a[4] > 1:
            continue                       # a character split over two lines: Chromium's united
                                           # rectangle is not a place on either
        if a[2] == 0 and a[3] == 0:
            continue                       # collapsed or hung to nothing
        if all(abs(x - y) <= tolerance for x, y in zip(a[:4], b)):
            continue
        problems.append("char %d %r: chromium %s, this %s"
                        % (i, ch, [round(v, 2) for v in a], [round(v, 2) for v in b]))
    box = [mine["w"] / 64.0, mine["h"] / 64.0]
    if abs(theirs["box"][2] - box[0]) > TOLERANCE or abs(theirs["box"][3] - box[1]) > TOLERANCE:
        problems.append("box: chromium %s, this %s" % (
            [round(v, 2) for v in theirs["box"][2:]], [round(v, 2) for v in box]))
    if verbose and not problems:
        print("  %s: agrees" % name)
    return problems


# ---------------------------------------------------------------------------------------- ink

INK_SIZE = 100          # pixels per em: an edge found to within a pixel is within a hundredth of an em
INK_MARGIN = 50         # white around the paragraph
INK_TOLERANCE = 1.6     # pixels: antialiasing moves a thresholded edge by up to one

# The cases are the ones where the position of the ink is the question: a turned glyph, an
# upright one centred on the axis, a vertical form, and each stacking direction.
INK_CASES = [
    ("ink-vertical-rl", "\u65e5AB\u672ca\u3001\u300c\u8a9e\u300d", "vertical-rl", "mixed", 900),
    ("ink-vertical-lr", "\u65e5AB\u672ca\u8a9e", "vertical-lr", "mixed", 330),
    ("ink-sideways-rl", "\u65e5AB\u672ca", "sideways-rl", "mixed", 900),
    ("ink-sideways-lr", "\u65e5AB\u672ca", "sideways-lr", "mixed", 900),
    ("ink-upright", "\u65e5AB\u672cab", "vertical-rl", "upright", 900),
    ("ink-sideways-text", "\u65e5AB\u672c", "vertical-rl", "sideways", 900),
    ("ink-horizontal", "AB\u65e5ab", "horizontal", "mixed", 900),
    ("ink-hyphen-h", "AB\u00adAB\u00adAB\u00adAB", "horizontal", "mixed", 300),
    ("ink-hyphen-v", "AB\u00adAB\u00adAB\u00adAB", "vertical-rl", "mixed", 300),
    ("ink-hyphen-sideways-lr", "AB\u00adAB\u00adAB\u00adAB", "sideways-lr", "mixed", 300),
    ("ink-hyphen-cjk-v", "\u65e5\u672c\u00ad\u8a9e\u65e5\u00ad\u672c\u8a9e", "vertical-rl", "mixed",
     250),
    ("ink-justify-h", "\u65e5AB\u672cab \u8a9eAB\u65e5\u672c ab\u8a9eAB", "horizontal", "mixed",
     1100, "justify"),
    ("ink-justify-v", "\u65e5AB\u672cab \u8a9eAB\u65e5\u672c ab\u8a9eAB", "vertical-rl", "mixed",
     1100, "justify"),
    ("ink-justify-sideways-lr", "\u65e5AB\u672cab \u8a9eAB\u65e5\u672c ab\u8a9eAB",
     "sideways-lr", "mixed", 1100, "justify"),
]

INK_PAGE = """<!doctype html><meta charset=utf-8>
<style>
@font-face { font-family: T; src: url("file://%(font)s"); }
html, body { margin: 0; background: #fff }
.t { font-family: T; font-size: %(size)dpx; line-height: normal; position: absolute;
     left: %(margin)dpx; top: %(margin)dpx; margin: 0; padding: 0; color: #000;
     white-space: pre-wrap; writing-mode: %(mode)s; text-orientation: %(orient)s; %(room)s;
     text-align: %(align)s }
</style>
<div class=t id=t></div>
<script>
document.fonts.load('%(size)dpx T', %(text)s).then(() => {
  document.getElementById('t').textContent = %(text)s;
});
</script>
"""


def read_png(path):
    """The dark pixels of a PNG as rows of booleans: 8 bits, RGB or RGBA, any filter."""
    with open(path, "rb") as h:
        data = h.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    idat = b""
    width = height = depth = ctype = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    assert depth == 8 and ctype in (2, 6), "unexpected PNG format %d/%d" % (depth, ctype)
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows = []
    prev = bytearray(stride)
    at = 0
    for _ in range(height):
        ft = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    dark = [[row[x * bpp] < 128 for x in range(width)] for row in rows]
    return width, height, dark


def components(dark, width, height):
    """The bounding boxes (x0, y0, x1, y1), end exclusive, of the 4-connected dark regions."""
    seen = [[False] * width for _ in range(height)]
    boxes = []
    for y in range(height):
        for x in range(width):
            if not dark[y][x] or seen[y][x]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            x0 = x1 = x
            y0 = y1 = y
            while stack:
                cx, cy = stack.pop()
                x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < width and 0 <= ny < height and dark[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            boxes.append((x0, y0, x1 + 1, y1 + 1))
    return boxes


def predicted_ink(answer, size, upem=1000):
    """Each glyph's ink box in pixels, from where this library placed it and how it sits."""
    scale = size / float(upem)
    boxes = []
    for line in answer["lines"]:
        for run in line["runs"]:
            for g, b in zip(run["g"], run["b"]):
                if b is None:
                    continue
                x, y = g[4] / 64.0, g[5] / 64.0
                x0, y0, x1, y1 = [v / 64.0 * scale for v in b]   # the box is in font units, as 26.6
                o = run["o"]
                if o in (0, 1):          # horizontal, upright: as drawn
                    boxes.append((x + x0, y - y1, x + x1, y - y0))
                elif o == 2:             # turned a quarter clockwise
                    boxes.append((x + y0, y + x0, x + y1, y + x1))
                else:                    # turned a quarter counterclockwise
                    boxes.append((x - y1, y - x1, x - y0, y - x0))
    return boxes


def ink_case(case):
    name, text, mode, orient, room = case[:5]
    align = case[5] if len(case) > 5 else "start"
    argv = [DRIVER, "--size", str(INK_SIZE), "--mode", mode, "--orient", orient,
            "--align", align, FONT]
    request = "%d\t%s\n" % (room * 64, text.encode("utf-8").hex())
    finished = subprocess.run(argv, input=request, capture_output=True, text=True)
    mine = json.loads(finished.stdout.splitlines()[0])
    layout_snapshot.record("css", name + "/ink", mine)
    width = int(math.ceil(mine["w"] / 64.0)) + 2 * INK_MARGIN
    height = int(math.ceil(mine["h"] / 64.0)) + 2 * INK_MARGIN
    side = "width" if mode == "horizontal" else "height"
    page = os.path.join(SCRATCH, "ink.html")
    shot = os.path.join(SCRATCH, "ink.png")
    if os.path.exists(shot):
        os.remove(shot)
    with open(page, "w", encoding="utf-8") as h:
        h.write(INK_PAGE % dict(font=FONT, size=INK_SIZE, margin=INK_MARGIN,
                                mode="horizontal-tb" if mode == "horizontal" else mode,
                                orient=orient, align=align, room="%s: %dpx" % (side, room),
                                text=json.dumps(text)))
    argv = oracle_env.command("chrome", [
        "chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
        "--allow-file-access-from-files", "--disable-dev-shm-usage", "--disable-lcd-text",
        "--force-device-scale-factor=1", "--force-color-profile=srgb",
        "--window-size=%d,%d" % (width, height), "--virtual-time-budget=5000",
        "--user-data-dir=" + os.path.join(SCRATCH, "profile"),
        "--screenshot=" + shot, "file://" + page], scratch=SCRATCH)
    subprocess.run(argv, capture_output=True, text=True)
    if not os.path.exists(shot):
        raise RuntimeError("%s: Chromium took no screenshot" % name)
    w, h, dark = read_png(shot)
    found = components(dark, w, h)
    want = [(a + INK_MARGIN, b + INK_MARGIN, c + INK_MARGIN, d + INK_MARGIN)
            for (a, b, c, d) in predicted_ink(mine, INK_SIZE)]
    problems = []
    if len(found) != len(want):
        problems.append("%d ink regions drawn, %d predicted" % (len(found), len(want)))
    unmatched = list(found)
    for box in want:
        best = min(unmatched, key=lambda f: sum(abs(p - q) for p, q in zip(f, box)),
                   default=None)
        if best is None or any(abs(p - q) > INK_TOLERANCE for p, q in zip(best, box)):
            problems.append("predicted ink %s, nearest drawn %s"
                            % ([round(v, 1) for v in box], best))
        else:
            unmatched.remove(best)
    return problems, len(want)


def main():
    verbose = "-v" in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith("-")]
    if "flow" in names:
        names = [name for name in names if name != "flow"] + FLOW_CASES
    cases = [c for c in CASES if not names or c[0] in names]
    measured = chrome_rects(cases)
    differ = compared = 0
    for case, theirs in zip(cases, measured):
        if case[0] in FLOW_CASES:
            flow = case[8]["flow"]
            chars = [r for r in theirs["chars"] if r is not None]
            if case[0] == "flow-circle-tapered-clear":
                bottom = flow["top"] + flow["height"]
                if any(r[1] < bottom - 1 for r in chars):
                    raise RuntimeError("unbreakable circle flow should clear to the float bottom")
            else:
                rows = {round(r[1], 2) for r in chars}
                inset_rows = [r for r in chars if r[1] < flow["height"]]
                active_side = [r for r in inset_rows
                               if (r[0] >= flow["left"] - 1 if flow["side"] == "left"
                                   else r[0] + r[2] <= 300 - flow["left"] + 1)]
                if len(rows) < 2 or not active_side:
                    raise RuntimeError("%s does not wrap into its narrowed float band" % case[0])
        mine = ours(case)
        problems = compare(case, theirs, mine, verbose)
        compared += sum(1 for c in theirs["chars"] if c is not None)
        if problems:
            differ += 1
            print("%s differs:" % case[0])
            for line in problems[:8]:
                print("   " + line)
    print("css_layout_diff: %d cases, %d characters compared, %d cases differ"
          % (len(cases), compared, differ))
    ink_cases = [c for c in INK_CASES if not names or c[0] in names]
    ink_differ = ink_glyphs = 0
    for case in ink_cases:
        problems, count = ink_case(case)
        ink_glyphs += count
        if problems:
            ink_differ += 1
            print("%s differs:" % case[0])
            for line in problems[:8]:
                print("   " + line)
        elif verbose:
            print("  %s: agrees" % case[0])
    print("css_layout_diff ink: %d cases, %d glyphs placed, %d cases differ"
          % (len(ink_cases), ink_glyphs, ink_differ))
    layout_snapshot.finish("css")
    return 1 if differ or ink_differ else 0


if __name__ == "__main__":
    sys.exit(main())
