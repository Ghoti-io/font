#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Compare U+FFFC object geometry with Chromium and Pango."""
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
from css_layout_diff import read_png, components

ROOT = Path(oracle_env.ROOT)
DRIVER = ROOT / "build/linux/release/apps/examples/font-layout"
FONT = ROOT / "tests/data/fonts/vertical-text.ttf"
SCRATCH = ROOT / "build/oracle/layout-l2"
TEXT = "A\ufffcB"
ADVANCE, ASCENT, DESCENT = 40, 30, 10
SIZE = 50
TOL = 0.6


def ours():
    argv = [str(DRIVER), "--size", str(SIZE), "--object-at", "1",
            "--object-advance", str(ADVANCE), "--object-ascent", str(ASCENT),
            "--object-descent", str(DESCENT), str(FONT)]
    p = subprocess.run(argv, input="u\t%s\n" % TEXT.encode().hex(), text=True,
                       capture_output=True, check=True)
    answer = json.loads(p.stdout)
    if "error" in answer:
        raise RuntimeError(answer["error"])
    if not any(run.get("object") for line in answer["lines"] for run in line["runs"]):
        raise RuntimeError("driver did not report its synthetic object run")
    return answer


def browser():
    os.makedirs(SCRATCH, exist_ok=True)
    page = SCRATCH / "object.html"
    # CSS inline-block baselines sit at the bottom edge; the library object's baseline is
    # ascent pixels below its top, so lower the CSS box by its descent to model the same box.
    page.write_text('''<!doctype html><meta charset=utf-8><style>
@font-face { font-family: T; src: url("file://%s"); }
body { margin: 0; } #p { position:absolute; margin:0; padding:0; font:50px T;
line-height:normal; white-space:pre; } #o { display:inline-block; width:40px; height:40px;
vertical-align:-%dpx; background:#f00; } #base { display:inline-block; width:0; height:0;
vertical-align:baseline; }
</style><div id=p>A<span id=o></span><span id=base></span>B</div><pre id=out></pre>
<script>
const p=document.querySelector('#p'), o=document.querySelector('#o'), base=document.querySelector('#base');
const r=document.createRange(); r.selectNode(o); const q=r.getBoundingClientRect();
const z=base.getBoundingClientRect();
const cs=[...p.childNodes]; const a=document.createRange(); a.setStart(cs[0],0);a.setEnd(cs[0],1);
const b=document.createRange(); b.setStart(cs[3],0);b.setEnd(cs[3],1);
document.querySelector('#out').textContent=JSON.stringify({o:[q.x,q.y,q.width,q.height],base:[z.x,z.y],
 a:[a.getBoundingClientRect().x,a.getBoundingClientRect().y,a.getBoundingClientRect().width,a.getBoundingClientRect().height],
 b:[b.getBoundingClientRect().x,b.getBoundingClientRect().y,b.getBoundingClientRect().width,b.getBoundingClientRect().height]});
</script>''' % (FONT, DESCENT))
    argv = oracle_env.command("chrome", ["chromium", "--headless=new", "--no-sandbox",
        "--disable-gpu", "--allow-file-access-from-files", "--disable-dev-shm-usage",
        "--user-data-dir=" + str(SCRATCH / "chrome-profile"), "--virtual-time-budget=5000",
        "--dump-dom", "file://" + str(page)], scratch=str(SCRATCH))
    p = subprocess.run(argv, capture_output=True, text=True, check=True)
    found = re.search(r'<pre id="out">(.*?)</pre>', p.stdout, re.S)
    if not found:
        raise RuntimeError("Chromium returned no object geometry: " + p.stderr[-500:])
    return json.loads(html.unescape(found.group(1)))


def pango():
    fonts = SCRATCH / "pango-fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    import shutil
    shutil.copy(FONT, fonts / "vertical-text.ttf")
    jobs = SCRATCH / "pango-jobs.json"
    out = SCRATCH / "pango-out.json"
    jobs.write_text(json.dumps([{"name":"object","text":TEXT,"family":"Vertical Text",
        "size":SIZE,"gravity":"SOUTH","hint":"NATURAL","shape":{"start":1,"end":4,
        "advance":ADVANCE,"ascent":ASCENT,"descent":DESCENT}}]))
    probe = ROOT / "tools/oracle/pango_probe.py"
    argv = oracle_env.command("pango", ["python3", str(probe), str(jobs), str(out), str(fonts)],
                              scratch=str(SCRATCH))
    subprocess.run(argv, check=True, capture_output=True, text=True)
    return json.loads(out.read_text())[0]


def firefox():
    page = SCRATCH / "firefox-object.html"
    shot = SCRATCH / "firefox-object.png"
    page.write_text('''<!doctype html><meta charset=utf-8><style>
html,body { margin:0; background:white; }
#p { position:absolute; left:0; top:0; margin:0; padding:0; white-space:pre;
font:50px "Vertical Text"; color:transparent; }
@font-face { font-family:"Vertical Text"; src:url("file://%s"); }
#o { display:inline-block; width:40px; height:40px; vertical-align:-%dpx; background:black; }
#base { display:inline-block; width:0; height:0; vertical-align:baseline; }
#mark { position:absolute; width:1px; height:1px; background:black; }
</style><div id=p>A<span id=o></span><span id=base></span>B</div><span id=mark></span>
<script>const r=document.querySelector('#base').getBoundingClientRect();
document.querySelector('#mark').style.top=r.y+'px';</script>''' % (FONT, DESCENT))
    argv = oracle_env.command("firefox", ["firefox-esr", "--headless", "--no-remote",
        "--window-size=200,120", "--screenshot", str(shot), "file://" + str(page)],
        scratch=str(SCRATCH))
    p = subprocess.run(argv, capture_output=True, text=True)
    if p.returncode or not shot.exists():
        raise RuntimeError("Firefox screenshot failed: " + p.stderr[-800:])
    width, height, dark = read_png(shot)
    boxes = components(dark, width, height)
    objects = [b for b in boxes if b[2] - b[0] > 1 and b[3] - b[1] > 1]
    baselines = [b for b in boxes if b[2] - b[0] == 1 and b[3] - b[1] == 1]
    if len(objects) != 1 or len(baselines) != 1:
        raise RuntimeError("Firefox produced %d object boxes and %d baseline markers" %
                           (len(objects), len(baselines)))
    x0, y0, x1, y1 = objects[0]
    return [x0, y0, x1 - x0, y1 - y0, baselines[0][1]]


def library_object(answer):
    for line in answer["lines"]:
        for run in line["runs"]:
            if run.get("object"):
                glyph = run["g"][0]
                return [glyph[4] / 64, glyph[5] / 64 - ASCENT,
                        run["advance"] / 64, ASCENT + DESCENT]
    raise RuntimeError("library returned no placed object glyph")


def main():
    names = [n for n in os.environ.get("GHOTI_LAYOUT_L2_ORACLES", "chrome,pango,firefox").split(",") if n]
    if not names or any(n not in ("chrome", "pango", "firefox") for n in names):
        raise RuntimeError("GHOTI_LAYOUT_L2_ORACLES must name chrome, pango, and/or firefox")
    print(oracle_env.provenance(names))
    mine = ours()
    lib = [[v / 64 for v in r] for r in mine["chars"]]
    obj = library_object(mine)
    c = browser() if "chrome" in names else None
    p = pango() if "pango" in names else None
    f = firefox() if "firefox" in names else None
    problems = []
    # The synthetic object must have the requested advance and ascent/descent box. Its baseline
    # is shared with the two text neighbors, so compare all three inline positions.
    if c and (abs(obj[0] - c["o"][0]) > 1.1 or abs(obj[2] - c["o"][2]) > TOL
              or abs(c["o"][3] - (ASCENT + DESCENT)) > TOL
              or abs((obj[1] - mine["lines"][0]["baseline"] / 64)
                     - (c["o"][1] - c["base"][1])) > 1.1):
        problems.append("Chromium object/baseline geometry %s, baseline %s, library %s" %
                        (c["o"], c.get("base"), obj))
    if abs(obj[2] - ADVANCE) > TOL or abs(obj[3] - (ASCENT + DESCENT)) > TOL:
        problems.append("library object size %s, expected %dx%d" %
                        (obj, ADVANCE, ASCENT + DESCENT))
    if p:
        pchars = p["chars"]
        if any(abs(obj[i] - pchars[1][i]) > 1.1 for i in range(4)):
            problems.append("Pango object rectangle %s, library %s" % (pchars[1], obj))
    if f and (abs(obj[0] - f[0]) > 1.1 or abs(obj[2] - f[2]) > TOL
              or abs(obj[3] - f[3]) > TOL
              or abs((obj[1] - mine["lines"][0]["baseline"] / 64)
                     - (f[1] - f[4])) > 1.1):
        problems.append("Firefox object/baseline geometry %s, baseline %.2f, library %s" %
                        (f[:4], f[4], obj))
    # Neighbor advances before and after the object must not be swallowed or counted twice.
    for idx, raw in (((0, c["a"]), (2, c["b"])) if c else ()):
        if abs(lib[idx][0] - raw[0]) > 1.1 or abs(lib[idx][2] - raw[2]) > 2.5:
            problems.append("neighbor %d Chromium inline position %s, library %s" %
                            (idx, raw, lib[idx]))
    if problems:
        print("layout_l2_oracle: DIFFERENCES\n  " + "\n  ".join(problems))
        return 1
    labels = {"chrome": "Chromium", "pango": "Pango", "firefox": "Firefox"}
    print("layout_l2_oracle: %s agree on U+FFFC advance, cross-axis box and adjacent text"
          % ", ".join(labels[n] for n in names))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
