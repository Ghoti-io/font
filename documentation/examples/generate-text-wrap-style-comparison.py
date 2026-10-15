#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Generate the side-by-side wrap-style sample from the Ghoti Font CLI output."""

import html
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "build/linux/release/apps/examples/font-layout"
FONT = ROOT / "tests/data/fonts/vertical-text.ttf"
OUTPUT = Path(__file__).with_name("text-wrap-style-comparison.html")
TEXT = "AB AB AB AB"
WIDTH = 260
SIZE = 50


def rendered_lines(style):
    request = "%d\t%s\n" % (WIDTH * 64, TEXT.encode("utf-8").hex())
    result = subprocess.run(
        [str(DRIVER), "--size", str(SIZE), "--text-wrap-style", style, str(FONT)],
        input=request, text=True, capture_output=True, check=True)
    layout = json.loads(result.stdout.splitlines()[0])
    return [TEXT[line["s"]:line["e"]] for line in layout["lines"]]


def panel(style, caption, class_name):
    lines = "\n".join("<span class=\"line\">%s</span>" % html.escape(line)
                      for line in rendered_lines(style))
    return """    <article>
      <h2>%s</h2>
      <p class="meta">%s</p>
      <div class="sample %s">%s</div>
    </article>""" % (style, caption, class_name, lines)


page = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ghoti Font: auto and pretty wrapping</title>
<style>
@font-face {
  font-family: GhotiFixture;
  src: url("../../tests/data/fonts/vertical-text.ttf") format("truetype");
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 36px; color: #17212b; background: #f3f6f8;
  font: 16px/1.5 system-ui, sans-serif;
}
main { max-width: 980px; margin: 0 auto; }
h1 { margin: 0 0 8px; font-size: 28px; }
.intro { margin: 0 0 28px; color: #52616d; }
.grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 18px; }
article {
  min-height: 270px; padding: 22px; border: 1px solid #d7e0e6;
  border-radius: 12px; background: white; box-shadow: 0 5px 18px #2437460d;
}
h2 { margin: 0 0 8px; font-size: 18px; }
.meta { margin: 0 0 24px; color: #667784; font-size: 13px; }
.sample {
  box-sizing: content-box; width: 260px; min-height: 112px; padding: 14px;
  border-left: 3px solid #4785a6; background: #f6fafc;
  font: 50px/1.25 GhotiFixture, monospace;
}
.line { display: block; white-space: pre; }
.pretty { border-left-color: #7d65b4; }
.note { margin: 22px 0 0; color: #52616d; font-size: 13px; }
@media (max-width: 680px) {
  body { padding: 20px; }
  .grid { grid-template-columns: 1fr; }
}
</style>
<main>
  <h1>One paragraph, two wrap styles</h1>
  <p class="intro">Ghoti Font renderer output · %d px text · %d px line measure</p>
  <div class="grid">
%s
%s
  </div>
  <p class="note">Generated from the current font-layout executable. The fixture font draws the glyphs; the explicit line breaks come from each Ghoti Font layout result.</p>
</main>
</html>
""" % (SIZE, WIDTH, panel("auto", "Greedy breaks as each line fills", "auto"),
       panel("pretty", "Ghoti's short-group balancing policy", "pretty"))

OUTPUT.write_text(page, encoding="utf-8")
print("Wrote %s from %s" % (OUTPUT, DRIVER))
