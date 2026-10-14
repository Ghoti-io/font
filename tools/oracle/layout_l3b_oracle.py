#!/usr/bin/env python3
"""Compare balanced line fragments with Chromium multi-column layout."""
from __future__ import annotations

import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env


ROOT = Path(oracle_env.ROOT)
FONT = ROOT / "tests/data/fonts/vertical-text.ttf"
DRIVER = ROOT / "build/linux/release/apps/examples/font-layout-columns"
SCRATCH = ROOT / "build/oracle/layout-l3b"


def driver(mode: str) -> dict:
    argv = [str(DRIVER), str(FONT)] + ([mode] if mode in
        ("auto", "conflict", "rtl", "vertical-rl", "vertical-lr", "balance-varied",
         "balance-minimax") else [])
    result = subprocess.run(argv, text=True, capture_output=True, check=True)
    return json.loads(result.stdout)


def chromium(mode: str) -> list[list[float]]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"chromium-columns-{mode}.html"
    labels = "<span>一</span><br><span>二</span><br><span>三</span><br><span>四</span>"
    if mode == "balance-varied":
        labels = ("<span style='font-size:10px'>一</span><br>"
                  "<span style='font-size:30px'>二</span><br>"
                  "<span style='font-size:10px'>三</span><br>"
                  "<span style='font-size:30px'>四</span>")
    if mode == "balance-minimax":
        labels = ("<div style='height:40px'>一</div>"
                  "<div style='height:10px'>二</div>"
                  "<div style='height:10px'>三</div>"
                  "<div style='height:10px'>四</div>")
    if mode == "auto":
        labels += "<br><span>五</span>"
    height = 82 if mode in ("vertical-rl", "vertical-lr") else (
        100 if mode in ("balance-varied", "balance-minimax") else
        61 if mode in ("auto", "conflict") else 50)
    fill = "auto" if mode in ("auto", "conflict") else "balance"
    constraints = "orphans:3;widows:2;" if mode == "conflict" else ""
    item_selector = "#columns > *" if mode == "balance-minimax" else "#columns span"
    page = f'''<!doctype html><meta charset="utf-8"><style>
@font-face {{ font-family:T; src:url("file://{FONT}"); }}
html,body {{ margin:0; background:white; }}
#columns {{ position:absolute; left:0; top:0;
width:{'41px' if mode in ('vertical-rl', 'vertical-lr') else '160px'};
height:{height}px; writing-mode:{mode if mode in ('vertical-rl', 'vertical-lr') else 'horizontal-tb'};
column-count:2; column-gap:0; column-fill:{fill}; font:20px T; line-height:20px;
direction:{'rtl' if mode == 'rtl' else 'ltr'}; {constraints} }}
span {{ break-inside:avoid; }}
</style><div id="columns">{labels}</div>
<pre id="out"></pre><script>document.fonts.ready.then(()=>{{
const lines=[...document.querySelectorAll('{item_selector}')].map(e=>{{
const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height];
}});document.querySelector('#out').textContent=JSON.stringify(lines);
}});</script>'''
    path.write_text(page)
    argv = oracle_env.command("chrome", ["chromium", "--headless=new", "--no-sandbox",
        "--disable-gpu", "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--user-data-dir=" + str(SCRATCH / "chrome-profile"), "--virtual-time-budget=5000",
        "--dump-dom", "file://" + str(path)], scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True, check=True)
    match = re.search(r'<pre id="out">(.*?)</pre>', result.stdout, re.S)
    if not match:
        raise RuntimeError("Chromium returned no column geometry")
    return json.loads(html.unescape(match.group(1)))


def main() -> int:
    partitions = {"balance": ([0, 2, 4], 4), "balance-varied": ([0, 2, 4], 4),
                  "balance-minimax": ([0, 1, 4], 4),
                  "auto": ([0, 3, 5], 5),
                  "rtl": ([0, 2, 4], 4),
                  "conflict": ([0, 3, 4], 4)}
    for mode in ("vertical-rl", "vertical-lr"):
        expected = driver(mode)
        actual = chromium(mode)
        if (not expected["complete"] or expected["columns"] != [0, 2, 4]
                or len(expected["lines"]) != 4 or len(actual) != 4):
            raise RuntimeError(f"unexpected {mode} grouping: layout={expected}, browser={actual}")
        for index, (layout_box, browser_box) in enumerate(zip(expected["lines"], actual)):
            lx, ly = layout_box[:2]
            bx, by = browser_box[:2]
            if (abs(lx / 64 - bx) > 1.5 or abs(ly / 64 - by) > 1.5
                    or abs(layout_box[4] / 64 - browser_box[2]) > 1.5):
                raise RuntimeError(
                    f"{mode} line {index} differs: layout={layout_box}, browser={browser_box}")
    for mode, (starts, line_count) in partitions.items():
        expected = driver(mode)
        actual = chromium(mode)
        if (not expected["complete"] or expected["columns"] != starts
                or len(expected["lines"]) != line_count or len(actual) != line_count):
            raise RuntimeError(f"unexpected {mode} partition: layout={expected}, browser={actual}")
        browser_starts = [0]
        browser_starts.extend(index for index in range(1, len(actual))
                              if actual[index][0] != actual[index - 1][0])
        browser_starts.append(len(actual))
        if browser_starts != starts:
            raise RuntimeError(f"{mode} browser column partition differs: "
                               f"layout={starts}, browser={browser_starts}")
        if mode == "balance-varied":
            continue
        if mode == "balance-minimax":
            for index, (layout_box, browser_box) in enumerate(zip(expected["lines"], actual)):
                if (abs(layout_box[1] / 64 - browser_box[1]) > 1.5
                        or abs(layout_box[3] / 64 - browser_box[3]) > 1.5):
                    raise RuntimeError(f"{mode} line {index} differs: "
                                       f"layout={layout_box}, browser={browser_box}")
            browser_heights = [actual[0][3], sum(line[3] for line in actual[1:])]
            layout_heights = [sum(line[3] / 64 for line in expected["lines"][:1]),
                              sum(line[3] / 64 for line in expected["lines"][1:])]
            if any(abs(a - b) > 1.5 for a, b in zip(layout_heights, browser_heights)):
                raise RuntimeError(f"balance-minimax column extents differ: "
                                   f"layout={layout_heights}, browser={browser_heights}")
            continue
        for index, (layout_box, browser_box) in enumerate(zip(expected["lines"], actual)):
            lx, ly = layout_box[:2]
            bx, by = browser_box[:2]
            extent, line_height = layout_box[4] / 64, layout_box[3] / 64
            column_x_matches = (abs(lx / 64 - bx) <= 1.5 if mode != "rtl"
                                else (lx > 0) == (bx > 80))
            if (not column_x_matches or abs(ly / 64 - by) > 1.5
                    or abs(extent - browser_box[2]) > 1.5
                    or abs(line_height - browser_box[3]) > 1.5):
                raise RuntimeError(
                    f"{mode} line {index} differs: layout={layout_box}, browser={browser_box}")
    print("layout_l3b_oracle: balanced and sequential column line positions agree with Chromium")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"layout_l3b_oracle: {error}", file=sys.stderr)
        raise SystemExit(1)
