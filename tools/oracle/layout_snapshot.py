#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Field-level layout snapshots shared by the independent layout oracle cases."""
import gzip
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GOLDEN = os.path.join(ROOT, "tests", "data", "golden")
_RECORDS = {}


def _fields(layout):
    if "error" in layout:
        return {"error": layout["error"]}
    out = {k: layout[k] for k in ("min", "max", "w", "h", "mode", "truncated", "ink")
           if k in layout}
    out["lines"] = []
    for line in layout.get("lines", []):
        item = {k: line[k] for k in ("s", "e", "x", "w", "t", "hh", "baseline", "ascent",
                                    "descent", "justified", "truncated", "box", "ink")
                if k in line}
        item["runs"] = [{k: run[k] for k in ("span", "face_index", "fallback", "ellipsis",
                                            "hyphen", "l", "o", "start", "end", "pos",
                                            "advance", "g", "py", "b") if k in run}
                        for run in line.get("runs", [])]
        out["lines"].append(item)
    return out


def record(corpus, key, layout):
    _RECORDS.setdefault(corpus, []).append({"case": key, "layout": _fields(layout)})


def finish(corpus):
    mode = os.environ.get("GHOTI_LAYOUT_SNAPSHOT_MODE")
    if not mode:
        return
    path = os.path.join(GOLDEN, "layout-%s-snapshot.jsonl.gz" % corpus)
    records = _RECORDS.get(corpus, [])
    actual = "".join(json.dumps(r, ensure_ascii=False, sort_keys=True,
                                separators=(",", ":")) + "\n" for r in records)
    if mode == "write":
        with gzip.open(path, "wt", encoding="utf-8", compresslevel=9) as h:
            h.write(actual)
        return
    if mode != "check":
        raise ValueError("unknown GHOTI_LAYOUT_SNAPSHOT_MODE %r" % mode)
    with gzip.open(path, "rt", encoding="utf-8") as h:
        expected = h.read()
    if actual != expected:
        raise SystemExit("field-level %s layout snapshot differs (%s)" % (corpus, path))
