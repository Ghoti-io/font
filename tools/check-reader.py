#!/usr/bin/env python3
"""Fail if anything under src/ reads font bytes without the checked reader.

documentation/design.md section 6: every read of file data goes through
GFNT_Reader, and that is not a style preference.  It is the mechanism behind
the threat model - a table cannot reach into its neighbour because the reader
it was given spans only its own extent - and it is what makes the fuzzers
mean something, because a crash then has two possible causes rather than
every parser in the library.

So the second kind of defect, a read that goes around the reader, has to be
caught mechanically.  This is that grep, with one wrinkle: it checks itself.
Each pattern is run against a planted violation in the same invocation, and a
pattern that no longer matches its own plant fails the build.  A gate that has
silently stopped matching reports a clean tree exactly as a clean tree does,
which is the failure mode this suite has hit often enough to write down.

Comments are stripped before scanning, so that a comment naming a forbidden
construct - reader.c's own do - is not a violation.  String literals are not
stripped: nothing in this library has a reason to spell one of these in a
string, and leaving them in keeps the stripping simple enough to trust.

Usage: check-reader.py [src-directory]
"""

import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src"

# Each entry: (name, pattern, why, exempt directories).
#
# The exemptions are directories rather than files so that a module can grow a
# second file, and each is checked for staleness below: an exemption that no
# longer covers anything is an exclusion nobody is looking at any more.
CHECKS = [
    (
        "cast-and-dereference of file bytes",
        r"\*\s*\(\s*(?:const\s+)?"
        r"(?:u?int(?:8|16|32|64)_t|unsigned\s+(?:char|short|int|long)"
        r"|short|long)\s*\*\s*\)",
        "undefined on an unaligned address, and host-endian where it works",
        [],
    ),
    (
        "host byte-order conversion",
        r"\b(?:ntohs|ntohl|htons|htonl|be16toh|be32toh|be64toh|le16toh"
        r"|le32toh|le64toh|bswap_16|bswap_32|bswap_64|__builtin_bswap16"
        r"|__builtin_bswap32|__builtin_bswap64)\s*\(",
        "the reader assembles bytes with shifts; there is no host order here",
        [],
    ),
    (
        "byte-order conditional",
        r"\b(?:__BYTE_ORDER__|__BYTE_ORDER|__BIG_ENDIAN__|__LITTLE_ENDIAN__"
        r"|BYTE_ORDER)\b",
        "a platform-dependent parse is one the cross container cannot check",
        [],
    ),
    (
        "memcpy into a scalar",
        r"\bmemcpy\s*\(\s*&",
        "a struct or scalar overlay on file data, in a different spelling",
        [],
    ),
    (
        "the blob's bytes",
        r"\bgfnt_blob_data\s*\(",
        "bytes enter the library through gfnt_reader_init_blob() and nowhere "
        "else",
        ["blob", "reader"],
    ),
    (
        "a reader's base pointer",
        r"(?:->|\.)base\b",
        "indexing the extent directly is exactly what the reader is for",
        ["reader"],
    ),
]

# What each pattern must still match, so that a pattern which has rotted into
# matching nothing is caught here rather than reported as a clean tree.
PLANTS = {
    "cast-and-dereference of file bytes": "x = *(const uint16_t *)(p + 2);",
    "host byte-order conversion": "x = ntohs(raw);",
    "byte-order conditional": "#if __BYTE_ORDER == __BIG_ENDIAN",
    "memcpy into a scalar": "memcpy(&value, p, sizeof value);",
    "the blob's bytes": "const uint8_t * p = gfnt_blob_data(blob);",
    "a reader's base pointer": "return reader->base[0];",
}


def strip_comments(text):
    """Remove /* */ and // comments, keeping line count intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                break
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def module_of(path):
    """The src/ subdirectory a file sits in, or "" for src/ itself."""
    relative = path.relative_to(src)
    return relative.parts[0] if len(relative.parts) > 1 else ""


def main():
    files = sorted(
        p for p in src.rglob("*") if p.suffix in (".c", ".h") and p.is_file())
    if not files:
        print("check-reader: found no sources under %s; this gate is "
              "measuring nothing" % src, file=sys.stderr)
        return 1

    problems = []
    covered = {}

    for name, pattern, why, exempt in CHECKS:
        regex = re.compile(pattern)
        plant = PLANTS[name]
        if not regex.search(strip_comments(plant)):
            problems.append(
                "the \"%s\" pattern no longer matches its own planted "
                "violation (%r); it would report a clean tree whatever the "
                "tree contained" % (name, plant))
            continue
        for path in files:
            text = strip_comments(path.read_text())
            hits = [
                i + 1 for i, line in enumerate(text.splitlines())
                if regex.search(line)
            ]
            if not hits:
                continue
            module = module_of(path)
            if module in exempt:
                covered.setdefault((name, module), 0)
                covered[(name, module)] += len(hits)
                continue
            for line in hits:
                problems.append("%s:%d: %s - %s" % (
                    path.relative_to(root), line, name, why))
        for module in exempt:
            if (name, module) not in covered:
                problems.append(
                    "src/%s/ is exempt from \"%s\" and no longer contains it; "
                    "delete the exemption rather than leaving an exclusion "
                    "nobody reads" % (module, name))

    if problems:
        print("\n### Reads that go around the checked reader ###\n",
              file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        print("\nEvery read of font bytes goes through GFNT_Reader "
              "(src/reader/reader.h).\nSee documentation/design.md section 6.",
              file=sys.stderr)
        return 1

    print("check-reader: %d source files scanned against %d patterns, each "
          "checked against a planted violation" % (len(files), len(CHECKS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
