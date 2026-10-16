/*
 * FreeType's hinted outline for every glyph at a pixel size, as 26.6 integers.
 *
 * Usage: freetype-hinted <font> <face> <version> <ppem> <stride> <first> [target]
 *
 *   version  35 or 40: the TrueType interpreter version FreeType is asked to run;
 *            0 asks for no hinting at all (the outline scaled and nothing else).
 *   ppem     whole pixels per em.
 *   stride   every nth glyph, from glyph `first`.
 *   target   normal (default), light, mono or lcd: the render target flag, which
 *            changes what a version 40 interpreter does.
 *
 * Prints, per glyph:
 *
 *   g<id> adv <horiAdvance> pts <n> ctr <n>
 *   e <last point of contour 0> <of contour 1> ...
 *   p <x> <y> <1 on curve | 0 off curve>        (one line per point)
 *
 * or `g<id> error <code>` when FreeType refused the glyph. Nothing is rounded or
 * adjusted here: the numbers are what the outline holds after hinting.
 *
 * Compiled in the image against the FreeType it is built with, and links nothing
 * else, so that the reference cannot reach the implementation it answers for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H

int main(int argc, char ** argv) {
  FT_Library library;
  FT_Face face;
  FT_Error error;
  FT_UInt version;
  FT_Int32 flags;
  long ppem;
  long stride;
  long first;
  const char * target = argc > 7 ? argv[7] : "normal";

  if (argc < 7) {
    fprintf(stderr, "usage: %s <font> <face> <version> <ppem> <stride> <first> [target]\n",
        argv[0]);
    return 2;
  }
  version = (FT_UInt)strtoul(argv[3], NULL, 10);
  ppem = strtol(argv[4], NULL, 10);
  stride = strtol(argv[5], NULL, 10);
  first = strtol(argv[6], NULL, 10);
  if (stride < 1) {
    stride = 1;
  }
  if (FT_Init_FreeType(&library)) {
    fprintf(stderr, "FT_Init_FreeType failed\n");
    return 1;
  }
  if (version == 35 || version == 40) {
    FT_UInt v = version;

    if (FT_Property_Set(library, "truetype", "interpreter-version", &v)) {
      fprintf(stderr, "this FreeType has no interpreter version %u\n", version);
      return 1;
    }
  }
  error = FT_New_Face(library, argv[1], strtol(argv[2], NULL, 10), &face);
  if (error) {
    fprintf(stderr, "%s: FT_New_Face error %d\n", argv[1], error);
    return 1;
  }
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem)) {
    fprintf(stderr, "FT_Set_Pixel_Sizes failed\n");
    return 1;
  }
  flags = FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT;
  if (version == 0) {
    flags |= FT_LOAD_NO_HINTING;
  }
  else if (strcmp(target, "light") == 0) {
    flags |= FT_LOAD_TARGET_LIGHT;
  }
  else if (strcmp(target, "mono") == 0) {
    flags |= FT_LOAD_TARGET_MONO;
  }
  else if (strcmp(target, "lcd") == 0) {
    flags |= FT_LOAD_TARGET_LCD;
  }
  else {
    flags |= FT_LOAD_TARGET_NORMAL;
  }
  for (long g = first; g < face->num_glyphs; g += stride) {
    FT_Outline * outline;

    error = FT_Load_Glyph(face, (FT_UInt)g, flags);
    if (error) {
      printf("g%ld error %d\n", g, error);
      continue;
    }
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
      printf("g%ld error format\n", g);
      continue;
    }
    outline = &face->glyph->outline;
    printf("g%ld adv %ld pts %d ctr %d\n", g, (long)face->glyph->metrics.horiAdvance,
        outline->n_points, outline->n_contours);
    printf("e");
    for (int c = 0; c < outline->n_contours; ++c) {
      printf(" %d", outline->contours[c]);
    }
    printf("\n");
    for (int p = 0; p < outline->n_points; ++p) {
      printf("p %ld %ld %d\n", (long)outline->points[p].x, (long)outline->points[p].y,
          outline->tags[p] & 1);
    }
  }
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}
