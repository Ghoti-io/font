/**
 * @file
 *
 * The CFF container: what the fixtures prove it reads, and what only a
 * hand-built table can make it refuse.
 *
 * documentation/design.md section 7.4. Two halves, and the split is the same one
 * `test_glyf.cpp` makes: the fixtures under `tests/data/fonts/cff-*.otf` are
 * written by fontTools in the pinned image and compared against it glyph by
 * glyph by `tools/oracle/cff_diff.py`, so what this suite asks of them is what
 * the *library's own API* says about them - names, advances, which Private DICT
 * governs which glyph. The refusals need bytes no font would carry, and those
 * are built here.
 *
 * `test_charstring.cpp` is the interpreter, which needs no container at all.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include "sfnt_builder.h"

#include <set>
#include <string>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>

#include <gtest/gtest.h>

namespace {

using gfnttest::Table;
using gfnttest::build_head;
using gfnttest::build_hhea;
using gfnttest::build_hmtx;
using gfnttest::build_maxp;
using gfnttest::build_sfnt;
using gfnttest::put_u8;
using gfnttest::put_u16;
using gfnttest::put_u32;

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
    EXPECT_EQ(result, GFNT_OK) << name;
  }
  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;

  operator const GFNT_Face *() const { return face; }
};

/** One glyph's name, or the reason there is none. */
std::string name_of(const GFNT_Face * face, uint32_t glyph) {
  char * name = nullptr;
  GFNT_Error error{};
  if (gfnt_face_glyph_name(face, glyph, nullptr, &name, nullptr, &error)
      != GFNT_OK) {
    return "-";
  }
  const std::string out = name;
  gfnt_glyph_name_free(nullptr, name);
  return out;
}

/** How many contours a glyph's outline has, or -1 if it was refused. */
int contours_of(const GFNT_Face * face, uint32_t glyph) {
  GFNT_Outline * outline = nullptr;
  if (gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline, nullptr)
      != GFNT_OK) {
    return -1;
  }
  const int count = (int)gfnt_outline_contour_count(outline);
  gfnt_outline_destroy(outline);
  return count;
}

/** The advance a glyph's charstring states, in whole font units. */
int charstring_width(const GFNT_Face * face, uint32_t glyph) {
  GFNT_CharstringMetrics metrics{};
  GFNT_Error error{};
  if (gfnt_face_glyph_charstring_metrics(face, glyph, &metrics, &error)
      != GFNT_OK) {
    return -1;
  }
  return metrics.width / GFNT_F16DOT16_ONE;
}

// ------------------------------------------------- a CFF table, byte by byte

/** An INDEX: a count, an offset size, count+1 one-based offsets, then data. */
std::vector<uint8_t> cff_index(
    const std::vector<std::vector<uint8_t>> & items) {
  std::vector<uint8_t> out;
  if (items.empty()) {
    put_u16(out, 0);
    return out;
  }
  std::vector<size_t> offsets{1};
  for (const auto & item : items) {
    offsets.push_back(offsets.back() + item.size());
  }
  put_u16(out, (uint16_t)items.size());
  put_u8(out, 2);
  for (size_t offset : offsets) {
    put_u16(out, (uint16_t)offset);
  }
  for (const auto & item : items) {
    out.insert(out.end(), item.begin(), item.end());
  }
  return out;
}

/** A DICT operand in the five-byte form, so patching never changes a size. */
std::vector<uint8_t> dict_offset(int32_t value) {
  std::vector<uint8_t> out{29};
  put_u32(out, (uint32_t)value);
  return out;
}

/** A DICT operand in the short integer form. */
std::vector<uint8_t> dict_int(int32_t value) {
  std::vector<uint8_t> out;
  if (value >= -107 && value <= 107) {
    put_u8(out, (uint8_t)(value + 139));
    return out;
  }
  out.push_back(28);
  put_u16(out, (uint16_t)(int16_t)value);
  return out;
}

/** One `(operands..., operator)` entry, appended. */
void dict_entry(std::vector<uint8_t> & out,
    const std::vector<std::vector<uint8_t>> & operands,
    const std::vector<uint8_t> & op) {
  for (const auto & operand : operands) {
    out.insert(out.end(), operand.begin(), operand.end());
  }
  out.insert(out.end(), op.begin(), op.end());
}

/** What a hand-built CFF table is made of, with every part optional. */
struct CffSpec {
  uint8_t major = 1;
  uint8_t header_size = 4;
  std::vector<std::vector<uint8_t>> names{{'T', 'e', 's', 't'}};
  std::vector<std::vector<uint8_t>> strings;
  std::vector<std::vector<uint8_t>> gsubrs;
  std::vector<std::vector<uint8_t>> charstrings;
  std::vector<uint8_t> charset;       ///< Empty for none at all.
  std::vector<uint8_t> encoding;      ///< Empty for none at all.
  std::vector<uint8_t> fdselect;      ///< CID only.
  std::vector<std::vector<uint8_t>> subrs; ///< The one Private DICT's locals.
  bool cid = false;
  int fd_count = 1;                   ///< Font DICTs, CID only.
  std::vector<uint8_t> extra_top;     ///< Entries written before the offsets.
  bool omit_charstrings = false;      ///< Leave the CharStrings entry out.
  bool omit_top_dict = false;         ///< An empty Top DICT INDEX.
  size_t top_dicts = 1;               ///< How many fonts the FontSet holds.
  size_t truncate_to = 0;             ///< Cut the table to this many bytes.
};

/**
 * A CFF table's bytes.
 *
 * Every offset is written in the five-byte form, which is what lets the layout
 * be computed in one pass: a DICT whose operands change width when their values
 * change moves every structure after it.
 */
std::vector<uint8_t> build_cff(const CffSpec & spec) {
  const std::vector<uint8_t> charstrings = cff_index(spec.charstrings);
  const std::vector<uint8_t> names = cff_index(spec.names);
  const std::vector<uint8_t> strings = cff_index(spec.strings);
  const std::vector<uint8_t> gsubrs = cff_index(spec.gsubrs);
  const std::vector<uint8_t> local = cff_index(spec.subrs);

  // A Private DICT with its Subrs offset pointing just past itself.
  std::vector<uint8_t> privat;
  dict_entry(privat, {dict_int(600)}, {20});
  dict_entry(privat, {dict_int(600)}, {21});
  if (!spec.subrs.empty()) {
    std::vector<uint8_t> probe = privat;
    dict_entry(probe, {dict_offset(0)}, {19});
    dict_entry(privat, {dict_offset((int32_t)probe.size())}, {19});
  }

  auto top_dict = [&](size_t charset_at, size_t encoding_at,
      size_t charstrings_at, size_t fdarray_at, size_t fdselect_at,
      size_t private_at) {
    std::vector<uint8_t> out = spec.extra_top;
    if (spec.cid) {
      dict_entry(out, {dict_int(391), dict_int(392), dict_int(0)}, {12, 30});
    }
    if (!spec.charset.empty()) {
      dict_entry(out, {dict_offset((int32_t)charset_at)}, {15});
    }
    if (!spec.encoding.empty()) {
      dict_entry(out, {dict_offset((int32_t)encoding_at)}, {16});
    }
    if (!spec.omit_charstrings) {
      dict_entry(out, {dict_offset((int32_t)charstrings_at)}, {17});
    }
    if (spec.cid) {
      dict_entry(out, {dict_offset((int32_t)fdarray_at)}, {12, 36});
      if (!spec.fdselect.empty()) {
        dict_entry(out, {dict_offset((int32_t)fdselect_at)}, {12, 37});
      }
    }
    else {
      dict_entry(out, {dict_offset((int32_t)privat.size()),
          dict_offset((int32_t)private_at)}, {18});
    }
    return out;
  };

  auto font_dicts = [&](size_t private_at) {
    std::vector<std::vector<uint8_t>> out;
    for (int fd = 0; fd < spec.fd_count; ++fd) {
      std::vector<uint8_t> dict;
      dict_entry(dict, {dict_offset((int32_t)privat.size()),
          dict_offset((int32_t)private_at)}, {18});
      out.push_back(dict);
    }
    return out;
  };

  auto top_index_of = [&](size_t charset_at, size_t encoding_at,
      size_t charstrings_at, size_t fdarray_at, size_t fdselect_at,
      size_t private_at) {
    if (spec.omit_top_dict) {
      return cff_index({});
    }
    const std::vector<uint8_t> one = top_dict(charset_at, encoding_at,
        charstrings_at, fdarray_at, fdselect_at, private_at);
    std::vector<std::vector<uint8_t>> entries(spec.top_dicts, one);
    return cff_index(entries);
  };
  const std::vector<uint8_t> top_probe = top_index_of(0, 0, 0, 0, 0, 0);
  const std::vector<uint8_t> top_index = top_probe;
  const std::vector<uint8_t> fdarray_probe = spec.cid
      ? cff_index(font_dicts(0)) : std::vector<uint8_t>{};

  size_t at = spec.header_size + names.size() + top_index.size()
      + strings.size() + gsubrs.size();
  const size_t charset_at = at;
  at += spec.charset.size();
  const size_t encoding_at = at;
  at += spec.encoding.size();
  const size_t fdselect_at = at;
  at += spec.fdselect.size();
  const size_t charstrings_at = at;
  at += charstrings.size();
  const size_t fdarray_at = at;
  at += fdarray_probe.size();
  const size_t private_at = at;

  std::vector<uint8_t> out;
  put_u8(out, spec.major);
  put_u8(out, 0);
  put_u8(out, spec.header_size);
  put_u8(out, 2);
  out.resize(spec.header_size, 0);
  auto append = [&out](const std::vector<uint8_t> & piece) {
    out.insert(out.end(), piece.begin(), piece.end());
  };
  append(names);
  append(top_index_of(charset_at, encoding_at, charstrings_at, fdarray_at,
      fdselect_at, private_at));
  append(strings);
  append(gsubrs);
  append(spec.charset);
  append(spec.encoding);
  append(spec.fdselect);
  append(charstrings);
  if (spec.cid) {
    append(cff_index(font_dicts(private_at)));
  }
  append(privat);
  append(local);
  if (spec.truncate_to && spec.truncate_to < out.size()) {
    out.resize(spec.truncate_to);
  }
  return out;
}

/** A charstring that draws one small square and states its advance. */
std::vector<uint8_t> square(int advance = 600) {
  std::vector<uint8_t> out;
  auto number = [&out](int value) {
    if (value >= -107 && value <= 107) {
      out.push_back((uint8_t)(value + 139));
    }
    else {
      out.push_back(28);
      put_u16(out, (uint16_t)(int16_t)value);
    }
  };
  number(advance);
  number(50);
  number(0);
  out.push_back(21);  // rmoveto
  number(100);
  out.push_back(6);   // hlineto
  number(100);
  out.push_back(7);   // vlineto
  number(-100);
  out.push_back(6);   // hlineto
  out.push_back(14);  // endchar
  return out;
}

/** An OTTO font around a CFF table, with the metric tables a face needs. */
struct Font {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Font(const std::vector<uint8_t> & cff, uint16_t num_glyphs = 2,
      GFNT_Tag tag = GFNT_TAG('C', 'F', 'F', ' ')) {
    std::vector<Table> tables = {
        Table{GFNT_TAG('h', 'e', 'a', 'd'), build_head()},
        Table{GFNT_TAG('h', 'h', 'e', 'a'),
            build_hhea(800, -200, 0, num_glyphs)},
        Table{GFNT_TAG('h', 'm', 't', 'x'),
            build_hmtx(std::vector<std::pair<uint16_t, int16_t>>(num_glyphs,
                {600, 0}))},
        Table{GFNT_TAG('m', 'a', 'x', 'p'), build_maxp(num_glyphs)},
        Table{tag, cff},
    };
    bytes = build_sfnt(GFNT_FLAVOUR_CFF, tables);
    gfnt_error_clear(&error);
    result = gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error);
    if (result != GFNT_OK) {
      return;
    }
    result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
  }
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;
};

/** Whether a message names something. */
void names(const GFNT_Error & error, const char * fragment) {
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find(fragment), std::string::npos)
      << error.message;
}

// ---------------------------------------------------------------- fixtures

TEST(Cff, EveryCurveFixtureGlyphDraws) {
  Fixture font("cff-curves.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 13u);
  // Glyph 1 is `space`, which draws nothing and is not a failure.
  EXPECT_EQ(contours_of(font, 1), 0);
  for (uint32_t glyph = 2; glyph < glyphs; ++glyph) {
    EXPECT_GT(contours_of(font, glyph), 0) << "glyph " << glyph;
  }
}

TEST(Cff, AGlyphNameComesFromTheCharsetAndNotFromPost) {
  // These fonts carry `post` version 3 - a header stating it has no names - and
  // fontTools takes their glyph order from the charset, as every reference does.
  // A reader that asked `post` first would report that a CFF font has no glyph
  // names at all.
  Fixture font("cff-curves.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  EXPECT_EQ(name_of(font, 0), ".notdef");
  // `space` is one of the 391 standard strings, so its SID names it.
  EXPECT_EQ(name_of(font, 1), "space");
  // `hv` is not, so it lives in this font's own String INDEX - the only way a
  // SID of 391 or more is reached.
  EXPECT_EQ(name_of(font, 2), "hv");

  uint32_t glyph = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "flex1y", &glyph, nullptr), GFNT_OK);
  EXPECT_EQ(name_of(font, glyph), "flex1y");
  EXPECT_EQ(gfnt_face_glyph_for_name(font, "nosuchglyph", &glyph, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Cff, TheTwoWidthDefaultsAreDifferentFacts) {
  // defaultWidthX is the advance of a charstring that states none, and
  // nominalWidthX is what a stated one is a delta from. A reader that swapped
  // them gets every advance in the font wrong and no outline comparison sees it.
  Fixture font("cff-subrs.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  uint32_t defaultw = GFNT_GLYPH_NONE;
  uint32_t nominalw = GFNT_GLYPH_NONE;
  uint32_t mismatch = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "defaultw", &defaultw, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "nominalw", &nominalw, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "mismatch", &mismatch, nullptr),
      GFNT_OK);

  EXPECT_EQ(charstring_width(font, defaultw), 555);
  EXPECT_EQ(charstring_width(font, nominalw), 545);

  // And `hmtx` is a different table with its own answer, which this font makes
  // disagree on purpose: 700 in the charstring, 400 in the metric table. Both
  // are readable, and a caller laying out text wants the second.
  EXPECT_EQ(charstring_width(font, mismatch), 700);
  int32_t advance = 0;
  ASSERT_EQ(gfnt_face_glyph_advance(font, mismatch, nullptr, &advance, nullptr),
      GFNT_OK);
  EXPECT_EQ(advance, 400);
}

TEST(Cff, AnAccentedCharacterIsDrawnFromTwoOtherGlyphs) {
  Fixture font("cff-seac.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  uint32_t base = GFNT_GLYPH_NONE;
  uint32_t accented = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "A", &base, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "Aacute", &accented, nullptr),
      GFNT_OK);

  // One contour each, so the accented character's two say it was assembled.
  EXPECT_EQ(contours_of(font, base), 1);
  EXPECT_EQ(contours_of(font, accented), 2);

  // And the library reports it as a composite, which is the same *fact* a
  // `glyf` composite reports reached by a different construction.
  bool composite = false;
  ASSERT_EQ(gfnt_face_glyph_is_composite(font, accented, &composite, nullptr),
      GFNT_OK);
  EXPECT_TRUE(composite);
  ASSERT_EQ(gfnt_face_glyph_is_composite(font, base, &composite, nullptr),
      GFNT_OK);
  EXPECT_FALSE(composite);
}

TEST(Cff, ACffGlyphStatesNoBoundingBoxOfItsOwn) {
  Fixture font("cff-curves.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Box box{};
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_stated_box(font, 2, &box, &error),
      GFNT_ERR_UNSUPPORTED);
  names(error, "FontBBox");

  // What the glyph's own bounds are is a different question, and it has an
  // answer.
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(font, 2, nullptr, nullptr, &outline,
      nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_bounds(outline, &box), GFNT_OK);
  EXPECT_FALSE(gfnt_box_is_empty(&box));
  gfnt_outline_destroy(outline);
}

TEST(Cff, ACidKeyedFontHasAPrivateDictPerGlyph) {
  // `FDSelect` names which of the `FDArray`'s dictionaries a glyph belongs to,
  // and they differ in the width defaults *and* in the local subroutines - so a
  // reader that used the first one for every glyph draws most CID fonts nearly
  // right.
  Fixture font("cff-cid.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  // Glyph 1 is in FD 0, whose defaultWidthX is 600; glyph 2 is in FD 1, whose
  // defaultWidthX is 480. Neither charstring states a width.
  EXPECT_EQ(charstring_width(font, 1), 600);
  EXPECT_EQ(charstring_width(font, 2), 480);

  // And each calls local subroutine 0, which is a different shape in each FD.
  GFNT_Outline * first = nullptr;
  GFNT_Outline * second = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(font, 1, nullptr, nullptr, &first, nullptr),
      GFNT_OK);
  ASSERT_EQ(gfnt_face_glyph_outline(font, 2, nullptr, nullptr, &second,
      nullptr), GFNT_OK);
  GFNT_Box wide{};
  GFNT_Box narrow{};
  ASSERT_EQ(gfnt_outline_control_box(first, &wide), GFNT_OK);
  ASSERT_EQ(gfnt_outline_control_box(second, &narrow), GFNT_OK);
  EXPECT_GT(wide.x_max, narrow.x_max);
  gfnt_outline_destroy(first);
  gfnt_outline_destroy(second);
}

TEST(Cff, ACidKeyedFontsCharsetHoldsCidsAndNotNames) {
  // A CID is a number in the character collection `ROS` names, and reading one
  // as a SID prints whichever standard string sits at that number - CID 11 came
  // out as `asterisk`. The glyph has no name, and that is what this reports.
  Fixture font("cff-cid.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  char * name = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_name(font, 1, nullptr, &name, nullptr, &error),
      GFNT_ERR_UNSUPPORTED);
  names(error, "by CID");

  // Glyph 0 is the exception, and not by convention: the format says glyph 0 is
  // `.notdef` in every CFF.
  EXPECT_EQ(name_of(font, 0), ".notdef");
}

TEST(Cff, ATypeOneCharstringInACffIsReadAsTypeOne) {
  Fixture font("cff-type1.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_CharstringType type = GFNT_CHARSTRING_TYPE2;
  const uint8_t * bytes = nullptr;
  size_t length = 0;
  ASSERT_EQ(gfnt_face_glyph_charstring(font, 2, &type, &bytes, &length,
      nullptr), GFNT_OK);
  // The language comes from the Top DICT's CharstringType rather than from the
  // table's name, which is the only way this font is readable at all.
  EXPECT_EQ(type, GFNT_CHARSTRING_TYPE1);
  EXPECT_GT(length, 0u);

  // And every glyph draws, including the accented character and the flex.
  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  for (uint32_t glyph = 2; glyph < glyphs; ++glyph) {
    EXPECT_GT(contours_of(font, glyph), 0) << "glyph " << glyph;
  }
  // Type 1 keeps the advance in the charstring, so this is the only place it is.
  EXPECT_EQ(charstring_width(font, 2), 600);
}

TEST(Cff, TheHintFixturesMasksAreSteppedOverCorrectly) {
  // Every glyph here ends in a drawing operator *after* a mask, so a mask
  // stepped over by the wrong number of bytes makes the rest of the program
  // nonsense - a refusal or a different shape, and either fails this.
  Fixture font("cff-hints.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 8u);
  for (uint32_t glyph = 2; glyph < glyphs; ++glyph) {
    EXPECT_EQ(contours_of(font, glyph), 1) << "glyph " << glyph;
  }
  // The advance carried on a stem operator, and on `vstem` - which the
  // specification's list of stack-clearing operators that may carry one does
  // not name, and which fontTools reads the same way this does.
  uint32_t widthstem = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "widthstem", &widthstem, nullptr),
      GFNT_OK);
  EXPECT_EQ(charstring_width(font, widthstem), 600);
}

TEST(Cff, TheArithmeticFixtureDrawsWhatItComputes) {
  Fixture font("cff-arith.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  for (uint32_t glyph = 2; glyph < glyphs; ++glyph) {
    EXPECT_EQ(contours_of(font, glyph), 1) << "glyph " << glyph;
  }
  // 600/5 across and 300 up, from a program that says neither number.
  uint32_t divide = GFNT_GLYPH_NONE;
  ASSERT_EQ(gfnt_face_glyph_for_name(font, "divide", &divide, nullptr),
      GFNT_OK);
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(font, divide, nullptr, nullptr, &outline,
      nullptr), GFNT_OK);
  GFNT_Box box{};
  ASSERT_EQ(gfnt_outline_control_box(outline, &box), GFNT_OK);
  EXPECT_EQ(box.x_max - box.x_min, 120 * GFNT_F26DOT6_ONE);
  EXPECT_EQ(box.y_max - box.y_min, 300 * GFNT_F26DOT6_ONE);
  gfnt_outline_destroy(outline);
}

TEST(Cff, AGlyphPastTheCharStringsIndexIsRefused) {
  Fixture font("cff-curves.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font, 99, nullptr, nullptr, &outline,
      &error), GFNT_ERR_INVALID);
  names(error, "past the face's glyph count");
}

TEST(Cff, AllocationFailureIsReportedAtEveryStep) {
  Fixture font("cff-curves.otf");
  ASSERT_EQ(font.result, GFNT_OK);

  // How many allocations one glyph costs, measured rather than assumed.
  gfnttest::FailingAllocator counter((size_t)-1);
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(font, 8, nullptr, counter.get(), &outline,
      nullptr), GFNT_OK);
  gfnt_outline_destroy(outline);
  const size_t requests = counter.requests();
  ASSERT_GT(requests, 1u);

  for (size_t at = 0; at < requests; ++at) {
    gfnttest::FailingAllocator allocator(at);
    GFNT_Outline * attempt = nullptr;
    GFNT_Error error{};
    const GFNT_Result result = gfnt_face_glyph_outline(font, 8, nullptr,
        allocator.get(), &attempt, &error);
    if (result != GFNT_OK) {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "refusing request " << at;
      EXPECT_EQ(attempt, nullptr) << "refusing request " << at;
    }
    allocator.stop_failing();
    gfnt_outline_destroy(attempt);
    EXPECT_EQ(allocator.live(), 0u) << "refusing request " << at;
  }
}

// ------------------------------------------------------------- hand-built

TEST(Cff, AReadableTableDrawsItsGlyphs) {
  // The control for every refusal below: the builder writes a CFF this library
  // reads, so a test that expects a refusal is testing the change it made and
  // not the builder.
  CffSpec spec;
  spec.charstrings = {square(), square(500)};
  Font font(build_cff(spec));
  ASSERT_EQ(font.result, GFNT_OK) << font.error.message;

  EXPECT_TRUE(gfnt_face_has_outlines(font.face));
  EXPECT_EQ(contours_of(font.face, 0), 1);
  EXPECT_EQ(contours_of(font.face, 1), 1);
}

TEST(Cff, ACffWhoseMajorVersionIsNotOneIsRefusedByName) {
  CffSpec spec;
  spec.major = 2;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  names(error, "CFF2 is a different format");
}

TEST(Cff, ACff2TableIsNotReadAtAll) {
  // Section 16 defers CFF2, and the refusal says which table it saw rather than
  // reporting that the face has no outlines.
  CffSpec spec;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1, GFNT_TAG('C', 'F', 'F', '2'));
  ASSERT_EQ(font.result, GFNT_OK);

  EXPECT_FALSE(gfnt_face_has_outlines(font.face));
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  names(error, "CFF2 charstrings");
}

TEST(Cff, ATopDictIndexWithNoEntriesDescribesNoFont) {
  CffSpec spec;
  spec.omit_top_dict = true;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_CORRUPT);
  names(error, "no Top DICT");
}

TEST(Cff, AFontSetWithMoreThanOneFontIsRefusedRatherThanGuessed) {
  // Legal CFF, and never what an sfnt-wrapped one is. Which font of a set a
  // caller meant is a question this API has no way to ask, so it is refused by
  // name rather than answered with the first.
  CffSpec spec;
  spec.names = {{'A'}, {'B'}};
  spec.top_dicts = 2;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  names(error, "more than one font");
}

TEST(Cff, ATopDictWithNoCharStringsHasNoGlyphsToFind) {
  CffSpec spec;
  spec.omit_charstrings = true;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_CORRUPT);
  names(error, "no CharStrings offset");
}

TEST(Cff, AHeaderShorterThanFourBytesIsCorrupt) {
  CffSpec spec;
  spec.header_size = 3;
  spec.charstrings = {square()};
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_CORRUPT);
  names(error, "four bytes");
}

TEST(Cff, ACharsetFormatTheFormatDoesNotDefineIsRefused) {
  CffSpec spec;
  spec.charstrings = {square(), square()};
  spec.charset = {9, 0, 1};  // Format 9 is not a charset format.
  Font font(build_cff(spec));
  ASSERT_EQ(font.result, GFNT_OK);

  char * name = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_name(font.face, 1, nullptr, &name, nullptr, &error),
      GFNT_ERR_UNSUPPORTED);
  names(error, "charset format");
}

TEST(Cff, AnFdSelectInFormatZeroIsAnArrayOfOneBytePerGlyph) {
  // The other FDSelect format, and the one a fixture would spend 64 kB of one
  // byte per glyph to carry.
  CffSpec spec;
  spec.cid = true;
  spec.fd_count = 1;
  spec.charstrings = {square(), square()};
  spec.fdselect = {0, 0, 0};  // Format 0, then one FD index per glyph.
  Font font(build_cff(spec));
  ASSERT_EQ(font.result, GFNT_OK) << font.error.message;

  EXPECT_EQ(contours_of(font.face, 0), 1);
  EXPECT_EQ(contours_of(font.face, 1), 1);
}

TEST(Cff, AnFdSelectNamingAFontDictThatIsNotThereIsCorrupt) {
  CffSpec spec;
  spec.cid = true;
  spec.fd_count = 1;
  spec.charstrings = {square(), square()};
  spec.fdselect = {0, 0, 7};  // Glyph 1 claims FD 7 of one.
  Font font(build_cff(spec));
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 1, nullptr, nullptr, &outline,
      &error), GFNT_ERR_CORRUPT);
  names(error, "the FDArray does not have");
}

TEST(Cff, AFontMatrixThatIsNotTheEmsOwnScaleIsRefused) {
  // A charstring's coordinates are in the font's own charstring space, and for
  // every font anybody ships that space is the em `unitsPerEm` describes. One
  // where they disagree would need the matrix applied, and applying it silently
  // would put this library and every reference pen in different spaces.
  CffSpec spec;
  spec.charstrings = {square()};
  // FontMatrix 0.0005 0 0 0.0005 0 0, against a 1000-unit em.
  std::vector<std::vector<uint8_t>> operands;
  for (int i = 0; i < 6; ++i) {
    // 0.0005 as a real, and 0 for the rest.
    if (i == 0 || i == 3) {
      // Nibbles: 0, '.', 0, 0, 0, 5, end - and the pad nibble that makes the
      // count even is another end, which is what the format says to write.
      operands.push_back({0x1e, 0x0a, 0x00, 0x05, 0xff});
    }
    else {
      // A real zero is two nibbles and one byte. A third byte here was read as
      // the operand that follows, and 255 is a byte a DICT reserves - so the
      // table came out corrupt rather than carrying a matrix, which is a
      // refusal for the wrong reason.
      operands.push_back({0x1e, 0x0f});
    }
  }
  dict_entry(spec.extra_top, operands, {12, 7});
  Font font(build_cff(spec), 1);
  ASSERT_EQ(font.result, GFNT_OK);

  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_outline(font.face, 0, nullptr, nullptr, &outline,
      &error), GFNT_ERR_UNSUPPORTED);
  names(error, "FontMatrix");
}

TEST(Cff, TheCharStringsCountJoinsTheGlyphCountMinimum) {
  // M12: `maxp` states a count and every table that indexes glyphs implies one.
  // A glyph past the end of `CharStrings` has no charstring at all - there is
  // nothing to condemn the way a broken `loca` entry condemns one glyph - so the
  // count is what shrinks, and the disagreement is kept so a caller can report
  // it.
  CffSpec spec;
  spec.charstrings = {square(), square()};
  Font font(build_cff(spec), 5);
  ASSERT_EQ(font.result, GFNT_OK);

  size_t glyphs = 0;
  ASSERT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 2u);
  size_t stated = 0;
  EXPECT_TRUE(gfnt_face_num_glyphs_disagreement(font.face, &stated));
  EXPECT_EQ(stated, 5u);
}

TEST(Cff, ATruncatedTableIsRefusedRatherThanRead) {
  CffSpec spec;
  spec.charstrings = {square(), square()};
  const std::vector<uint8_t> whole = build_cff(spec);

  // Every length from the header to one byte short of the whole: each either
  // refuses or answers exactly as the whole table does, and none crashes.
  size_t refusals = 0;
  for (size_t length = 4; length < whole.size(); ++length) {
    CffSpec cut = spec;
    cut.truncate_to = length;
    Font font(build_cff(cut));
    if (font.result != GFNT_OK) {
      ++refusals;
      continue;
    }
    GFNT_Outline * outline = nullptr;
    if (gfnt_face_glyph_outline(font.face, 1, nullptr, nullptr, &outline,
        nullptr) != GFNT_OK) {
      ++refusals;
      continue;
    }
    // A prefix that still parses must draw what the whole table draws: a
    // truncation that produced a *different* glyph would be the dangerous
    // answer, and this is where it would show.
    EXPECT_EQ(gfnt_outline_contour_count(outline), 1u) << "cut to " << length;
    gfnt_outline_destroy(outline);
  }
  // The denominator: a sweep where nothing was refused would be measuring
  // nothing, and one where everything was would mean the builder is broken.
  EXPECT_GT(refusals, 10u);
  EXPECT_LT(refusals, whole.size() - 4);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
