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
"""How an oracle is spelled, so that no tool here spells one itself.

**Copied from `libs/unicode/tools/oracle/oracle_env.py`**, which landed the
pattern. What is
changed: the PROBE table names fontTools instead of CPython and ICU, and
`check_pin()` runs in **both** modes again rather than only in container mode.
That reversal is the whole difference between the two libraries and it is not a
style preference - see `check_pin()`.

`command("fonttools")` returns the argv prefix to run the reference with, which
is a `docker run` into the image built by `containers/fonttools/Containerfile`.

Three properties, in the order they matter:

  1. **It does not fail open.** A missing image, a missing engine, a version
     that does not match its pin - each raises. The mode that uses host tools
     has to be asked for by name. A gate that cannot reach its reference must
     say so and fail; "skipped" printed where a comparison should be is the
     failure this directory exists to avoid.

  2. **It says which instrument answered.** `provenance()` returns the line
     every gate prints beside its numbers. A clean `cmap` differential against
     fontTools 4.66.0 is a different claim from one against some fontTools, and
     a run that does not say which cannot be read a week later.

  3. **Paths mean the same thing on both sides.** The repository is mounted at
     its own host path, so a path a caller has built already resolves and no
     tool needs translating. A `scratch` directory is mounted the same way, and
     this library does need one: the corpus of real fonts lives **inside the
     image** (design.md section 14.5 keeps third-party fonts out of the
     repository), so a differential materialises it into a scratch directory
     that both sides can read.

Modes, from GHOTI_ORACLE_MODE:

  container  (default) run the reference in its pinned image
  host                 run this machine's own fontTools, and print what it is
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Two directories up, so this file has to live at <repo>/tools/oracle/.
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGES = os.path.join(HERE, "containers", "IMAGES")

MODE = os.environ.get("GHOTI_ORACLE_MODE", "container")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "docker")


class OracleUnavailable(Exception):
    """The reference cannot be reached. Never caught into a skip."""


# How to ask each reference for its version, and what the answer must contain.
#
# The probe is a program *in the image* rather than a shell one-liner here,
# because a one-liner puts a quoting layer between the check and the fact it
# checks - the first draft of that image wrote the script with printf and its
# \x27 escapes reached the file literally. `unicode` avoids the same trap by
# compiling a version program into its ICU image rather than parsing icuinfo.
#
# fontTools' own version is the whole claim here, unlike `unicode` where the
# interpreter's version was incidental and the UCD it carried was the pin.
PROBE = {
    "fonttools": (["fonttools-version"], "fontTools "),
    # FreeType's probe is a compiled program for the same reason `unicode`'s ICU
    # image compiles one: it reports `FT_Library_Version` from the shared object
    # the driver itself links, which is the claim check_pin() needs. The ARG in
    # the Containerfile says what the build was told to fetch, and an image is not
    # its recipe.
    "freetype": (["freetype-version"], "FreeType "),
}

_pins = None
_cache = {}


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    _pins = {}
    if not os.path.exists(IMAGES):
        return _pins
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r"
                    % line)
            name, image, version = parts[0], parts[1], parts[2]
            _pins[name] = (image, version, parts[3] if len(parts) > 3 else "")
    return _pins


def _engine_ok():
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable(
            "%s is not on PATH, and GHOTI_ORACLE_MODE is 'container'.\n"
            "Install it, or run with GHOTI_ORACLE_MODE=host to use this "
            "machine's own interpreter - which answers a different question, "
            "and says so in the line it prints." % ENGINE)


def _have_image(image):
    finished = subprocess.run([ENGINE, "image", "exists", image],
        capture_output=True)
    if finished.returncode == 0:
        return True
    # `image exists` is podman's. Fall back to a docker-portable spelling.
    finished = subprocess.run([ENGINE, "image", "inspect", image],
        capture_output=True)
    return finished.returncode == 0


def ensure(name):
    """Make the reference runnable, or raise saying what is missing."""
    if MODE == "host":
        binary = PROBE.get(name, ([name], ""))[0][0]
        if shutil.which(binary) is None:
            raise OracleUnavailable(
                "GHOTI_ORACLE_MODE=host and %s is not on PATH" % binary)
        return
    if MODE != "container":
        raise OracleUnavailable("GHOTI_ORACLE_MODE=%r is not a mode" % MODE)
    _engine_ok()
    table = pins()
    if name not in table:
        raise OracleUnavailable(
            "no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if _have_image(image):
        return
    if os.environ.get("GHOTI_ORACLE_PULL", "1") != "1":
        raise OracleUnavailable(
            "image for %s is not present and GHOTI_ORACLE_PULL is off: %s"
            % (name, image))
    sys.stderr.write("oracle: pulling %s\n" % image)
    finished = subprocess.run([ENGINE, "pull", image], capture_output=True,
        text=True)
    if finished.returncode != 0:
        raise OracleUnavailable(
            "could not pull the pinned image for %s.\n  %s\n%s"
            % (name, image, finished.stderr.strip()))


# Ask one oracle's questions of a different pin, e.g.
#   GHOTI_ORACLE_ALIAS=fonttools=fonttools-next make check-oracle-cmap
# There is one pin today, so nothing uses this yet. It is kept because the
# reading it exists for - the same corpus against two reference versions, where
# the disagreement is what the reference changed rather than what this library
# got wrong - is exactly what a fontTools upgrade will want, and because
# provenance() below has to name the pin that *answered* either way.
ALIAS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("GHOTI_ORACLE_ALIAS", "").split(",")
    if "=" in pair)


def command(name, argv=None, scratch=None):
    """The argv prefix that runs `name`'s reference.

    `argv` is what to run *inside*, defaulting to the reference's own
    interpreter. The repository is bind-mounted at its own path, so any path a
    caller has built already resolves - the driver under this directory is
    named by its absolute path and needs no translation.

    `scratch` is a directory the reference must be able to *write*, named the
    same way. This library does use one, and for a reason worth stating: the
    corpus of real fonts is in the image and nowhere else (design.md section
    14.5), so a differential asks the reference to copy the fonts it is about to
    answer for into scratch, and then both sides read the same bytes. Passing
    the directory is deliberate rather than mounting /tmp, so that a tool which
    forgets to declare it fails on a path that does not exist instead of writing
    somewhere nobody reads.

    Everything else is closed. `--network none` because no reference here has
    business reaching the network, and the tree is read-only because a corpus
    quietly edited by the thing being compared against it is not a comparison.
    """
    name = ALIAS.get(name, name)
    ensure(name)
    inner = argv if argv is not None else [PROBE.get(name, ([name],))[0][0]]
    if MODE == "host":
        return list(inner)
    image = pins()[name][0]
    argv_out = [ENGINE, "run", "--rm", "-i",
            "--network", "none",
            "--volume", "%s:%s:ro" % (ROOT, ROOT)]
    for path in ([scratch] if isinstance(scratch, str) else (scratch or [])):
        argv_out += ["--volume", "%s:%s:rw" % (path, path)]
    return argv_out + ["--workdir", ROOT, image] + list(inner)


def version(name):
    """What the reference says it is. Runs it; the answer is cached."""
    name = ALIAS.get(name, name)
    key = ("version", name)
    if key in _cache:
        return _cache[key]
    probe, expect = PROBE.get(name, ([name, "--version"], ""))
    finished = subprocess.run(command(name, probe), capture_output=True,
        text=True)
    # stdout only. `docker` on this machine is a podman shim that prints a
    # banner to stderr on every invocation, and a probe that reads both streams
    # reads the banner. The same trap waits for any driver that merges them:
    # the reference's answers and the engine's chatter would interleave on one
    # stream and the extra line would be scored as a disagreement.
    text = finished.stdout.strip().splitlines()
    text = text[0] if text else ""
    if expect and expect not in text:
        raise OracleUnavailable(
            "%s answered %r, which does not look like a version" %
            (name, text))
    _cache[key] = text
    return text


def check_pin(name):
    """Raise unless the reference's version matches containers/IMAGES.

    **In both modes**, which is where this file departs from the `unicode` copy
    it came from. The run-time version check is the real guarantee for an image
    **built here**, because two builds of one Containerfile are not two copies
    of one image and there is no digest to trust. `unicode` could relax
    it in host mode because every image it names is a stock one pinned by
    digest; this library names no stock image at all, so relaxing the check
    would leave nothing checking anything.

    The cost is that host mode is a dead end on this machine, which has no
    fontTools - and that is the honest outcome rather than a gap: a differential
    against an unknown fontTools is not the claim the gate prints.
    """
    name = ALIAS.get(name, name)
    table = pins()
    if name not in table:
        return version(name)
    said = table[name][1]
    got = version(name)
    if said not in got:
        raise OracleUnavailable(
            "%s: IMAGES says %s and it answers %r" % (name, said, got))
    return got


def provenance(names):
    """One line naming every reference that answered, and how.

    The name printed is the one that *answered*, not the one the gate asked
    for. Under an alias those differ, and printing the requested name makes the
    line name the wrong pin: `GHOTI_ORACLE_ALIAS=python=python-next` reported
    `oracle(container): python Python 3.15.0rc2`, where the version is right and
    the name is the one a reader would grep for. The alias is shown too, because
    "which gate was this" is the other question the line has to answer.
    """
    # Not "host, unpinned": check_pin() applies in both modes here, so a host
    # run that got this far has matched the pin like any other.
    where = MODE
    parts = []
    for name in names:
        resolved = ALIAS.get(name, name)
        label = resolved if resolved == name else "%s as %s" % (resolved, name)
        parts.append("%s %s" % (label, check_pin(name)))
    return "oracle(%s): %s" % (where, ", ".join(parts))
