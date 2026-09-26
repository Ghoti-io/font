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
"""The committed renderings, reproduced on a **big-endian** target.

documentation/design.md sections 1 and 14.4. This library's determinism promise
is that the same font at the same size produces byte-identical pixels on every
platform, and it is kept by there being no `float` and no byte-order-dependent
read anywhere between a font's bytes and a pixel's coverage. Nothing else in the
repository can see a violation of it: `testGolden` re-renders on this machine and
would agree with itself, and the fontTools differentials compare numbers rather
than platforms.

So this builds the library for each big-endian target the cross container has,
runs the golden driver under qemu, and requires the output to equal
`tests/data/golden/coverage.txt` byte for byte.

**What it detects, precisely.** A difference between the host's renderings and a
big-endian target's means byte order, alignment or word size reached the
arithmetic. It does **not** detect every `float`: two IEEE-754 targets rounding
the same expression the same way would agree, so a float that crept in is caught
by there being no floating-point type in these headers and by `-Wfloat-conversion`
rather than here. Verified by planting a `memcpy` of a `uint32_t`'s bytes into
`gfnt_coverage_hash()` - which is identical to the explicit shifts on this
machine, so the whole unit suite stayed green while this gate failed on s390x.

**It cross-builds `font` alone.** cutil is supplied by
`tools/golden/cross_shim.c`, six functions over stdio, and that file says why:
cross-building cutil would need cutil's generated headers produced for the
target, and a generated header produced for the wrong target is the very class of
defect this gate exists to catch. The shim's list of six is **checked** against
what the library actually needs, so a seventh dependency fails the gate rather
than quietly linking the host's cutil.

**The comparison carries its own control.** A gate that cannot see a difference
reports agreement, so one run is made against a deliberately corrupted copy of
the committed file and has to fail.

Usage:
    check_golden.py [--quiet] [--targets a,b,c]
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))
GOLDEN = os.path.join(ROOT, "tests", "data", "golden", "coverage.txt")
GENERATOR = os.path.join(HERE, "make_golden.py")
SHIM = os.path.join(HERE, "cross_shim.c")

# The image the workspace's cross toolchains live in, and the targets to use.
# Big-endian is the point; sparc64 is here as well because it is big-endian
# *and* strict-alignment, which is the other thing the checked reader promises.
IMAGE = "localhost/ghoti-xarch:deb13"
TARGETS = {
    "s390x": ("s390x-linux-gnu-gcc", "qemu-s390x", "s390x-linux-gnu",
              "64-bit big-endian"),
    "powerpc64": ("powerpc64-linux-gnu-gcc", "qemu-ppc64",
                  "powerpc64-linux-gnu", "64-bit big-endian"),
    "sparc64": ("sparc64-linux-gnu-gcc", "qemu-sparc64", "sparc64-linux-gnu",
                "64-bit big-endian, strict alignment"),
}

# What cross_shim.c provides. Checked against the library's undefined symbols,
# so the shim cannot silently fall behind what the library needs.
SHIMMED = {
    "gcu_allocator_default", "gcu_file_read", "gcu_file_free",
    "gcu_file_result_string", "gcu_mmap_open", "gcu_mmap_close",
}


class Unavailable(Exception):
    """The cross container is not here, with a reason worth printing."""


def engine():
    for candidate in ("podman", "docker"):
        if shutil.which(candidate):
            return candidate
    raise Unavailable("neither podman nor docker is on PATH")


def have_image(tool):
    found = subprocess.run([tool, "image", "exists", IMAGE],
        capture_output=True)
    if found.returncode != 0:
        raise Unavailable(
            "%s is not built; see notes/suite/CONTAINERS.md section 3" % IMAGE)


def committed():
    if not os.path.exists(GOLDEN):
        raise SystemExit("%s does not exist; run `make golden`" % GOLDEN)
    with open(GOLDEN, encoding="utf-8") as handle:
        return handle.read()


def cutil_dependencies():
    """The cutil symbols the built library needs, so the shim can be checked."""
    archive = os.path.join(ROOT, "build", "linux", "release", "apps",
                           "libghoti.io-font-0.a")
    if not os.path.exists(archive):
        raise SystemExit("%s is not built; run `make`" % archive)
    listed = subprocess.run(["nm", archive], capture_output=True, text=True)
    wanted = set()
    for line in listed.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U" and "cutil" in parts[1]:
            # ghotiio_cutil_0_gcu_file_read -> gcu_file_read
            name = parts[1]
            at = name.find("gcu_")
            if at >= 0:
                wanted.add(name[at:])
    return wanted


def script(label, compiler, qemu, triple):
    """The shell the container runs for one target: build, then render."""
    out = "/tmp/golden-%s" % label
    return r"""
set -e
mkdir -p %(out)s
cd %(root)s
%(cc)s -std=c17 -O2 -w -fno-strict-aliasing \
    -I include -I build/linux/release/generated \
    -I %(cutil)s \
    -o %(out)s/font-render \
    $(find src -name '*.c') examples/font-render.c %(shim)s
for name in %(fixtures)s; do
    %(qemu)s -L /usr/%(triple)s %(out)s/font-render "$name" \
        tests/data/fonts/"$name"
done
""" % {
        "out": out,
        "root": ROOT,
        "cc": compiler,
        "cutil": os.path.join(WORKSPACE, ".local", "include", "ghoti.io",
                              "cutil-0"),
        "shim": SHIM,
        "qemu": qemu,
        "triple": triple,
        "fixtures": " ".join(golden_fixtures()),
    }


def golden_fixtures():
    """The fixtures the committed file covers, in its own order.

    Read out of the file rather than imported from the generator, so that the
    two cannot disagree about which fonts a run should render: the file is the
    contract and this reads the contract.
    """
    names = []
    for line in committed().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        name = line.split()[0]
        if name not in names:
            names.append(name)
    return names


def render_on(tool, label):
    """One target's output, from inside the container."""
    compiler, qemu, triple, _ = TARGETS[label]
    argv = [tool, "run", "--rm", "-i", "--network", "none",
            "--volume", "%s:%s:ro" % (WORKSPACE, WORKSPACE),
            "--workdir", ROOT, IMAGE,
            "sh", "-c", script(label, compiler, qemu, triple)]
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise SystemExit("the %s build or run failed:\n%s%s"
                         % (label, finished.stdout[-4000:],
                            finished.stderr[-4000:]))
    return finished.stdout


def body(text):
    """The renderings, without the comment header."""
    return [line for line in text.splitlines()
            if line.strip() and not line.startswith("#")]


def compare(label, expected, got):
    """Returns a list of complaints, empty when the two agree."""
    want = body(expected)
    mine = body(got)
    if want == mine:
        return []
    out = ["%s: %d rendering(s) expected, %d produced" % (label, len(want),
                                                          len(mine))]
    shown = 0
    for index in range(max(len(want), len(mine))):
        left = want[index] if index < len(want) else "(missing)"
        right = mine[index] if index < len(mine) else "(missing)"
        if left == right:
            continue
        out.append("  %s: expected %s" % (label, left))
        out.append("  %s: got      %s" % (label, right))
        shown += 1
        if shown >= 8:
            out.append("  %s: ...and more" % label)
            break
    return out


def control(label, expected, got):
    """A run against a corrupted expectation must fail.

    Without this, a comparison that had stopped comparing - an empty output, a
    body() that filtered everything away - would report agreement.
    """
    lines = body(expected)
    if not lines:
        return ["%s: THE CONTROL HAS NO INPUT: the committed file has no "
                "renderings in it" % label]
    middle = len(lines) // 2
    damaged = list(lines)
    damaged[middle] = damaged[middle] + " tampered"
    if not compare(label, "\n".join(damaged), got):
        return ["%s: THE CONTROL WAS NOT DETECTED: the comparison accepted a "
                "committed file with a line changed, so its clean result means "
                "nothing" % label]
    return []


def main(argv):
    quiet = "--quiet" in argv
    wanted = list(TARGETS)
    for index, argument in enumerate(argv[1:], start=1):
        if argument == "--targets":
            wanted = argv[index + 1].split(",")

    expected = committed()

    # The shim's list against what the library needs, before anything is built:
    # a seventh cutil dependency would otherwise be a confusing link error.
    needed = cutil_dependencies()
    missing = needed - SHIMMED
    if missing:
        sys.stderr.write(
            "check-golden: this library now needs cutil's %s, which "
            "tools/golden/cross_shim.c does not provide. Add it there (and to "
            "SHIMMED) rather than letting the cross build reach for cutil.\n"
            % ", ".join(sorted(missing)))
        return 1
    unused = SHIMMED - needed
    if unused:
        # Not a failure: a shim function the library has stopped calling is dead
        # weight rather than a hole. Said, so that it can be removed.
        print("check-golden: cross_shim.c still provides %s, which this "
              "library no longer calls" % ", ".join(sorted(unused)))

    try:
        tool = engine()
        have_image(tool)
    except Unavailable as why:
        # Loudly, with the word SKIPPED, and only for the one reason a machine
        # can have: the container is not there. Every other failure is a
        # failure. `testGolden` covers the same file with no container, so the
        # suite is not relying on this to have run.
        print("check-golden: SKIPPED (%s)" % why)
        return 0

    complaints = []
    for label in wanted:
        if label not in TARGETS:
            sys.stderr.write("check-golden: no such target: %s\n" % label)
            return 1
        got = render_on(tool, label)
        complaints.extend(compare(label, expected, got))
        complaints.extend(control(label, expected, got))
        if not complaints and not quiet:
            print("  %-12s %s: identical to the committed renderings"
                  % (label, TARGETS[label][3]))

    if complaints:
        sys.stderr.write("\n".join(complaints) + "\n")
        sys.stderr.write(
            "\ncheck-golden: a rendering that differs by platform is a "
            "host-dependent read or a float in the path from a font's bytes to "
            "a pixel. See documentation/design.md sections 1 and 14.4.\n")
        return 1

    print("check-golden: %d rendering(s) reproduced on %s, control passed"
          % (len(body(expected)), ", ".join(wanted)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
