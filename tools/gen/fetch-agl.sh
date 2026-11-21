#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Fetch the Adobe Glyph List and the ITC Zapf Dingbats names at a pinned commit, check
# them against pinned hashes, and regenerate src/glyph/agl_data.h.
#
#     tools/gen/fetch-agl.sh
#
# Adobe publishes both under a three-clause BSD licence at
# https://github.com/adobe-type-tools/agl-aglfn (the notice is carried in
# documentation/agl-license.txt). Raising the pin is deliberate: a name that moves is a
# glyph whose text extraction moves.
set -eu
COMMIT=4036a9ca80a62f64f9de4f7321a9a045ad0ecfd6
BASE=https://raw.githubusercontent.com/adobe-type-tools/agl-aglfn/$COMMIT
SHA_AGL=a3b2f61ced9f3644cc0d4ecde5c59df34ca286c689d9484a43a710a81c466789
SHA_ZAPF=f6394e3cb8a447e84a1dad75d4baaf2aa7f45dc104faf369f4720e1a774ef2dc
SHA_LICENSE=58147d341e7a34aa2196862395a34d2fd95716c41d5ed26efb59ab0e12f92089
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/gen
mkdir -p "$OUT"
fetch() {
  [ -f "$OUT/$1" ] || curl -sfL -o "$OUT/$1" "$BASE/$1"
  echo "$2  $OUT/$1" | sha256sum -c - >/dev/null || { echo "$1 is not the pinned file" >&2; exit 1; }
}
fetch glyphlist.txt "$SHA_AGL"
fetch zapfdingbats.txt "$SHA_ZAPF"
fetch LICENSE.md "$SHA_LICENSE"
mkdir -p "$ROOT/documentation"
cp "$OUT/LICENSE.md" "$ROOT/documentation/agl-license.txt"
python3 "$HERE/agl_data.py" "$OUT/glyphlist.txt" "$OUT/zapfdingbats.txt" > "$ROOT/src/glyph/agl_data.h"
echo "wrote src/glyph/agl_data.h"
