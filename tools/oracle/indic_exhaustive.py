#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Every short syllable, every Indic script and tag, against HarfBuzz, with a ledger.

indic_random_diff.py samples; this enumerates. For each unit - (script, tag, feature) -
it builds the pair-reveal font of indic_random_diff.py (a ligature for every ordered
pair of glyphs under that one feature, so the output shows which glyphs carry the
feature's mask and in what order they stood), shapes every string of up to --maxlen
roles over an alphabet with HarfBuzz and with this library, and keeps the strings
that differ. A failing string is kept only if it is minimal (deleting any one role
gives a string that does not fail, so the shorter string is the witness). Minimal
failures are grouped by pattern - consonants other than Ra read as C - so that the
unit of work is a class of failures and not a string.

The ledger (--state, JSON) holds one record per unit: strings tried, strings that
differ, classes, an example per class, and when. --report prints it as a table, with
what is done and what remains. A unit already in the ledger at the same --maxlen is
skipped unless --redo. --shard I/N takes every Nth unit, for running on several
machines and merging the ledgers with --merge.

Usage: indic_exhaustive.py [--scripts a,b] [--tags old,new] [--feats f,g] [--maxlen N]
                           [--alphabet R,R] [--workers N] [--state FILE] [--shard I/N]
                           [--redo] [--report] [--merge FILE...] [--driver PATH]
"""

import itertools
import json
import multiprocessing
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
import indic_random_diff as M

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "indic-exhaustive")
DEFAULT_STATE = os.path.join(SCRATCH, "ledger.json")
FEATS = ["nukt", "akhn", "rphf", "rkrf", "pref", "blwf", "abvf", "half", "pstf", "vatu",
         "cjct", "init", "pres", "abvs", "blws", "psts", "haln"]
ALPHABET = ["KA", "TA", "RA", "NUKTA", "H", "ZWJ", "ZWNJ", "I", "E", "U"]
TOP = 40
KEYS = ("g", "cl", "ax", "ay", "dx", "dy")


def norm(gl):
    return [tuple(x.get(k) for k in KEYS) for x in gl]


def pattern(roles):
    return ' '.join("C" if r in ("KA", "TA") else r for r in roles)


def joiner_free(key):
    """A pattern with ZWJ and ZWNJ both read as J, to group a family of strings."""
    return ' '.join("J" if x in ("ZWJ", "ZWNJ") else x for x in key.split())


def strings(alphabet, maxlen):
    for n in range(1, maxlen + 1):
        for s in itertools.product(alphabet, repeat=n):
            yield s


def unit_name(script, tag, feat):
    return "%s/%s/%s" % (script, tag, feat)


def run_unit(job):
    script, tag, feat, alphabet, maxlen, driver = job
    t0 = time.time()
    work = os.path.join(SCRATCH, "w-%s-%s-%s" % (script, tag, feat))
    os.makedirs(work, exist_ok=True)
    M.PAIRS = True
    M.REVEAL = True
    g, cp = M.glyph_map(script)
    alpha = [r for r in alphabet if r in g]
    font = os.path.join(work, "f.ttf")
    text = os.path.join(work, "t.txt")
    with open(font, "wb") as h:
        h.write(G.base_font(nglyphs=M.NG, cmap_map={cp[k]: v for k, v in g.items()},
                            extra={'GSUB': M.build_reveal(g, tag, (feat,))}))
    all_strings = list(strings(alpha, maxlen))
    with open(text, "w", encoding="utf-8") as h:
        for s in all_strings:
            h.write(''.join(chr(cp[r]) for r in s) + "\n")
    iso = M.SCRIPTS[script][0]
    runner = os.path.join(work, "run.sh")
    with open(runner, "w") as h:
        h.write("hb-shape --font-file='%s' --output-format=json --no-glyph-names "
                "--script=%s --text-file='%s' > '%s.hb' 2>/dev/null\n" % (font, iso, text, font))
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        return unit_name(script, tag, feat), {"error": "hb-shape: " + ref.stderr[-200:]}
    with open(font + ".hb", encoding="utf-8") as h:
        hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
    with open(text, "rb") as h:
        ours = subprocess.run([driver, "--batch", "--script", tag, font], stdin=h,
                              capture_output=True, text=True)
    mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
    if len(hb) != len(all_strings) or len(mine) != len(all_strings):
        return unit_name(script, tag, feat), {"error": "line counts %d %d %d" % (
            len(hb), len(mine), len(all_strings))}
    failing = {s for s, a, b in zip(all_strings, hb, mine) if norm(a) != norm(b)}
    inv = {v: k for k, v in g.items()}
    hbmap = {s: a for s, a in zip(all_strings, hb)}
    ourmap = {s: b for s, b in zip(all_strings, mine)}
    classes = {}
    for s in sorted(failing, key=lambda s: (len(s), s)):
        if any(s[:i] + s[i + 1:] in failing for i in range(len(s))):
            continue   # a shorter string already fails
        key = pattern(s)
        c = classes.setdefault(key, {"count": 0, "example": None})
        c["count"] += 1
        if c["example"] is None:
            def dec(gl):
                out = []
                for t in norm(gl):
                    x = t[0]
                    if x >= 40:
                        x -= 40
                        x = "(%s %s)" % (inv.get(x // 16, '?'), inv.get(x % 16, '?'))
                    else:
                        x = inv.get(x, str(x))
                    out.append("%s@%s" % (x.replace(' ', '+'), t[1]))
                return ' '.join(out)
            c["example"] = {"string": ' '.join(s), "hb": dec(hbmap[s]), "our": dec(ourmap[s])}
    for f in (font, text, font + ".hb", runner):
        try:
            os.remove(f)
        except OSError:
            pass
    return unit_name(script, tag, feat), {
        "maxlen": maxlen, "alphabet": alpha, "tried": len(all_strings),
        "differ": len(failing), "minimal": sum(c["count"] for c in classes.values()),
        "classes": classes, "seconds": round(time.time() - t0, 1),
        "when": time.strftime("%Y-%m-%d %H:%M")}


def load(path):
    try:
        with open(path, encoding="utf-8") as h:
            return json.load(h)
    except (OSError, ValueError):
        return {}


def save(path, ledger):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as h:
        json.dump(ledger, h, indent=1, sort_keys=True)
    os.replace(tmp, path)


def report(ledger, units):
    done = [u for u in units if u in ledger and "error" not in ledger[u]]
    clean = [u for u in done if ledger[u]["differ"] == 0]
    errs = [u for u in units if u in ledger and "error" in ledger[u]]
    todo = [u for u in units if u not in ledger]
    print("units %d: clean %d, with differences %d, errors %d, not run %d"
          % (len(units), len(clean), len(done) - len(clean), len(errs), len(todo)))
    allclasses = {}
    for u in done:
        for k, c in ledger[u]["classes"].items():
            k = joiner_free(k)
            d = allclasses.setdefault(k, {"units": set(), "strings": 0, "example": None, "unit": None})
            d["units"].add(u)
            d["strings"] += c["count"]
            if d["example"] is None:
                d["example"], d["unit"] = c["example"], u
    print("failure classes (by pattern, ZWJ/ZWNJ read as J, across units): %d" % len(allclasses))
    for k, d in sorted(allclasses.items(), key=lambda kv: -len(kv[1]["units"]))[:TOP]:
        e = d["example"]
        print("  %-26s %3d units %5d strings  e.g. %s %s: %s  hb %s | our %s"
              % (k, len(d["units"]), d["strings"], d["unit"], "", e["string"], e["hb"], e["our"]))
    byfeat = {}
    for u in done:
        f = u.split("/")[2]
        byfeat.setdefault(f, [0, 0])
        byfeat[f][0] += ledger[u]["differ"]
        byfeat[f][1] += 1
    print("by feature (differing strings, units run):",
          ' '.join("%s=%d/%d" % (f, byfeat[f][0], byfeat[f][1]) for f in sorted(byfeat)))
    for u in errs:
        print("  error", u, ledger[u]["error"])
    if todo:
        print("not run:", len(todo), "e.g.", ', '.join(todo[:5]))


def main(argv):
    opt = {"--scripts": list(M.SCRIPTS), "--tags": ["old", "new"], "--feats": FEATS,
           "--maxlen": 4, "--alphabet": ALPHABET, "--workers": max(1, os.cpu_count() - 2),
           "--state": DEFAULT_STATE, "--driver": G.DRIVER, "--shard": None}
    i = 0
    flags = {"--redo", "--report", "--merge"}
    global TOP
    if "--top" in argv:
        TOP = int(argv[argv.index("--top") + 1])
        del argv[argv.index("--top"):argv.index("--top") + 2]
    merge = []
    while i < len(argv):
        a = argv[i]
        if a == "--merge":
            merge = argv[i + 1:]
            break
        if a in flags:
            opt[a] = True
        elif a in opt:
            v = argv[i + 1]
            i += 1
            if a in ("--scripts", "--tags", "--feats", "--alphabet"):
                v = v.split(",")
            elif a in ("--maxlen", "--workers"):
                v = int(v)
            opt[a] = v
        else:
            sys.stderr.write("unknown option %s\n" % a)
            return 2
        i += 1
    units = [unit_name(s, t, f) for s in opt["--scripts"] for t in opt["--tags"]
             for f in opt["--feats"]]
    ledger = load(opt["--state"])
    if merge:
        for p in merge:
            for k, v in load(p).items():
                ledger[k] = v
        save(opt["--state"], ledger)
        report(ledger, units)
        return 0
    if opt.get("--report"):
        report(ledger, units)
        return 0
    jobs = []
    for n, u in enumerate(units):
        if opt["--shard"]:
            si, sn = [int(x) for x in opt["--shard"].split("/")]
            if n % sn != si:
                continue
        s, t, f = u.split("/")
        old = ledger.get(u)
        if old and "error" not in old and old.get("maxlen") == opt["--maxlen"] \
                and old.get("alphabet") == [r for r in opt["--alphabet"] if r in M.glyph_map(s)[0]] \
                and not opt.get("--redo"):
            continue
        tagname = M.SCRIPTS[s][2] if t == "old" else M.SCRIPTS[s][1]
        jobs.append((s, tagname, f, opt["--alphabet"], opt["--maxlen"], opt["--driver"], u))
    print("%d units to run of %d (workers %d, maxlen %d)" % (len(jobs), len(units),
          opt["--workers"], opt["--maxlen"]), flush=True)
    os.makedirs(SCRATCH, exist_ok=True)
    work = [j[:6] for j in jobs]
    names = [j[6] for j in jobs]

    with multiprocessing.Pool(opt["--workers"]) as pool:
        for n, (name, rec) in zip(names, pool.imap(run_unit, work)):
            ledger[n] = rec
            save(opt["--state"], ledger)
            print("%-24s %s" % (n, ("error " + rec["error"]) if "error" in rec else
                  "%d tried, %d differ, %d classes, %.0fs" % (
                      rec["tried"], rec["differ"], len(rec["classes"]), rec["seconds"])),
                  flush=True)
    report(ledger, units)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
