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
    page.write_text('''<!doctype html><meta charset=utf-8><style>
@font-face { font-family: T; src: url("file://%s"); }
body { margin: 0; } #p { position:absolute; margin:0; padding:0; font:50px T;
line-height:normal; white-space:pre; } #o { display:inline-block; width:40px; height:40px;
vertical-align:baseline; background:#f00; }
</style><div id=p>A<span id=o></span>B</div><pre id=out></pre>
<script>
const p=document.querySelector('#p'), o=document.querySelector('#o');
const r=document.createRange(); r.selectNode(o); const q=r.getBoundingClientRect();
const cs=[...p.childNodes]; const a=document.createRange(); a.setStart(cs[0],0);a.setEnd(cs[0],1);
const b=document.createRange(); b.setStart(cs[2],0);b.setEnd(cs[2],1);
document.querySelector('#out').textContent=JSON.stringify({o:[q.x,q.y,q.width,q.height],
 a:[a.getBoundingClientRect().x,a.getBoundingClientRect().y,a.getBoundingClientRect().width,a.getBoundingClientRect().height],
 b:[b.getBoundingClientRect().x,b.getBoundingClientRect().y,b.getBoundingClientRect().width,b.getBoundingClientRect().height]});
</script>''' % FONT)
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
font:50px "Vertical Text"; color:white; }
@font-face { font-family:"Vertical Text"; src:url("file://%s"); }
#o { display:inline-block; width:40px; height:40px; vertical-align:baseline; background:black; }
</style><div id=p>A<span id=o></span>B</div>''' % FONT)
    argv = oracle_env.command("firefox", ["firefox-esr", "--headless", "--no-remote",
        "--window-size=200,120", "--screenshot", str(shot), "file://" + str(page)],
        scratch=str(SCRATCH))
    p = subprocess.run(argv, capture_output=True, text=True)
    if p.returncode or not shot.exists():
        raise RuntimeError("Firefox screenshot failed: " + p.stderr[-800:])
    width, height, dark = read_png(shot)
    boxes = components(dark, width, height)
    if len(boxes) != 1:
        raise RuntimeError("Firefox produced %d dark components, expected the single object" % len(boxes))
    return boxes[0]


def main():
    print(oracle_env.provenance(["chrome", "pango", "firefox"]))
    mine = ours()
    lib = [[v / 64 for v in r] for r in mine["chars"]]
    c = browser()
    p = pango()
    f = firefox()
    problems = []
    # The synthetic object must have the requested advance and ascent/descent box. Its baseline
    # is shared with the two text neighbors, so compare all three inline positions.
    if abs(c["o"][2] - ADVANCE) > TOL or abs(lib[1][0] - c["o"][0]) > 1.1:
        problems.append("Chromium object x/advance %s, library %s" % (c["o"], lib[1]))
    if abs(lib[1][2] - ADVANCE) > TOL:
        problems.append("library object advance %.2f, expected %d" % (lib[1][2], ADVANCE))
    pchars = p["chars"]
    if abs(pchars[1][2] - ADVANCE) > TOL or abs(pchars[1][3] - (ASCENT + DESCENT)) > TOL:
        problems.append("Pango shape rectangle %s differs from expected [%d,%d]" %
                        (pchars[1], ADVANCE, ASCENT + DESCENT))
    if f[2] - f[0] != ADVANCE or f[3] - f[1] != ASCENT + DESCENT:
        problems.append("Firefox rendered object box %s, expected %dx%d" %
                        (f, ADVANCE, ASCENT + DESCENT))
    # Neighbor advances before and after the object must not be swallowed or counted twice.
    for idx, raw in ((0, c["a"]), (2, c["b"])):
        if abs(lib[idx][0] - raw[0]) > 1.1:
            problems.append("neighbor %d Chromium start %.2f, library %s" % (idx, raw[0], lib[idx]))
    if problems:
        print("layout_l2_oracle: DIFFERENCES\n  " + "\n  ".join(problems))
        return 1
    print("layout_l2_oracle: Chromium, Pango and Firefox agree on U+FFFC advance, box and adjacent text")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
