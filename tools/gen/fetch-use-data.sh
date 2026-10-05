#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Fetch the data the Universal Shaping Engine shaper is generated from, at a pinned
# commit, check it against pinned hashes, and regenerate src/shape/use_data.h.
#
#     tools/gen/fetch-use-data.sh
#
# Microsoft publishes the two override files under the MIT licence at
# https://github.com/microsoft/font-tools/tree/master/USE. Raising the pin is
# deliberate: new overrides move categories, and a category that moves can move a
# syllable.
set -eu
COMMIT=9dcd14bcc9d9bb634b813fe06e1aaa2bc0209a7d
BASE=https://raw.githubusercontent.com/microsoft/font-tools/$COMMIT/USE
SHA_ISC=1f1d8748ebdf3e348bf7893ea0d5f3ac2544c6448a81457063c744e6faf431fa
SHA_IPC=58153c312c6a14851101d109ce5978c4aa81d09ac8ad97842ab10bb5b899747d
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/gen
mkdir -p "$OUT"
fetch() {
  [ -f "$OUT/$1" ] || curl -sfL -o "$OUT/$1" "$BASE/$1"
  echo "$2  $OUT/$1" | sha256sum -c - >/dev/null || { echo "$1 is not the pinned file" >&2; exit 1; }
}
fetch IndicSyllabicCategory-Additional.txt "$SHA_ISC"
fetch IndicPositionalCategory-Additional.txt "$SHA_IPC"
python3 "$HERE/use_data.py" "$OUT/IndicSyllabicCategory-Additional.txt" \
    "$OUT/IndicPositionalCategory-Additional.txt" > "$ROOT/src/shape/use_data.h"
echo "wrote src/shape/use_data.h"
