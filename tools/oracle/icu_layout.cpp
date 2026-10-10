/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * ICU's answers to the three questions paragraph layout asks of Unicode, for
 * `layout_diff.py`: the embedding level and script of every character, where a
 * line may and must break, and the visual order of the runs of one line.
 *
 * **This file links ICU and nothing else.** It is compiled inside the unicode
 * image (ICU 78.3) so that the only Unicode tables in the process are ICU's, as
 * `libs/unicode`'s own driver does.
 *
 * Requests, one per line on standard input, text as the hex of its UTF-8 bytes;
 * every offset is in *code points*, which is what this library's layout counts
 * in after it has decoded:
 *
 *   info  <ltr|rtl|auto> <hex>
 *       {"pl":LEVEL,"lv":[...],"sc":["Latn",...],"brk":[...],"hard":[...]}
 *       `brk` lists the offsets at which a line may break (before that
 *       character, the end of the text excluded), `hard` those at which it
 *       must.
 *   runs  <ltr|rtl|auto> <hex> <start> <end>
 *       {"runs":[[start,length,rtl],...]}
 *       the runs of the line [start, end) in visual order, with the line's
 *       trailing whitespace reset to the paragraph level (rule L1) by
 *       `ubidi_setLine`.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>

namespace {

int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Text {
  std::vector<UChar> u16;
  std::vector<int> cp_of_unit;   // code point index of each UTF-16 unit
  std::vector<int> unit_of_cp;   // first UTF-16 unit of each code point, plus the end
  std::vector<UChar32> cps;
};

bool decode(const char * hex, Text * t) {
  std::string bytes;
  while (hexval(hex[0]) >= 0 && hexval(hex[1]) >= 0) {
    bytes.push_back(static_cast<char>(hexval(hex[0]) * 16 + hexval(hex[1])));
    hex += 2;
  }
  int32_t cap = static_cast<int32_t>(bytes.size()) + 1;
  t->u16.resize(cap);
  int32_t len = 0;
  UErrorCode status = U_ZERO_ERROR;
  u_strFromUTF8(t->u16.data(), cap, &len, bytes.data(),
      static_cast<int32_t>(bytes.size()), &status);
  if (U_FAILURE(status)) {
    return false;
  }
  t->u16.resize(len);
  for (int32_t i = 0; i < len;) {
    UChar32 c;
    int32_t at = i;
    U16_NEXT(t->u16.data(), i, len, c);
    t->unit_of_cp.push_back(at);
    t->cps.push_back(c);
    for (int32_t k = at; k < i; ++k) {
      t->cp_of_unit.push_back(static_cast<int>(t->cps.size()) - 1);
    }
  }
  t->unit_of_cp.push_back(len);
  return true;
}

UBiDiLevel level_for(const char * dir) {
  if (strcmp(dir, "rtl") == 0) return 1;
  if (strcmp(dir, "auto") == 0) return UBIDI_DEFAULT_LTR;
  return 0;
}

}  // namespace

int main() {
  char * line = nullptr;
  size_t cap = 0;

  while (getline(&line, &cap, stdin) >= 0) {
    char mode[16], dir[8];
    char * hex = static_cast<char *>(malloc(strlen(line) + 1));
    int start = 0, end = 0;
    int fields = sscanf(line, "%15s %7s %s %d %d", mode, dir, hex, &start, &end);
    Text t;
    UErrorCode status = U_ZERO_ERROR;

    if (fields < 3 || !decode(hex, &t)) {
      printf("{\"error\":\"bad request\"}\n");
      free(hex);
      continue;
    }
    free(hex);
    const int32_t units = static_cast<int32_t>(t.u16.size());
    const int ncp = static_cast<int>(t.cps.size());
    UBiDi * bidi = ubidi_open();
    ubidi_setPara(bidi, t.u16.data(), units, level_for(dir), nullptr, &status);
    if (U_FAILURE(status)) {
      printf("{\"error\":\"bidi\"}\n");
      ubidi_close(bidi);
      continue;
    }
    if (strcmp(mode, "info") == 0) {
      const UBiDiLevel * levels = ubidi_getLevels(bidi, &status);
      printf("{\"pl\":%d,\"lv\":[", static_cast<int>(ubidi_getParaLevel(bidi)));
      for (int i = 0; i < ncp; ++i) {
        printf("%s%d", i ? "," : "", static_cast<int>(levels[t.unit_of_cp[i]]));
      }
      printf("],\"sc\":[");
      for (int i = 0; i < ncp; ++i) {
        UErrorCode e = U_ZERO_ERROR;
        UScriptCode sc = uscript_getScript(t.cps[i], &e);
        printf("%s\"%s\"", i ? "," : "", uscript_getShortName(sc));
      }
      printf("],\"brk\":[");
      UBreakIterator * brk = ubrk_open(UBRK_LINE, "en", t.u16.data(), units, &status);
      std::vector<int> may, must;
      if (U_SUCCESS(status)) {
        for (int32_t b = ubrk_first(brk); b != UBRK_DONE; b = ubrk_next(brk)) {
          if (b == 0 || b >= units) {
            continue;
          }
          int cp = t.cp_of_unit[b];
          may.push_back(cp);
          if (ubrk_getRuleStatus(brk) >= UBRK_LINE_HARD) {
            must.push_back(cp);
          }
        }
        ubrk_close(brk);
      }
      for (size_t i = 0; i < may.size(); ++i) {
        printf("%s%d", i ? "," : "", may[i]);
      }
      printf("],\"hard\":[");
      for (size_t i = 0; i < must.size(); ++i) {
        printf("%s%d", i ? "," : "", must[i]);
      }
      printf("]}\n");
    }
    else if (strcmp(mode, "runs") == 0 && fields == 5 && start >= 0 && end <= ncp
             && start < end) {
      UBiDi * part = ubidi_open();
      ubidi_setLine(bidi, t.unit_of_cp[start], t.unit_of_cp[end], part, &status);
      int32_t count = ubidi_countRuns(part, &status);
      printf("{\"runs\":[");
      for (int32_t r = 0; r < count; ++r) {
        int32_t at = 0, length = 0;
        UBiDiDirection d = ubidi_getVisualRun(part, r, &at, &length);
        // at is relative to the line's first unit.
        int first = t.cp_of_unit[t.unit_of_cp[start] + at];
        int last = t.cp_of_unit[t.unit_of_cp[start] + at + length - 1];
        printf("%s[%d,%d,%d]", r ? "," : "", first, last - first + 1,
            d == UBIDI_RTL ? 1 : 0);
      }
      printf("]}\n");
      ubidi_close(part);
    }
    else {
      printf("{\"error\":\"request\"}\n");
    }
    ubidi_close(bidi);
  }
  free(line);
  return 0;
}
