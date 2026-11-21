#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Regenerate src/glyph/pdf_encodings.h: the six base encodings a PDF font names its codes by.
#
#     tools/gen/pdf-encodings.sh [ZAPF.afm]
#
# Five of them (StandardEncoding, WinAnsiEncoding, MacRomanEncoding, MacExpertEncoding and
# Symbol's built-in encoding) are read from Ghostscript by running it, which prints the 256
# names of each; none of Ghostscript's files is read. ZapfDingbats' built-in encoding is
# read from the URW++ clone's metrics file (the default is where Debian's fonts-urw-base35
# puts it). Every file is checked against a pinned hash, and the generator cross-checks
# what it can against a second source (see pdf_encodings.py), so that a different
# Ghostscript or a different file is noticed rather than absorbed.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/gen/pdf-encodings
ZAPF=${1:-/usr/share/fonts/type1/urw-base35/D050000L.afm}
SHA_STANDARD=ca72466b7da0d30730e0f26b6bd63fadc1e6b42e377794e5d3400e8a721f80e2
SHA_WINANSI=bdf8af8abb6c11474f9c17947a4848e127cf595227819e2d9a123155bee64c4d
SHA_MACROMAN=302d682a9f623bf38ca42f8f3bf74218d48889ab2070fc5af3c2fb5015b6e718
SHA_MACEXPERT=600261acf5e46b82d4abe74df3be487afb6c8307c4cde8fd004b3541df08148f
SHA_SYMBOL=ace96d16ead504eef49bd2ad1396b39e65e9cd5d22ae46a9c4066249255984a3
SHA_ZAPF=74dc56c84ed90216f0ebc4ab80fbff2d7863512ec8187b19889cbbd81b1d6de9
mkdir -p "$OUT"
dump() {
  gs -q -dNODISPLAY -dNOSAFER -c "/$1 findencoding { = } forall quit" > "$OUT/$1.txt"
  echo "$2  $OUT/$1.txt" | sha256sum -c - >/dev/null || { echo "$1 is not the pinned listing" >&2; exit 1; }
}
dump StandardEncoding "$SHA_STANDARD"
dump WinAnsiEncoding "$SHA_WINANSI"
dump MacRomanEncoding "$SHA_MACROMAN"
dump MacExpertEncoding "$SHA_MACEXPERT"
dump SymbolEncoding "$SHA_SYMBOL"
echo "$SHA_ZAPF  $ZAPF" | sha256sum -c - >/dev/null || { echo "$ZAPF is not the pinned file" >&2; exit 1; }
python3 "$HERE/pdf_encodings.py" "$OUT" "$ZAPF" "$ROOT/build/gen/glyphlist.txt" "$ROOT/build/gen/zapfdingbats.txt" \
    > "$ROOT/src/glyph/pdf_encodings.h"
echo "wrote src/glyph/pdf_encodings.h"
