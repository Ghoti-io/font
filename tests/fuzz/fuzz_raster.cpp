/**
 * @file
 *
 * libFuzzer harness for the scan converter alone.
 *
 * documentation/design.md sections 8.1 and 14.2. The other harnesses feed bytes
 * to a parser; this one feeds them to a rasteriser, which has no bytes of its
 * own - so the input is read as a **path**: a list of points and tags that
 * become an outline directly, with no font anywhere.
 *
 * That is the point. Reaching the rasteriser through a font means every
 * interesting input has to be a font first, and the shapes that break a scan
 * converter are not the shapes a fuzzer finds by mutating a `glyf` table. They
 * are the degenerate ones: a contour of one point, coordinates at the edge of
 * the type, a curve whose control points are a million pixels away, two
 * contours that coincide exactly, a shape whose bounding box is one pixel and
 * whose points are not.
 *
 * The options byte drives the fill rule, the sub-pixel origin, the tolerance
 * and `GFNT_Limits`, because each of those changes which arm of the rasteriser
 * runs.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

namespace {

/** A stream the dumps can be written to without producing output. */
FILE * sink() {
  static FILE * stream = fopen("/dev/null", "wb");
  return stream;
}

/** A signed 26.6 coordinate from two of the fuzzer's bytes. */
GFNT_F26Dot6 coordinate(const uint8_t * bytes) {
  const int16_t raw = static_cast<int16_t>(
      (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
  return static_cast<GFNT_F26Dot6>(raw);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 6) {
    return 0;
  }

  const uint8_t options = data[0];
  const uint8_t shape = data[1];

  GFNT_Limits limits;
  gfnt_limits_default(&limits);
  if ((options & 0x01u) != 0) {
    // Small enough to refuse an ordinary glyph, so the cap's arm runs.
    limits.max_ppem = 1u + (shape >> 4);
  }
  if ((options & 0x02u) != 0) {
    limits.max_raster_bytes = 1u + shape;
  }

  GFNT_Outline * outline = nullptr;
  GFNT_Error error;

  gfnt_error_clear(&error);
  if (gfnt_outline_create(nullptr, &outline, &error) != GFNT_OK) {
    return 0;
  }

  // Five bytes per point: x, y, and a tag that also says whether to start a
  // new contour. A tag byte drives both, so the fuzzer can make a contour of
  // one point - the shortest path through the walk - without spending a byte
  // on saying so.
  bool open = false;
  for (size_t at = 2; at + 5 <= size; at += 5) {
    GFNT_Point point;
    const uint8_t tag_byte = data[at + 4];

    point.x = coordinate(data + at);
    point.y = coordinate(data + at + 2);
    if (!open || (tag_byte & 0x40u) != 0) {
      if (gfnt_outline_begin_contour(outline, &error) != GFNT_OK) {
        break;
      }
      open = true;
    }
    if (gfnt_outline_add_point(outline, point,
            static_cast<GFNT_PointTag>(tag_byte % GFNT_POINT_TAG_COUNT),
            &error)
        != GFNT_OK) {
      break;
    }
  }

  // The walk itself, before any pixels: an outline whose tags make no curve is
  // refused here, and that refusal is a path of its own.
  GFNT_Box box;
  memset(&box, 0, sizeof box);
  gfnt_outline_bounds(outline, &box);
  if ((options & 0x80u) != 0) {
    gfnt_outline_path_dump(outline, sink());
  }

  if (gfnt_outline_scale(outline,
          (options & 0x04u) != 0 ? GFNT_F16DOT16_ONE / 4 : GFNT_F16DOT16_ONE,
          &error)
      == GFNT_OK) {
    GFNT_RasterOptions raster;
    GFNT_Coverage coverage;

    memset(&raster, 0, sizeof raster);
    memset(&coverage, 0, sizeof coverage);
    raster.fill = (options & 0x08u) != 0 ? GFNT_FILL_EVEN_ODD
                                         : GFNT_FILL_NONZERO;
    raster.origin_x = (options & 0x10u) != 0 ? (shape & 0x3F) : 0;
    raster.origin_y = (options & 0x20u) != 0 ? (shape >> 2) : 0;
    raster.tolerance = (options & 0x40u) != 0 ? 1 + (shape & 0x0F) : 0;

    if (gfnt_raster_outline(outline, &raster, &limits, nullptr, &coverage,
            &error)
        == GFNT_OK) {
      // Every reader of the coverage, because a rasteriser that writes one byte
      // past its stride is found by whatever reads the last row.
      gfnt_coverage_total(&coverage);
      gfnt_coverage_hash(&coverage);
      if ((options & 0x80u) != 0) {
        gfnt_coverage_dump(&coverage, sink());
        gfnt_coverage_dump_art(&coverage, sink());
      }
      for (uint32_t row = 0; row < coverage.height; ++row) {
        for (uint32_t column = 0; column < coverage.width; ++column) {
          (void)gfnt_coverage_at(&coverage, column, row);
        }
      }
    }
    gfnt_coverage_destroy(&coverage);
  }

  gfnt_outline_destroy(outline);
  return 0;
}
