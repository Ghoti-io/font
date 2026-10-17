/**
 * @file
 *
 * The TrueType interpreter, from a font's three programs and a glyph's own.
 *
 * documentation/design.md section 8.5. A font's `fpgm`, `prep` and glyph programs are
 * untrusted code, and the interpreter is the one place in the library that runs
 * the input rather than reading it. What it has to survive is a program that
 * does anything: jumps into the middle of a push, calls itself, loops without
 * end, names a point or a control value that is not there, pops from an empty
 * stack, divides by zero, redefines an instruction.
 *
 * The input is a version and size, then `fpgm`, `prep` and a glyph program each
 * after a length byte, then the bytes the control values and the glyph's points are
 * made of. The face is built around them: one simple glyph with the program, and
 * a composite of it twice with a program of its own. Every glyph is hinted in a
 * tolerant context and a strict one.
 *
 * Besides "nothing crashes" it checks what no sanitizer sees: a hinted glyph has
 * the points and contours the unhinted one has; a glyph that was not hinted *is*
 * the unhinted one, to the point; a strict context never reports a program that
 * went wrong as hinted; asking twice gives one answer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/outline.h>

#include "../sfnt_builder.h"

namespace {

using Bytes = std::vector<uint8_t>;

/** A reader over the input that gives zeros when it runs out. */
struct Cursor {
  const uint8_t * data;
  size_t size;
  size_t at = 0;

  uint8_t byte() { return at < size ? data[at++] : 0; }
  Bytes take(size_t n) {
    Bytes out;
    while (n-- && at < size) out.push_back(data[at++]);
    return out;
  }
};

void fail(const char * what) {
  fprintf(stderr, "fuzz_hint: %s\n", what);
  abort();
}

/** The points of an outline, flat. */
std::vector<int32_t> points_of(const GFNT_Outline * outline) {
  std::vector<int32_t> out;
  for (size_t i = 0; i < gfnt_outline_point_count(outline); ++i) {
    GFNT_Point p{};
    GFNT_PointTag tag{};

    gfnt_outline_point_at(outline, i, &p, &tag);
    out.push_back(p.x);
    out.push_back(p.y);
    out.push_back(static_cast<int32_t>(tag));
  }
  return out;
}

struct Hinted {
  GFNT_Result result = GFNT_ERR_INTERNAL;
  GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
  std::vector<int32_t> points;
  size_t contours = 0;
  int32_t advance = 0;
};

Hinted hint(const GFNT_Face * face, const GFNT_HintContext * context, uint32_t glyph) {
  Hinted h;
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};

  h.result = gfnt_face_hinted_outline(face, context, glyph, nullptr, &outline, &h.status,
      &h.advance, &error);
  if (h.result == GFNT_OK) {
    h.points = points_of(outline);
    h.contours = gfnt_outline_contour_count(outline);
  }
  gfnt_outline_destroy(outline);
  return h;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  Cursor in{data, size};
  const uint8_t options = in.byte();
  const uint32_t ppem = 1u + (in.byte() % 96u);
  const Bytes fpgm = in.take(in.byte());
  const Bytes prep = in.take(in.byte());
  const Bytes program = in.take(in.byte());
  const Bytes composite_program = in.take(in.byte() % 64u);

  // The control values and the points.
  Bytes cvt;
  for (int i = 0; i < 16; ++i) {
    uint8_t high = in.byte();
    uint8_t low = in.byte();

    cvt.push_back(high);
    cvt.push_back(low);
  }
  std::vector<gfnttest::GlyfPoint> points;
  for (int i = 0; i < 12; ++i) {
    int16_t x = static_cast<int16_t>(in.byte() * 4 - 100);
    int16_t y = static_cast<int16_t>(in.byte() * 4 - 100);

    points.push_back({x, y, (in.byte() & 3) != 0});
  }

  Bytes simple = gfnttest::build_glyf_glyph({{points.begin(), points.begin() + 6},
      {points.begin() + 6, points.end()}}, program);
  Bytes composite = gfnttest::build_glyf_composite(
      {{0, 0x0003, static_cast<int16_t>(in.byte()), static_cast<int16_t>(in.byte()), {}},
       {0, static_cast<uint16_t>(0x0203 | ((options & 8) ? 0x0004 : 0)), 40, -30, {}}},
      composite_program);
  Bytes glyf;
  Bytes loca;
  bool is_long = false;
  Bytes maxp = gfnttest::build_maxp(2);

  gfnttest::build_glyf_and_loca({simple, composite}, &glyf, &loca, &is_long);
  gfnttest::patch_u16(maxp, 16, 8);                       // twilight points
  gfnttest::patch_u16(maxp, 18, 16);                      // storage
  gfnttest::patch_u16(maxp, 20, 8);                       // functions
  gfnttest::patch_u16(maxp, 22, 4);                       // instruction definitions
  gfnttest::patch_u16(maxp, 24, 64);                      // stack
  std::vector<gfnttest::Table> tables = {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, is_long ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 2)},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx({{500, 20}, {600, 0}}, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      {GFNT_TAG('c', 'v', 't', ' '), cvt},
  };
  if (!fpgm.empty()) tables.push_back({GFNT_TAG('f', 'p', 'g', 'm'), fpgm});
  if (!prep.empty()) tables.push_back({GFNT_TAG('p', 'r', 'e', 'p'), prep});
  Bytes font = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);

  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  if (gfnt_blob_create_memory(font.data(), font.size(), GFNT_BLOB_BORROWED, nullptr, nullptr,
          &blob, &error) != GFNT_OK) {
    return 0;
  }
  if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error) != GFNT_OK) {
    gfnt_blob_destroy(blob);
    return 0;
  }

  const GFNT_HintVersion version = (options & 1) ? GFNT_HINT_VERSION_35 : GFNT_HINT_VERSION_40;

  for (GFNT_HintMode mode : {GFNT_HINT_MODE_FONT, GFNT_HINT_MODE_FONT_STRICT}) {
    GFNT_HintContext * context = nullptr;

    if (gfnt_hint_context_create(face, ppem, version, mode, nullptr, &context, &error) != GFNT_OK) {
      continue;
    }
    for (uint32_t glyph = 0; glyph < 2; ++glyph) {
      Hinted first = hint(face, context, glyph);
      Hinted again = hint(face, context, glyph);
      GFNT_Outline * plain = nullptr;
      GFNT_Result drawn = gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &plain, &error);

      if (first.result != again.result || first.status != again.status
          || first.points != again.points || first.advance != again.advance) {
        fail("asking twice gave two answers");
      }
      if (first.result == GFNT_OK && drawn == GFNT_OK) {
        if (first.points.size() != 3u * gfnt_outline_point_count(plain)
            || first.contours != gfnt_outline_contour_count(plain)) {
          fail("a hinted glyph has other points than the glyph");
        }
        if (mode == GFNT_HINT_MODE_FONT_STRICT && first.status == GFNT_HINT_STATUS_HINTED_WITH_ERRORS) {
          fail("a strict context reported a program that went wrong as hinted");
        }
        // Of the simple glyph only: a composite is placed by its offsets, which the flag
        // `ROUND_XY_TO_GRID` rounds whether or not any program runs.
        if (glyph == 0 && (first.status == GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED
            || first.status == GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM)) {
          if (gfnt_outline_scale(plain, gfnt_scale_for_ppem(1000, ppem), &error) == GFNT_OK) {
            std::vector<int32_t> expected = points_of(plain);

            // An unhinted glyph is the unhinted one, but for the bearing it is placed by,
            // which moves every x together.
            if (!expected.empty() && expected.size() == first.points.size()) {
              int32_t shift = first.points[0] - expected[0];

              for (size_t i = 0; i < expected.size(); i += 3) {
                // Within two 26.6 units (the shift is taken from the first point, which carries its own): the hinter scales a font unit once, the outline scales
                // the 26.6 value it already holds, and the two round in a different place.
                if (std::abs(first.points[i] - expected[i] - shift) > 2
                    || std::abs(first.points[i + 1] - expected[i + 1]) > 1
                    || first.points[i + 2] != expected[i + 2]) {
                  fprintf(stderr, "glyph %u point %zu: hinted-path (%d,%d) outline (%d,%d) shift %d mode %d status %d ppem %u\n", glyph, i / 3, first.points[i], first.points[i + 1], expected[i], expected[i + 1], shift, (int)mode, (int)first.status, ppem);
                  fail("an unhinted glyph is not the scaled outline");
                }
              }
            }
          }
        }
      }
      gfnt_outline_destroy(plain);
    }
    gfnt_hint_context_destroy(context);
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return 0;
}
