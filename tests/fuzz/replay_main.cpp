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
 * The replay gate's driver (`make fuzz-replay`): every file given is fed once
 * through a harness's `LLVMFuzzerTestOneInput`, in an ordinary build with the
 * build's own compiler, so that a seed a campaign kept is a test and not a
 * campaign to repeat.  Linked with each harness in turn.  It fails on no input
 * at all, which would report success over a population of zero; a crash or an
 * abort in the harness fails the run, naming the file it was reading.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);
extern "C" int LLVMFuzzerInitialize(int * argc, char *** argv) __attribute__((weak));

int main(int argc, char ** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: replay FILE...\n");
    return 2;
  }
  if (LLVMFuzzerInitialize) {
    LLVMFuzzerInitialize(&argc, &argv);
  }
  for (int i = 1; i < argc; i++) {
    std::FILE * file = std::fopen(argv[i], "rb");
    if (!file) {
      std::fprintf(stderr, "replay: cannot open %s\n", argv[i]);
      return 2;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    std::fprintf(stderr, "replay: %s\n", argv[i]); // the last name printed is the input a crash came from
    static const uint8_t empty = 0; // libFuzzer never passes NULL, even for an empty input
    LLVMFuzzerTestOneInput(bytes.empty() ? &empty : bytes.data(), bytes.size());
#ifdef GFNT_REPLAY_PLANT
    std::abort(); // the gate's plant: a crash after the first input, which the gate must see
#endif
  }
  std::printf("replay: %d inputs, no crash\n", argc - 1);
  return 0;
}
