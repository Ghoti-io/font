/**
 * @file
 *
 * libFuzzer harness for the charstring interpreters, with no font at all.
 *
 * documentation/design.md section 14.2, and the same argument `fuzz_raster`
 * makes: the interpreter has no bytes of its own inside a container, so reaching
 * it through a CFF would mean every interesting *program* had to be a valid font
 * first. A charstring is a program, and what breaks an interpreter is a program -
 * a stack that underflows, an operand that ends mid-number, a subroutine that
 * calls itself, a `hintmask` whose stem count nothing declared.
 *
 * The input is split into a charstring and up to four subroutines, both local and
 * global, and the fuzzer picks where the cuts are. Subroutines are the reason for
 * the split: a program's own bytes cannot produce a subroutine call that *lands*
 * anywhere unless the harness supplies an INDEX for it to land in, and
 * subroutine dispatch - biased for Type 2, unbiased for Type 1 - is the part a
 * caller cannot see and a font can lie about.
 *
 * **Both languages run on every input.** They divide bytes into operators
 * differently: 255 introduces a 16.16 in one and an integer in the other, 13 is
 * `hsbw` in one and reserved in the other, and the same bytes are therefore two
 * programs. Running only one would leave half the dispatch unreached.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ghoti.io/font/charstring.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/outline.h>

namespace {

/** A stream the dump can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

/** The subroutines the harness hands the interpreter. */
struct Subrs {
  std::vector<std::vector<uint8_t>> items;

  static GFNT_Result at(void * user, uint32_t index, const uint8_t ** bytes,
      size_t * length) {
    Subrs * self = static_cast<Subrs *>(user);
    if (index >= self->items.size()) {
      return GFNT_ERR_CORRUPT;
    }
    *bytes = self->items[index].empty() ? nullptr : self->items[index].data();
    *length = self->items[index].size();
    return GFNT_OK;
  }
};

/**
 * A charstring for an accented character's base and accent.
 *
 * Every code answers with the *same* program, which is what makes `seac`
 * reachable at all: the fuzzer would otherwise have to produce a resolver as
 * well. The nesting guard is what stops it recursing, and that guard is one of
 * the things worth fuzzing.
 */
struct Standard {
  std::vector<uint8_t> program;

  static GFNT_Result at(void * user, uint8_t code, const uint8_t ** bytes,
      size_t * length) {
    Standard * self = static_cast<Standard *>(user);
    (void)code;
    *bytes = self->program.empty() ? nullptr : self->program.data();
    *length = self->program.size();
    return GFNT_OK;
  }
};

void fail(const char * what) {
  fprintf(stderr, "fuzz_charstring: %s\n", what);
  abort();
}

/** The points of an outline, flat, tags included. */
std::vector<int32_t> flat(const GFNT_Outline * outline) {
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

/**
 * Run the program again recording its hints, and check what recording promises: it changes neither the
 * result nor a point, and the record is within its bounds and indexes real points.
 */
void check_recording(GFNT_CharstringType type, const std::vector<uint8_t> & program,
    const GFNT_CharstringContext * context, GFNT_Result plain_result, const GFNT_Outline * plain) {
  GFNT_CharstringContext recording = *context;
  static GFNT_CharstringHints hints;
  GFNT_Outline * outline = nullptr;
  GFNT_Error error;

  gfnt_error_clear(&error);
  memset(&hints, 0xA5, sizeof hints);
  recording.hints = &hints;
  if (gfnt_outline_create(nullptr, &outline, &error) != GFNT_OK) {
    return;
  }
  const GFNT_Result result = gfnt_charstring_run(type, program.empty() ? nullptr : program.data(),
      program.size(), &recording, outline, nullptr, &error);

  if (result != plain_result) {
    fail("recording hints changed the result");
  }
  if (result == GFNT_OK) {
    if (flat(outline) != flat(plain)) {
      fail("recording hints changed the outline");
    }
    if (hints.stem_count > GFNT_CS_MAX_STEMS || hints.mask_count > GFNT_CS_MAX_MASKS) {
      fail("a hint record past its bounds");
    }
    if (hints.valid) {
      const size_t points = gfnt_outline_point_count(outline);
      uint32_t last = 0;

      for (size_t i = 0; i < hints.mask_count; ++i) {
        if (hints.masks[i].first_point < last || hints.masks[i].first_point > points) {
          fail("a hint mask at a point that is not there or out of order");
        }
        last = hints.masks[i].first_point;
      }
      if (hints.accent && hints.accent_first_point > points) {
        fail("an accent that starts past the outline");
      }
    }
  }
  gfnt_outline_destroy(outline);
}

/** Run one program in one language, into a fresh outline. */
void run(GFNT_CharstringType type, const std::vector<uint8_t> & program,
    GFNT_CharstringContext * context, bool dump, bool record) {
  GFNT_Outline * outline = nullptr;
  GFNT_CharstringMetrics metrics;
  GFNT_Error error;

  memset(&metrics, 0, sizeof metrics);
  gfnt_error_clear(&error);
  if (gfnt_outline_create(nullptr, &outline, &error) != GFNT_OK) {
    return;
  }
  const GFNT_Result result = gfnt_charstring_run(type, program.empty() ? nullptr : program.data(),
      program.size(), context, outline, &metrics, &error);

  if (record) {
    check_recording(type, program, context, result, outline);
  }
  if (result == GFNT_OK) {
    GFNT_Box box;

    memset(&box, 0, sizeof box);
    gfnt_outline_bounds(outline, &box);
    gfnt_outline_control_box(outline, &box);
    gfnt_outline_path_dump(outline, sink());
  }
  if (dump) {
    // With the metrics and without: the two divide the bytes differently at a
    // `hintmask` that follows a subroutine call, so both walks get the input.
    gfnt_charstring_dump(type, program.empty() ? nullptr : program.data(),
        program.size(), &metrics, sink());
    gfnt_charstring_dump(type, program.empty() ? nullptr : program.data(),
        program.size(), nullptr, sink());
  }
  gfnt_outline_destroy(outline);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }

  const uint8_t options = data[0];
  const uint8_t cuts = data[1];
  const uint8_t * rest = data + 2;
  const size_t left = size - 2;

  // The program, then up to four subroutines, at positions the fuzzer picks.
  const size_t split = (static_cast<size_t>(cuts) * left) / 256u;
  std::vector<uint8_t> program(rest, rest + split);
  Subrs local;
  Subrs global;
  Standard standard;

  const size_t tail = left - split;
  if (tail > 0) {
    const size_t pieces = 1u + (options & 0x03u);
    const size_t each = tail / pieces + 1u;
    for (size_t at = split; at < left; at += each) {
      const size_t end = at + each < left ? at + each : left;
      local.items.push_back(std::vector<uint8_t>(rest + at, rest + end));
    }
    // The global INDEX is the same pieces in the other order, so that a
    // subroutine number that lands in one lands somewhere different in the
    // other and the two index spaces are not one.
    global.items.assign(local.items.rbegin(), local.items.rend());
    standard.program = local.items.front();
  }

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if ((options & 0x04u) != 0) {
    limits.max_outline_points = 1u + (options >> 4);
  }
  if ((options & 0x08u) != 0) {
    limits.max_charstring_depth = options >> 5;
  }
  if ((options & 0x10u) != 0) {
    limits.max_charstring_ops = 1u + (options >> 2);
  }

  GFNT_CharstringContext context;

  memset(&context, 0, sizeof context);
  context.local.at = &Subrs::at;
  context.local.user = &local;
  context.local.count = local.items.size();
  context.global.at = &Subrs::at;
  context.global.user = &global;
  context.global.count = global.items.size();
  context.limits = &limits;
  // The width defaults, from the options byte: a stated advance is a delta from
  // one of them and a program that states none takes the other, so leaving both
  // at zero would make the two indistinguishable.
  context.nominal_width = static_cast<GFNT_F16Dot16>(options) << 16;
  context.default_width = static_cast<GFNT_F16Dot16>(cuts) << 16;
  if ((options & 0x20u) != 0) {
    context.standard_code = &Standard::at;
    context.standard_user = &standard;
  }

  const bool dump = (options & 0x40u) != 0;
  const bool record = (options & 0x80u) != 0;
  run(GFNT_CHARSTRING_TYPE2, program, &context, dump, record);
  run(GFNT_CHARSTRING_TYPE1, program, &context, dump, record);
  return 0;
}
