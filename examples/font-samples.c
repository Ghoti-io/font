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
 * Make text for a font, out of the characters it maps.
 *
 * Usage: font-samples <font>
 *
 * Prints a header line `#script <ISO 15924> <OpenType tag>` naming the script
 * most of the font's non-Latin characters belong to, then a dozen lines of text in
 * it, one per line, as UTF-8. The text is made of what the font maps: syllables
 * built from the Indic syllabic categories of the characters (a consonant, a
 * consonant and a vowel sign, a conjunct joined by a virama, with and without a
 * joiner, with a nukta, a bindu, a tone mark), and for a script that has none of
 * those, runs of its letters and marks. The same font always gives the same text.
 *
 * **It exists for `tools/oracle/hb_diff.py`**, which cannot write a corpus for the
 * hundred and thirty scripts the fonts it is pointed at are made for. A syllable
 * the script would never spell is as good a test as one it would: a shaper's
 * answer to a broken cluster is as much its behaviour as its answer to a good one.
 */

#include <ghoti.io/unicode/char.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/shape.h>

#define MAX_CHARS 4096

typedef struct Bucket {
  uint32_t cp[MAX_CHARS];
  size_t count;
} Bucket;

enum {
  B_CONSONANT, B_VOWEL_INDEPENDENT, B_VOWEL_DEPENDENT, B_VIRAMA, B_NUKTA,
  B_BINDU, B_TONE, B_MEDIAL, B_NUMBER, B_LETTER, B_MARK, B_OTHER, B_JOINER,
  B_COUNT
};

static uint64_t rng_state = 88172645463325252ull;

static uint64_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

static void add(Bucket * b, uint32_t cp) {
  if (b->count < MAX_CHARS) {
    b->cp[b->count++] = cp;
  }
}

static uint32_t pick(const Bucket * b) {
  return b->count ? b->cp[rng() % b->count] : 0;
}

/** Append a UTF-8 encoding of @p cp to @p out, at *@p n. */
static void put(char * out, size_t * n, uint32_t cp) {
  if (!cp) {
    return;
  }
  if (cp < 0x80) {
    out[(*n)++] = (char)cp;
  }
  else if (cp < 0x800) {
    out[(*n)++] = (char)(0xC0 | (cp >> 6));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000) {
    out[(*n)++] = (char)(0xE0 | (cp >> 12));
    out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  }
  else {
    out[(*n)++] = (char)(0xF0 | (cp >> 18));
    out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  }
}

/** One syllable, of a shape chosen by @p kind, in the characters of @p b. */
static void syllable(char * out, size_t * n, Bucket * b, unsigned kind) {
  uint32_t c = pick(&b[B_CONSONANT]);
  uint32_t v = pick(&b[B_VOWEL_DEPENDENT]);
  uint32_t vir = pick(&b[B_VIRAMA]);
  uint32_t zwj = 0x200D;
  uint32_t zwnj = 0x200C;

  if (!c) {
    c = pick(&b[B_LETTER]);
  }
  switch (kind % 18) {
    case 0: put(out, n, c); break;
    case 1: put(out, n, c); put(out, n, v); break;
    case 2: put(out, n, c); put(out, n, v); put(out, n, pick(&b[B_BINDU])); break;
    case 3: put(out, n, c); put(out, n, vir); put(out, n, pick(&b[B_CONSONANT])); break;
    case 4: put(out, n, c); put(out, n, vir); put(out, n, pick(&b[B_CONSONANT]));
        put(out, n, v); break;
    case 5: put(out, n, c); put(out, n, vir); put(out, n, pick(&b[B_CONSONANT]));
        put(out, n, vir); put(out, n, pick(&b[B_CONSONANT])); put(out, n, v); break;
    case 6: put(out, n, c); put(out, n, pick(&b[B_NUKTA])); put(out, n, v); break;
    case 7: put(out, n, pick(&b[B_VOWEL_INDEPENDENT])); break;
    case 8: put(out, n, pick(&b[B_VOWEL_INDEPENDENT]));
        put(out, n, pick(&b[B_BINDU])); break;
    case 9: put(out, n, c); put(out, n, vir); put(out, n, zwj);
        put(out, n, pick(&b[B_CONSONANT])); put(out, n, v); break;
    case 10: put(out, n, c); put(out, n, vir); put(out, n, zwnj);
        put(out, n, pick(&b[B_CONSONANT])); put(out, n, v); break;
    case 11: put(out, n, c); put(out, n, v); put(out, n, pick(&b[B_VOWEL_DEPENDENT]));
        put(out, n, pick(&b[B_TONE])); break;
    case 12: put(out, n, c); put(out, n, vir); break;
    case 13: put(out, n, c); put(out, n, pick(&b[B_MEDIAL]));
        put(out, n, pick(&b[B_CONSONANT])); put(out, n, v); break;
    case 14: put(out, n, c); put(out, n, pick(&b[B_MARK])); put(out, n, pick(&b[B_MARK]));
        break;
    case 15: put(out, n, pick(&b[B_LETTER])); put(out, n, pick(&b[B_LETTER]));
        put(out, n, pick(&b[B_MARK])); break;
    case 16: put(out, n, pick(&b[B_NUMBER])); put(out, n, pick(&b[B_NUMBER]));
        put(out, n, pick(&b[B_NUMBER])); break;
    default: put(out, n, c); put(out, n, pick(&b[B_NUKTA])); put(out, n, vir);
        put(out, n, pick(&b[B_CONSONANT])); put(out, n, v);
        put(out, n, pick(&b[B_TONE])); break;
  }
}

int main(int argc, char ** argv) {
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_Error error;
  static Bucket buckets[B_COUNT];
  uint32_t mapped[200000];
  GFNT_Tag tags[200000];
  size_t mapped_count = 0;
  GFNT_Tag best = 0;
  size_t best_count = 0;
  char iso[5];
  uint32_t cp;
  size_t line;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font>\n", argv[0]);
    return 2;
  }
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK
      || gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK) {
    fprintf(stderr, "%s: cannot open\n", argv[1]);
    return 1;
  }
  // What the font maps, and which script each belongs to.
  for (cp = 0x80; cp < 0x20000; cp++) {
    uint32_t glyph = 0;
    GFNT_Tag tag;

    if ((cp >= 0xD800 && cp < 0xE000)
        || gfnt_face_glyph_for_codepoint(face, cp, &glyph, NULL) != GFNT_OK
        || !glyph) {
      continue;
    }
    tag = gfnt_shape_script_of(&cp, 1);
    mapped[mapped_count] = cp;
    tags[mapped_count] = tag;
    mapped_count++;
  }
  // The script with the most characters, not counting the Latin, Greek and Cyrillic
  // every font has some of and not counting the ideographs.
  {
    size_t i;

    for (i = 0; i < mapped_count; i++) {
      size_t n = 0;
      size_t j;

      if (!tags[i] || tags[i] == GFNT_TAG('l', 'a', 't', 'n')
          || tags[i] == GFNT_TAG('g', 'r', 'e', 'k')
          || tags[i] == GFNT_TAG('c', 'y', 'r', 'l')) {
        continue;
      }
      for (j = 0; j < mapped_count; j++) {
        if (tags[j] == tags[i]) {
          n++;
        }
      }
      if (n > best_count) {
        best_count = n;
        best = tags[i];
      }
    }
  }
  if (!best || best_count < 8) {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    return 0;
  }
  {
    char tag[5] = {(char)(best >> 24), (char)(best >> 16), (char)(best >> 8),
        (char)best, 0};
    size_t i;

    for (i = 0; i < 4; i++) {
      iso[i] = (char)(i == 0 && tag[i] >= 'a' && tag[i] <= 'z' ? tag[i] - 32
                                                              : tag[i]);
    }
    iso[4] = 0;
    if (strcmp(tag, "nko ") == 0) {
      strcpy(iso, "Nkoo");
    }
    if (strcmp(tag, "lao ") == 0) {
      strcpy(iso, "Laoo");
    }
    printf("#script %s %s\n", iso, tag);
  }
  for (size_t i = 0; i < mapped_count; i++) {
    uint32_t u = mapped[i];
    GUNI_IndicSyllabicCategory isc;
    uint32_t gc;

    if (tags[i] != best) {
      continue;
    }
    isc = guni_indic_syllabic_category(u);
    gc = (uint32_t)guni_general_category(u);
    switch (isc) {
      case GUNI_INSC_CONSONANT: case GUNI_INSC_CONSONANT_DEAD:
      case GUNI_INSC_CONSONANT_HEAD_LETTER: case GUNI_INSC_CONSONANT_INITIAL_POSTFIXED:
      case GUNI_INSC_CONSONANT_FINAL: case GUNI_INSC_CONSONANT_WITH_STACKER:
        add(&buckets[B_CONSONANT], u); break;
      case GUNI_INSC_VOWEL_INDEPENDENT: case GUNI_INSC_VOWEL:
        add(&buckets[B_VOWEL_INDEPENDENT], u); break;
      case GUNI_INSC_VOWEL_DEPENDENT: add(&buckets[B_VOWEL_DEPENDENT], u); break;
      case GUNI_INSC_VIRAMA: case GUNI_INSC_PURE_KILLER:
      case GUNI_INSC_INVISIBLE_STACKER: add(&buckets[B_VIRAMA], u); break;
      case GUNI_INSC_NUKTA: add(&buckets[B_NUKTA], u); break;
      case GUNI_INSC_BINDU: case GUNI_INSC_VISARGA:
      case GUNI_INSC_SYLLABLE_MODIFIER: case GUNI_INSC_AVAGRAHA:
        add(&buckets[B_BINDU], u); break;
      case GUNI_INSC_TONE_MARK: case GUNI_INSC_TONE_LETTER:
      case GUNI_INSC_REGISTER_SHIFTER: add(&buckets[B_TONE], u); break;
      case GUNI_INSC_CONSONANT_MEDIAL: case GUNI_INSC_CONSONANT_SUBJOINED:
      case GUNI_INSC_CONSONANT_PRECEDING_REPHA: case GUNI_INSC_CONSONANT_PREFIXED:
      case GUNI_INSC_CONSONANT_SUCCEEDING_REPHA: add(&buckets[B_MEDIAL], u); break;
      case GUNI_INSC_NUMBER: add(&buckets[B_NUMBER], u); break;
      default: break;
    }
    // The coarser buckets, for a script that has no syllabic categories.
    if (gc == GUNI_GC_NONSPACING_MARK || gc == GUNI_GC_SPACING_MARK
        || gc == GUNI_GC_ENCLOSING_MARK) {
      add(&buckets[B_MARK], u);
    }
    else if (gc == GUNI_GC_OTHER_LETTER || gc == GUNI_GC_LOWERCASE_LETTER
        || gc == GUNI_GC_UPPERCASE_LETTER || gc == GUNI_GC_MODIFIER_LETTER) {
      add(&buckets[B_LETTER], u);
    }
    else if (gc == GUNI_GC_DECIMAL_NUMBER) {
      add(&buckets[B_NUMBER], u);
    }
  }
  rng_state ^= (uint64_t)best * 0x9E3779B97F4A7C15ull;
  for (line = 0; line < 14; line++) {
    char text[1024];
    size_t n = 0;
    unsigned words = 4 + (unsigned)(rng() % 4);

    for (unsigned w = 0; w < words; w++) {
      unsigned syllables = 1 + (unsigned)(rng() % 3);

      for (unsigned s = 0; s < syllables; s++) {
        syllable(text, &n, buckets, (unsigned)(rng() % 18));
      }
      if (w + 1 < words) {
        text[n++] = ' ';
      }
    }
    text[n] = 0;
    // A line with nothing in it (a font with none of what a pattern asks for) is
    // not worth a line.
    if (n > 3) {
      printf("%s\n", text);
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
