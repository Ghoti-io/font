/**
 * @file
 *
 * libFuzzer harness for the sfnt container and every table parser behind it.
 *
 * documentation/design.md section 14.2. The whole input after the first byte is
 * a font file, so the fuzzer works on the directory, the collection header and
 * every table this library parses at once - which is the right shape for layer
 * 0, because those are all offset arithmetic over the same bytes.
 *
 * **The first byte is the options byte**, and it drives GFNT_Limits and the
 * face index. Without it the fuzzer would only ever exercise the default caps,
 * and a limit nothing reaches is a promise nobody has checked (section 15.2).
 *
 * The blob borrows the fuzzer's buffer rather than copying it, so a read one
 * byte past the end is an ASan report rather than a read of this harness's own
 * heap.
 *
 * Copyright 2026 by Corey Pennycuff

/**
 * @file
 *
 * Fuzz reading a kept font set back.
 *
 * The first byte is the options byte every seed here begins with, and is not used.
 *
 * ::gfnt_fontset_deserialize() ends in a checksum, and a fuzzer cannot forge one, so a
 * harness handed the whole form would never get past it. The input is therefore the *body*:
 * this adds the magic and the checksum itself, and every byte pattern reaches the parser. What is
 * checked beyond not crashing: a set that was read serialises, reads back, and serialises to the
 * same bytes (what is read is a fixed point of writing), and every string in it is a string.
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include <ghoti.io/font/discover.h>

namespace {

uint32_t checksum(const uint8_t * p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

std::vector<uint8_t> written(const GFNT_FontSet * set) {
  size_t need = 0;
  gfnt_fontset_serialize(set, nullptr, 0, &need, nullptr);
  std::vector<uint8_t> out(need);
  size_t wrote = 0;
  if (gfnt_fontset_serialize(set, out.data(), out.size(), &wrote, nullptr) != GFNT_OK
      || wrote != need) {
    __builtin_trap();
  }
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }
  ++data;
  --size;
  std::vector<uint8_t> form = {'G', 'F', 'N', 'T', 'S', 'E', 'T', '1'};
  form.insert(form.end(), data, data + size);
  const uint32_t sum = checksum(form.data(), form.size());
  for (int i = 0; i < 4; ++i) {
    form.push_back(static_cast<uint8_t>(sum >> (8 * i)));
  }
  GFNT_FontSet * set = nullptr;
  if (gfnt_fontset_deserialize(form.data(), form.size(), nullptr, &set, nullptr) != GFNT_OK) {
    return 0;
  }
  for (size_t i = 0; i < gfnt_fontset_count(set); ++i) {
    const GFNT_FontRecord * r = gfnt_fontset_at(set, i);
    if (!r || !r->path || !r->family || !r->style || r->slant > GFNT_SLANT_OBLIQUE) {
      __builtin_trap();
    }
  }
  const std::vector<uint8_t> once = written(set);
  GFNT_FontSet * again = nullptr;
  if (gfnt_fontset_deserialize(once.data(), once.size(), nullptr, &again, nullptr) != GFNT_OK) {
    __builtin_trap();
  }
  if (written(again) != once) {
    __builtin_trap();
  }
  gfnt_fontset_free(again);
  gfnt_fontset_free(set);
  return 0;
}
