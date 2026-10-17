#ifndef GFNT_TESTS_HINT_FIXTURE_H
#define GFNT_TESTS_HINT_FIXTURE_H

/*
 * Faces for the hinting tests: a built-from-bytes TrueType face on a 640-unit em,
 * hinted at 10 pixels per em by default, so a font unit is one 26.6 unit and the
 * expected numbers can be worked out by hand.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <utility>
#include <vector>

#include <ghoti.io/font/outline.h>

extern "C" {
#include "../src/hint/vm.h"
}

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<uint8_t>;
using gfnttest::GlyfComponent;

enum : uint16_t {
  XY = 0x0002, ROUND = 0x0004, USE_MY = 0x0200, SCALED = 0x0800, UNSCALED = 0x1000,
  SCALE_FLAG = 0x0008,
};

Bytes op(std::initializer_list<int> opcodes) {
  Bytes out;
  for (int o : opcodes) out.push_back(static_cast<uint8_t>(o));
  return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

/** Push signed words. */
Bytes push(std::initializer_list<int> values) {
  Bytes out{0x41, static_cast<uint8_t>(values.size())};
  for (int v : values) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }
  return out;
}

struct Metric {
  uint16_t advance;
  int16_t bearing;
};

/** A face over the given glyphs, and a hinting context over it. */
struct Fixture {
  Bytes bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_HintContext * ctx = nullptr;
  GFNT_Error error{};

  Fixture(const std::vector<Bytes> & glyphs, const std::vector<Metric> & metrics,
          const Bytes & prep = {}, int version = 35, uint32_t ppem = 10,
          const Bytes & gasp = {}, const std::vector<gfnttest::Table> & extra = {},
          const std::vector<GFNT_F2Dot14> & location = {}, bool tolerant = false) {
    Bytes glyf;
    Bytes loca;
    bool is_long = false;
    Bytes maxp = gfnttest::build_maxp(static_cast<uint16_t>(glyphs.size()));
    std::vector<std::pair<uint16_t, int16_t>> hmtx;

    gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &is_long);
    gfnttest::patch_u16(maxp, 16, 8);    // twilight
    gfnttest::patch_u16(maxp, 18, 8);    // storage
    gfnttest::patch_u16(maxp, 20, 4);    // functions
    gfnttest::patch_u16(maxp, 22, 2);
    gfnttest::patch_u16(maxp, 24, 64);   // stack
    for (const Metric & m : metrics) hmtx.push_back({m.advance, m.bearing});
    std::vector<gfnttest::Table> tables = {
        {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(640, is_long ? 1 : 0)},
        {GFNT_TAG('h', 'h', 'e', 'a'),
            gfnttest::build_hhea(500, -100, 0, static_cast<uint16_t>(glyphs.size()))},
        {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(hmtx, {})},
        {GFNT_TAG('m', 'a', 'x', 'p'), maxp},
        {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
        {GFNT_TAG('l', 'o', 'c', 'a'), loca},
    };
    if (!prep.empty()) tables.push_back({GFNT_TAG('p', 'r', 'e', 'p'), prep});
    if (!gasp.empty()) tables.push_back({GFNT_TAG('g', 'a', 's', 'p'), gasp});
    for (const gfnttest::Table & t : extra) tables.push_back(t);
    bytes = gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_BORROWED,
        nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
    GFNT_Variation variation{location.data(), location.size(), GFNT_DELTA_ROUND_HALF_UP};

    EXPECT_EQ(gfnt_hint_ctx_from_face_at(face, ppem, version, true, tolerant, nullptr,
        location.empty() ? nullptr : &variation, &ctx, &error), GFNT_OK);
  }
  ~Fixture() {
    gfnt_hint_ctx_destroy(ctx);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/** One hinted glyph: points, status, advance. */
struct Result {
  GFNT_Outline * outline = nullptr;
  GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
  GFNT_HintFault fault{};
  int32_t advance = 0;
  GFNT_Result result = GFNT_ERR_INTERNAL;

  Result(const Fixture & f, uint32_t glyph) {
    GFNT_Error error{};
    result = gfnt_hint_glyph_outline(f.face, f.ctx, glyph, &outline, &status, &fault,
        &advance, nullptr, 0, nullptr, &error);
  }
  ~Result() { gfnt_outline_destroy(outline); }
  Result(const Result &) = delete;
  Result & operator=(const Result &) = delete;

  size_t count() const { return gfnt_outline_point_count(outline); }
  GFNT_Point at(size_t i) const {
    GFNT_Point p{};
    gfnt_outline_point_at(outline, i, &p, nullptr);
    return p;
  }
};

const std::vector<gfnttest::GlyfPoint> kSquare = {
    {40, 13, true}, {240, 13, true}, {240, 213, true}, {40, 213, true}};
const std::vector<gfnttest::GlyfPoint> kTriangle = {
    {0, 0, true}, {100, 0, true}, {50, 100, true}};

}  // namespace


#endif
