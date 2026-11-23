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
 * BCP 47 language tags to OpenType script and language-system tags. See shape.h.
 */

#include <ghoti.io/font/shape.h>
#include <string.h>

/** One language: its default script and its OpenType language-system tag (`ISO 639` code to `ROM `, `TRK `...). */
typedef struct GFNT_LangRow {
  const char * code;
  const char * script;
  const char * language;
} GFNT_LangRow;

static const GFNT_LangRow gfnt_lang_rows[] = {
  {"en", "latn", "ENG "},
  {"de", "latn", "DEU "},
  {"fr", "latn", "FRA "},
  {"es", "latn", "ESP "},
  {"it", "latn", "ITA "},
  {"pt", "latn", "PTG "},
  {"nl", "latn", "NLD "},
  {"sv", "latn", "SVE "},
  {"da", "latn", "DAN "},
  {"no", "latn", "NOR "},
  {"nb", "latn", "NOR "},
  {"nn", "latn", "NYN "},
  {"fi", "latn", "FIN "},
  {"pl", "latn", "PLK "},
  {"cs", "latn", "CSY "},
  {"sk", "latn", "SKY "},
  {"hu", "latn", "HUN "},
  {"ro", "latn", "ROM "},
  {"bg", "cyrl", "BGR "},
  {"ru", "cyrl", "RUS "},
  {"uk", "cyrl", "UKR "},
  {"be", "cyrl", "BEL "},
  {"mk", "cyrl", "MKD "},
  {"sr", "cyrl", "SRB "},
  {"hr", "latn", "HRV "},
  {"bs", "latn", "BOS "},
  {"sl", "latn", "SLV "},
  {"sq", "latn", "SQI "},
  {"lt", "latn", "LTH "},
  {"lv", "latn", "LVI "},
  {"et", "latn", "ETI "},
  {"is", "latn", "ISL "},
  {"ga", "latn", "IRI "},
  {"cy", "latn", "WEL "},
  {"mt", "latn", "MTS "},
  {"ca", "latn", "CAT "},
  {"eu", "latn", "EUQ "},
  {"gl", "latn", "GAL "},
  {"oc", "latn", "OCI "},
  {"br", "latn", "BRE "},
  {"gd", "latn", "GAE "},
  {"lb", "latn", "LTZ "},
  {"fo", "latn", "FOS "},
  {"af", "latn", "AFK "},
  {"sw", "latn", "SWK "},
  {"id", "latn", "IND "},
  {"ms", "latn", "MLY "},
  {"tl", "latn", "TGL "},
  {"fil", "latn", "PIL "},
  {"vi", "latn", "VIT "},
  {"tr", "latn", "TRK "},
  {"az", "latn", "AZE "},
  {"uz", "latn", "UZB "},
  {"tk", "latn", "TKM "},
  {"la", "latn", "LAT "},
  {"el", "grek", "ELL "},
  {"ka", "geor", "KAT "},
  {"hy", "armn", "HYE "},
  {"he", "hebr", "IWR "},
  {"yi", "hebr", "JII "},
  {"ar", "arab", "ARA "},
  {"fa", "arab", "FAR "},
  {"ur", "arab", "URD "},
  {"ps", "arab", "PAS "},
  {"sd", "arab", "SND "},
  {"ku", "arab", "KUR "},
  {"kk", "cyrl", "KAZ "},
  {"ky", "cyrl", "KIR "},
  {"tg", "cyrl", "TAJ "},
  {"mn", "cyrl", "MNG "},
  {"hi", "deva", "HIN "},
  {"mr", "deva", "MAR "},
  {"ne", "deva", "NEP "},
  {"sa", "deva", "SAN "},
  {"bn", "beng", "BEN "},
  {"gu", "gujr", "GUJ "},
  {"pa", "guru", "PAN "},
  {"ta", "taml", "TAM "},
  {"te", "telu", "TEL "},
  {"kn", "knda", "KAN "},
  {"ml", "mlym", "MAL "},
  {"si", "sinh", "SNH "},
  {"th", "thai", "THA "},
  {"lo", "lao ", "LAO "},
  {"km", "khmr", "KHM "},
  {"my", "mymr", "BRM "},
  {"bo", "tibt", "TIB "},
  {"am", "ethi", "AMH "},
  {"ti", "ethi", "TGY "},
  {"ja", "kana", "JAN "},
  {"ko", "hang", "KOR "},
  {"zh", "hani", "ZHS "},
  {"yue", "hani", "ZHH "},
  {"cmn", "hani", "ZHS "},
};

static GFNT_Tag gfnt_lang_tag(const char * s) {
  return GFNT_TAG(s[0], s[1], s[2], s[3]);
}

/** One subtag, lower-cased in place into @p out, which holds 9. Returns its length. */
static size_t gfnt_lang_subtag(const char * tag, size_t length, size_t * at, char * out, bool * ok) {
  size_t n = 0;

  while (*at < length && tag[*at] != '-') {
    unsigned char c = (unsigned char)tag[*at];

    if (n >= 8u || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      *ok = false;
      return 0;
    }
    out[n++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    *at += 1u;
  }
  out[n] = '\0';
  if (n == 0) {
    *ok = false;
  }
  if (*at < length) {
    *at += 1u;   // the hyphen; a trailing one leaves the next subtag empty and fails there
    if (*at == length) {
      *ok = false;
    }
  }
  return n;
}

static bool gfnt_lang_alpha(const char * s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < 'a' || s[i] > 'z') {
      return false;
    }
  }
  return true;
}

/** An ISO 15924 script subtag as the OpenType script tag it is shaped with, or 0 for Common and Inherited. */
static GFNT_Tag gfnt_lang_script_tag(const char * s) {
  static const struct {
    const char * iso;
    const char * ot;
  } special[] = {
    {"hans", "hani"}, {"hant", "hani"}, {"hanb", "hani"}, {"hira", "kana"}, {"kana", "kana"}, {"hrkt", "kana"},
    {"jpan", "kana"}, {"kore", "hang"}, {"zyyy", NULL}, {"zinh", NULL}, {"zzzz", NULL},
  };

  for (size_t i = 0; i < sizeof special / sizeof special[0]; ++i) {
    if (strcmp(s, special[i].iso) == 0) {
      return special[i].ot ? gfnt_lang_tag(special[i].ot) : 0;
    }
  }
  return GFNT_TAG(s[0], s[1], s[2], s[3]);
}

GFNT_Result gfnt_language_from_bcp47(const char * tag, size_t length, GFNT_Tag * out_script, GFNT_Tag * out_language,
    bool * out_found) {
  char lang[9];
  char sub[9];
  char script[5] = "";
  char region[4] = "";
  char ext[4] = "";
  size_t at = 0;
  size_t n;
  size_t extlangs = 0;
  bool ok = true;
  const GFNT_LangRow * row = NULL;
  GFNT_Tag script_tag = 0;
  GFNT_Tag language_tag = 0;

  if (!out_script || !out_language || !out_found) {
    return GFNT_ERR_INVALID;
  }
  *out_script = 0;
  *out_language = 0;
  *out_found = false;
  if (!tag || length == 0) {
    return GFNT_ERR_INVALID;
  }
  n = gfnt_lang_subtag(tag, length, &at, lang, &ok);
  if (!ok) {
    return GFNT_ERR_INVALID;
  }
  if (n == 1u) {
    // "x-..." (private use) and "i-..." (an irregular grandfathered tag) are well formed and say nothing.
    return (lang[0] == 'x' || lang[0] == 'i') && at < length ? GFNT_OK : GFNT_ERR_INVALID;
  }
  if (n < 2u || n > 8u || !gfnt_lang_alpha(lang, n)) {
    return GFNT_ERR_INVALID;
  }
  while (at < length) {
    n = gfnt_lang_subtag(tag, length, &at, sub, &ok);
    if (!ok) {
      return GFNT_ERR_INVALID;
    }
    if (n == 1u) {
      break;   // an extension or private-use singleton ends what we read; the rest is only checked for shape below
    }
    if (n == 3u && extlangs < 3u && !script[0] && !region[0] && gfnt_lang_alpha(sub, n)) {
      if (extlangs == 0) {
        memcpy(ext, sub, 4);
      }
      extlangs += 1u;
    }
    else if (n == 4u && !script[0] && !region[0] && gfnt_lang_alpha(sub, n)) {
      memcpy(script, sub, 5);
    }
    else if (!region[0] && ((n == 2u && gfnt_lang_alpha(sub, n)) || (n == 3u && sub[0] >= '0' && sub[0] <= '9'))) {
      memcpy(region, sub, n + 1u);
    }
  }
  while (at < length) {
    // The rest (extensions, private use) is checked to be subtags of well-formed shape.
    gfnt_lang_subtag(tag, length, &at, sub, &ok);
    if (!ok) {
      return GFNT_ERR_INVALID;
    }
  }
  // An extended language subtag stands for the language it names: "zh-yue" is Cantonese.
  for (size_t i = 0; i < sizeof gfnt_lang_rows / sizeof gfnt_lang_rows[0]; ++i) {
    const char * want = extlangs && strcmp(lang, "zh") == 0 && strcmp(ext, "yue") == 0 ? "yue" : lang;

    if (strcmp(gfnt_lang_rows[i].code, want) == 0) {
      row = &gfnt_lang_rows[i];
      break;
    }
  }
  if (script[0]) {
    script_tag = gfnt_lang_script_tag(script);
  }
  else if (row) {
    script_tag = gfnt_lang_tag(row->script);
  }
  if (row) {
    language_tag = gfnt_lang_tag(row->language);
    if (strcmp(row->code, "zh") == 0 || strcmp(row->code, "cmn") == 0) {
      // OpenType separates Simplified, Traditional and Hong Kong Chinese; the script subtag speaks first,
      // then the region, and a bare "zh" is Simplified.
      bool hant = strcmp(script, "hant") == 0;
      bool hans = strcmp(script, "hans") == 0;
      bool hk = strcmp(region, "hk") == 0 || strcmp(region, "mo") == 0;
      bool tw = strcmp(region, "tw") == 0;

      if (hk && !hans) {
        language_tag = GFNT_TAG('Z', 'H', 'H', ' ');
      }
      else if (hant || (tw && !hans)) {
        language_tag = GFNT_TAG('Z', 'H', 'T', ' ');
      }
      else {
        language_tag = GFNT_TAG('Z', 'H', 'S', ' ');
      }
    }
    else if (strcmp(row->code, "ro") == 0 && strcmp(region, "md") == 0) {
      language_tag = GFNT_TAG('M', 'O', 'L', ' ');
    }
    *out_found = true;
  }
  *out_script = script_tag;
  *out_language = language_tag;
  return GFNT_OK;
}
