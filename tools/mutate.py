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
"""Mutate one source file of the shaping engine and see what the random corpora notice.

The corpora are the fonts the random generators write (`morx_random_diff.py`,
`kerx_random_diff.py`), shaped by the unmutated build once to make a baseline. A
mutant (one operator flipped on one line) is killed when any string shapes
differently, or the shaper hangs or crashes. What survives either is equivalent or
is a gap in the corpora. The unmutated build agrees with HarfBuzz on every one of
these strings, so a difference from the baseline is a difference from HarfBuzz.

    mutate.py corpus DIR [--offline]   write the corpora under DIR (the HarfBuzz container, unless --offline)
    mutate.py baseline DIR        shape them with the current build
    mutate.py run DIR FILE... [-j N] [--tree T] [--prefix P]   mutate each file; each worker builds in a copy
                                  of the library tree

Every worker has its own tree (mutants build in place) and restores the file after
each mutant; a control run of the pristine tree comes before the first mutant.
"""

import concurrent.futures as cf
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "oracle"))
SHAPE = "build/linux/release/apps/examples/font-shape"

# name, generator, its arguments, font-shape's extra arguments
CORPORA = [
    ("morx-plain", "morx_random_diff.py", ["--seeds", "120", "--dont", "0"], []),
    ("morx-feat", "morx_random_diff.py", ["--seeds", "120", "--dont", "0", "--aatfeatures"], None),
    ("morx-ctx", "morx_random_diff.py", ["--seeds", "120", "--types", "2", "--dont", "0.3"], []),
    ("morx-cover", "morx_random_diff.py", ["--seeds", "120", "--coverage", "--dont", "0"], []),
    ("morx-rtl", "morx_random_diff.py", ["--seeds", "100", "--dont", "0", "--direction", "rtl"], ["--rtl"]),
    ("morx-dont", "morx_random_diff.py", ["--seeds", "120", "--dont", "0.3"], []),
    ("morx-del", "morx_random_diff.py", ["--seeds", "150", "--delete", "0.25", "--dont", "0.2", "--coverage"], []),
    ("morx-del-rtl", "morx_random_diff.py", ["--seeds", "150", "--delete", "0.25", "--dont", "0.2", "--coverage", "--direction", "rtl"], ["--rtl"]),
    ("morx-ins", "morx_random_diff.py", ["--seeds", "150", "--types", "5", "--dontins", "0.3"], []),
    ("morx-ins-rtl", "morx_random_diff.py", ["--seeds", "100", "--types", "5", "--dontins", "0.3", "--direction", "rtl"], ["--rtl"]),
    ("mort", "morx_random_diff.py", ["--mort", "--types", "0,1,2,4", "--dont", "0.2", "--coverage", "--seeds", "150"], []),
    ("mort-ins", "morx_random_diff.py", ["--mort", "--types", "5", "--dontins", "0.3", "--seeds", "100"], []),
    ("kerx", "kerx_random_diff.py", ["--seeds", "100"], []),
    ("kerx-dont", "kerx_random_diff.py", ["--kinds", "1", "--dont", "0.4", "--seeds", "100"], []),
    ("kerx-rtl", "kerx_random_diff.py", ["--seeds", "80", "--direction", "rtl"], ["--rtl"]),
    ("kerx-ttb", "kerx_random_diff.py", ["--seeds", "80", "--direction", "ttb"], ["--ttb"]),
    ("kerx-attach", "kerx_random_diff.py", ["--kinds", "0,1,2,4,5,6", "--cross", "0.3", "--seeds", "150"], []),
    ("kerx-attach-dont", "kerx_random_diff.py", ["--kinds", "4,5", "--dont", "0.4", "--seeds", "100"], []),
    ("kern", "kerx_random_diff.py", ["--table", "kern", "--seeds", "100"], []),
]


def corpus(d, offline=False):
    """Write the corpora; `offline` writes only the fonts and texts, which is all the
    mutants need (the baseline is this build's own output), so no container is wanted."""
    for name, gen, args, _ in CORPORA:
        out = os.path.join(d, name)
        os.makedirs(out, exist_ok=True)
        cmd = [sys.executable, os.path.join(HERE, "oracle", gen)] + args + ["--scratch", out]
        if offline:
            subprocess.run(cmd + ["--generate-only"], cwd=ROOT, check=False)
        else:
            subprocess.run([sys.executable, os.path.join(HERE, "oracle", "oracle_run.py"),
                            "harfbuzz", "--"] + cmd, cwd=ROOT, check=False)


def extra_for(name, seed):
    spec = next(c for c in CORPORA if c[0] == name)
    if spec[3] is not None:
        return spec[3]
    import morx_random_diff as M
    return ["--features", M.features_for(seed)]


def jobs(d):
    for name, _, _, _ in CORPORA:
        out = os.path.join(d, name)
        if not os.path.isdir(out):
            continue
        for f in sorted(os.listdir(out)):
            m = re.fullmatch(r"s(\d+)\.ttf", f)
            if m and os.path.exists(os.path.join(out, f[:-4] + ".txt")):
                yield name, int(m.group(1)), os.path.join(out, f), os.path.join(out, f[:-4] + ".txt")


def shape(tree, name, seed, font, text):
    with open(text, "rb") as h:
        try:
            r = subprocess.run([os.path.join(tree, SHAPE), "--batch", "--script", "latn"]
                               + extra_for(name, seed) + [font], stdin=h,
                               capture_output=True, timeout=10)
        except subprocess.TimeoutExpired:
            return "timeout"
    return "%d:%s" % (r.returncode, hashlib.sha1(r.stdout).hexdigest())


def baseline(d, tree=ROOT):
    sig = {}
    for name, seed, font, text in jobs(d):
        sig["%s/%d" % (name, seed)] = shape(tree, name, seed, font, text)
    with open(os.path.join(d, "baseline.json"), "w") as h:
        json.dump(sig, h)
    print("baseline: %d fonts" % len(sig))


OPS = [" <= ", " >= ", " == ", " != ", " && ", " || ", " < ", " > ", " + ", " - "]
SWAP = {" < ": " <= ", " <= ": " < ", " > ": " >= ", " >= ": " > ", " == ": " != ",
        " != ": " == ", " && ": " || ", " || ": " && ", " + ": " - ", " - ": " + "}


def mutants(path):
    out = []
    in_comment = False
    for n, line in enumerate(open(path).read().split("\n")):
        s = line.strip()
        if s.startswith("/*") and "*/" not in s:
            in_comment = True
        if in_comment or s.startswith("//") or s.startswith("*") or s.startswith("#"):
            if "*/" in s:
                in_comment = False
            continue
        code = line.split("//")[0]
        for op in OPS:
            start = 0
            while True:
                i = code.find(op, start)
                if i < 0:
                    break
                # `<=` is found as `<` too: only whole operators
                if op in (" < ", " > ") and code[i:i + 3] in (" <= ", " >= "):
                    start = i + 1
                    continue
                out.append((n, i, op))
                start = i + len(op)
    return out


def worker_tree(base, k):
    t = os.path.join(base, "w%d" % k)
    if not os.path.exists(t):
        os.makedirs(t)
        for item in ("src", "include", "Makefile", "examples", "tools", "apps"):
            p = os.path.join(ROOT, item)
            if os.path.exists(p):
                if os.path.isdir(p):
                    shutil.copytree(p, os.path.join(t, item), symlinks=True)
                else:
                    shutil.copy2(p, os.path.join(t, item))
        os.makedirs(os.path.join(t, "build/linux"), exist_ok=True)
        shutil.copytree(os.path.join(ROOT, "build/linux/release"), os.path.join(t, "build/linux/release"), symlinks=True)
        os.makedirs(os.path.join(t, "build/generated"), exist_ok=True)
    return t


def rebuild(tree, prefix):
    for g in ("build/linux/release/apps/examples", "build/linux/release/apps"):
        p = os.path.join(tree, g)
        for f in os.listdir(p) if os.path.isdir(p) else []:
            fp = os.path.join(p, f)
            if os.path.isfile(fp) and (g.endswith("examples") or f.endswith(".a")):
                os.remove(fp)
    r = subprocess.run(["make", "PREFIX=" + prefix, "examples"], cwd=tree, capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(os.path.join(tree, SHAPE))


def killed(tree, d, base):
    for name, seed, font, text in jobs(d):
        if shape(tree, name, seed, font, text) != base["%s/%d" % (name, seed)]:
            return "%s/%d" % (name, seed)
    return None


def run_file(args):
    k, tree, d, base, prefix, rel, todo = args
    path = os.path.join(tree, rel)
    orig = open(path).read()
    lines = orig.split("\n")
    res = []
    for (n, i, op) in todo:
        line = lines[n]
        mutated = lines[:]
        mutated[n] = line[:i] + SWAP[op] + line[i + len(op):]
        open(path, "w").write("\n".join(mutated))
        try:
            if not rebuild(tree, prefix):
                res.append((n + 1, op.strip(), "nobuild", line.strip()))
                continue
            by = killed(tree, d, base)
            res.append((n + 1, op.strip(), by or "SURVIVED", line.strip()))
        finally:
            open(path, "w").write(orig)
    rebuild(tree, prefix)
    return res


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, d = argv[0], os.path.abspath(argv[1])
    if cmd == "corpus":
        corpus(d, "--offline" in argv)
        return 0
    if cmd == "baseline":
        baseline(d)
        return 0
    jn = int(argv[argv.index("-j") + 1]) if "-j" in argv else 4
    tmp = argv[argv.index("--tree") + 1] if "--tree" in argv else os.path.join(d, "trees")
    prefix = (argv[argv.index("--prefix") + 1] if "--prefix" in argv
              else os.environ.get("PREFIX") or os.path.join(ROOT, "..", "..", ".local"))
    files = [a for a in argv[2:] if a.endswith(".c")]
    base = json.load(open(os.path.join(d, "baseline.json")))
    trees = [worker_tree(tmp, k) for k in range(jn)]
    for t in trees:
        if not rebuild(t, prefix):
            print("control build failed in", t)
            return 2
        if killed(t, d, base):
            print("control: the unmutated build differs from the baseline in", t)
            return 2
    surv = 0
    total = 0
    for rel in files:
        ms = mutants(os.path.join(ROOT, rel))
        # one task per tree: a tree holds one mutant at a time
        work = [(k, trees[k], d, base, prefix, rel, ms[k::jn]) for k in range(jn)]
        with cf.ThreadPoolExecutor(jn) as ex:
            for res in ex.map(run_file, work):
                for (n, op, by, line) in res:
                    total += 1
                    if by in ("SURVIVED", "nobuild"):
                        surv += 1
                        print("%s:%d %s %s  | %s" % (rel, n, op, by, line), flush=True)
    print("%d mutants, %d survived or did not build" % (total, surv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
