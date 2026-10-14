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
EMPHASIS_STYLES = {
    "emphasis": "filled circle",
    # Omitting the shape asks CSS to choose its writing-mode default.
    "emphasis-default": "filled",
    "emphasis-dot": "filled dot",
    "emphasis-open-circle": "open circle",
    "emphasis-double-circle": "filled double-circle",
    "emphasis-triangle": "filled triangle",
    "emphasis-open-sesame": "open sesame",
    "emphasis-string": '"x"',
}


def driver(mode: str, scenario: str = "simple") -> tuple[float, ...] | dict:
    argv = [str(DRIVER), str(FONT), mode]
    if scenario != "simple":
        argv.append(scenario)
    result = subprocess.run(argv, text=True,
                            capture_output=True, check=True)
    data = json.loads(result.stdout)
    if scenario != "simple":
        return data
    return tuple(value / 64 for value in data)


def page(mode: str, firefox: bool = False) -> str:
    writing_mode = "horizontal-tb" if mode == "horizontal" else mode
    is_vertical = mode in ("vertical-rl", "vertical-lr")
    vertical_mode = "true" if is_vertical else "false"
    ruby_mode = mode if is_vertical else "horizontal-tb"
    ruby_height = "auto" if is_vertical else "40px"
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
 ? ann.left>=base.right
 : ann.bottom<=base.top;
const split=document.createRange();split.selectNodeContents(document.querySelector('#split-base'));
const props=!CSS.supports('ruby-merge','merge')&&!CSS.supports('ruby-position','inter-character');
const vertical=getComputedStyle(document.querySelector('#vertical')).writingMode==='vertical-rl';
const nested=document.querySelectorAll('#double > ruby > rt').length===1;
const doubleRects=[...document.querySelectorAll('#double rt')].map(x=>x.getBoundingClientRect());
const doubleTracks=doubleRects.length===2&&doubleRects.every(x=>x.width>0&&x.height>0)
 && (doubleRects[0].bottom<=doubleRects[1].top||doubleRects[1].bottom<=doubleRects[0].top);
const fragmentText=[...document.querySelectorAll('#fragments rt')].map(x=>x.textContent).join('');
const fragmentRects=[...document.querySelectorAll('#fragments ruby')].map(x=>x.getBoundingClientRect());
const fragments=fragmentText==='にほんごぶん'&&fragmentRects.length===2
 && fragmentRects.every(x=>x.width>0&&x.height>0)&&fragmentRects[0].y!==fragmentRects[1].y;
const emphasis=getComputedStyle(document.querySelector('#emphasis')).textEmphasisStyle.includes('circle');
const justified=getComputedStyle(document.querySelector('#justify')).textAlign==='justify';
const aligned=getComputedStyle(document.querySelector('#align')).rubyAlign==='space-between';
if(!selected.length||!selectionExcludesAnnotation||split.getClientRects().length<2||!props||!vertical||!nested||!doubleTracks||!fragments||!emphasis||!justified||!aligned)
 document.body.style.background='black';
});'''
    else:
        measurement = '''document.fonts.ready.then(()=>{
const r=document.querySelector('#ann').getBoundingClientRect();
const range=document.createRange();range.selectNodeContents(document.querySelector('#base'));
const selected=range.getClientRects();
const ann=document.querySelector('#ann').getBoundingClientRect(),base=selected[0];
const selectionExcludesAnnotation={vertical_mode}
 ? ann.left>=base.right
 : ann.bottom<=base.top;
const split=document.createRange();split.selectNodeContents(document.querySelector('#split-base'));
const props=!CSS.supports('ruby-merge','merge')&&!CSS.supports('ruby-position','inter-character');
const vertical=getComputedStyle(document.querySelector('#vertical')).writingMode==='vertical-rl';
const nested=document.querySelectorAll('#double > ruby > rt').length===1;
const doubleRects=[...document.querySelectorAll('#double rt')].map(x=>x.getBoundingClientRect());
const doubleTracks=doubleRects.length===2&&doubleRects.every(x=>x.width>0&&x.height>0)
 && (doubleRects[0].bottom<=doubleRects[1].top||doubleRects[1].bottom<=doubleRects[0].top);
const fragmentText=[...document.querySelectorAll('#fragments rt')].map(x=>x.textContent).join('');
const fragmentRects=[...document.querySelectorAll('#fragments ruby')].map(x=>x.getBoundingClientRect());
const fragments=fragmentText==='にほんごぶん'&&fragmentRects.length===2
 && fragmentRects.every(x=>x.width>0&&x.height>0)&&fragmentRects[0].y!==fragmentRects[1].y;
const emphasis=getComputedStyle(document.querySelector('#emphasis')).textEmphasisStyle.includes('circle');
const justified=getComputedStyle(document.querySelector('#justify')).textAlign==='justify';
const aligned=getComputedStyle(document.querySelector('#align')).rubyAlign==='space-between';
document.querySelector('#out').textContent=JSON.stringify([r.x,r.y,r.width,r.height,
 base.x,base.y,base.width,base.height,selected.length,selectionExcludesAnnotation,
 split.getClientRects().length,props,vertical,nested,doubleTracks,fragments,emphasis,justified,aligned]);
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
#contract {{ position:absolute; left:-30000px; top:0; font:40px T; line-height:1; }}
#emphasis {{ text-emphasis:filled circle; }}
#justify {{ width:120px; text-align:justify; text-justify:inter-character; }}
#align {{ ruby-align:space-between; }}
{'#out { display:none; }' if firefox else ''}
</style><div id="p"><ruby><span id="base">{TEXT}</span><rt><span id="ann">{RT}</span></rt></ruby></div>
<div id="probe"><ruby><span id="split-base">日本<br>語文</span><rt>にほんごぶん</rt></ruby></div>
<div id="vertical">縦書き<ruby>日本<rt>にほん</rt></ruby></div>
<div id="contract"><ruby id="double"><ruby>日<rt>に</rt></ruby><rt>Japan</rt></ruby>
<span id="emphasis">日本</span><span id="justify">日本語日本語</span><ruby id="align">日本<rt>にほん</rt></ruby>
<div id="fragments"><ruby>日本<rt>にほん</rt></ruby><br><ruby>語文<rt>ごぶん</rt></ruby></div></div>
<pre id="out"></pre><script>{measurement}</script>'''


def scenario_page(scenario: str, mode: str = "horizontal", firefox: bool = False) -> str:
    if scenario == "double-side":
        markup = '''<ruby id="double"><ruby><span id="base">日</span><rt id="over">に</rt></ruby><rt id="under">Japan</rt></ruby>'''
        script = '''document.fonts.ready.then(()=>{
const box=e=>{const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height]};
document.querySelector('#out').textContent=JSON.stringify({base:box(document.querySelector('#base')),
over:box(document.querySelector('#over')),under:box(document.querySelector('#under'))});});'''
        width = ""
    elif scenario == "nested":
        markup = '''<ruby id="outer"><span id="base">日本語</span><rt><ruby id="inner">にほんご<rt id="child">ニホン</rt></ruby></rt></ruby>'''
        script = '''document.fonts.ready.then(()=>{
const box=e=>{const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height]};
document.querySelector('#out').textContent=JSON.stringify({base:box(document.querySelector('#base')),
parent:box(document.querySelector('#outer > rt')),child:box(document.querySelector('#child'))});});'''
        width = ""
    elif scenario == "fragments":
        markup = '''<ruby id="fragment"><span id="base">日本語日本語</span><rt id="reading">にほんごにほんご</rt></ruby>'''
        script = '''document.fonts.ready.then(()=>{
const chars=id=>{const e=document.querySelector(id),n=e.firstChild,out=[];
for(let i=0;i<n.length;i++){const r=document.createRange();r.setStart(n,i);r.setEnd(n,i+1);
const b=r.getBoundingClientRect();out.push({t:n.data[i],x:b.x,y:b.y,w:b.width,h:b.height});}return out;};
document.querySelector('#out').textContent=JSON.stringify({base:chars('#base'),reading:chars('#reading')});});'''
        width = "width:300px;"
    elif scenario in ("overhang", "overhang-none", "overhang-wrap", "overhang-wrap-none"):
        markup = '''<ruby id="ruby"><span id="base">日</span><rt id="rt">日本語</rt></ruby><span id="next">本語</span>'''
        script = '''document.fonts.ready.then(()=>{
const box=e=>{const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height]};
const n=document.querySelector('#next').firstChild, q=document.createRange();
q.setStart(n,0);q.setEnd(n,1);const nr=q.getBoundingClientRect();
document.querySelector('#out').textContent=JSON.stringify({base:box(document.querySelector('#base')),
rt:box(document.querySelector('#rt')),next:box(document.querySelector('#next')),
nextFirst:[nr.x,nr.y,nr.width,nr.height],
supported:CSS.supports('ruby-overhang','auto')});});'''
        overhang_rule = "#ruby { ruby-overhang:auto; }" if scenario in ("overhang", "overhang-wrap") else "#ruby { ruby-overhang:none; }"
        width = "width:210px; white-space:normal;" if scenario.startswith("overhang-wrap") else ""
    elif scenario in EMPHASIS_STYLES:
        markup = '''<span id="base">日本語</span>'''
        script = '''document.fonts.ready.then(()=>{
const r=document.querySelector('#base').getBoundingClientRect();
document.querySelector('#out').textContent=JSON.stringify([r.x,r.y,r.width,r.height]);});'''
        width = ""
    else:
        raise ValueError(scenario)
    writing_mode = "horizontal-tb" if mode == "horizontal" else mode
    annotation_writing_mode = "horizontal-tb" if mode == "horizontal" else mode
    return f'''<!doctype html><meta charset="utf-8"><style>
@font-face {{ font-family:T; src:url("file://{FONT}"); }} html,body {{ margin:0; }}
#fixture {{ position:absolute; left:0; top:0; {width} font:100px T; line-height:normal; ruby-align:center;
writing-mode:{writing_mode}; text-orientation:mixed; }}
#double {{ ruby-position:under; }} #double > ruby {{ ruby-position:over; }}
rt {{ writing-mode:{annotation_writing_mode}; text-orientation:mixed; }}
{'#fixture { color:transparent; } rt { color:white; }' if firefox else ''}
{overhang_rule if scenario in ('overhang','overhang-none','overhang-wrap','overhang-wrap-none') else ''}
{('#base { color:white; text-emphasis:' + EMPHASIS_STYLES[scenario] + '; text-emphasis-position:over right; text-emphasis-color:black; }' if scenario in EMPHASIS_STYLES else '')}
rt {{ display:ruby-text; font:40px T; line-height:40px; }} rtc {{ display:ruby-text-container; }} #out {{ display:none; }}
</style><div id="fixture">{markup}</div><pre id="out"></pre><script>{script}</script>'''


def chromium_scenario(scenario: str, mode: str = "horizontal") -> dict:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"chrome-{scenario}-{mode}-scenario.html"
    path.write_text(scenario_page(scenario, mode))
    argv = oracle_env.command("chrome", ["chromium", "--headless=new", "--no-sandbox",
        "--disable-gpu", "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--user-data-dir=" + str(SCRATCH / f"chrome-scenario-{scenario}-{mode}"),
        "--virtual-time-budget=5000", "--dump-dom", "file://" + str(path)], scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True, check=True)
    match = re.search(r'<pre id="out">(.*?)</pre>', result.stdout, re.S)
    if not match:
        raise RuntimeError(f"Chromium returned no {scenario} geometry")
    return json.loads(html.unescape(match.group(1)))


def chromium_emphasis_boxes(mode: str = "horizontal", scenario: str = "emphasis") -> list[tuple[int, int, int, int]]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"chrome-emphasis-ink-{scenario}-{mode}.html"
    shot = SCRATCH / f"chrome-emphasis-ink-{scenario}-{mode}.png"
    path.write_text(scenario_page(scenario, mode))
    argv = oracle_env.command("chrome", ["chromium", "--headless=new", "--no-sandbox",
        "--disable-gpu", "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--window-size=800,400", "--screenshot=" + str(shot),
        "--user-data-dir=" + str(SCRATCH / f"chrome-emphasis-profile-{scenario}-{mode}"),
        "--virtual-time-budget=5000", "file://" + str(path)], scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode or not shot.exists():
        raise RuntimeError("Chromium emphasis screenshot failed: " + result.stderr[-500:])
    width, height, dark = read_png(shot)
    return [box for box in components(dark, width, height)
            if box[2] - box[0] > 1 and box[3] - box[1] > 1]


def firefox_emphasis_boxes(mode: str, scenario: str) -> list[tuple[int, int, int, int]]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / f"firefox-emphasis-ink-{scenario}-{mode}.html"
    shot = SCRATCH / f"firefox-emphasis-ink-{scenario}-{mode}.png"
    page_source = scenario_page(scenario, mode, firefox=True)
    css_style = json.dumps(EMPHASIS_STYLES[scenario])
    support_probe = f'''<style>#oracle-support {{ position:fixed; right:4px; bottom:4px;
width:8px; height:8px; background:white; }}</style><span id="oracle-support"></span>
<script>if(CSS.supports('text-emphasis-style',{css_style}))
document.querySelector('#oracle-support').style.background='black';</script>'''
    marker = '<pre id="out"></pre><script>'
    if marker not in page_source:
        raise RuntimeError("Firefox emphasis fixture support-probe marker was not found")
    path.write_text(page_source.replace(marker, support_probe + marker))
    argv = oracle_env.command("firefox", ["firefox-esr", "--headless", "--no-remote",
        "--window-size=1000,600", "--screenshot", str(shot), "file://" + str(path)],
        scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode or not shot.exists():
        raise RuntimeError(f"Firefox {mode} {scenario} emphasis screenshot failed: "
                           + result.stderr[-500:])
    width, height, dark = read_png(shot)
    support_region = [dark[y][x]
                      for y in range(max(0, height - 16), height - 2)
                      for x in range(max(0, width - 16), width - 2)]
    if not any(support_region):
        raise RuntimeError(f"Firefox does not support text-emphasis-style: "
                           f"{EMPHASIS_STYLES[scenario]!r} ({mode}, {scenario})")
    return [box for box in components(dark, width, height)
            if box[0] < width - 20 and box[1] < height - 20
            if box[2] - box[0] > 1 and box[3] - box[1] > 1]


def group_inline_ink(boxes: list[tuple[int, int, int, int]], axis: int,
                     count: int = 3) -> list[tuple[int, int, int, int]]:
    """Merge disconnected pixels for each mark, split at the largest inter-mark gaps."""
    ordered = sorted(boxes, key=lambda box: (box[axis] + box[axis + 2]) / 2)
    if len(ordered) < count:
        return []
    gaps = [((ordered[i + 1][axis] + ordered[i + 1][axis + 2]) / 2
             - (ordered[i][axis] + ordered[i][axis + 2]) / 2, i)
            for i in range(len(ordered) - 1)]
    cuts = sorted(index for _, index in sorted(gaps, reverse=True)[:count - 1])
    groups = []
    first = 0
    for cut in cuts:
        groups.append(ordered[first:cut + 1])
        first = cut + 1
    groups.append(ordered[first:])
    if len(groups) != count or any(not group for group in groups):
        return []
    return [(min(box[0] for box in group), min(box[1] for box in group),
             max(box[2] for box in group), max(box[3] for box in group))
            for group in groups]


def firefox_double_side_boxes(mode: str, track: str | None = None) -> list[tuple[int, int, int, int]]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    suffix = "all" if track is None else track
    path = SCRATCH / f"firefox-double-side-{mode}-{suffix}.html"
    shot = SCRATCH / f"firefox-double-side-{mode}-{suffix}.png"
    page_source = scenario_page("double-side", mode, firefox=True)
    marker = '''document.fonts.ready.then(()=>{
const box=e=>{const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height]};
document.querySelector('#out').textContent=JSON.stringify({base:box(document.querySelector('#base')),
over:box(document.querySelector('#over')),under:box(document.querySelector('#under'))});});'''
    overlay = '''window.addEventListener("load",()=>{
for(const id of TRACKS){const r=document.querySelector('#'+id).getBoundingClientRect();
const e=document.createElement('span');e.style.cssText=`position:absolute;left:${r.x}px;top:${r.y}px;`+
`width:${r.width}px;height:${r.height}px;background:black;z-index:1000`;document.body.append(e);}});'''
    if marker not in page_source:
        raise RuntimeError("Firefox double-sided fixture script marker was not found")
    overlay = overlay.replace("TRACKS", json.dumps([track] if track else ["over", "under"]))
    path.write_text(page_source.replace(marker, overlay))
    argv = oracle_env.command("firefox", ["firefox-esr", "--headless", "--no-remote",
        "--window-size=1000,600", "--screenshot", str(shot), "file://" + str(path)],
        scratch=str(SCRATCH))
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode or not shot.exists():
        raise RuntimeError("Firefox double-sided ruby screenshot failed: " + result.stderr[-500:])
    width, height, dark = read_png(shot)
    return [box for box in components(dark, width, height)
            if box[2] - box[0] > 2 and box[3] - box[1] > 2]


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
    if (len(result) != 19 or result[8] != 1 or not result[9] or result[10] < 2
            or not all(result[11:])):
        raise RuntimeError(f"Chromium failed ruby track, fragment, selection, or CSS assertions: {result}")
    return result[:8]


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
    indices = (1, 3) if mode in ("vertical-rl", "vertical-lr") else (0, 2)
    deltas = [abs(expected[i] - actual[i]) for i in indices]
    if engine == "Chromium":
        # Compare the range rectangle's inline start and advance. The vertical
        # writing mode implementations use different column cross-axis origins.
        range_indices = (5, 7) if mode in ("vertical-rl", "vertical-lr") else (4, 6)
        deltas += [abs(expected[i] - actual[i]) for i in range_indices]
    if max(deltas) > TOLERANCE:
        raise RuntimeError(f"{mode} ruby rectangle differs from {engine}: "
                           f"layout={expected}, browser={actual}, delta={deltas}")


def _group_text(chars: list[dict]) -> list[tuple[str, float, float]]:
    groups: dict[float, list[dict]] = {}
    for char in chars:
        groups.setdefault(float(char["y"]), []).append(char)
    return [("".join(c["t"] for c in group), y,
             max(c["h"] for c in group)) for y, group in sorted(groups.items())]


def _group_inline_text(chars: list[dict]) -> list[tuple[str, float, float]]:
    """Return each visual line's text and the union of its DOM Range boxes."""
    groups: dict[float, list[dict]] = {}
    for char in chars:
        groups.setdefault(float(char["y"]), []).append(char)
    result = []
    for y, group in sorted(groups.items()):
        left = min(float(c["x"]) for c in group)
        right = max(float(c["x"]) + float(c["w"]) for c in group)
        result.append(("".join(c["t"] for c in group), left, right - left))
    return result


def compare_implementation_scenarios() -> None:
    # Nested CSS ruby boxes reserve the full base width in Chromium, while this API
    # reports the shaped track advance. The outer track is centered in both models,
    # so compare its center; its width is intentionally not comparable. Child tracks
    # use different inline containing boxes (expanded CSS rt vs paragraph-frame API
    # coordinates), so only their cross-axis geometry is comparable.
    nested = driver("horizontal", "nested")
    nested_browser = chromium_scenario("nested")
    try:
        outer = nested["lines"][0]["annotations"][0]
        child = outer["children"][0]
        if len(nested["lines"]) != 1 or len(outer["children"]) != 1:
            raise ValueError("unexpected nested annotation tree")
    except (KeyError, IndexError, TypeError) as error:
        raise RuntimeError(f"invalid nested ruby driver result: {nested}") from error
    for label, impl_box, browser_box in (
        ("outer ruby", outer["box"], nested_browser["parent"]),
        ("nested ruby", child["box"], nested_browser["child"]),
    ):
        impl_y, impl_height = impl_box[1] / 64, impl_box[3] / 64
        if max(abs(impl_y - browser_box[1]), abs(impl_height - browser_box[3])) > TOLERANCE:
            raise RuntimeError(f"{label} cross-axis geometry differs from Chromium: "
                               f"layout={impl_box}, browser={browser_box}")
    impl_outer = outer["box"]
    outer_center = impl_outer[0] / 64 + impl_outer[2] / 128
    browser_center = nested_browser["parent"][0] + nested_browser["parent"][2] / 2
    if abs(outer_center - browser_center) > TOLERANCE:
        raise RuntimeError(f"outer ruby inline center differs from Chromium: "
                           f"layout={outer_center}, browser={browser_center}")

    for mode in ("horizontal", "vertical-rl", "vertical-lr"):
        double_side = driver(mode, "double-side")
        double_browser = chromium_scenario("double-side", mode)
        firefox_boxes = firefox_double_side_boxes(mode)
        tracks = double_side["lines"][0]["annotations"]
        if len(tracks) != 2 or len(firefox_boxes) != 2:
            raise RuntimeError(f"expected two tracks from layout and Firefox: "
                               f"layout={double_side}, Firefox={firefox_boxes}")
        browser_boxes = [double_browser["over"], double_browser["under"]]
        if mode == "horizontal":
            firefox_boxes.sort(key=lambda b: b[1])
            if firefox_boxes[0][1] + firefox_boxes[0][3] > firefox_boxes[1][1]:
                raise RuntimeError(f"Firefox double-sided horizontal tracks overlap: "
                                   f"{firefox_boxes}")
        else:
            firefox_boxes.sort(key=lambda b: b[0])
            if firefox_boxes[0][0] + firefox_boxes[0][2] > firefox_boxes[1][0]:
                raise RuntimeError(f"Firefox double-sided {mode} tracks overlap: "
                                   f"{firefox_boxes}")
        # Firefox's screenshot exposes glyph ink bounds rather than the ruby line box,
        # so compare the two-side ordering there. Chromium's DOM geometry is the metric
        # comparison above; unit tests assert the library's box side and stack positions.
        for label, impl, browser in (
                ("over", tracks[0]["box"], browser_boxes[0]),
                ("under", tracks[1]["box"], browser_boxes[1])):
            ibox = [v / 64 for v in impl]
            # Browsers allocate each ruby track to the base width; this API returns the
            # shaped reading advance. Compare the cross-axis placement and track thickness.
            indices = (1, 3) if mode == "horizontal" else (0, 2)
            if max(abs(ibox[i] - browser[i]) for i in indices) > TOLERANCE:
                raise RuntimeError(f"double-sided {mode} {label} ruby differs from Chromium: "
                                   f"layout={ibox}, browser={browser}")

    fragments = driver("horizontal", "fragments")
    fragment_browser = chromium_scenario("fragments")
    base = "日本語日本語"
    reading = "にほんごにほんご"
    base_bytes, reading_bytes = base.encode(), reading.encode()
    impl_base: list[str] = []
    impl_reading: list[str] = []
    impl_tracks: list[tuple[float, float]] = []
    impl_track_centers: list[float] = []
    impl_base_inline: list[tuple[float, float]] = []
    cursor = 0
    for line in fragments["lines"]:
        impl_base.append(base_bytes[line["start"]:line["end"]].decode("utf-8"))
        impl_base_inline.append((line["runs"][0][2] / 64,
                                 line["extent"] / 64))
        if len(line["annotations"]) != 1:
            raise RuntimeError(f"expected one ruby fragment on each line: {fragments}")
        annotation = line["annotations"][0]
        byte_count = sum(end - start for start, end in annotation["runs"])
        impl_reading.append(reading_bytes[cursor:cursor + byte_count].decode("utf-8"))
        cursor += byte_count
        impl_tracks.append((annotation["box"][1] / 64, annotation["box"][3] / 64))
        impl_track_centers.append(annotation["box"][0] / 64 +
                                  annotation["box"][2] / 128)
    if cursor != len(reading_bytes):
        raise RuntimeError(f"implementation ruby fragments account for {cursor} of "
                           f"{len(reading_bytes)} reading bytes")
    browser_base = _group_text(fragment_browser["base"])
    browser_reading = _group_text(fragment_browser["reading"])
    browser_base_inline = _group_inline_text(fragment_browser["base"])
    browser_reading_inline = _group_inline_text(fragment_browser["reading"])
    if impl_base != [item[0] for item in browser_base]:
        raise RuntimeError(f"wrapped base fragments differ from Chromium: "
                           f"layout={impl_base}, browser={[item[0] for item in browser_base]}")
    if len(impl_base_inline) != len(browser_base_inline):
        raise RuntimeError("implementation and Chromium disagree on base line geometry")
    for index, ((ix, iw), (_, bx, bw)) in enumerate(zip(impl_base_inline,
                                                        browser_base_inline)):
        if max(abs(ix - bx), abs(iw - bw)) > TOLERANCE:
            raise RuntimeError(f"wrapped base fragment {index} inline geometry differs "
                               f"from Chromium: layout={(ix, iw)}, browser={(bx, bw)}")
    if impl_reading != [item[0] for item in browser_reading]:
        raise RuntimeError(f"wrapped ruby text differs from Chromium: "
                           f"layout={impl_reading}, browser={[item[0] for item in browser_reading]}")
    if len(impl_tracks) != len(browser_reading):
        raise RuntimeError("implementation and Chromium disagree on ruby fragment count")
    for index, ((iy, ih), (_, by, bh)) in enumerate(zip(impl_tracks, browser_reading)):
        if max(abs(iy - by), abs(ih - bh)) > TOLERANCE:
            raise RuntimeError(f"ruby fragment {index} cross-axis geometry differs from "
                               f"Chromium: layout={(iy, ih)}, browser={(by, bh)}")
    # The annotation's glyph advance is narrower than CSS's allocated ruby track.
    # Its inline center remains comparable: CSS distributes the reading over the
    # corresponding base fragment, and this API centers the shaped track on it.
    for index, (impl_center, (_, bx, bw)) in enumerate(zip(impl_track_centers,
                                                           browser_reading_inline)):
        browser_center = bx + bw / 2
        if abs(impl_center - browser_center) > TOLERANCE:
            raise RuntimeError(f"ruby fragment {index} inline center differs from "
                               f"Chromium: layout={impl_center}, "
                               f"browser={browser_center}")

    # CSS Ruby leaves the amount of allowed overhang to the UA. Compare the annotation
    # box itself and require both systems to pull following text toward it; compare the
    # direction, not the amount of the pull.
    overhang_auto = driver("horizontal", "overhang")
    overhang_none = driver("horizontal", "overhang-none")
    browser_auto = chromium_scenario("overhang")
    browser_none = chromium_scenario("overhang-none")
    if not browser_auto.get("supported"):
        raise RuntimeError("Chromium does not support ruby-overhang:auto")
    ruby_box = overhang_auto["lines"][0]["annotations"][0]["box"]
    browser_ruby = browser_auto["rt"]
    geom = [ruby_box[0] / 64, ruby_box[1] / 64, ruby_box[2] / 64, ruby_box[3] / 64]
    if max(abs(a - b) for a, b in zip(geom, browser_ruby)) > TOLERANCE:
        raise RuntimeError(f"AUTO ruby annotation box differs from Chromium: "
                           f"layout={geom}, browser={browser_ruby}")
    impl_following = overhang_none["lines"][0]["runs"][0][4][1][1] - \
        overhang_auto["lines"][0]["runs"][0][4][1][1]
    browser_following = browser_none["next"][0] - browser_auto["next"][0]
    if impl_following <= 0 or browser_following <= 0:
        raise RuntimeError("AUTO ruby overhang did not pull following text toward the ruby "
                           f"(layout shift={impl_following / 64:g}px, "
                           f"Chromium shift={browser_following:g}px)")

    # Under a constrained width, the full ruby reservation makes the first
    # following ideograph wrap. AUTO can reclaim reserved advance and keep it
    # with the base. The UA chooses the allowed overhang amount, so compare the
    # line-break decision rather than demanding identical coordinates.
    wrap_auto = driver("horizontal", "overhang-wrap")
    wrap_none = driver("horizontal", "overhang-wrap-none")
    browser_wrap_auto = chromium_scenario("overhang-wrap")
    browser_wrap_none = chromium_scenario("overhang-wrap-none")
    if wrap_auto["lines"][0]["end"] <= 3 or wrap_none["lines"][0]["end"] > 3:
        raise RuntimeError("library AUTO overhang did not change the constrained line break: "
                           f"auto={wrap_auto['lines']}, none={wrap_none['lines']}")
    if (abs(browser_wrap_auto["nextFirst"][1] - browser_wrap_auto["base"][1]) > TOLERANCE
            or abs(browser_wrap_none["nextFirst"][1] - browser_wrap_none["base"][1]) <= TOLERANCE):
        raise RuntimeError("Chromium AUTO overhang did not change the constrained line break: "
                           f"auto={browser_wrap_auto}, none={browser_wrap_none}")

    for mode in ("horizontal", "vertical-rl", "vertical-lr"):
        for scenario in EMPHASIS_STYLES:
            emphasis = driver(mode, scenario)
            impl_marks = emphasis["lines"][0]["annotations"]
            if len(impl_marks) != 3:
                raise RuntimeError(f"{mode} {scenario} library mark count differs: {len(impl_marks)}")
            impl_centers = [(a["box"][0] / 64 + a["box"][2] / 128,
                             a["box"][1] / 64 + a["box"][3] / 128)
                            for a in impl_marks]
            inline_axis = 0 if mode == "horizontal" else 1
            impl_centers.sort(key=lambda center: center[inline_axis])
            emphasis_tolerance = 12 if mode != "horizontal" else 8
            axes = (0, 1) if scenario == "emphasis" else ((0,) if mode == "horizontal" else (1,))
            for browser_name, capture in (("Chromium", chromium_emphasis_boxes),
                                          ("Firefox", firefox_emphasis_boxes)):
                browser_marks = capture(mode, scenario)
                browser_marks = group_inline_ink(browser_marks, inline_axis)
                if len(browser_marks) != 3:
                    raise RuntimeError(f"{browser_name} {mode} {scenario} emphasis ink "
                                       f"count differs: {len(browser_marks)}; expected 3 "
                                       "(unsupported CSS styles need an explicit skip)")
                browser_centers = [((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
                                   for b in browser_marks]
                browser_centers.sort(key=lambda center: center[inline_axis])
                if any(max(abs(impl[axis] - browser[axis]) for axis in axes)
                       > emphasis_tolerance
                       for impl, browser in zip(impl_centers, browser_centers)):
                    raise RuntimeError(f"{browser_name} {mode} {scenario} mark centers differ: "
                                       f"layout={impl_centers}, browser ink={browser_centers}")


def main() -> int:
    compare_implementation_scenarios()
    for mode in ("horizontal", "vertical-rl", "vertical-lr"):
        expected = driver(mode)
        compare(mode, "Chromium", expected, chromium(mode))
        compare(mode, "Firefox", expected[:4], firefox(mode))
    for mode in ("sideways-rl", "sideways-lr"):
        expected = driver(mode, "double-side")
        browser = chromium_scenario("double-side", mode)
        firefox_tracks = {}
        for side in ("over", "under"):
            boxes = firefox_double_side_boxes(mode, side)
            if len(boxes) != 1:
                raise RuntimeError(f"Firefox {mode} expected one {side} track: {boxes}")
            firefox_tracks[side] = boxes[0]
        if (firefox_tracks["over"][0] < firefox_tracks["under"][0]
                and firefox_tracks["over"][0] + firefox_tracks["over"][2] > firefox_tracks["under"][0]
                or firefox_tracks["under"][0] < firefox_tracks["over"][0]
                and firefox_tracks["under"][0] + firefox_tracks["under"][2] > firefox_tracks["over"][0]):
            raise RuntimeError(f"Firefox {mode} ruby tracks overlap: {firefox_tracks}")
        firefox_order = tuple(sorted((box[0], side) for side, box in firefox_tracks.items()))
        # The screenshot fixture overlays each named Firefox track separately, preserving
        # over/under identity while comparing physical order.
        impl_tracks = expected["lines"][0]["annotations"]
        impl_order = sorted((track["box"][0] / 64, side)
                            for track, side in zip(impl_tracks, ("over", "under")))
        if tuple(side for _, side in impl_order) != tuple(side for _, side in firefox_order):
            raise RuntimeError(f"library {mode} track order {impl_order} differs from Firefox "
                               f"physical order {firefox_order}: {firefox_tracks}")
        for index, side in enumerate(("over", "under")):
            impl_box = expected["lines"][0]["annotations"][index]["box"]
            browser_box = browser[side]
            impl_cross = (impl_box[0] / 64, impl_box[2] / 64)
            browser_cross = (browser_box[0], browser_box[2])
            if max(abs(a - b) for a, b in zip(impl_cross, browser_cross)) > TOLERANCE:
                raise RuntimeError(f"double-sided {mode} {side} placement differs: "
                                   f"layout={impl_cross}, browser={browser_cross}")
    print("layout_l4_oracle: nested and wrapped ruby, AUTO overhang, and emphasis marks "
          "agree with Chromium within the documented comparisons; primary ruby inline "
          "placement and advance agree with Chromium and Firefox; sideways track order "
          "agrees with Firefox")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"layout_l4_oracle: {error}", file=sys.stderr)
        raise SystemExit(1)
