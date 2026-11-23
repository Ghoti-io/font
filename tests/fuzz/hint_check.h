/**
 * @file
 *
 * What the fuzz harnesses for charstring-flavoured faces hold the hinter to.
 *
 * Shared by `fuzz_cff`, `fuzz_cff2` and `fuzz_type1`: the three formats reach one engine, and a check
 * written three times is a check that drifts. It asserts what no sanitizer sees: the stems and masks a
 * glyph states are within their bounds and index real points; a hinted outline is in pixels, has the
 * contours the unhinted one has, has a whole-pixel advance when it was hinted, and is the same when
 * asked twice.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GFNT_FUZZ_HINT_CHECK_H
#define GHOTI_IO_GFNT_FUZZ_HINT_CHECK_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/outline.h>

namespace gfntfuzz {

inline void hint_fail(const char * what) {
  fprintf(stderr, "hint check: %s\n", what);
  abort();
}

/** Hint @p glyph at @p ppem (at @p variation, or the default when NULL) and check it. */
inline void check_hinting(GFNT_Face * face, uint32_t glyph, uint32_t ppem, const GFNT_Variation * variation) {
  static GFNT_CharstringHints hints;
  GFNT_CharstringHintParams params;
  GFNT_Error error;
  GFNT_Outline * unhinted = nullptr;

  gfnt_error_clear(&error);
  if (gfnt_face_glyph_outline(face, glyph, variation, nullptr, &unhinted, &error) != GFNT_OK) {
    return;
  }
  memset(&hints, 0, sizeof hints);
  if (gfnt_face_glyph_hints(face, glyph, variation, &hints, &params, &error) == GFNT_OK) {
    if (hints.stem_count > GFNT_CS_MAX_STEMS || hints.mask_count > GFNT_CS_MAX_MASKS) {
      hint_fail("a hint record past its bounds");
    }
    if (hints.valid) {
      const size_t points = gfnt_outline_point_count(unhinted);
      uint32_t last = 0;

      for (size_t i = 0; i < hints.mask_count; ++i) {
        if (hints.masks[i].first_point > points || hints.masks[i].first_point < last) {
          hint_fail("a hint mask at a point that is not there, or out of order");
        }
        last = hints.masks[i].first_point;
      }
      if (hints.accent && hints.accent_first_point > points) {
        hint_fail("an accent that starts past the outline");
      }
    }
  }
  // The font's own hint program and the automatic hinter, which share every property checked here.
  for (GFNT_HintMode mode : {GFNT_HINT_MODE_FONT, GFNT_HINT_MODE_AUTO}) {
    GFNT_HintContext * context = nullptr;

    if (gfnt_hint_context_create(face, ppem, GFNT_HINT_VERSION_AUTO, mode, variation, &context, &error)
        == GFNT_OK) {
      GFNT_Outline * first = nullptr;
      GFNT_Outline * second = nullptr;
      GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
      GFNT_HintStatus again = GFNT_HINT_STATUS_HINTED;
      GFNT_F26Dot6 advance = 0;
      GFNT_F26Dot6 advance_again = 0;

      if (gfnt_face_hinted_outline(face, context, glyph, nullptr, &first, &status, &advance, &error) == GFNT_OK) {
        if (gfnt_outline_space(first) != GFNT_OUTLINE_PIXELS) {
          hint_fail("a hinted outline that is not in pixels");
        }
        if (gfnt_outline_contour_count(first) != gfnt_outline_contour_count(unhinted)) {
          hint_fail("a hinted outline with a different number of contours");
        }
        if (status == GFNT_HINT_STATUS_HINTED && advance % 64 != 0) {
          hint_fail("a hinted advance that is not a whole pixel");
        }
        if (gfnt_face_hinted_outline(face, context, glyph, nullptr, &second, &again, &advance_again, &error)
                != GFNT_OK
            || status != again || advance != advance_again
            || gfnt_outline_point_count(first) != gfnt_outline_point_count(second)) {
          hint_fail("a hinted glyph that is not the same when asked twice");
        }
        for (size_t i = 0; i < gfnt_outline_point_count(first); ++i) {
          GFNT_Point a{};
          GFNT_Point b{};

          gfnt_outline_point_at(first, i, &a, nullptr);
          gfnt_outline_point_at(second, i, &b, nullptr);
          if (a.x != b.x || a.y != b.y) {
            hint_fail("a hinted point that is not the same when asked twice");
          }
        }
      }
      gfnt_outline_destroy(first);
      gfnt_outline_destroy(second);
      gfnt_hint_context_destroy(context);
    }
  }
  gfnt_outline_destroy(unhinted);
}

}  // namespace gfntfuzz

#endif
