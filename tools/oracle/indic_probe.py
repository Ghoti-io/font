#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Show HarfBuzz's and this library's glyphs and clusters side by side for given role strings.

Usage: indic_probe.py SCRIPT TAG FEATURE "ROLE ROLE ..." ["ROLE ..."]...
The font is the pair-reveal font of indic_random_diff.py for FEATURE; TAG is old or new.
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
import indic_random_diff as M
import indic_exhaustive as X


def main(argv):
    script, which, feat = argv[:3]
    strs = [a.split() for a in argv[3:]]
    tag = M.SCRIPTS[script][2] if which == "old" else M.SCRIPTS[script][1]
    M.PAIRS = M.REVEAL = True
    g, cp = M.glyph_map(script)
    inv = {v: k for k, v in g.items()}
    work = os.path.join(X.SCRATCH, "probe")
    os.makedirs(work, exist_ok=True)
    font, text = os.path.join(work, "f.ttf"), os.path.join(work, "t.txt")
    with open(font, "wb") as h:
        h.write(G.base_font(nglyphs=M.NG, cmap_map={cp[k]: v for k, v in g.items()},
                            extra={'GSUB': M.build_reveal(g, tag, (feat,))}))
    with open(text, "w", encoding="utf-8") as h:
        for s in strs:
            h.write(''.join(chr(cp[r]) for r in s) + "\n")
    run = os.path.join(work, "run.sh")
    with open(run, "w") as h:
        h.write("hb-shape --font-file='%s' --output-format=json --no-glyph-names --script=%s "
                "--text-file='%s' > '%s.hb' 2>/dev/null\n" % (font, M.SCRIPTS[script][0], text, font))
    subprocess.run(oracle_env.command("harfbuzz", ["sh", run], scratch=X.SCRATCH), capture_output=True)
    hb = [json.loads(l) for l in open(font + ".hb").read().split("\n")[:-1]]
    ours = subprocess.run([G.DRIVER, "--batch", "--script", tag, font], stdin=open(text, "rb"),
                          capture_output=True, text=True)
    mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]

    def dec(gl):
        out = []
        for t in X.norm(gl):
            x = t[0]
            x = ("(%s+%s)" % (inv.get(x - 40 >> 4, '?'), inv.get(x - 40 & 15, '?'))) if x >= 40 else inv.get(x, str(x))
            out.append("%s@%s" % (x, t[1]))
        return ' '.join(out)
    for s, a, b in zip(strs, hb, mine):
        print("%-28s hb: %-44s our: %-44s %s" % (' '.join(s), dec(a), dec(b), "" if X.norm(a) == X.norm(b) else "<<<"))


if __name__ == "__main__":
    main(sys.argv[1:])
