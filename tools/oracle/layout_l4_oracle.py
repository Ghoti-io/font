#!/usr/bin/env python3
"""Compare a shaped ruby track with Chromium and Firefox."""
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
from css_layout_diff import components, read_png


ROOT = Path(oracle_env.ROOT)
FONT = ROOT / "tests/data/fonts/vertical-text.ttf"
DRIVER = ROOT / "build/linux/release/apps/examples/font-layout-annotation"
SCRATCH = ROOT / "build/oracle/layout-l4"
TEXT = "日本語"
RT = "日本"
TOLERANCE = 1.5


def driver(mode: str) -> tuple[float, ...]:
    result = subprocess.run([str(DRIVER), str(FONT), mode], text=True,
                            capture_output=True, check=True)
    box = json.loads(result.stdout)
    return tuple(value / 64 for value in box)


def page(mode: str, firefox: bool = False) -> str:
    writing_mode = "horizontal-tb" if mode == "horizontal" else mode
    vertical_mode = "true" if mode == "vertical-rl" else "false"
    ruby_mode = "vertical-rl" if mode == "vertical-rl" else "horizontal-tb"
    ruby_height = "auto" if mode == "vertical-rl" else "40px"
    color = "transparent" if firefox else "black"
    if firefox:
        measurement = '''window.addEventListener("load",()=>{
const r=document.querySelector('#ann').getBoundingClientRect();
const m=document.createElement("span");m.style.position="absolute";m.style.left=r.x+"px";
m.style.top=r.y+"px";m.style.width=r.width+"px";m.style.height=r.height+"px";
m.style.background="black";document.body.append(m);
const range=document.createRange();range.selectNodeContents(document.querySelector('#base'));
const selected=range.getClientRects();
const ann=document.querySelector('#ann').getBoundingClientRect(),base=selected[0];
const selectionExcludesAnnotation={vertical_mode}
 ? base.right<=ann.left||base.left>=ann.right
 : base.bottom<=ann.top||base.top>=ann.bottom;
const split=document.createRange();split.selectNodeContents(document.querySelector('#split-base'));
const props=!CSS.supports('ruby-merge','merge')&&!CSS.supports('ruby-position','inter-character');
const vertical=getComputedStyle(document.querySelector('#vertical')).writingMode==='vertical-rl';
const nested=document.querySelectorAll('#double rt').length===2;
const emphasis=getComputedStyle(document.querySelector('#emphasis')).textEmphasisStyle.includes('circle');
const justified=getComputedStyle(document.querySelector('#justify')).textAlign==='justify';
const aligned=getComputedStyle(document.querySelector('#align')).rubyAlign==='space-between';
if(!selected.length||!selectionExcludesAnnotation||split.getClientRects().length<2||!props||!vertical||!nested||!emphasis||!justified||!aligned)
 document.body.style.background='black';
});'''
    else:
        measurement = '''document.fonts.ready.then(()=>{
const r=document.querySelector('#ann').getBoundingClientRect();
const range=document.createRange();range.selectNodeContents(document.querySelector('#base'));
const selected=range.getClientRects();
const ann=document.querySelector('#ann').getBoundingClientRect(),base=selected[0];
const selectionExcludesAnnotation={vertical_mode}
 ? base.right<=ann.left||base.left>=ann.right
 : base.bottom<=ann.top||base.top>=ann.bottom;
const split=document.createRange();split.selectNodeContents(document.querySelector('#split-base'));
const props=!CSS.supports('ruby-merge','merge')&&!CSS.supports('ruby-position','inter-character');
const vertical=getComputedStyle(document.querySelector('#vertical')).writingMode==='vertical-rl';
const nested=document.querySelectorAll('#double rt').length===2;
const emphasis=getComputedStyle(document.querySelector('#emphasis')).textEmphasisStyle.includes('circle');
const justified=getComputedStyle(document.querySelector('#justify')).textAlign==='justify';
const aligned=getComputedStyle(document.querySelector('#align')).rubyAlign==='space-between';
document.querySelector('#out').textContent=JSON.stringify([r.x,r.y,r.width,r.height,
 selected.length,selectionExcludesAnnotation,split.getClientRects().length,props,vertical,nested,emphasis,justified,aligned]);
});'''
    measurement = measurement.replace("{vertical_mode}", vertical_mode)
    return f'''<!doctype html><meta charset="utf-8"><style>
@font-face {{ font-family:T; src:url("file://{FONT}"); }}
html,body {{ margin:0; background:white; }}
#p {{ position:absolute; left:0; top:0; margin:0; padding:0; white-space:pre;
font:100px T; line-height:normal; writing-mode:{writing_mode};
text-orientation:mixed; color:{color}; ruby-position:over; ruby-align:center; }}
rt {{ font:40px T; line-height:40px; writing-mode:{ruby_mode}; text-orientation:upright;
color:{'white' if firefox else 'black'}; }}
#ann {{ display:inline-block; height:{ruby_height}; line-height:40px; writing-mode:{ruby_mode};
text-orientation:upright; background:transparent; }}
#probe {{ position:absolute; left:-10000px; top:0; width:40px; font:100px T; white-space:normal; }}
#vertical {{ position:absolute; left:-20000px; top:0; writing-mode:vertical-rl; }}
#contract {{ position:absolute; left:-30000px; top:0; }}
#emphasis {{ text-emphasis:filled circle; }}
#justify {{ width:120px; text-align:justify; text-justify:inter-character; }}
#align {{ ruby-align:space-between; }}
{'#out { display:none; }' if firefox else ''}
</style><div id="p"><ruby><span id="base">{TEXT}</span><rt><span id="ann">{RT}</span></rt></ruby></div>
<div id="probe"><ruby><span id="split-base">日本<br>語文</span><rt>にほんごぶん</rt></ruby></div>
<div id="vertical">縦書き<ruby>日本<rt>にほん</rt></ruby></div>
<div id="contract"><ruby id="double"><ruby>日<rt>に</rt></ruby><rt style="ruby-position:under">Japan</rt></ruby>
<span id="emphasis">日本</span><span id="justify">日本語日本語</span><ruby id="align">日本<rt>にほん</rt></ruby></div>
<pre id="out"></pre><script>{measurement}</script>'''


def chromium(mode: str) -> tuple[float, ...]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"chrome-{mode}.html"
    path.write_text(page(mode))
    argv = oracle_env.command("chrome", ["chromium", "--headless=new", "--no-sandbox",
        "--disable-gpu", "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--user-data-dir=" + str(SCRATCH / f"chrome-profile-{mode}"),
        "--virtual-time-budget=5000", "--dump-dom", "file://" + str(path)], scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True, check=True)
    match = re.search(r'<pre id="out">(.*?)</pre>', result.stdout, re.S)
    if not match:
        raise RuntimeError("Chromium returned no ruby geometry")
    result = tuple(json.loads(html.unescape(match.group(1))))
    if len(result) != 13 or result[4] < 1 or not result[5] or result[6] < 2 or not all(result[7:]):
        raise RuntimeError(f"Chromium failed selection, line-fragment, or CSS support assertions: {result}")
    return result[:4]


def firefox(mode: str) -> tuple[float, ...]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"firefox-{mode}.html"
    shot = SCRATCH / f"firefox-{mode}.png"
    path.write_text(page(mode, firefox=True))
    argv = oracle_env.command("firefox", ["firefox-esr", "--headless", "--no-remote",
        "--window-size=1000,600", "--screenshot", str(shot), "file://" + str(path)],
        scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode or not shot.exists():
        raise RuntimeError("Firefox screenshot failed: " + result.stderr[-500:])
    width, height, dark = read_png(shot)
    boxes = [box for box in components(dark, width, height)
             if box[2] - box[0] > 2 and box[3] - box[1] > 2]
    if len(boxes) != 1:
        raise RuntimeError(f"Firefox returned {len(boxes)} ruby rectangles, expected one")
    x0, y0, x1, y1 = boxes[0]
    return float(x0), float(y0), float(x1 - x0), float(y1 - y0)


def compare(mode: str, engine: str, expected: tuple[float, ...], actual: tuple[float, ...]) -> None:
    # Compare the inline placement and advance in both engines. Firefox's normal ruby
    # cross-axis box uses the glyph ink height (31px here), while Chromium exposes the
    # full 40px line box; the library deliberately reports font metrics in that axis.
    indices = (1, 3) if mode == "vertical-rl" else (0, 2)
    deltas = [abs(expected[i] - actual[i]) for i in indices]
    if max(deltas) > TOLERANCE:
        raise RuntimeError(f"{mode} ruby rectangle differs from {engine}: "
                           f"layout={expected}, browser={actual}, delta={deltas}")


def main() -> int:
    for mode in ("horizontal", "vertical-rl"):
        expected = driver(mode)
        compare(mode, "Chromium", expected, chromium(mode))
        compare(mode, "Firefox", expected, firefox(mode))
    print("layout_l4_oracle: ruby inline placement and advance agree with Chromium and Firefox")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"layout_l4_oracle: {error}", file=sys.stderr)
        raise SystemExit(1)
