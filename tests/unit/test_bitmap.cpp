/**
 * @file
 *
 * The four standalone bitmap containers: PCF, BDF, PSF 1 and 2, and `.hex`.
 *
 * documentation/design.md sections 7.1 and 7.5.
 *
 * **What this suite leans on, and why it is not just more expectations of this
 * library's own.** Four unrelated files hold one design - eight glyphs of eight by
 * sixteen pixels - and four readers with no code in common must produce identical
 * pixels from them. A wrong shift, a reversed bit order or an off-by-one row would
 * have to be made in four places, the same way, to pass. That is the same argument
 * the PFB, PFA and CFF spellings of one Type 1 program make, and it is why the
 * fixtures were built as one design in the first place.
 *
 * `bitmap.pcf` and `bitmap-lsb.pcf` are the sharper case: the same glyphs with
 * their bits reversed, their bytes reversed, a two-byte scan unit and rows padded
 * to four. On a little-endian machine no font in the world exercises that path, so
 * the fixture is the only thing that can.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <memory>
#include <sstream>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>

#include <ghoti.io/font/bitmap.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/name.h>
#include <ghoti.io/font/raster.h>

#include <gtest/gtest.h>

#include "sfnt_builder.h"

#include "../../src/bitmap/bitmap.h"
#include "../../src/bitmap/eblc.h"
#include "../../src/blob/gzip.h"

namespace {

/** The shared design's shape, from tools/fixtures/make_fixtures.py. */
constexpr size_t kGlyphs = 8;
constexpr uint32_t kWidth = 8;
constexpr uint32_t kRows = 16;
constexpr int32_t kAscent = 14;
constexpr int32_t kDescent = -2;

/** A fixture read from disk, with a blob and a face over it. */
struct Fixture {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Fixture(const std::string & name) {
    gfnt_error_clear(&error);
    const std::string path = gfnttest::data("fonts/" + name);
    result = gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error);
    EXPECT_EQ(result, GFNT_OK) << "could not read " << path;
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    EXPECT_EQ(result, GFNT_OK) << name << ": " << error.message;
  }

  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

/** A face over bytes this test built, for the refusal arms. */
struct Crafted {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Crafted(const std::string & bytes,
      const GFNT_Limits * limits = nullptr) {
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_COPY, limits, nullptr, &blob, &error);
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, limits, nullptr, &face, &error);
  }

  ~Crafted() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }

  Crafted(const Crafted &) = delete;
  Crafted & operator=(const Crafted &) = delete;
};

/** A fixture's bytes, for the tests that damage one. */
std::string read_fixture(const std::string & name) {
  const std::string path = gfnttest::data("fonts/" + name);
  FILE * handle = fopen(path.c_str(), "rb");
  std::string out;
  if (!handle) {
    return out;
  }
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, handle)) > 0) {
    out.append(buffer, got);
  }
  fclose(handle);
  return out;
}

/**
 * @p data as a gzip member, built here rather than read from a fixture.
 *
 * A bomb is 256 KiB of zeros in 300 bytes, which no committed fixture should be:
 * `check-fixtures` would regenerate it every run and the repository would carry a
 * file whose only purpose is to be refused. So the two tests that need one build it,
 * with a **stored** deflate block for the header and a run-length one for the body -
 * the minimum of RFC 1951 that produces a member every inflater accepts.
 */
std::string gzip_of(const std::string & data) {
  // A gzip member wrapping a single dynamic-Huffman-free deflate stream is awkward
  // to write by hand, so this uses the deflate encoder the library already links:
  // the point of these tests is this library's *decoder* and its ceiling, and an
  // encoder from the same dependency is the shortest path to a legal member.
  void * out = nullptr;
  size_t size = 0;
  std::string result;

  if (gcomp_encode_alloc(nullptr, "gzip", nullptr, data.data(), data.size(),
          &out, &size)
      != GCOMP_OK) {
    return result;
  }
  result.assign(static_cast<const char *>(out), size);
  gcomp_buffer_free(nullptr, out);
  return result;
}

/** One glyph's pixels as a string of rows, for comparing containers. */
std::string pixels(const GFNT_Face * face, uint32_t glyph) {
  GFNT_BitmapGlyph bitmap{};
  GFNT_Error error{};
  std::string out;

  if (gfnt_face_glyph_bitmap(face, glyph, 0, &bitmap, &error) != GFNT_OK) {
    return std::string("refused: ") + error.message;
  }
  out += std::to_string(bitmap.width) + "x" + std::to_string(bitmap.height)
      + "\n";
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      out += gfnt_bitmap_pixel(&bitmap, x, y) ? '#' : '.';
    }
    out += '\n';
  }
  return out;
}

/** The containers that hold the shared design, in no particular order. */
const std::vector<std::string> & shared_design() {
  static const std::vector<std::string> names = {
      "bitmap.hex", "bitmap.psf", "bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf",
      "bitmap-swap.pcf",
  };
  return names;
}

/** The internal parse, for the two facts the public API deliberately hides. */
const GFNT_BitmapFont * parsed(const GFNT_Face * face) {
  const GFNT_BitmapFont * font = nullptr;
  if (gfnt_face_bitmap(face, &font, nullptr) != GFNT_OK) {
    return nullptr;
  }
  return font;
}

// --------------------------------------------------------------- one design

TEST(Bitmap, FourContainersDrawOneDesign) {
  // The whole argument of this suite. If the reference pixels below are wrong,
  // they are wrong in the fixture generator - and then four readers that share no
  // code would all have to be wrong the same way for this to pass.
  std::vector<std::vector<std::string>> each;
  for (const std::string & name : shared_design()) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    size_t glyphs = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK)
        << name;
    ASSERT_EQ(glyphs, kGlyphs) << name;

    std::vector<std::string> mine;
    for (uint32_t glyph = 0; glyph < kGlyphs; ++glyph) {
      mine.push_back(pixels(fixture.face, glyph));
    }
    each.push_back(mine);
  }
  ASSERT_EQ(each.size(), shared_design().size());
  for (size_t i = 1; i < each.size(); ++i) {
    EXPECT_EQ(each[0], each[i])
        << shared_design()[0] << " and " << shared_design()[i]
        << " hold one design and read differently";
  }
}

TEST(Bitmap, AGzippedPcfIsThePcfInsideIt) {
  // Every PCF in the world ships as `.pcf.gz` - all 234 in the oracle image do - so
  // this is not an extra feature but the shape the format is found in. The
  // assertion is that gzip decides *nothing*: same flavour, same strike, same
  // glyphs, same pixels, and the same directory the uncompressed file has.
  Fixture compressed("bitmap-gz.pcf.gz");
  Fixture plain("bitmap.pcf");
  ASSERT_EQ(compressed.result, GFNT_OK) << compressed.error.message;
  ASSERT_EQ(plain.result, GFNT_OK);

  EXPECT_EQ(gfnt_face_flavour(compressed.face), GFNT_FLAVOUR_PCF)
      << "the wrapper's flavour reached the face";
  EXPECT_EQ(gfnt_face_table_count(compressed.face),
      gfnt_face_table_count(plain.face));

  size_t glyphs = 0;
  size_t theirs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(compressed.face, &glyphs, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_num_glyphs(plain.face, &theirs, nullptr), GFNT_OK);
  ASSERT_EQ(glyphs, theirs);
  for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
    EXPECT_EQ(pixels(compressed.face, glyph), pixels(plain.face, glyph))
        << "glyph " << glyph;
  }

  // The offsets are into the *inflated* bytes, and the face says which blob it
  // reads: a tool that printed the file's size beside a table offset past it would
  // be printing two different things as one.
  size_t offset = 0;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_table_range(compressed.face, GFNT_TAG('B', 'M', 'A', 'P'),
      &offset, &length), GFNT_OK);
  EXPECT_GT(offset + length, gfnt_blob_size(compressed.blob))
      << "the fixture no longer compresses, so this asserts nothing";

  // And the cheap question answers without a face being built.
  size_t faces = 0;
  ASSERT_EQ(gfnt_face_count(compressed.blob, nullptr, &faces, nullptr), GFNT_OK);
  EXPECT_EQ(faces, 1u);
}

TEST(Bitmap, GzipIsRefusedWhenItIsNotGzip) {
  // The wrapper's own refusals. Each is a file that passes the three-byte magic and
  // then is not a gzip member, which is the only way to reach this layer's errors -
  // a file that fails the magic is handed to the container probes instead and comes
  // back as ERR_FORMAT.
  const std::string magic("\x1f\x8b\x08", 3);

  // A header and nothing else.
  {
    Crafted crafted(magic);
    EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT) << crafted.error.message;
    EXPECT_EQ(crafted.error.table, GFNT_TAG('G', 'Z', 'I', 'P'))
        << "the refusal did not name the layer it came from";
  }
  // A truncated member: the deflate stream stops mid-block.
  {
    std::string bytes = read_fixture("bitmap-gz.pcf.gz");
    ASSERT_GT(bytes.size(), 40u);
    Crafted crafted(bytes.substr(0, bytes.size() / 2));
    EXPECT_NE(crafted.result, GFNT_OK);
  }
  // A member whose trailing CRC and length do not match the data. RFC 1952 puts
  // both at the end, and a reader that ignored them would hand a corrupted font on
  // to the container probe, where it would be refused with the wrong diagnostic -
  // or worse, read.
  {
    std::string bytes = read_fixture("bitmap-gz.pcf.gz");
    ASSERT_GT(bytes.size(), 8u);
    bytes[bytes.size() - 5] = static_cast<char>(bytes[bytes.size() - 5] ^ 0xFF);
    Crafted crafted(bytes);
    EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT) << crafted.error.message;
  }
  // Something that inflates to bytes no container recognises: the wrapper succeeds
  // and the *probe* refuses, with its own message rather than the gzip layer's.
  {
    Crafted crafted(gzip_of(std::string(64, 'x')));
    EXPECT_EQ(crafted.result, GFNT_ERR_FORMAT) << crafted.error.message;
    EXPECT_NE(std::string(crafted.error.message).find("not an sfnt"),
        std::string::npos) << crafted.error.message;
  }
}

TEST(Bitmap, AnEmptyGzipMemberIsNotAFont) {
  // A legal gzip member holding nothing. It passes the magic, inflates, and yields
  // no bytes - so the wrapper refuses rather than handing an empty blob to the
  // container probe, where "not an sfnt" would be the wrong thing to say about a
  // file that is a perfectly good gzip of nothing.
  Crafted crafted(gzip_of(std::string()));
  EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT) << crafted.error.message;
  EXPECT_NE(std::string(crafted.error.message).find("no bytes at all"),
      std::string::npos) << crafted.error.message;
}

TEST(Bitmap, TheInflaterRefusesItsOwnCallerErrors) {
  // The internal entry point, called wrongly on purpose. Reached from nowhere else:
  // the face load only calls it after the magic has matched and with its own
  // limits, so these arms exist for the next caller rather than for this one.
  GFNT_Blob * blob = nullptr;
  GFNT_Blob * out = nullptr;
  GFNT_Error error{};
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  const std::string bytes = gzip_of(std::string("hello", 5));
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
      &limits, nullptr, &blob, &error), GFNT_OK);

  EXPECT_EQ(gfnt_gzip_inflate(nullptr, &limits, nullptr, &out, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_gzip_inflate(blob, nullptr, nullptr, &out, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_gzip_inflate(blob, &limits, nullptr, nullptr, &error),
      GFNT_ERR_INVALID);
  // And correctly, which is what says the refusals above are about the arguments.
  ASSERT_EQ(gfnt_gzip_inflate(blob, &limits, nullptr, &out, &error), GFNT_OK)
      << error.message;
  EXPECT_EQ(gfnt_blob_size(out), 5u);
  gfnt_blob_destroy(out);
  gfnt_blob_destroy(blob);
}

TEST(Bitmap, AGzipBombIsRefusedByTheBlobCeiling) {
  // The threat model's own case: a small file that inflates to more than a caller
  // agreed to hold. The cap is GFNT_Limits::max_blob_bytes - the same one a file
  // read from disk is held to, because what comes out of the inflater *is* the font
  // file from there on - and `compress` checks it before each enlargement, so this
  // is a refusal rather than an allocation that happens to fail.
  const std::string bomb = gzip_of(std::string(256 * 1024, '\0'));
  ASSERT_LT(bomb.size(), 2048u) << "the bomb did not compress, so it is not one";

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_blob_bytes = 4096;
  Crafted crafted(bomb, &limits);
  EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT) << crafted.error.message;

  // Under a ceiling that admits it, the same bytes inflate and are then refused for
  // what they are rather than for their size - which is what says the limit was the
  // thing that fired above.
  gfnt_limits_default(&limits);
  Crafted admitted(bomb, &limits);
  EXPECT_EQ(admitted.result, GFNT_ERR_FORMAT) << admitted.error.message;
}

TEST(Bitmap, TheTwoBitOrdersAreNotEachOther) {
  // The control for the identity above. Two glyphs of the design are a single
  // column - the leftmost and the rightmost - and under a reversed bit order each
  // reads as the other while **every other glyph still looks like a glyph**. So
  // this asserts which is which, in every container, rather than trusting that a
  // mistake would have been visible.
  for (const std::string & name : shared_design()) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    GFNT_BitmapGlyph left{};
    GFNT_BitmapGlyph right{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 4, 0, &left, nullptr),
        GFNT_OK) << name;
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 5, 0, &right, nullptr),
        GFNT_OK) << name;
    for (uint32_t y = 0; y < kRows; ++y) {
      EXPECT_EQ(gfnt_bitmap_pixel(&left, 0, y), 255u) << name << " row " << y;
      EXPECT_EQ(gfnt_bitmap_pixel(&left, kWidth - 1, y), 0u) << name;
      EXPECT_EQ(gfnt_bitmap_pixel(&right, kWidth - 1, y), 255u) << name;
      EXPECT_EQ(gfnt_bitmap_pixel(&right, 0, y), 0u) << name;
    }
  }
}

TEST(Bitmap, ABaselineIsStatedOrItIsNot) {
  // The honest half of the design. BDF states FONT_ASCENT and FONT_DESCENT and PCF
  // compiles them into its accelerators; PSF is a console cell and `.hex` is a row
  // of bits, and neither says where the baseline is. So the two pairs report
  // *different* metrics for the same pixels, and the flag says which is a
  // measurement and which is the box reported where it is.
  struct Case {
    const char * name;
    bool states;
  };
  const std::vector<Case> cases = {
      {"bitmap.bdf", true}, {"bitmap.pcf", true}, {"bitmap-lsb.pcf", true},
      {"bitmap-swap.pcf", true}, {"bitmap.hex", false}, {"bitmap.psf", false},
  };
  for (const Case & entry : cases) {
    Fixture fixture(entry.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << entry.name;

    const GFNT_BitmapFont * font = parsed(fixture.face);
    ASSERT_NE(font, nullptr) << entry.name;
    EXPECT_EQ(font->states_baseline, entry.states) << entry.name;

    GFNT_Strike strike{};
    ASSERT_EQ(gfnt_face_strike_at(fixture.face, 0, &strike, nullptr), GFNT_OK)
        << entry.name;
    EXPECT_EQ(strike.ppem_x, kRows) << entry.name;
    EXPECT_EQ(strike.ppem_y, kRows) << entry.name;
    EXPECT_EQ(strike.bit_depth, 1) << entry.name;
    EXPECT_EQ(strike.kind, GFNT_GLYPH_BITMAP_MONO) << entry.name;
    if (entry.states) {
      EXPECT_EQ(strike.ascent, kAscent) << entry.name;
      EXPECT_EQ(strike.descent, kDescent) << entry.name;
    }
    else {
      // The box, reported where it is: the whole height above the line, nothing
      // below it. Not Unifont's 14 and 2, which is Unifont's and not the format's.
      EXPECT_EQ(strike.ascent, static_cast<int32_t>(kRows)) << entry.name;
      EXPECT_EQ(strike.descent, 0) << entry.name;
    }

    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
        GFNT_OK) << entry.name;
    EXPECT_EQ(bitmap.bearing_y, strike.ascent) << entry.name;
    EXPECT_EQ(bitmap.bearing_x, 0) << entry.name;
    EXPECT_EQ(bitmap.advance, static_cast<int32_t>(kWidth)) << entry.name;
  }
}

TEST(Bitmap, EveryContainerIsItsOwnFlavourAndHasNoOutlines) {
  struct Case {
    const char * name;
    GFNT_Tag flavour;
  };
  const std::vector<Case> cases = {
      {"bitmap.hex", GFNT_FLAVOUR_HEX},
      {"bitmap-wide.hex", GFNT_FLAVOUR_HEX},
      {"bitmap.psf", GFNT_FLAVOUR_PSF},
      {"bitmap-v1.psf", GFNT_FLAVOUR_PSF},
      {"bitmap.bdf", GFNT_FLAVOUR_BDF},
      {"bitmap-ink.bdf", GFNT_FLAVOUR_BDF},
      {"bitmap.pcf", GFNT_FLAVOUR_PCF},
      {"bitmap-lsb.pcf", GFNT_FLAVOUR_PCF},
      {"bitmap-swap.pcf", GFNT_FLAVOUR_PCF},
  };
  for (const Case & entry : cases) {
    Fixture fixture(entry.name);
    ASSERT_EQ(fixture.result, GFNT_OK) << entry.name;

    EXPECT_EQ(gfnt_face_flavour(fixture.face), entry.flavour) << entry.name;
    EXPECT_FALSE(gfnt_face_has_outlines(fixture.face)) << entry.name;

    size_t strikes = 0;
    EXPECT_EQ(gfnt_face_strike_count(fixture.face, &strikes, nullptr), GFNT_OK)
        << entry.name;
    EXPECT_EQ(strikes, 1u) << entry.name;
  }
}

// ------------------------------------------------------------ what each says

TEST(Bitmap, PcfCarriesOneEntryPerTableItCanRead) {
  // The multi-entry synthetic directory (design.md section 7.1): a PCF's table of
  // contents is a list of extents, so each becomes a directory entry and each
  // table parse gets a reader over itself. A single entry spanning the file would
  // have worked and would have given every table the whole file to read.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('T', 'O', 'C', ' ')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('M', 'T', 'R', 'C')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('B', 'M', 'A', 'P')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('E', 'N', 'C', 'O')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('G', 'N', 'A', 'M')));
  EXPECT_TRUE(gfnt_face_has_table(fixture.face, GFNT_TAG('P', 'R', 'O', 'P')));
  // Absent from this one and present in the other, which is how the fixture pair
  // shows that a table the reader never asks for is skipped rather than stumbled
  // over.
  EXPECT_FALSE(gfnt_face_has_table(fixture.face, GFNT_TAG('I', 'M', 'T', 'R')));

  Fixture other("bitmap-lsb.pcf");
  ASSERT_EQ(other.result, GFNT_OK);
  EXPECT_TRUE(gfnt_face_has_table(other.face, GFNT_TAG('I', 'M', 'T', 'R')));

  // Every entry is bounded by the file, which is what the directory is for. A
  // table whose extent left the blob would have been refused at load.
  size_t offset = 0;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_table_range(fixture.face, GFNT_TAG('B', 'M', 'A', 'P'),
      &offset, &length), GFNT_OK);
  EXPECT_GT(length, 0u);
}

TEST(Bitmap, HexWidthComesFromTheDigitCount) {
  // Nothing in a `.hex` line states a width: the number of digits is the width,
  // and a reader that assumed eight pixels would read a CJK page as rubbish.
  Fixture fixture("bitmap-wide.hex");
  ASSERT_EQ(fixture.result, GFNT_OK);

  const std::vector<uint32_t> widths = {8, 16, 32};
  for (uint32_t glyph = 0; glyph < widths.size(); ++glyph) {
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, glyph, 0, &bitmap, nullptr),
        GFNT_OK) << glyph;
    EXPECT_EQ(bitmap.width, widths[glyph]) << glyph;
    EXPECT_EQ(bitmap.height, kRows) << glyph;
    EXPECT_EQ(bitmap.advance, static_cast<int32_t>(widths[glyph])) << glyph;
    EXPECT_EQ(bitmap.stride, widths[glyph] / 8u) << glyph;
  }
  // A six-digit codepoint, which is how every astral-plane glyph is written.
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x20000, &glyph,
      nullptr), GFNT_OK);
  EXPECT_EQ(glyph, 2u);
  // The 16- and 32-pixel glyphs set their outermost columns and nothing between,
  // so a scan that lost a byte would show here.
  GFNT_BitmapGlyph wide{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &wide, nullptr), GFNT_OK);
  for (uint32_t y = 0; y < kRows; ++y) {
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 0, y), 255u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 15, y), 255u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 7, y), 0u) << y;
    EXPECT_EQ(gfnt_bitmap_pixel(&wide, 8, y), 0u) << y;
  }
}

TEST(Bitmap, PsfMapsSeveralCodepointsToOneCellAndWalksPastSequences) {
  // One cell can stand for several characters, and for a *sequence* of them. A
  // sequence is not a codepoint and cannot be in a codepoint map, so it has to be
  // read past - refusing the font over one would refuse fonts the console draws.
  for (const char * name : {"bitmap.psf", "bitmap-v1.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 'A', &glyph, nullptr),
        GFNT_OK) << name;
    EXPECT_EQ(glyph, 2u) << name;
    // The second character of the same cell: a Greek capital alpha, which looks
    // like an A and in this font is one.
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x0391, &glyph,
        nullptr), GFNT_OK) << name;
    EXPECT_EQ(glyph, 2u) << name;
    // The combining acute of the sequence is **not** a mapping of its own.
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x0301, &glyph,
        nullptr), GFNT_OK) << name;
    EXPECT_EQ(glyph, 0u) << name << ": a sequence's members were mapped singly";
  }
}

TEST(Bitmap, Psf1CountComesFromOneBitOfTheModeByte) {
  // Version 1 states no glyph count anywhere else, and for a height of eight the
  // file would be the same length either way.
  Fixture fixture("bitmap-v1.psf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 512u);

  // And the glyphs past the shared design are there, which is what says the count
  // was believed rather than clamped to what the first 256 cells hold.
  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 511, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(bitmap.width, kWidth);
  EXPECT_EQ(bitmap.height, kRows);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 512, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Bitmap, BdfStatesABoxAndAnAdvancePerGlyph) {
  // What only the two per-glyph formats can say. Each of these is a real file
  // somewhere and a different way to be wrong.
  Fixture fixture("bitmap-ink.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(fixture.face, &glyphs, nullptr), GFNT_OK);
  ASSERT_EQ(glyphs, 5u);

  struct Expected {
    uint32_t glyph;
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    int32_t advance;
  };
  const std::vector<Expected> cases = {
      // An empty box with no BITMAP rows at all: a space, and its advance is
      // still its own.
      {0, 0, 0, 0, 0, 8},
      // Hangs two pixels left of the pen, and is wider than one byte.
      {1, 12, 14, -2, 10, 10},
      // A descender: BBX y is -4, so the box's *top* is at 8 and its bottom is
      // four rows below the baseline. Reading BBX y as the top would report 12.
      {2, 8, 12, 1, 8, 9},
      // An advance wider than the glyph, which is what a spacing accent has.
      {3, 4, 4, 0, 14, 12},
      {4, 8, 8, 0, 8, 8},
  };
  for (const Expected & entry : cases) {
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, entry.glyph, 0, &bitmap,
        nullptr), GFNT_OK) << entry.glyph;
    EXPECT_EQ(bitmap.width, entry.width) << entry.glyph;
    EXPECT_EQ(bitmap.height, entry.height) << entry.glyph;
    EXPECT_EQ(bitmap.bearing_x, entry.bearing_x) << entry.glyph;
    EXPECT_EQ(bitmap.bearing_y, entry.bearing_y) << entry.glyph;
    EXPECT_EQ(bitmap.advance, entry.advance) << entry.glyph;
    if (entry.height == 0) {
      EXPECT_EQ(bitmap.bits, nullptr) << entry.glyph;
      EXPECT_EQ(bitmap.stride, 0u) << entry.glyph;
    }
  }

  // ENCODING -1: a glyph with no character. It must be a glyph - dropping it
  // renumbers every glyph after it - and it must not be in the map.
  size_t mappings = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &mappings, nullptr),
      GFNT_OK);
  EXPECT_EQ(mappings, 4u) << "the unencoded glyph was given a codepoint";
  for (size_t i = 0; i < mappings; ++i) {
    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, i, nullptr, &glyph,
        nullptr), GFNT_OK);
    EXPECT_NE(glyph, 4u);
  }
  char * name = nullptr;
  ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 4, nullptr, &name, nullptr,
      nullptr), GFNT_OK);
  EXPECT_STREQ(name, "unencoded");
  gfnt_glyph_name_free(nullptr, name);
}

TEST(Bitmap, TheEncodingIsEnumerableAndSortedByCodepoint) {
  // A `cmap` can be enumerated and these formats otherwise could not be, which is
  // the one thing bitmap.h adds beyond the pixels.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &count, nullptr),
      GFNT_OK);
  ASSERT_EQ(count, kGlyphs);

  uint32_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t codepoint = 0;
    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, i, &codepoint, &glyph,
        nullptr), GFNT_OK) << i;
    if (i > 0) {
      EXPECT_GT(codepoint, previous) << i;
    }
    previous = codepoint;
    // And every pair round-trips through the lookup, which is the other half of
    // the same table.
    uint32_t looked_up = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, codepoint, &looked_up,
        nullptr), GFNT_OK) << i;
    EXPECT_EQ(looked_up, glyph) << i;
  }
  EXPECT_EQ(gfnt_face_bitmap_encoding_at(fixture.face, count, nullptr, nullptr,
      nullptr), GFNT_ERR_INVALID);

  // A codepoint the font does not have is glyph 0, as every cmap subtable
  // answers: a caller looping over a string cannot be asked to treat one
  // container's misses as errors and another's as zero.
  uint32_t missing = 99;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(fixture.face, 0x4E00, &missing,
      nullptr), GFNT_OK);
  EXPECT_EQ(missing, 0u);
}

TEST(Bitmap, NamesComeFromTheContainersThatStateThem) {
  // BDF names every character and PCF compiles those names into a table; PSF and
  // `.hex` state none at all, and the refusal says which of the two it is.
  for (const char * name : {"bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf",
      "bitmap-swap.pcf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * text = nullptr;
    ASSERT_EQ(gfnt_face_glyph_name(fixture.face, 2, nullptr, &text, nullptr,
        nullptr), GFNT_OK) << name;
    EXPECT_STREQ(text, "A") << name;
    gfnt_glyph_name_free(nullptr, text);

    uint32_t glyph = GFNT_GLYPH_NONE;
    ASSERT_EQ(gfnt_face_glyph_for_name(fixture.face, "checker", &glyph, nullptr),
        GFNT_OK) << name;
    EXPECT_EQ(glyph, 7u) << name;
    EXPECT_EQ(gfnt_face_glyph_for_name(fixture.face, "nosuchglyph", &glyph,
        nullptr), GFNT_ERR_INVALID) << name;
  }
  for (const char * name : {"bitmap.hex", "bitmap.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * text = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_glyph_name(fixture.face, 2, nullptr, &text, nullptr,
        &error), GFNT_ERR_UNSUPPORTED) << name;
    EXPECT_NE(std::string(error.message).find("states no glyph names"),
        std::string::npos) << name << ": " << error.message;
  }
}

TEST(Bitmap, TheFontsOwnNamesComeFromItsProperties) {
  // BDF states them as properties and PCF compiles the same properties into a
  // table, so both answer `name.h` - and the bytes are Latin-1, which is what
  // XLFD says a property string is.
  for (const char * name : {"bitmap.bdf", "bitmap.pcf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * family = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
        nullptr, &family, nullptr, nullptr), GFNT_OK) << name;
    EXPECT_STREQ(family, "Ghoti Fixture Bitmap") << name;
    gfnt_name_free(nullptr, family);

    char * copyright = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_COPYRIGHT,
        GFNT_LANGUAGE_ANY, nullptr, &copyright, nullptr, nullptr), GFNT_OK)
        << name;
    EXPECT_NE(std::string(copyright).find("LGPL-3.0-only"), std::string::npos)
        << name;
    gfnt_name_free(nullptr, copyright);

    // The XLFD name answers both "the full name" and "the PostScript name": it is
    // the one string that names the whole font, and a caller asking for the name
    // of this font should not be refused because the font is not PostScript.
    for (uint16_t id : {GFNT_NAME_FULL, GFNT_NAME_POSTSCRIPT}) {
      char * full = nullptr;
      ASSERT_EQ(gfnt_face_name(fixture.face, id, GFNT_LANGUAGE_ANY, nullptr,
          &full, nullptr, nullptr), GFNT_OK) << name << " id " << id;
      EXPECT_EQ(std::string(full).rfind("-Ghoti.io-", 0), 0u) << full;
      gfnt_name_free(nullptr, full);
    }

    char * weight = nullptr;
    ASSERT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_SUBFAMILY,
        GFNT_LANGUAGE_ANY, nullptr, &weight, nullptr, nullptr), GFNT_OK)
        << name;
    EXPECT_STREQ(weight, "Medium") << name;
    gfnt_name_free(nullptr, weight);

    // A name id neither format has a property for.
    char * version = nullptr;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_VERSION, GFNT_LANGUAGE_ANY,
        nullptr, &version, nullptr, &error), GFNT_ERR_UNSUPPORTED) << name;
  }
  // The two formats with nowhere to put a name at all.
  for (const char * name : {"bitmap.hex", "bitmap.psf"}) {
    Fixture fixture(name);
    ASSERT_EQ(fixture.result, GFNT_OK) << name;

    char * family = nullptr;
    EXPECT_EQ(gfnt_face_name(fixture.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
        nullptr, &family, nullptr, nullptr), GFNT_ERR_UNSUPPORTED) << name;
  }
}

// ------------------------------------------------------------- the refusals

TEST(Bitmap, FontUnitMetricsAreRefusedRatherThanAnsweredInPixels) {
  // The unit error this refusal exists to prevent: these functions' unit is font
  // units, a strike's advance is pixels, and a caller handed one for the other
  // lays out text at whatever ratio the em happened to be.
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  uint16_t upem = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_units_per_em(fixture.face, &upem, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("no em"), std::string::npos)
      << error.message;

  int32_t advance = 0;
  EXPECT_EQ(gfnt_face_glyph_advance(fixture.face, 2, nullptr, &advance, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("pixels"), std::string::npos)
      << error.message;

  int32_t bearing = 0;
  EXPECT_EQ(gfnt_face_glyph_side_bearing(fixture.face, 2, nullptr, &bearing,
      &error), GFNT_ERR_UNSUPPORTED);

  // And the pixels the refusal points at are there.
  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(bitmap.advance, static_cast<int32_t>(kWidth));
}

TEST(Bitmap, StrikePoliciesResolveAgainstTheOneStrike) {
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_Strike strike{};
  bool from_outlines = true;
  GFNT_Error error{};

  // OUTLINES_ONLY is the policy that cannot surprise a caller who never heard of
  // strikes, and on a font that is nothing but a strike it has to fail.
  EXPECT_EQ(gfnt_face_select_strike(fixture.face, 16, GFNT_STRIKE_OUTLINES_ONLY,
      &strike, &from_outlines, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("outlines only"), std::string::npos)
      << error.message;

  for (GFNT_StrikePolicy policy : {GFNT_STRIKE_EXACT, GFNT_STRIKE_NEAREST,
      GFNT_STRIKE_PREFER_STRIKE}) {
    from_outlines = true;
    ASSERT_EQ(gfnt_face_select_strike(fixture.face, 16, policy, &strike,
        &from_outlines, nullptr), GFNT_OK) << policy;
    EXPECT_FALSE(from_outlines) << policy;
    EXPECT_EQ(strike.ppem_y, kRows) << policy;
  }

  // EXACT is the one policy that can refuse a strike this face has, which is the
  // whole reason it exists beside NEAREST: a font drawn at sixteen pixels cannot
  // answer for thirteen, and M9 is what pretending otherwise looks like.
  EXPECT_EQ(gfnt_face_select_strike(fixture.face, 13, GFNT_STRIKE_EXACT, &strike,
      &from_outlines, nullptr), GFNT_ERR_UNSUPPORTED);
  ASSERT_EQ(gfnt_face_select_strike(fixture.face, 13, GFNT_STRIKE_NEAREST,
      &strike, &from_outlines, nullptr), GFNT_OK);
  EXPECT_EQ(strike.ppem_y, kRows);

  EXPECT_EQ(gfnt_face_strike_at(fixture.face, 1, &strike, nullptr),
      GFNT_ERR_INVALID);
  // A strike index a one-strike font does not have, which is every index but 0.
  GFNT_BitmapGlyph second{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 1, &second, &error),
      GFNT_ERR_INVALID);
  EXPECT_NE(std::string(error.message).find("exactly one strike"),
      std::string::npos) << error.message;
}

// ---------------------------------------------------------------------------
// EBLC: strikes inside an sfnt, and the first face with more than one.
//
// Everything above this line is a container that *is* a strike, so a strike index
// was always 0 and "the nearest strike" was always the only strike. strikes.ttf
// has three - 10, 12 and 16 ppem - and these are the tests that could not be
// written before it existed.

/** strikes.ttf, from tools/fixtures/make_fixtures.py. */
constexpr uint32_t kStrikePpems[] = {10, 12, 16};
constexpr size_t kStrikeCount = 3;

TEST(Eblc, TheStrikeListIsEveryBitmapSizeTable) {
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_strike_count(fixture.face, &count, &fixture.error),
      GFNT_OK) << fixture.error.message;
  ASSERT_EQ(count, kStrikeCount);

  for (size_t i = 0; i < kStrikeCount; ++i) {
    GFNT_Strike strike{};

    ASSERT_EQ(gfnt_face_strike_at(fixture.face, i, &strike, &fixture.error),
        GFNT_OK) << "strike " << i << ": " << fixture.error.message;
    EXPECT_EQ(strike.index, i);
    EXPECT_EQ(strike.ppem_x, kStrikePpems[i]);
    EXPECT_EQ(strike.ppem_y, kStrikePpems[i]);
    EXPECT_EQ(strike.bit_depth, 1);
    EXPECT_EQ(strike.kind, GFNT_GLYPH_BITMAP_MONO);
    // The strike's own baseline, from its sbitLineMetrics and not from `hhea`:
    // the generator writes the box's height as the ascent and -1 as the descent,
    // so the numbers differ per strike and a reader taking them from the face
    // would report the same pair three times.
    EXPECT_EQ(strike.ascent, (int32_t)kStrikePpems[i] - 4);
    EXPECT_EQ(strike.descent, -1);
  }
  EXPECT_EQ(gfnt_face_strike_at(fixture.face, kStrikeCount, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Eblc, NearestChoosesAmongSeveralStrikes) {
  // The test this whole fixture exists for. GFNT_STRIKE_NEAREST has been in the
  // API and documented since phase 0, and until strikes.ttf every face that had
  // a strike had exactly one - so the search was `strike_at(face, 0)` and no
  // input could tell a nearest-strike implementation from a first-strike one.
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  struct Want {
    uint32_t ppem;
    uint32_t expected;
    const char * why;
  };
  // 11 is the case that matters most: it is one away from 10 and one away from
  // 12, so the answer is a stated rule rather than whichever came first in the
  // table. Larger wins, because the alternative throws pixels away.
  const Want wants[] = {
    {1, 10, "far below every strike"},
    {9, 10, "just below the smallest"},
    {10, 10, "exactly the smallest"},
    {11, 12, "a tie between 10 and 12, broken upwards"},
    {12, 12, "exactly the middle"},
    {13, 12, "nearer 12 than 16"},
    {14, 16, "a tie between 12 and 16, broken upwards"},
    {15, 16, "nearer 16"},
    {16, 16, "exactly the largest"},
    {64, 16, "far above every strike"},
  };

  for (const Want & want : wants) {
    for (GFNT_StrikePolicy policy : {GFNT_STRIKE_NEAREST,
        GFNT_STRIKE_PREFER_STRIKE}) {
      GFNT_Strike strike{};
      bool from_outlines = true;

      ASSERT_EQ(gfnt_face_select_strike(fixture.face, want.ppem, policy,
          &strike, &from_outlines, &fixture.error), GFNT_OK)
          << want.ppem << ": " << fixture.error.message;
      EXPECT_FALSE(from_outlines) << want.why;
      EXPECT_EQ(strike.ppem_y, want.expected)
          << "at " << want.ppem << " ppem, policy " << policy << ": "
          << want.why;
    }
  }
}

TEST(Eblc, NearestAndPreferStrikeAgreeBecauseNothingScales) {
  // Deliberately recorded rather than left as an accident. The two policies
  // differ in whether the library may *scale* the strike it picked, and nothing
  // in this library scales a strike (raster.h says so). So they select
  // identically, and will keep doing so until something does - at which point
  // this test is the one that has to change, which is the point of it.
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  for (uint32_t ppem = 1; ppem <= 40; ++ppem) {
    GFNT_Strike nearest{};
    GFNT_Strike prefer{};

    ASSERT_EQ(gfnt_face_select_strike(fixture.face, ppem, GFNT_STRIKE_NEAREST,
        &nearest, nullptr, nullptr), GFNT_OK) << ppem;
    ASSERT_EQ(gfnt_face_select_strike(fixture.face, ppem,
        GFNT_STRIKE_PREFER_STRIKE, &prefer, nullptr, nullptr), GFNT_OK) << ppem;
    EXPECT_EQ(nearest.index, prefer.index) << "at " << ppem << " ppem";
  }
}

TEST(Eblc, ExactTakesTheOutlinesWhenNoStrikeMatches) {
  // A face with strikes *and* outlines, which no fixture had before: every
  // standalone container has no outlines at all, so EXACT's fallback has only
  // ever been exercised in its failing direction.
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);
  ASSERT_TRUE(gfnt_face_has_outlines(fixture.face));

  for (uint32_t ppem : {10u, 12u, 16u}) {
    GFNT_Strike strike{};
    bool from_outlines = true;

    ASSERT_EQ(gfnt_face_select_strike(fixture.face, ppem, GFNT_STRIKE_EXACT,
        &strike, &from_outlines, &fixture.error), GFNT_OK)
        << ppem << ": " << fixture.error.message;
    EXPECT_FALSE(from_outlines);
    EXPECT_EQ(strike.ppem_y, ppem);
  }
  for (uint32_t ppem : {9u, 11u, 13u, 17u}) {
    GFNT_Strike strike{};
    bool from_outlines = false;

    ASSERT_EQ(gfnt_face_select_strike(fixture.face, ppem, GFNT_STRIKE_EXACT,
        &strike, &from_outlines, &fixture.error), GFNT_OK)
        << ppem << ": " << fixture.error.message;
    EXPECT_TRUE(from_outlines) << "no strike at " << ppem << " and outlines "
        "are there to scale";
  }

  // And OUTLINES_ONLY ignores the strikes entirely, which on this face succeeds
  // where on a PCF it has to fail.
  GFNT_Strike strike{};
  bool from_outlines = false;
  ASSERT_EQ(gfnt_face_select_strike(fixture.face, 12, GFNT_STRIKE_OUTLINES_ONLY,
      &strike, &from_outlines, nullptr), GFNT_OK);
  EXPECT_TRUE(from_outlines);
}

TEST(Eblc, TheStrikesAreListedAndTheirPixelsAreNotReadYet) {
  // Three answers live on one path and the whole value of the path is that they
  // stay apart: a face with no strikes, a face whose strike table this library
  // cannot read, and - new here - a face whose strikes are listed and whose
  // glyph data is still out of reach.
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph glyph{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &glyph, &fixture.error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(fixture.error.table, GFNT_TAG('E', 'B', 'D', 'T'));
  EXPECT_NE(std::string(fixture.error.message).find("not read yet"),
      std::string::npos) << fixture.error.message;
}

TEST(Eblc, AnEblcWithNoEbdtIsStrikesNobodyCanRead) {
  // The pair rule, and the reason it is a pair: a strike list pointing at glyph
  // data the file does not contain is not a font with no bitmaps, and it is not
  // a font whose bitmaps this library can offer either. The same shape as `glyf`
  // without `loca`.
  std::vector<gfnttest::Table> tables;
  tables.push_back({GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head()});
  // A well-formed EBLC - version 2.0, no strikes - so that what the refusal is
  // about is the missing EBDT and not bytes that would fail to parse anyway.
  tables.push_back({GFNT_TAG('E', 'B', 'L', 'C'),
      std::vector<uint8_t>{0, 2, 0, 0, 0, 0, 0, 0}});
  const std::vector<uint8_t> bytes =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  Crafted crafted(std::string(reinterpret_cast<const char *>(bytes.data()),
      bytes.size()));
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 99;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_strike_count(crafted.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(count, 99u) << "nothing is written on failure";
  EXPECT_EQ(error.table, GFNT_TAG('E', 'B', 'L', 'C'));
}

TEST(Eblc, AnEblcThatListsNoStrikesIsZeroRatherThanARefusal) {
  // The pair is there and the list is empty, which is a well-formed table saying
  // the font has no bitmaps. That is a count of zero and not a refusal - the one
  // place in this file where zero is the honest answer for a face that has the
  // table.
  std::vector<gfnttest::Table> tables;
  tables.push_back({GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head()});
  tables.push_back({GFNT_TAG('E', 'B', 'L', 'C'),
      std::vector<uint8_t>{0, 2, 0, 0, 0, 0, 0, 0}});
  tables.push_back({GFNT_TAG('E', 'B', 'D', 'T'),
      std::vector<uint8_t>{0, 2, 0, 0}});
  const std::vector<uint8_t> bytes =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  Crafted crafted(std::string(reinterpret_cast<const char *>(bytes.data()),
      bytes.size()));
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 99;
  GFNT_Error error{};
  ASSERT_EQ(gfnt_face_strike_count(crafted.face, &count, &error), GFNT_OK)
      << error.message;
  EXPECT_EQ(count, 0u);
  GFNT_Strike strike{};
  EXPECT_EQ(gfnt_face_strike_at(crafted.face, 0, &strike, nullptr),
      GFNT_ERR_INVALID);
}

/**
 * An sfnt whose `EBLC` is these bytes, with an `EBDT` beside it.
 *
 * The pair is what makes the strike list readable at all, so a test about the
 * *list* needs an `EBDT` it never looks at - four bytes of version is enough, and
 * the one test about the missing half builds the font without this helper.
 */
std::string eblc_font(const std::vector<uint8_t> & eblc) {
  std::vector<gfnttest::Table> tables;
  tables.push_back({GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head()});
  tables.push_back({GFNT_TAG('E', 'B', 'L', 'C'), eblc});
  tables.push_back({GFNT_TAG('E', 'B', 'D', 'T'),
      std::vector<uint8_t>{0, 2, 0, 0}});
  const std::vector<uint8_t> bytes =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/**
 * One `bitmapSizeTable`, every field a parameter so a test can spoil one.
 *
 * Written here rather than copied from the generator because what these tests
 * need is a field at a time *wrong*, and a generator that could produce those
 * would be a generator with no opinion about what a font is.
 */
std::vector<uint8_t> size_table(uint32_t index_offset, uint32_t index_size,
    uint32_t subtables, uint16_t first, uint16_t last, uint8_t ppem_x,
    uint8_t ppem_y, uint8_t depth, int8_t flags, int8_t ascender = 6,
    int8_t descender = -1) {
  std::vector<uint8_t> out;
  gfnttest::put_u32(out, index_offset);
  gfnttest::put_u32(out, index_size);
  gfnttest::put_u32(out, subtables);
  gfnttest::put_u32(out, 0); // colorRef
  for (int direction = 0; direction < 2; ++direction) {
    // Horizontal first, then vertical. The vertical pair is zeros, which is what
    // the population states and what makes a reader taking the wrong one visible.
    const int8_t asc = direction == 0 ? ascender : 0;
    const int8_t desc = direction == 0 ? descender : 0;
    gfnttest::put_u8(out, (uint8_t)asc);
    gfnttest::put_u8(out, (uint8_t)desc);
    for (int i = 0; i < 10; ++i) {
      gfnttest::put_u8(out, 0);
    }
  }
  gfnttest::put_u16(out, first);
  gfnttest::put_u16(out, last);
  gfnttest::put_u8(out, ppem_x);
  gfnttest::put_u8(out, ppem_y);
  gfnttest::put_u8(out, depth);
  gfnttest::put_u8(out, (uint8_t)flags);
  return out;
}

/**
 * An `EBLC` header, @p count size tables, then an index region.
 *
 * The region is eight bytes of nothing and exists only so that a size table's
 * `indexSubTableArrayOffset` can point *inside* the table. Nothing in this commit
 * reads it - but every strike has to clear the one check that is made on the
 * offset, or a test aimed at some other field is refused for this one instead,
 * and would pass while asserting the wrong sentence.
 */
std::vector<uint8_t> eblc_header(uint32_t version, uint32_t count,
    const std::vector<uint8_t> & body = {}) {
  std::vector<uint8_t> out;
  gfnttest::put_u32(out, version);
  gfnttest::put_u32(out, count);
  out.insert(out.end(), body.begin(), body.end());
  for (int i = 0; i < 8; ++i) {
    gfnttest::put_u8(out, 0);
  }
  return out;
}

/** The offset a one-strike EBLC's index region starts at: header plus one table. */
constexpr uint32_t kOneStrikeIndexBase = 8 + 48;

TEST(Eblc, EveryWayAStrikeTableCanContradictItselfIsRefused) {
  // One field wrong at a time, each with the arm it is supposed to reach. Nine
  // refusals, and the point of the table is that each is a *different* sentence:
  // a reader that collapsed them into "corrupt EBLC" would pass a test that only
  // checked the code.
  struct Case {
    const char * why;
    std::vector<uint8_t> eblc;
    GFNT_Result result;
    const char * phrase;
  };
  const uint8_t ok_index[] = {0, 1, 0, 1, 0, 0, 0, 8};
  std::vector<uint8_t> one_strike = size_table(kOneStrikeIndexBase,
      sizeof ok_index, 1, 1, 1, 12, 12, 1, 1);

  const Case cases[] = {
    {"a table shorter than its own header",
     std::vector<uint8_t>{0, 2, 0, 0, 0, 0},
     GFNT_ERR_CORRUPT, "shorter than its own header"},
    {"a version this library has not been written against",
     eblc_header(0x00030000, 0),
     GFNT_ERR_UNSUPPORTED, "version this library does not read"},
    {"more strikes than the table has room for",
     eblc_header(0x00020000, 4, one_strike),
     GFNT_ERR_CORRUPT, "ends past the table"},
    {"a pixel size of zero across",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 0, 12, 1, 1)),
     GFNT_ERR_CORRUPT, "pixel size of zero"},
    {"a pixel size of zero down",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 0, 1, 1)),
     GFNT_ERR_CORRUPT, "pixel size of zero"},
    {"a bit depth that is not a power of two the format defines",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, 3, 1)),
     GFNT_ERR_CORRUPT, "bit depth"},
    {"a colour bit depth, which is CBLC's and not this table's",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, 32, 1)),
     GFNT_ERR_CORRUPT, "bit depth"},
    {"a glyph range that runs backwards",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 9, 2, 12, 12, 1, 1)),
     GFNT_ERR_CORRUPT, "runs backwards"},
    {"neither direction's line metrics claimed",
     eblc_header(0x00020000, 1,
         size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, 1, 0)),
     GFNT_ERR_CORRUPT, "neither horizontal nor vertical"},
    {"an index array that starts past the end of the table",
     eblc_header(0x00020000, 1,
         size_table(0x7FFFFFFF, 8, 1, 1, 1, 12, 12, 1, 1)),
     GFNT_ERR_CORRUPT, "past the table"},
  };

  for (const Case & test : cases) {
    Crafted crafted(eblc_font(test.eblc));
    ASSERT_EQ(crafted.result, GFNT_OK) << test.why;

    size_t count = 99;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_strike_count(crafted.face, &count, &error),
        test.result) << test.why;
    EXPECT_EQ(count, 99u) << test.why << ": nothing is written on failure";
    EXPECT_NE(std::string(error.message ? error.message : "").find(test.phrase),
        std::string::npos) << test.why << ": got " << error.message;
    EXPECT_EQ(error.table, GFNT_TAG('E', 'B', 'L', 'C')) << test.why;
  }
}

TEST(Eblc, AStrikeTableCutShortIsRefusedWhereverItRanOut) {
  // Every length a size table can be cut to, so that each of the twelve field
  // reads is the one that fails for some input - including the two
  // sbitLineMetrics, whose own failure arm no single truncation reaches.
  //
  // This is also what says there is no second guard: the first draft checked
  // count * 48 bytes up front, which refused all of these before a field was
  // read and left the read's own arm unreachable.
  const std::vector<uint8_t> whole = size_table(kOneStrikeIndexBase, 8, 1, 1, 1,
      12, 12, 1, 1);
  size_t refused = 0;

  for (size_t keep = 0; keep < whole.size(); ++keep) {
    std::vector<uint8_t> body(whole.begin(), whole.begin() + (long)keep);
    std::vector<uint8_t> eblc;
    gfnttest::put_u32(eblc, 0x00020000);
    gfnttest::put_u32(eblc, 1);
    eblc.insert(eblc.end(), body.begin(), body.end());

    Crafted crafted(eblc_font(eblc));
    ASSERT_EQ(crafted.result, GFNT_OK) << keep;
    size_t count = 99;
    GFNT_Error error{};
    EXPECT_EQ(gfnt_face_strike_count(crafted.face, &count, &error),
        GFNT_ERR_CORRUPT) << "cut to " << keep << " bytes";
    EXPECT_EQ(count, 99u) << keep;
    EXPECT_NE(std::string(error.message ? error.message : "").find(
                  "ends past the table"),
        std::string::npos) << "cut to " << keep << ": " << error.message;
    ++refused;
  }
  EXPECT_EQ(refused, whole.size()) << "every length from nothing to one byte "
      "short, and a bitmapSizeTable is 48 bytes";
}

TEST(Eblc, GreyDepthsAreGreyGlyphsAndOneBitIsMono) {
  // The kind a strike reports follows from its depth, and the four depths the
  // format defines split two ways. Nothing in the population has a depth but 1,
  // so these three are the only evidence the grey arm exists.
  struct Case {
    uint8_t depth;
    GFNT_GlyphKind kind;
  };
  const Case cases[] = {
    {1, GFNT_GLYPH_BITMAP_MONO},
    {2, GFNT_GLYPH_BITMAP_GRAY},
    {4, GFNT_GLYPH_BITMAP_GRAY},
    {8, GFNT_GLYPH_BITMAP_GRAY},
  };

  for (const Case & test : cases) {
    Crafted crafted(eblc_font(eblc_header(0x00020000, 1,
        size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, test.depth, 1))));
    ASSERT_EQ(crafted.result, GFNT_OK) << (unsigned)test.depth;

    GFNT_Strike strike{};
    ASSERT_EQ(gfnt_face_strike_at(crafted.face, 0, &strike, nullptr), GFNT_OK)
        << (unsigned)test.depth;
    EXPECT_EQ(strike.bit_depth, test.depth);
    EXPECT_EQ(strike.kind, test.kind) << "depth " << (unsigned)test.depth;
  }
}

TEST(Eblc, AVerticalOnlyStrikeReportsTheVerticalBaseline) {
  // flags bit 1 and not bit 0. The only input where the vertical sbitLineMetrics
  // is the one that answers, and the control for the horizontal case: the
  // generator writes the vertical pair as zeros in every fixture, so a reader
  // taking the wrong one reports 0/0 there and the right one here.
  Crafted crafted(eblc_font(eblc_header(0x00020000, 1,
      size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, 1, 2, 7, -3))));
  ASSERT_EQ(crafted.result, GFNT_OK);

  GFNT_Strike strike{};
  ASSERT_EQ(gfnt_face_strike_at(crafted.face, 0, &strike, nullptr), GFNT_OK);
  // size_table() writes the ascender it is given into the *horizontal* pair and
  // zeros into the vertical, so a vertical-only strike reports zero - which is
  // what this table states and is the point: the flags choose, and the choice is
  // visible.
  EXPECT_EQ(strike.ascent, 0);
  EXPECT_EQ(strike.descent, 0);
}

TEST(Eblc, MoreStrikesThanTheLimitAllowsIsALimit) {
  // Counted before anything is allocated, so a font claiming four billion strikes
  // costs the header and not the arithmetic. LIMIT and not CORRUPT: the table may
  // be perfectly well formed and simply larger than this caller allowed.
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_strikes = 2;

  std::vector<uint8_t> body;
  for (int i = 0; i < 3; ++i) {
    const std::vector<uint8_t> one = size_table(8 + 48 * 3, 8, 1, 1, 1,
        (uint8_t)(10 + i), (uint8_t)(10 + i), 1, 1);
    body.insert(body.end(), one.begin(), one.end());
  }
  const std::string bytes = eblc_font(eblc_header(0x00020000, 3, body));

  // Three strikes read fine at the default ceiling, which is the control: the
  // refusal below has to be the limit and not the bytes.
  Crafted allowed(bytes);
  ASSERT_EQ(allowed.result, GFNT_OK);
  size_t count = 0;
  ASSERT_EQ(gfnt_face_strike_count(allowed.face, &count, nullptr), GFNT_OK);
  EXPECT_EQ(count, 3u);

  Crafted crafted(bytes, &limits);
  ASSERT_EQ(crafted.result, GFNT_OK);
  GFNT_Error error{};
  count = 99;
  EXPECT_EQ(gfnt_face_strike_count(crafted.face, &count, &error),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(count, 99u);
  EXPECT_NE(std::string(error.message).find("max_strikes"), std::string::npos)
      << error.message;
}

TEST(Eblc, TheStrikeListSurvivesEveryRefusedAllocation) {
  // One allocation, so one request to refuse - and the sweep is written as a
  // sweep anyway, because a parse that grows an array later must not quietly stop
  // being covered here.
  const std::string bytes = read_fixture("strikes.ttf");
  ASSERT_FALSE(bytes.empty());

  for (size_t at = 0; at < 64; ++at) {
    gfnttest::FailingAllocator failing(at);
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    GFNT_Error error{};

    if (gfnt_blob_create_memory(bytes.data(), bytes.size(), GFNT_BLOB_COPY,
            nullptr, failing.get(), &blob, &error) != GFNT_OK) {
      continue;
    }
    if (gfnt_face_load(blob, 0, nullptr, failing.get(), &face, &error)
        != GFNT_OK) {
      gfnt_blob_destroy(blob);
      continue;
    }
    size_t count = 0;
    const GFNT_Result result =
        gfnt_face_strike_count(face, &count, &error);
    EXPECT_TRUE(result == GFNT_OK || result == GFNT_ERR_OOM)
        << "request " << at << ": " << error.message;
    if (result == GFNT_OK) {
      EXPECT_EQ(count, kStrikeCount) << "request " << at;
    }
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    EXPECT_EQ(failing.live(), 0u) << "request " << at;
  }
}

TEST(Eblc, TheDumpNamesEveryStrikeAndCarriesAWriteFailureOut) {
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  const GFNT_Eblc * eblc = nullptr;
  ASSERT_EQ(gfnt_face_eblc(fixture.face, &eblc, &fixture.error), GFNT_OK)
      << fixture.error.message;

  char buffer[2048] = {0};
  std::unique_ptr<FILE, int (*)(FILE *)> out(
      fmemopen(buffer, sizeof buffer, "w"), fclose);
  ASSERT_NE(out.get(), nullptr);
  ASSERT_EQ(gfnt_eblc_dump(eblc, out.get()), GFNT_OK);
  out.reset();

  const std::string text(buffer);
  EXPECT_NE(text.find("3 strikes"), std::string::npos) << text;
  for (uint32_t ppem : kStrikePpems) {
    EXPECT_NE(text.find("ppem " + std::to_string(ppem) + "x"
                  + std::to_string(ppem)),
        std::string::npos) << text;
  }

  EXPECT_EQ(gfnt_eblc_dump(nullptr, stdout), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_eblc_dump(eblc, nullptr), GFNT_ERR_INVALID);

  // Two fprintf arms, one before the loop and one inside it, and a sink that
  // refuses the nth write is what reaches each. Counted, because a sweep that
  // never ran out of budget would report zero failures and pass.
  size_t failures = 0;
  for (size_t allow = 0; allow < 6; ++allow) {
    gfnttest::FailingSink sink(allow);
    ASSERT_NE(sink.get(), nullptr);
    if (gfnt_eblc_dump(eblc, sink.get()) == GFNT_ERR_IO) {
      ++failures;
    }
  }
  // Four writes: the header and three strike lines. So budgets 0 through 3 fail
  // and 4 and 5 do not, which pins *where* the arms are and not just that they
  // exist.
  EXPECT_EQ(failures, 4u);
}

TEST(Eblc, ADirectoryEntryThatLeavesTheFileFailsTheLoadAndNotTheParse) {
  // Written to reach `gfnt_face_table_reader()`'s failure arm inside the EBLC
  // parse, and it cannot: the **face load** validates every entry's extent, so a
  // font whose EBLC runs past the end never produces a face to ask. That arm is
  // therefore unreachable by construction, exactly as the identical line in every
  // other table parser is, and this test is what says so rather than leaving the
  // line looking merely untested.
  //
  // It also pins the boundary the EBLC parse relies on: section 7.8's "each table
  // is validated on first use" is about a table's *contents*. Its extent is the
  // directory's business and is settled at load, which is why `indexTablesSize` -
  // a length stated *inside* EBLC - is the one length this parser has to bound
  // itself.
  std::string bytes = eblc_font(eblc_header(0x00020000, 1,
      size_table(kOneStrikeIndexBase, 8, 1, 1, 1, 12, 12, 1, 1)));

  // Arithmetic on the directory rather than a literal offset, for the reason
  // build_outline_broken_loca gives: a hard-coded offset patches the wrong table
  // the first time anything else about the font changes.
  const size_t count = ((unsigned char)bytes[4] << 8) | (unsigned char)bytes[5];
  size_t entry = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = 12 + 16 * i;
    if (memcmp(bytes.data() + at, "EBLC", 4) == 0) {
      entry = at;
      break;
    }
  }
  ASSERT_NE(entry, 0u) << "the directory has to name EBLC for this to mean "
      "anything";

  // The control: untouched, this font loads and lists its one strike. So the
  // refusal below is the overstated length and not anything else about the bytes.
  {
    Crafted control(bytes);
    ASSERT_EQ(control.result, GFNT_OK) << control.error.message;
    size_t strikes = 0;
    ASSERT_EQ(gfnt_face_strike_count(control.face, &strikes, nullptr), GFNT_OK);
    EXPECT_EQ(strikes, 1u);
  }

  for (size_t i = 0; i < 4; ++i) {
    bytes[entry + 12 + i] = (char)0x7F;
  }
  Crafted crafted(bytes);
  EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(crafted.face, nullptr);
}

/**
 * An allocator that counts, safely, from several threads at once.
 *
 * `gfnttest::FailingAllocator` counts with plain `size_t` members, which is right
 * for every other sweep in this suite and wrong here: eight threads incrementing
 * it lose updates, and the first version of the race test below read a live count
 * of **-1** with the code correct and a live count of 7 with it broken, both from
 * the same unsynchronised counter. Neither number meant anything.
 */
class ThreadSafeCounter {
public:
  ThreadSafeCounter() {
    allocator_.ctx = this;
    allocator_.malloc_fn = &ThreadSafeCounter::malloc_fn;
    allocator_.calloc_fn = &ThreadSafeCounter::calloc_fn;
    allocator_.realloc_fn = &ThreadSafeCounter::realloc_fn;
    allocator_.free_fn = &ThreadSafeCounter::free_fn;
  }

  const GFNT_Allocator * get() const { return &allocator_; }

  /** Blocks allocated and not yet freed. Zero at the end, or something leaked. */
  long live() const { return live_.load(); }
  /** How many allocations were served, for a denominator. */
  long requests() const { return requests_.load(); }

private:
  static void * malloc_fn(void * ctx, size_t size) {
    ThreadSafeCounter * self = static_cast<ThreadSafeCounter *>(ctx);
    void * block = malloc(size);
    if (block) {
      ++self->requests_;
      ++self->live_;
    }
    return block;
  }

  static void * calloc_fn(void * ctx, size_t count, size_t size) {
    ThreadSafeCounter * self = static_cast<ThreadSafeCounter *>(ctx);
    void * block = calloc(count, size);
    if (block) {
      ++self->requests_;
      ++self->live_;
    }
    return block;
  }

  static void * realloc_fn(void * ctx, void * block, size_t size) {
    ThreadSafeCounter * self = static_cast<ThreadSafeCounter *>(ctx);
    void * grown = realloc(block, size);
    if (grown && !block) {
      ++self->requests_;
      ++self->live_;
    }
    return grown;
  }

  static void free_fn(void * ctx, void * block) {
    ThreadSafeCounter * self = static_cast<ThreadSafeCounter *>(ctx);
    if (block) {
      --self->live_;
    }
    free(block);
  }

  GFNT_Allocator allocator_{};
  std::atomic<long> live_{0};
  std::atomic<long> requests_{0};
};

TEST(Eblc, RacingThreadsLeakNothingWhenOneLosesThePublication) {
  // The EBLC memo is the first one in this library that is **both** parsed lazily
  // and holds an allocation. Every allocating memo before it - the bitmap
  // containers', Type 1's - is parsed during the face load, where there is one
  // thread and no race to lose; and every lazily parsed memo before it holds
  // offsets into the blob and owns nothing.
  //
  // gfnt_table_cached() deliberately parses with no lock held, so two threads can
  // parse one table at once and only one publishes. The loser's scratch holds
  // everything its parse built, and dropping it is a leak that lives as long as
  // the face.
  //
  // **Two things here are what make the test able to see that, and the first
  // draft had neither.** Eight threads over strikes.ttf's three strikes never
  // raced once in thirty-two passes - spawning a thread takes longer than parsing
  // 152 bytes, so the first one was always finished before the second started, and
  // removing the release hook changed nothing. So: the threads wait on a barrier
  // and are released together, and the font carries the most strikes the default
  // limit allows, which makes the parse long enough to overlap. With both, the
  // dropped scratch is caught.
  constexpr std::ptrdiff_t kRaceThreads = 8;
  std::vector<uint8_t> body;
  const size_t strikes = 256;
  for (size_t i = 0; i < strikes; ++i) {
    const std::vector<uint8_t> one = size_table(
        (uint32_t)(8 + 48 * strikes), 8, 1, 1, 1,
        (uint8_t)(1 + i % 255), (uint8_t)(1 + i % 255), 1, 1);
    body.insert(body.end(), one.begin(), one.end());
  }
  const std::string bytes = eblc_font(eblc_header(0x00020000,
      (uint32_t)strikes, body));

  size_t raced = 0;
  // One face per pass: the race can only happen on a memo nobody has parsed yet,
  // and a second pass over the same face reads the published answer.
  for (int pass = 0; pass < 24; ++pass) {
    ThreadSafeCounter counter;
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    GFNT_Error error{};

    ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_COPY, nullptr, counter.get(), &blob, &error), GFNT_OK);
    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, counter.get(), &face, &error),
        GFNT_OK) << error.message;

    const long before = counter.requests();
    // A blocking latch and not a spin. The first version spun on an atomic, which
    // works on hardware and **hangs under valgrind**: its scheduler runs one
    // thread at a time, so eight threads busy-waiting on a ninth store make no
    // progress worth the cycles, and `make test-valgrind` stopped there rather
    // than failing. A latch parks them in the kernel and releases them together,
    // which is what the test wanted in the first place.
    std::latch start{kRaceThreads + 1};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kRaceThreads; ++i) {
      threads.emplace_back([face, &failures, &start, strikes]() {
        start.arrive_and_wait();
        size_t count = 0;
        if (gfnt_face_strike_count(face, &count, nullptr) != GFNT_OK
            || count != strikes) {
          ++failures;
        }
      });
    }
    start.arrive_and_wait();
    for (std::thread & thread : threads) {
      thread.join();
    }
    EXPECT_EQ(failures.load(), 0) << "pass " << pass;
    // More than one strike array allocated means more than one thread ran the
    // parse, which is the only situation this test is about.
    if (counter.requests() - before > 1) {
      ++raced;
    }

    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
    // The assertion. Without the release hook this is the strike array of however
    // many threads lost the race.
    EXPECT_EQ(counter.live(), 0L) << "pass " << pass;
  }
  // The denominator. A run where no pass raced proves nothing about the hook, and
  // would pass whether it was there or not - so say so rather than reporting
  // green. It is not an EXPECT_GT because a loaded machine can legitimately
  // serialise every pass; what must not happen silently is *believing* the test
  // ran.
  if (raced == 0) {
    GTEST_SKIP() << "no pass actually raced, so the release hook was never "
        "exercised; this says nothing either way";
  }
}

TEST(Eblc, ReleasingNothingIsHarmless) {
  // Called from gfnt_face_free() with a real allocator and a real table on every
  // other path, so the guards are reachable from here and nowhere else - and they
  // have to be right, because the memo contract in tables.h makes this function
  // run on a struct a refused parse left zeroed.
  GFNT_Eblc eblc{};
  gfnt_eblc_release(nullptr, &eblc);
  gfnt_eblc_release(gfnt_allocator_default(), nullptr);
  gfnt_eblc_release(gfnt_allocator_default(), &eblc);
  EXPECT_EQ(eblc.strikes, nullptr);
  EXPECT_EQ(eblc.strike_count, 0u);
}

TEST(Eblc, NullArgumentsAreRefusedByName) {
  Fixture fixture("strikes.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  const GFNT_Eblc * eblc = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_eblc(nullptr, &eblc, &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_eblc(fixture.face, nullptr, &error), GFNT_ERR_INVALID);
  EXPECT_FALSE(gfnt_face_has_eblc(nullptr));

  // A face with no EBLC at all refuses by name rather than answering an empty
  // table, which is the distinction gfnt_face_strike_count() reports as zero.
  Fixture plain("basic.ttf");
  ASSERT_EQ(plain.result, GFNT_OK);
  EXPECT_FALSE(gfnt_face_has_eblc(plain.face));
  EXPECT_EQ(gfnt_face_eblc(plain.face, &eblc, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("no EBLC"), std::string::npos)
      << error.message;
}

TEST(Bitmap, AnOutlineFaceHasNoBitmapsAndSaysSo) {
  // The distinction glyph.h exists to keep: "this font has no strikes" and "this
  // library cannot read this font's strikes" are both UNSUPPORTED and the
  // diagnostic separates them.
  Fixture fixture("basic.ttf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &bitmap, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("no bitmap strikes at all"),
      std::string::npos) << error.message;

  size_t count = 0;
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
}

// --------------------------------------------------------- the coverage bridge

TEST(Bitmap, AStrikeBecomesCoverageWithoutBeingScaled) {
  // So that a caller compositing a run needs one code path. The bearings are the
  // coverage's origin, which is the whole reason the conversion is three lines.
  Fixture fixture("bitmap.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);

  GFNT_Coverage coverage{};
  ASSERT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, nullptr),
      GFNT_OK);
  EXPECT_EQ(coverage.width, bitmap.width);
  EXPECT_EQ(coverage.height, bitmap.height);
  EXPECT_EQ(coverage.left, bitmap.bearing_x);
  EXPECT_EQ(coverage.top, bitmap.bearing_y);

  size_t set = 0;
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      const uint8_t pixel = gfnt_bitmap_pixel(&bitmap, x, y);

      EXPECT_EQ(gfnt_coverage_at(&coverage, x, y), pixel) << x << "," << y;
      if (pixel) {
        ++set;
      }
    }
  }
  // A 1-bit strike is either 0 or 255, so the total is the pixel count times 255
  // exactly - no partial coverage anywhere, which is the difference from a
  // rasterised outline.
  EXPECT_EQ(gfnt_coverage_total(&coverage), static_cast<uint64_t>(set) * 255u);
  gfnt_coverage_destroy(&coverage);

  // And the containers' coverages hash alike *within* a baseline group. The
  // hash covers `left` and `top`, so the two groups deliberately differ: BDF and
  // PCF put the box's top at 14 and PSF and `.hex` at 16, because the second pair
  // states no baseline. That is the documented consequence of the silence and not
  // a disagreement about pixels - the pixel comparison above is what says the
  // rows are identical.
  struct Group {
    std::vector<std::string> names;
    bool states_baseline;
  };
  const std::vector<Group> groups = {
      {{"bitmap.bdf", "bitmap.pcf", "bitmap-lsb.pcf", "bitmap-swap.pcf"}, true},
      {{"bitmap.hex", "bitmap.psf"}, false},
  };
  std::vector<uint64_t> per_group;
  for (const Group & group : groups) {
    std::vector<uint64_t> hashes;
    for (const std::string & name : group.names) {
      Fixture each(name);
      ASSERT_EQ(each.result, GFNT_OK) << name;
      uint64_t combined = 0;
      for (uint32_t glyph = 0; glyph < kGlyphs; ++glyph) {
        GFNT_BitmapGlyph one{};
        GFNT_Coverage converted{};
        ASSERT_EQ(gfnt_face_glyph_bitmap(each.face, glyph, 0, &one, nullptr),
            GFNT_OK) << name;
        ASSERT_EQ(gfnt_coverage_from_bitmap(&one, nullptr, &converted, nullptr),
            GFNT_OK) << name;
        EXPECT_EQ(converted.top, group.states_baseline ? kAscent
            : static_cast<int32_t>(kRows)) << name << " glyph " << glyph;
        combined = combined * 31u + gfnt_coverage_hash(&converted);
        gfnt_coverage_destroy(&converted);
      }
      hashes.push_back(combined);
    }
    for (size_t i = 1; i < hashes.size(); ++i) {
      EXPECT_EQ(hashes[0], hashes[i]) << group.names[i];
    }
    per_group.push_back(hashes[0]);
  }
  // And the two groups are *not* equal, which is the control: if this passed as
  // well, the position would not be in the hash and the check above would be
  // comparing pixels twice.
  EXPECT_NE(per_group[0], per_group[1]);
}


// ------------------------------------------------- crafted refusals: builders

/** A little-endian 32-bit number, which is what PCF's own integers are. */
void put32(std::string & out, uint32_t value, bool lsb = true) {
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned shift = lsb ? (8 * i) : (8 * (3 - i));

    out += static_cast<char>((value >> shift) & 0xFF);
  }
}

void put16(std::string & out, uint16_t value, bool lsb = true) {
  for (unsigned i = 0; i < 2; ++i) {
    const unsigned shift = lsb ? (8 * i) : (8 * (1 - i));

    out += static_cast<char>((value >> shift) & 0xFF);
  }
}

/**
 * The smallest PCF this library will read, with each table separately editable.
 *
 * Hand-built rather than generated, for the reason `tests/sfnt_builder.h` gives:
 * the refusal arms are bytes no writer emits, so a fixture generator cannot make
 * them. One glyph, 8 by 8, most-significant bit and byte first.
 */
struct PcfBuilder {
  // Bits most-significant first, bytes **least**, one-byte rows, a one-byte scan
  // unit. Little-endian content is what `put32` below writes, and this is the
  // combination that needs no byte swap - the swapping cases are the fixture
  // pair's, where the bytes come from the generator rather than from here.
  uint32_t format = 0x08;
  uint32_t metrics_format = 0x08;
  uint32_t metrics_count = 1;
  uint32_t bitmaps_count = 1;
  uint32_t names_count = 1;
  bool with_names = true;
  bool with_metrics = true;
  bool with_bitmaps = true;
  bool with_encodings = true;
  uint32_t toc_format_override = 0;  // 0: use each table's own
  uint32_t bitmap_offset = 0;
  uint32_t block_size_override = 0;
  uint16_t encoding_glyph = 0;
  uint16_t encoding_min = 0x41;
  uint16_t encoding_max = 0x41;
  uint16_t encoding_min_high = 0;
  uint16_t encoding_max_high = 0;
  int16_t left = 0;
  int16_t right = 8;
  int16_t ascent = 8;
  int16_t descent = 0;
  uint32_t toc_count_override = 0;
  /** Bytes to add to the last table's *stated* size without writing them. */
  uint32_t overstate_last = 0;
  /** A properties table to include verbatim, for the malformed ones. */
  std::string properties;
  /** Whether to include a table of a type this library does not read. */
  bool with_unknown_table = false;
  /** Cut this table's payload to this many bytes, keeping the rest of the file. */
  uint32_t cut_type = 0;
  uint32_t cut_to = 0;

  std::string build() const {
    std::vector<std::pair<uint32_t, std::string>> tables;

    if (with_metrics) {
      std::string payload;
      put32(payload, metrics_format);
      put32(payload, metrics_count);
      for (uint32_t i = 0; i < metrics_count; ++i) {
        put16(payload, static_cast<uint16_t>(left));
        put16(payload, static_cast<uint16_t>(right));
        put16(payload, 8);
        put16(payload, static_cast<uint16_t>(ascent));
        put16(payload, static_cast<uint16_t>(descent));
        put16(payload, 0);
      }
      tables.push_back({4, payload});
    }
    if (with_bitmaps) {
      std::string payload;
      const uint32_t rows = static_cast<uint32_t>(ascent + descent);
      put32(payload, format);
      put32(payload, bitmaps_count);
      for (uint32_t i = 0; i < bitmaps_count; ++i) {
        put32(payload, bitmap_offset);
      }
      const uint32_t block = block_size_override ? block_size_override
          : rows * bitmaps_count;
      for (unsigned pad = 0; pad < 4; ++pad) {
        put32(payload, block * (1u << pad));
      }
      payload.append(block, '\x81');
      tables.push_back({8, payload});
    }
    if (with_encodings) {
      std::string payload;
      put32(payload, format);
      put16(payload, encoding_min);
      put16(payload, encoding_max);
      put16(payload, encoding_min_high);
      put16(payload, encoding_max_high);
      put16(payload, 0xFFFF);
      for (uint32_t high = encoding_min_high; high <= encoding_max_high;
          ++high) {
        for (uint32_t code = encoding_min; code <= encoding_max; ++code) {
          put16(payload, encoding_glyph);
        }
      }
      tables.push_back({32, payload});
    }
    if (!properties.empty()) {
      tables.push_back({1, properties});
    }
    if (with_unknown_table) {
      // A type PCF does not define and this library does not read. X may add one,
      // and a reader that stumbled over it would refuse a font it can draw.
      std::string payload;
      put32(payload, format);
      put32(payload, 0);
      tables.push_back({1 << 20, payload});
    }
    if (with_names) {
      std::string payload;
      put32(payload, format);
      put32(payload, names_count);
      for (uint32_t i = 0; i < names_count; ++i) {
        put32(payload, 0);
      }
      put32(payload, 2);
      payload += std::string("A", 1) + std::string(1, '\0');
      tables.push_back({128, payload});
    }

    if (cut_type) {
      for (auto & table : tables) {
        if (table.first == cut_type && table.second.size() > cut_to) {
          table.second.resize(cut_to);
        }
      }
    }
    std::string out = "\x01" "fcp";
    put32(out, toc_count_override ? toc_count_override
        : static_cast<uint32_t>(tables.size()));
    uint32_t at = static_cast<uint32_t>(8 + 16 * tables.size());
    std::string body;
    for (const auto & table : tables) {
      std::string payload = table.second;
      payload.append((4 - payload.size() % 4) % 4, '\0');
      const bool last = &table == &tables.back();

      put32(out, table.first);
      put32(out, toc_format_override ? toc_format_override
          : (table.first == 4 ? metrics_format : format));
      put32(out, static_cast<uint32_t>(payload.size())
          + (last ? overstate_last : 0u));
      put32(out, at);
      at += static_cast<uint32_t>(payload.size());
      body += payload;
    }
    return out + body;
  }
};

/** A BDF with every keyword a valid one needs, so a test can spoil just one. */
struct BdfBuilder {
  std::string font = "-ghoti-test-medium-r-normal--8-80-75-75-c-80-iso10646-1";
  std::string box = "8 8 0 0";
  bool with_box = true;
  bool with_endfont = true;
  std::string chars = "1";
  bool with_chars = true;
  std::string glyph_name = "A";
  std::string encoding = "65";
  std::string bbx = "8 8 0 0";
  bool with_bbx = true;
  bool with_dwidth = true;
  bool with_bitmap = true;
  std::vector<std::string> rows = {"81", "42", "24", "18", "18", "24", "42",
      "81"};
  std::string extra;

  std::string build() const {
    std::string out = "STARTFONT 2.1\nFONT " + font + "\nSIZE 8 75 75\n";
    if (with_box) {
      out += "FONTBOUNDINGBOX " + box + "\n";
    }
    out += "STARTPROPERTIES 2\nFONT_ASCENT 8\nFONT_DESCENT 0\n"
           "ENDPROPERTIES\n";
    if (with_chars) {
      out += "CHARS " + chars + "\n";
    }
    out += "STARTCHAR " + glyph_name + "\nENCODING " + encoding + "\n";
    if (with_dwidth) {
      out += "DWIDTH 8 0\n";
    }
    if (with_bbx) {
      out += "BBX " + bbx + "\n";
    }
    out += extra;
    if (with_bitmap) {
      out += "BITMAP\n";
      for (const std::string & row : rows) {
        out += row + "\n";
      }
    }
    out += "ENDCHAR\n";
    if (with_endfont) {
      out += "ENDFONT\n";
    }
    return out;
  }
};

/** A PSF 2 with one 8x8 glyph. */
std::string psf2(uint32_t flags = 0, uint32_t version = 0,
    uint32_t header_size = 32, uint32_t count = 1, uint32_t charsize = 8,
    uint32_t height = 8, uint32_t width = 8,
    const std::string & table = std::string()) {
  std::string out;
  put32(out, 0x864AB572);
  put32(out, version);
  put32(out, header_size);
  put32(out, flags);
  put32(out, count);
  put32(out, charsize);
  put32(out, height);
  put32(out, width);
  out.append(header_size > 32 ? header_size - 32 : 0, '\0');
  out.append(static_cast<size_t>(count) * charsize, '\x5A');
  out += table;
  return out;
}

/** Every refusal, as (bytes, what the message must mention). */
struct Refusal {
  std::string bytes;
  const char * because;
  GFNT_Result result = GFNT_ERR_CORRUPT;
};

void expect_refusals(const std::vector<Refusal> & cases,
    const GFNT_Limits * limits = nullptr) {
  for (const Refusal & entry : cases) {
    Crafted crafted(entry.bytes, limits);

    EXPECT_EQ(crafted.result, entry.result) << entry.because;
    if (crafted.result != GFNT_OK) {
      EXPECT_NE(std::string(crafted.error.message).find(entry.because),
          std::string::npos)
          << "expected a message mentioning \"" << entry.because
          << "\", got \"" << crafted.error.message << "\"";
    }
  }
}

// --------------------------------------------------------- crafted refusals

TEST(Bitmap, PaddingBitsPastTheWidthAreCleared) {
  // Two faces of one design have to compare equal, and a container is entitled to
  // leave rubbish in the bits past the last pixel of a row - BDF calls them
  // undefined. Eight pixels is a whole byte, so the shared design cannot show
  // this; the ink fixture's 12-pixel glyph has a first row that sets all four.
  //
  // **This test could not fail until that row existed.** Deleting the masking in
  // `bitmap.c` left all twenty-eight tests passing, because every row of every
  // fixture already had zeros there. The fixture is the control.
  Fixture fixture("bitmap-ink.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &bitmap, nullptr),
      GFNT_OK);
  ASSERT_EQ(bitmap.width, 12u);
  ASSERT_EQ(bitmap.stride, 2u);
  // The row that carries them, first: twelve pixels set and four bits discarded.
  for (uint32_t x = 0; x < 12; ++x) {
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, x, 0), 255u) << x;
  }
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    EXPECT_EQ(bitmap.bits[y * bitmap.stride + 1] & 0x0F, 0)
        << "row " << y << " has bits set past the glyph's width";
  }

  // And a row written the same way by hand, so the assertion does not rest on one
  // fixture staying as it is.
  BdfBuilder builder;
  builder.bbx = "12 2 0 0";
  builder.box = "12 2 0 0";
  builder.rows = {"FFFF", "FFFF"};
  Crafted crafted(builder.build());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
  GFNT_BitmapGlyph crafted_glyph{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &crafted_glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(crafted_glyph.bits[1] & 0x0F, 0);
  EXPECT_EQ(crafted_glyph.bits[3] & 0x0F, 0);
}

TEST(Bitmap, HexRefusesWhatIsNotItsGrammar) {
  // The probe is the grammar, so most malformations are "not a font at all"
  // rather than a corrupt one: a file whose first line is not a record was never
  // claimed by this container. The ones that reach ERR_CORRUPT are files whose
  // *first* line is a record and whose later ones are not - which is the only way
  // to be a broken `.hex` rather than something else entirely.
  const std::string good = "0041:00000000000000000000000000000000\n";

  expect_refusals({
      {good + "0042:0000\n", "sixteen rows of hexadecimal"},
      {good + "0043:0000000000000000000000000000000G\n",
          "sixteen rows of hexadecimal"},
      {good + "00430000000000000000000000000000000\n",
          "sixteen rows of hexadecimal"},
      // Ten rows' worth of digits: a multiple of two, not a multiple of 32.
      {good + "0044:00000000000000000000\n", "sixteen rows of hexadecimal"},
      // Wider than the format's widest, which is where a reader that trusted the
      // digit count would allocate whatever it was told.
      {good + "0045:" + std::string(160, '0') + "\n",
          "sixteen rows of hexadecimal"},
  });

  // And these are not `.hex` at all, so the face load reports the format rather
  // than a corruption inside one.
  for (const std::string & bytes : {std::string("hello world\n"),
      std::string("0041\n"), std::string("41:0000\n"), std::string("\n\n\n")}) {
    Crafted crafted(bytes);
    EXPECT_EQ(crafted.result, GFNT_ERR_FORMAT) << bytes;
  }
}

TEST(Bitmap, HexRefusesALineLongerThanTheLimit) {
  // A limit rather than a corruption: a long line is a legal thing for a text
  // file to contain and GFNT_Limits is what says this reader will not walk it.
  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  limits.max_line_length = 40;

  Crafted crafted("0041:" + std::string(64, '0') + "\n", &limits);
  EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  EXPECT_NE(std::string(crafted.error.message).find("max_line_length"),
      std::string::npos) << crafted.error.message;
}

TEST(Bitmap, PsfRefusesAHeaderThatContradictsItself) {
  expect_refusals({
      // Version 1's mode byte, with a bit the format does not define.
      {std::string("\x36\x04\x40\x08", 4), "bits the format reserves"},
      {std::string("\x36\x04\x00\x00", 4), "no pixels in one direction"},
      // Version 2: a version number, a header that cannot hold the fields, and
      // the size that has to agree with the height and width.
      {psf2(0, 1), "version this library does not read",
          GFNT_ERR_UNSUPPORTED},
      {psf2(0, 0, 16), "smaller than the fields"},
      {psf2(0, 0, 32, 1, 9), "height times its width"},
      {psf2(0, 0, 32, 1, 0, 0), "no pixels in one direction"},
      // The count is believed and then checked against what the file holds.
      {psf2(0, 0, 32, 4).substr(0, 32 + 8), "ends before the glyph"},
  });
}

TEST(Bitmap, PsfRefusesAUnicodeTableItCannotRead) {
  expect_refusals({
      // An entry with no terminator at all: the table ends mid-glyph.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("A")),
          "ends before its terminator"},
      // A continuation byte with no lead byte.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\x80\xff", 2)),
          "bytes that are not UTF-8"},
      // U+0041 written in two bytes. An overlong form maps a second sequence to
      // one character, and then which glyph a lookup finds depends on which was
      // written.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\xc1\x81\xff", 3)),
          "bytes that are not UTF-8"},
      // A surrogate, which is not a character.
      {psf2(1, 0, 32, 1, 8, 8, 8, std::string("\xed\xa0\x80\xff", 4)),
          "bytes that are not UTF-8"},
  });

  // And the version 1 spelling of the same defect.
  Crafted unterminated(std::string("\x36\x04\x02\x08", 4)
      + std::string(256 * 8, '\0') + std::string("\x41\x00", 2));
  EXPECT_EQ(unterminated.result, GFNT_ERR_CORRUPT);
  EXPECT_NE(std::string(unterminated.error.message).find("terminator"),
      std::string::npos) << unterminated.error.message;
}

TEST(Bitmap, PsfWithoutAUnicodeTableStatesNoCharacters) {
  // Its glyph indices are positions in a console's character generator. Mapping
  // index to codepoint would invent a font that claims to hold U+0001.
  Crafted crafted(psf2());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 0;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("states no mapping"),
      std::string::npos) << error.message;

  uint32_t glyph = GFNT_GLYPH_NONE;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 'A', &glyph, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("console"), std::string::npos)
      << error.message;

  // The glyph itself is still there: an unmapped font is readable by index, which
  // is how `setfont` uses one.
  GFNT_BitmapGlyph bitmap{};
  EXPECT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
      GFNT_OK);
}

TEST(Bitmap, BdfRefusesEachMissingPieceByName) {
  {
    BdfBuilder builder;
    ASSERT_EQ(Crafted(builder.build()).result, GFNT_OK)
        << "the control does not parse, so every case below proves nothing";
  }
  std::vector<Refusal> cases;
  {
    BdfBuilder builder;
    builder.with_endfont = false;
    cases.push_back({builder.build(), "without ENDFONT"});
  }
  {
    BdfBuilder builder;
    builder.with_box = false;
    cases.push_back({builder.build(), "no FONTBOUNDINGBOX"});
  }
  {
    BdfBuilder builder;
    builder.chars = "4";
    cases.push_back({builder.build(), "than its CHARS states"});
  }
  {
    BdfBuilder builder;
    builder.with_bbx = false;
    builder.with_bitmap = false;
    cases.push_back({builder.build(), "no BBX"});
  }
  {
    BdfBuilder builder;
    builder.with_dwidth = false;
    cases.push_back({builder.build(), "no DWIDTH"});
  }
  {
    BdfBuilder builder;
    builder.bbx = "8 -8 0 0";
    cases.push_back({builder.build(), "negative size"});
  }
  {
    BdfBuilder builder;
    builder.bbx = "8 8";
    cases.push_back({builder.build(), "four numbers"});
  }
  {
    BdfBuilder builder;
    builder.rows.pop_back();
    cases.push_back({builder.build(), "fewer rows than its BBX"});
  }
  {
    BdfBuilder builder;
    builder.rows.push_back("FF");
    cases.push_back({builder.build(), "more rows than its BBX"});
  }
  {
    BdfBuilder builder;
    builder.rows[3] = "ZZ";
    cases.push_back({builder.build(), "not hexadecimal"});
  }
  {
    BdfBuilder builder;
    builder.rows[3] = "8";
    cases.push_back({builder.build(), "fewer digits than the glyph is wide"});
  }
  {
    BdfBuilder builder;
    builder.encoding = "";
    cases.push_back({builder.build(), "an ENCODING with no number"});
  }
  {
    BdfBuilder builder;
    builder.extra = "STARTCHAR B\n";
    cases.push_back({builder.build(), "never ended"});
  }
  {
    BdfBuilder builder;
    builder.with_bbx = false;
    cases.push_back({builder.build(), "before its BBX"});
  }
  expect_refusals(cases);

  // Not a BDF at all: the keyword has to be the first thing in the file, because
  // a scan that found STARTFONT further down would claim a shell script that
  // mentions one.
  EXPECT_EQ(Crafted("# a comment\nSTARTFONT 2.1\n").result, GFNT_ERR_FORMAT);
}

TEST(Bitmap, BdfBoxWithHeightAndNoWidthHasNoRowsToRead) {
  // `BBX 0 4 0 0`: a box with rows and no columns, whose BITMAP then has rows in it.
  // There is nothing in them to read and no buffer to read them into - the row
  // buffer is allocated from the box's byte count, which is zero - so the offset
  // arithmetic was `NULL + 0`, which is undefined behaviour that every compiler
  // computes correctly. **UBSan found it in the fuzzer**, on a shape no fixture had:
  // the glyph loads, so nothing failed and nothing was wrong with the answer.
  BdfBuilder builder;
  builder.bbx = "0 4 0 0";
  builder.box = "8 8 0 0";
  builder.rows = {"00", "00", "00", "00"};
  Crafted crafted(builder.build());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
      GFNT_OK);
  // No width means no pixels at all, whatever the height said.
  EXPECT_EQ(bitmap.width, 0u);
  EXPECT_EQ(bitmap.height, 0u);
  EXPECT_EQ(bitmap.bits, nullptr);
  EXPECT_EQ(bitmap.advance, 8);

  // And the rows are still counted, so a BITMAP with more of them than the BBX
  // states is still refused - which is what the permissive path must not cost.
  BdfBuilder extra;
  extra.bbx = "0 4 0 0";
  extra.box = "8 8 0 0";
  extra.rows = {"00", "00", "00", "00", "00"};
  Crafted refused(extra.build());
  EXPECT_EQ(refused.result, GFNT_ERR_CORRUPT) << refused.error.message;
  EXPECT_NE(std::string(refused.error.message).find("more rows than its BBX"),
      std::string::npos) << refused.error.message;
}

TEST(Bitmap, BdfAcceptsWhatTheSpecificationAllows) {
  // The other half of the refusals: three things that look like defects and are
  // not, each of which a stricter reader would refuse a real font over.
  {
    // A row padded to the font's bounding box rather than the glyph's, which is
    // what several writers emit.
    BdfBuilder builder;
    for (std::string & row : builder.rows) {
      row += "00";
    }
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
        GFNT_OK);
    EXPECT_EQ(bitmap.width, 8u);
  }
  {
    // CRLF line endings, and a lone CR, both of which are files that exist.
    BdfBuilder builder;
    std::string text = builder.build();
    std::string crlf;
    for (char c : text) {
      if (c == '\n') {
        crlf += '\r';
      }
      crlf += c;
    }
    EXPECT_EQ(Crafted(crlf).result, GFNT_OK) << "CRLF";
    std::string cr = text;
    for (char & c : cr) {
      if (c == '\n') {
        c = '\r';
      }
    }
    EXPECT_EQ(Crafted(cr).result, GFNT_OK) << "a lone CR";
  }
  {
    // A comment, and a property this library does not read.
    BdfBuilder builder;
    builder.extra = "COMMENT this glyph is a test\nSWIDTH 1000 0\n";
    EXPECT_EQ(Crafted(builder.build()).result, GFNT_OK);
  }
}

TEST(Bitmap, PcfRefusesATableThatDisagreesWithItsDirectory) {
  {
    PcfBuilder builder;
    Crafted control(builder.build());
    ASSERT_EQ(control.result, GFNT_OK)
        << "the control does not parse: " << control.error.message;
  }
  std::vector<Refusal> cases;
  {
    // The format word is written twice - once in the table of contents and once
    // at the head of the table - and it decides how every number after it is read.
    // A file where the two disagree is one where a reader would have to choose.
    PcfBuilder builder;
    builder.toc_format_override = 0x0C;
    cases.push_back({builder.build(), "disagrees with its directory entry"});
  }
  {
    PcfBuilder builder;
    builder.format = 0x08 | 0x30;  // an eight-byte scan unit
    builder.metrics_format = 0x08 | 0x30;
    cases.push_back({builder.build(), "scan unit of eight bytes"});
  }
  {
    PcfBuilder builder;
    builder.format = 0x08 | 0x00000400;
    builder.metrics_format = builder.format;
    cases.push_back({builder.build(), "format this library does not read",
        GFNT_ERR_UNSUPPORTED});
  }
  {
    PcfBuilder builder;
    builder.metrics_count = 0;
    cases.push_back({builder.build(), "no glyphs in it"});
  }
  {
    PcfBuilder builder;
    builder.bitmaps_count = 2;
    cases.push_back({builder.build(), "disagree about how many glyphs"});
  }
  {
    PcfBuilder builder;
    builder.names_count = 2;
    cases.push_back({builder.build(), "different number of glyphs than it has"});
  }
  {
    PcfBuilder builder;
    builder.encoding_glyph = 7;
    cases.push_back({builder.build(), "naming a glyph the font does not have"});
  }
  {
    PcfBuilder builder;
    builder.encoding_min = 0x50;
    builder.encoding_max = 0x41;
    cases.push_back({builder.build(), "range that runs backwards"});
  }
  {
    // An offset into the bitmap block, past its end. The block's own length is
    // the bound, and it is the one of four stated sizes that this format names.
    PcfBuilder builder;
    builder.bitmap_offset = 100;
    cases.push_back({builder.build(), "past the bitmap block"});
  }
  {
    PcfBuilder builder;
    builder.block_size_override = 4;
    cases.push_back({builder.build(), "past the bitmap block"});
  }
  {
    PcfBuilder builder;
    builder.left = 8;
    builder.right = 0;
    cases.push_back({builder.build(), "run backwards"});
  }
  {
    PcfBuilder builder;
    builder.toc_count_override = 1000;
    cases.push_back({builder.build(), "more tables than its own file can hold"});
  }
  expect_refusals(cases);

  // Without metrics there is no box for any bitmap, and without bitmaps there is
  // nothing to draw: both are "the font has no such table", which section 7.8
  // spells UNSUPPORTED.
  {
    PcfBuilder builder;
    builder.with_metrics = false;
    Crafted crafted(builder.build());
    EXPECT_EQ(crafted.result, GFNT_ERR_UNSUPPORTED) << crafted.error.message;
  }
  {
    PcfBuilder builder;
    builder.with_bitmaps = false;
    Crafted crafted(builder.build());
    EXPECT_EQ(crafted.result, GFNT_ERR_UNSUPPORTED) << crafted.error.message;
  }
  // A PCF with no encodings table is legal and is a font reached by index.
  {
    PcfBuilder builder;
    builder.with_encodings = false;
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    size_t count = 0;
    EXPECT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
        GFNT_ERR_UNSUPPORTED);
  }
  // And one with no glyph names is legal too, and then names are refused per
  // glyph rather than the font being refused.
  {
    PcfBuilder builder;
    builder.with_names = false;
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    char * name = nullptr;
    EXPECT_EQ(gfnt_face_glyph_name(crafted.face, 0, nullptr, &name, nullptr,
        nullptr), GFNT_ERR_UNSUPPORTED);
  }
  // Truncated at every length. Section 14.3's idea applied to a container whose
  // tables are its own - and what it asserts is **not** that every truncation is
  // refused, which was the first version of this loop and was wrong: a PCF's table
  // entries are clamped to the file (see
  // PcfStatedTableSizesAreNotBelievedPastTheFile), so a file missing only the
  // padding after its last table is readable, and so is one missing a table the
  // parse never needed.
  //
  // What must hold for every length is that the face either refuses or reads
  // consistently: every glyph it claims can be fetched, and every fetch stays
  // inside the file. The second half of that is what `make test-asan` and
  // valgrind check over this same loop; here it is the absence of a crash and the
  // agreement between the count and what can be read.
  {
    const std::string whole = PcfBuilder().build();
    size_t readable = 0;
    for (size_t length = 1; length < whole.size(); ++length) {
      Crafted crafted(whole.substr(0, length));

      if (crafted.result != GFNT_OK) {
        continue;
      }
      ++readable;
      size_t glyphs = 0;
      ASSERT_EQ(gfnt_face_num_glyphs(crafted.face, &glyphs, nullptr), GFNT_OK)
          << length;
      for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
        GFNT_BitmapGlyph bitmap{};

        EXPECT_EQ(gfnt_face_glyph_bitmap(crafted.face, glyph, 0, &bitmap,
            nullptr), GFNT_OK) << length << " glyph " << glyph;
      }
    }
    // A handful rather than none and rather than most: the file is 164 bytes and
    // only the last few cuts remove nothing the parse reads. Asserting the count
    // rather than "some" is what would catch a clamp that started swallowing real
    // truncations.
    EXPECT_LE(readable, 4u) << "the clamp accepted " << readable
                            << " truncations, which is more than its padding";
    EXPECT_GE(readable, 1u);
  }
}

TEST(Bitmap, PcfStatedTableSizesAreNotBelievedPastTheFile) {
  // **Every PCF in the world overstates one table's size.** `bdftopcf` writes the
  // last entry's size as that of an accelerator table with ink bounds whether it
  // wrote those or not, so the final table of every Terminus font overruns the
  // file by twenty-eight bytes. libXfont and FreeType both read such a file
  // because neither compares a stated size against the file's length.
  //
  // This library refused all 234 of them until the differential met one. So a
  // length past the end is clamped, and the two halves of that decision are both
  // asserted here: the file loads, and a table whose *own contents* are cut short
  // still fails - which is what the clamp must not cost.
  {
    PcfBuilder builder;
    builder.overstate_last = 28;
    Crafted crafted(builder.build());
    EXPECT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
  }
  {
    // The same overstatement on a table the parse has to read all of. The entry is
    // clamped to what is there, the read then runs off the end of it, and the
    // refusal names the table rather than the directory.
    PcfBuilder builder;
    const std::string whole = builder.build();
    Crafted crafted(whole.substr(0, whole.size() - 40));
    EXPECT_NE(crafted.result, GFNT_OK);
  }
  {
    // An offset past the end is refused rather than clamped: it names no bytes at
    // all, and clamping it would produce an empty table the parse would then
    // report as corrupt with a less useful message.
    PcfBuilder builder;
    std::string bytes = builder.build();
    // The fourth field of the first table-of-contents entry is its offset.
    const size_t at = 8 + 12;
    bytes[at] = static_cast<char>(0xFF);
    bytes[at + 1] = static_cast<char>(0xFF);
    Crafted crafted(bytes);
    EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT);
    EXPECT_NE(std::string(crafted.error.message).find("past the end of the file"),
        std::string::npos) << crafted.error.message;
  }
}

TEST(Bitmap, PcfTwoByteEncodingsArePairs) {
  // A PCF whose `min_byte1` is not zero maps pairs: the code is the two bytes
  // together, and every font here whose codes are above 0xFF is one. No fixture is
  // - the shared design is Latin-1 so that all four containers can hold it - so
  // this is the only thing that walks the outer loop of the range, and a mutation
  // that read the pair wrongly was not caught until it existed.
  PcfBuilder builder;
  builder.encoding_min_high = 0x4E;
  builder.encoding_max_high = 0x4F;
  builder.encoding_min = 0x00;
  builder.encoding_max = 0x02;
  Crafted crafted(builder.build());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
      GFNT_OK);
  // Two high bytes times three low ones, all pointing at the font's one glyph.
  EXPECT_EQ(count, 6u);

  const std::vector<uint32_t> expected = {0x4E00, 0x4E01, 0x4E02, 0x4F00,
      0x4F01, 0x4F02};
  for (size_t i = 0; i < expected.size() && i < count; ++i) {
    uint32_t codepoint = 0;
    uint32_t glyph = 99;

    ASSERT_EQ(gfnt_face_bitmap_encoding_at(crafted.face, i, &codepoint, &glyph,
        nullptr), GFNT_OK) << i;
    EXPECT_EQ(codepoint, expected[i]) << i;
    EXPECT_EQ(glyph, 0u) << i;
  }
  uint32_t glyph = 99;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 0x4E01, &glyph,
      nullptr), GFNT_OK);
  EXPECT_EQ(glyph, 0u);
  // And the low byte on its own is not a code this font has, which is what a
  // reader that dropped the high byte would answer.
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 0x01, &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 0u);
  ASSERT_EQ(gfnt_face_bitmap_encoding_at(crafted.face, 0, &glyph, nullptr,
      nullptr), GFNT_OK);
  EXPECT_NE(glyph, 0x01u);
}

TEST(Bitmap, TheDumpIsTheShapeTheDifferentialParses) {
  // `gfnt_bitmap_dump()` was reached by nothing in this suite: the tests read
  // pixels through the accessor, and the dump is only exercised by
  // `examples/font-bitmap` and the differential that parses its output. So a
  // change to the line it writes would have broken `bitmap_diff.py` with every
  // test still passing, which the coverage report is what noticed.
  Fixture fixture("bitmap.bdf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 1, 0, &bitmap, nullptr),
      GFNT_OK);

  char * text = nullptr;
  size_t size = 0;
  FILE * stream = open_memstream(&text, &size);
  ASSERT_NE(stream, nullptr);
  ASSERT_EQ(gfnt_bitmap_dump(&bitmap, stream), GFNT_OK);
  ASSERT_EQ(fclose(stream), 0);
  const std::string dumped(text, size);
  free(text);

  // The header, in the shape the driver's parser splits on.
  EXPECT_EQ(dumped.rfind("bitmap: 8x16, bearing 0,14, advance 8, 1 bpp, "
                         "strike 16x16\n", 0), 0u) << dumped;
  // One line per row, `#` set and `.` clear, and the same pixels the accessor
  // gives - which is what makes the dump a second reading rather than a second
  // format.
  std::vector<std::string> rows;
  std::istringstream reader(dumped.substr(dumped.find('\n') + 1));
  std::string row;
  while (std::getline(reader, row)) {
    rows.push_back(row);
  }
  ASSERT_EQ(rows.size(), bitmap.height);
  for (uint32_t y = 0; y < bitmap.height; ++y) {
    ASSERT_EQ(rows[y].size(), bitmap.width) << y;
    for (uint32_t x = 0; x < bitmap.width; ++x) {
      EXPECT_EQ(rows[y][x] == '#', gfnt_bitmap_pixel(&bitmap, x, y) != 0)
          << y << "," << x;
    }
  }

  // A glyph with no pixels writes its header and no rows, which is what a space
  // looks like to the differential.
  GFNT_BitmapGlyph empty{};
  Fixture ink("bitmap-ink.bdf");
  ASSERT_EQ(ink.result, GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_bitmap(ink.face, 0, 0, &empty, nullptr), GFNT_OK);
  stream = open_memstream(&text, &size);
  ASSERT_NE(stream, nullptr);
  ASSERT_EQ(gfnt_bitmap_dump(&empty, stream), GFNT_OK);
  ASSERT_EQ(fclose(stream), 0);
  EXPECT_EQ(std::string(text, size),
      "bitmap: 0x0, bearing 0,0, advance 8, 1 bpp, strike 16x16\n");
  free(text);
}

TEST(Bitmap, HexadecimalIsReadInEitherCase) {
  // Both text formats read rows as hexadecimal, and both are written in upper case
  // by every tool that writes them - so the lower-case arm of the digit reader was
  // reached by nothing. A file written by hand is not obliged to shout.
  {
    BdfBuilder builder;
    builder.rows = {"81", "42", "24", "18", "18", "24", "42", "81"};
    std::vector<std::string> lower;
    for (const std::string & row : builder.rows) {
      lower.push_back(row);
    }
    BdfBuilder other;
    other.rows = {"8a", "bc", "de", "f1", "18", "24", "42", "81"};
    Crafted crafted(other.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
        GFNT_OK);
    // 0x8a is 1000 1010.
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 0, 0), 255u);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 4, 0), 255u);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 6, 0), 255u);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 1, 0), 0u);
  }
  {
    Crafted crafted("0041:8abcdef101020304050607080900a0b0\n");
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    GFNT_BitmapGlyph bitmap{};
    ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
        GFNT_OK);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 0, 0), 255u);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 1, 0), 0u);
    EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 4, 0), 255u);
  }
}

TEST(Bitmap, PsfUnicodeTableReachesPastTheBasicPlane) {
  // A four-byte UTF-8 sequence, which is what a console font holding an emoji or a
  // CJK extension character states. The fixture's table is Latin-1 and Greek, so
  // the four-byte arm of the decoder was reached by nothing.
  const std::string astral = "\xf0\x9f\x92\xa9";  // U+1F4A9
  Crafted crafted(psf2(1, 0, 32, 1, 8, 8, 8, astral + std::string("\xff", 1)));
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 0x1F4A9, &glyph,
      nullptr), GFNT_OK);
  EXPECT_EQ(glyph, 0u);

  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
      GFNT_OK);
  EXPECT_EQ(count, 1u);
  uint32_t codepoint = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_at(crafted.face, 0, &codepoint, nullptr,
      nullptr), GFNT_OK);
  EXPECT_EQ(codepoint, 0x1F4A9u);
}

TEST(Bitmap, PcfWithoutAcceleratorsTakesItsBaselineFromItsGlyphs) {
  // A PCF may ship without either accelerator table - X computes those from the
  // glyphs in the first place - and then the tallest ascent and deepest descent any
  // glyph states is the line the font was drawn on. Nothing reached that arm,
  // because both fixtures carry BDF accelerators as bdftopcf writes them.
  PcfBuilder builder;
  Crafted control(builder.build());
  ASSERT_EQ(control.result, GFNT_OK) << control.error.message;
  GFNT_Strike strike{};
  ASSERT_EQ(gfnt_face_strike_at(control.face, 0, &strike, nullptr), GFNT_OK);
  // The builder writes no accelerators at all, so this *is* the glyph-derived
  // answer: one glyph, ascent 8, descent 0.
  EXPECT_EQ(strike.ascent, 8);
  EXPECT_EQ(strike.descent, 0);
  EXPECT_EQ(strike.ppem_y, 8u);

  // And with a descent, which is the half of the loop a glyph sitting on the line
  // cannot reach.
  PcfBuilder lower;
  lower.ascent = 6;
  lower.descent = 3;
  Crafted deep(lower.build());
  ASSERT_EQ(deep.result, GFNT_OK) << deep.error.message;
  ASSERT_EQ(gfnt_face_strike_at(deep.face, 0, &strike, nullptr), GFNT_OK);
  EXPECT_EQ(strike.ascent, 6);
  EXPECT_EQ(strike.descent, -3);
  // No PIXEL_SIZE property either, so the ppem is the height the baseline implies.
  EXPECT_EQ(strike.ppem_y, 9u);
}

TEST(Bitmap, TwoCharactersOfOneCodeResolveToTheLowerGlyph) {
  // A file can spell what no format permits: two characters with the same
  // ENCODING. The map is sorted by glyph within a codepoint so that the answer is
  // *stable* - a lookup that depended on the order two equal keys happened to land
  // in would be a different font on a different qsort.
  BdfBuilder builder;
  builder.chars = "2";
  builder.extra = "";
  std::string text = builder.build();
  const std::string second =
      "STARTCHAR Aalt\nENCODING 65\nDWIDTH 8 0\nBBX 8 8 0 0\nBITMAP\n"
      "FF\nFF\nFF\nFF\nFF\nFF\nFF\nFF\nENDCHAR\n";
  text.insert(text.find("ENDFONT"), second);
  Crafted crafted(text);
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(crafted.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 2u);
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 'A', &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 0u) << "the lower glyph must win, and stably";
  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
      GFNT_OK);
  EXPECT_EQ(count, 2u) << "both mappings are stated and both are enumerable";
}

TEST(Bitmap, PcfPropertiesThatPointOutsideTheirStringsAreRefused) {
  // A property's name and its value are both offsets into the table's own string
  // block, which is a bound the file states about itself. Neither arm was reached:
  // the fixtures' properties are all well formed, because a generator wrote them.
  //
  // The rest of the font is the builder's, so what is under test is the properties
  // parse and not a hand-built PCF's ability to load at all.
  struct Case {
    uint32_t name_offset;
    uint8_t is_string;
    uint32_t value;
    const char * because;
  };
  const std::vector<Case> cases = {
      {99, 0, 0, "whose name is outside the table's strings"},
      // A string-valued property whose *value* is outside them. The name has to be
      // one this library reads, or the value is never looked at - which is itself
      // the reason the two arms are separate.
      {0, 1, 99, "whose value is outside the table's strings"},
  };
  for (const Case & entry : cases) {
    std::string properties;
    const std::string strings = std::string("FAMILY_NAME", 11)
        + std::string(1, '\0') + std::string("Ghoti", 5) + std::string(1, '\0');

    put32(properties, 0x08);
    put32(properties, 1);
    put32(properties, entry.name_offset);
    properties += static_cast<char>(entry.is_string);
    put32(properties, entry.value);
    properties.append(3, '\0');   // the padding one property gets
    put32(properties, static_cast<uint32_t>(strings.size()));
    properties += strings;

    PcfBuilder builder;
    builder.properties = properties;
    Crafted crafted(builder.build());

    EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT) << entry.because;
    EXPECT_NE(std::string(crafted.error.message).find(entry.because),
        std::string::npos) << crafted.error.message;
  }

  // And a well-formed one through the same path, so that the refusals above are
  // not simply "this library cannot read a properties table".
  {
    std::string properties;
    const std::string strings = std::string("FAMILY_NAME", 11)
        + std::string(1, '\0') + std::string("Ghoti", 5) + std::string(1, '\0');

    put32(properties, 0x08);
    put32(properties, 1);
    put32(properties, 0);
    properties += '\x01';
    put32(properties, 12);
    properties.append(3, '\0');
    put32(properties, static_cast<uint32_t>(strings.size()));
    properties += strings;

    PcfBuilder builder;
    builder.properties = properties;
    Crafted crafted(builder.build());
    ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;
    char * family = nullptr;
    ASSERT_EQ(gfnt_face_name(crafted.face, GFNT_NAME_FAMILY, GFNT_LANGUAGE_ANY,
        nullptr, &family, nullptr, nullptr), GFNT_OK);
    EXPECT_STREQ(family, "Ghoti");
    gfnt_name_free(nullptr, family);
  }
}

TEST(Bitmap, PcfSkipsATableTypeItDoesNotRead) {
  // PCF's types are a set X may add to, so an unknown one is a table nothing here
  // asks for. It gets no directory entry rather than an invented tag - which keeps
  // gfnt_face_table_count() the number of tables that can be *read* - and it must
  // not stop the font from loading.
  PcfBuilder builder;
  builder.with_unknown_table = true;
  Crafted crafted(builder.build());
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(crafted.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 1u);

  PcfBuilder plain_font;
  Crafted without(plain_font.build());
  ASSERT_EQ(without.result, GFNT_OK);
  // The same tables either way: the extra one is absent from the directory, which
  // is the whole of what "skipped" means here.
  EXPECT_EQ(gfnt_face_table_count(crafted.face),
      gfnt_face_table_count(without.face));
}

TEST(Bitmap, ASfntWithStrikesThisLibraryCannotReadSaysSoSeparately) {
  // The distinction `glyph.h` exists to keep, and the only input in the repository
  // that can produce it: a face carrying `EBLC` has strikes this library does not
  // parse, and answering "zero strikes" would tell a caller a bitmap font has no
  // bitmaps (M9). A font with no bitmap table at all answers zero.
  //
  // Hand-built rather than generated, because what it needs is a table whose
  // *presence* is the fact - fontTools would want to write a real one.
  std::vector<gfnttest::Table> tables;
  tables.push_back({GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head()});
  tables.push_back({GFNT_TAG('E', 'B', 'L', 'C'),
      std::vector<uint8_t>{0, 2, 0, 0, 0, 0, 0, 0}});
  const std::vector<uint8_t> bytes =
      gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, tables);
  Crafted crafted(std::string(reinterpret_cast<const char *>(bytes.data()),
      bytes.size()));
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 99;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_strike_count(crafted.face, &count, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_EQ(count, 99u) << "a refusal must not also write a count";
  EXPECT_NE(std::string(error.message).find("does not parse"),
      std::string::npos) << error.message;

  // And the bitmap accessor carries that refusal outward rather than reporting
  // "no strikes at all", which is the arm no other input reaches.
  GFNT_BitmapGlyph bitmap{};
  gfnt_error_clear(&error);
  EXPECT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, &error),
      GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(std::string(error.message).find("does not parse"),
      std::string::npos) << error.message;
}

TEST(Bitmap, BdfToleratesIndentationAndTrailingSpace) {
  // Real BDFs are written by tools and by hand, and a hand-written one has
  // indented keywords, trailing spaces and blank lines before STARTFONT. None of
  // that changes what the font is, and each of the three reached no code until
  // here.
  BdfBuilder builder;
  std::string text = builder.build();
  std::string spaced;
  for (const std::string & line : std::vector<std::string>{}) {
    (void)line;
  }
  std::istringstream reader(text);
  std::string line;
  while (std::getline(reader, line)) {
    spaced += "  " + line + "  \n";
  }
  Crafted crafted("\n \n\t\n" + spaced);
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  GFNT_BitmapGlyph bitmap{};
  ASSERT_EQ(gfnt_face_glyph_bitmap(crafted.face, 0, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(bitmap.width, 8u);
  EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 0, 0), 255u);
  char * name = nullptr;
  ASSERT_EQ(gfnt_face_glyph_name(crafted.face, 0, nullptr, &name, nullptr,
      nullptr), GFNT_OK);
  EXPECT_STREQ(name, "A") << "a trailing space became part of the name";
  gfnt_glyph_name_free(nullptr, name);
}

TEST(Bitmap, OneCodepointStatedTwiceForOneGlyphIsNotTwoMappings) {
  // A PSF Unicode table may list a character twice for one cell - a hand-edited
  // table does - and the two entries are then identical in both fields, which is
  // the case the map's ordering has to have an answer for.
  Crafted crafted(psf2(1, 0, 32, 1, 8, 8, 8, std::string("AA\xff", 3)));
  ASSERT_EQ(crafted.result, GFNT_OK) << crafted.error.message;

  size_t count = 0;
  ASSERT_EQ(gfnt_face_bitmap_encoding_count(crafted.face, &count, nullptr),
      GFNT_OK);
  // Both are kept - the file states both - and both name the same glyph, so a
  // lookup is unambiguous however they were ordered.
  EXPECT_EQ(count, 2u);
  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_codepoint(crafted.face, 'A', &glyph, nullptr),
      GFNT_OK);
  EXPECT_EQ(glyph, 0u);
}

TEST(Bitmap, EveryPcfTableCutShortIsRefusedByName) {
  // design.md section 14.3: truncate each *table*, not the file. A file cut at the
  // end tests the last parser; a table cut short tests the one that reads it, and
  // for PCF there are six of them - each of which states a count and then the
  // records the count promised.
  //
  // Every table is cut to just its format word and then to a few bytes past it, so
  // that the count's own read fails and then a record's does. What must hold is
  // that the refusal names the table: a font refused as "corrupt" with no table tag
  // is a diagnostic nobody can act on (section 5.6).
  struct Table {
    uint32_t type;
    const char * name;
  };
  const std::vector<Table> tables = {
      {4, "metrics"}, {8, "bitmaps"}, {32, "encodings"}, {128, "glyph names"},
      {256, "accelerators"},
  };
  size_t refusals = 0;
  for (const Table & table : tables) {
    for (uint32_t keep : {4u, 6u, 10u, 16u}) {
      PcfBuilder builder;
      builder.cut_type = table.type;
      builder.cut_to = keep;
      Crafted crafted(builder.build());

      if (crafted.result == GFNT_OK) {
        // Legal: a table this font did not need, cut to nothing. The glyph names
        // and the accelerators are both optional, so a short one means the parse
        // read what was there and asked for no more.
        continue;
      }
      refusals += 1;
      EXPECT_EQ(crafted.result, GFNT_ERR_CORRUPT)
          << table.name << " cut to " << keep;
      EXPECT_NE(crafted.error.table, 0u)
          << table.name << " cut to " << keep
          << " was refused without naming a table";
    }
  }
  // Rather than "some": twenty variants over five tables, and a run where most had
  // become readable would mean a bound had stopped being checked.
  EXPECT_GE(refusals, 8u) << "only " << refusals
                          << " of twenty truncations were refused";
}

TEST(Bitmap, LimitsAreEnforcedPerContainer) {
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  limits.max_glyphs = 4;
  {
    // PSF states its count in its header, so the limit is checked before an
    // allocation is made with the number.
    Crafted crafted(psf2(0, 0, 32, 8), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_glyphs"),
        std::string::npos) << crafted.error.message;
  }
  {
    // `.hex` states no count anywhere, so its limit is per glyph.
    std::string many;
    for (unsigned i = 0; i < 8; ++i) {
      many += "004" + std::to_string(i) + ":"
          + std::string(32, '0') + "\n";
    }
    Crafted crafted(many, &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  }
  {
    PcfBuilder builder;
    builder.metrics_count = 8;
    builder.bitmaps_count = 8;
    builder.names_count = 8;
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
  }

  gfnt_limits_default(&limits);
  limits.max_ppem = 8;
  {
    // A glyph taller than the largest size this library will rasterise. The cap
    // is what keeps a claimed box from sizing an allocation.
    BdfBuilder builder;
    builder.bbx = "8 40 0 0";
    builder.rows.assign(40, "81");
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_ppem"),
        std::string::npos) << crafted.error.message;
  }

  gfnt_limits_default(&limits);
  limits.max_raster_bytes = 4;
  {
    BdfBuilder builder;
    Crafted crafted(builder.build(), &limits);
    EXPECT_EQ(crafted.result, GFNT_ERR_LIMIT);
    EXPECT_NE(std::string(crafted.error.message).find("max_raster_bytes"),
        std::string::npos) << crafted.error.message;
  }
}

TEST(Bitmap, ALineLengthLimitOfZeroIsACallerError) {
  // The two text formats read through a line cursor, and a cap of zero is a limit
  // no line can satisfy - so it is refused where the cursor is created rather than
  // reported as a corrupt font. A caller who zeroed a field of GFNT_Limits by
  // memset gets that answer instead of "your BDF is broken".
  GFNT_Limits limits;

  gfnt_limits_default(&limits);
  limits.max_line_length = 0;
  {
    Crafted crafted(BdfBuilder().build(), &limits);
    EXPECT_NE(crafted.result, GFNT_OK);
    EXPECT_NE(std::string(crafted.error.message).find("read the file's lines"),
        std::string::npos) << crafted.error.message;
  }
  {
    Crafted crafted("0041:00000000000000000000000000000000\n", &limits);
    EXPECT_NE(crafted.result, GFNT_OK);
    EXPECT_NE(std::string(crafted.error.message).find("read the file's lines"),
        std::string::npos) << crafted.error.message;
  }
}

TEST(Bitmap, EveryAllocationCanFail) {
  // design.md section 15.1: an allocation failure is a code path, and the only
  // way to walk it is to refuse each request in turn. The four parsers allocate
  // different numbers of things - PCF alone allocates its directory, its metrics,
  // its name offsets and three arenas - so the sweep is per container and the
  // requirement is the same: no crash, no leak, and a refusal that says OOM.
  for (const char * name : {"bitmap.hex", "bitmap.psf", "bitmap.bdf",
      "bitmap.pcf", "bitmap-lsb.pcf", "bitmap-swap.pcf", "bitmap-ink.bdf",
      "bitmap-v1.psf"}) {
    const std::string path = gfnttest::data(std::string("fonts/") + name);
    GFNT_Blob * blob = nullptr;
    GFNT_Error error{};

    ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error), GFNT_OK) << name;

    size_t requests = 0;
    {
      gfnttest::FailingAllocator counter(static_cast<size_t>(-1));
      GFNT_Face * face = nullptr;

      ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, counter.get(), &face, &error),
          GFNT_OK) << name;
      gfnt_face_free(face);
      requests = counter.requests();
      EXPECT_EQ(counter.live(), 0u) << name;
    }
    ASSERT_GT(requests, 1u) << name;

    for (size_t at = 0; at < requests; ++at) {
      gfnttest::FailingAllocator failing(at);
      GFNT_Face * face = nullptr;
      GFNT_Result result;

      gfnt_error_clear(&error);
      result = gfnt_face_load(blob, 0, nullptr, failing.get(), &face, &error);
      if (result == GFNT_OK) {
        // A refused request the parse did not need: the reallocation that was
        // going to be asked for anyway, served from the capacity already there.
        gfnt_face_free(face);
        continue;
      }
      EXPECT_EQ(result, GFNT_ERR_OOM) << name << " request " << at << ": "
                                      << error.message;
      EXPECT_EQ(failing.live(), 0u) << name << " request " << at;
    }
    gfnt_blob_destroy(blob);
  }
}

TEST(Bitmap, NullArgumentsAreCallerErrors) {
  Fixture fixture("bitmap.pcf");
  ASSERT_EQ(fixture.result, GFNT_OK);

  GFNT_BitmapGlyph bitmap{};
  size_t count = 0;
  GFNT_Coverage coverage{};

  EXPECT_EQ(gfnt_face_glyph_bitmap(nullptr, 0, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 0, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_bitmap(fixture.face, 99, 0, &bitmap, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(nullptr, &count, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_bitmap_encoding_count(fixture.face, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_from_bitmap(nullptr, nullptr, &coverage, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_bitmap_dump(nullptr, stdout), GFNT_ERR_INVALID);

  // A pixel outside the glyph is a pixel the glyph does not cover, which is zero
  // rather than an error - a caller drawing a box around a glyph reads it.
  ASSERT_EQ(gfnt_face_glyph_bitmap(fixture.face, 2, 0, &bitmap, nullptr),
      GFNT_OK);
  EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, bitmap.width, 0), 0u);
  EXPECT_EQ(gfnt_bitmap_pixel(&bitmap, 0, bitmap.height), 0u);
  EXPECT_EQ(gfnt_bitmap_pixel(nullptr, 0, 0), 0u);

  // A deeper strike than any container here produces: the bridge refuses rather
  // than reading a 1-bit row and calling it grey.
  bitmap.bit_depth = 4;
  EXPECT_EQ(gfnt_coverage_from_bitmap(&bitmap, nullptr, &coverage, nullptr),
      GFNT_ERR_UNSUPPORTED);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
