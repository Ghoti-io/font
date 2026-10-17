/*
 * One line of text rasterised by FreeType, as an 8-bit PGM.
 *
 * Usage: freetype-render <font> <ppem> <text> <hinted 0|1> <version 35|40> <out.pgm>
 *
 * The line is laid out as a person would see it and as the other side of the
 * comparison lays it out: each glyph at an integer pen position, advanced by its
 * hinted advance (hinted) or its scaled advance rounded to a pixel (unhinted),
 * with the baseline at 1.5 em below the top of a row two em and six pixels tall.
 * Nothing here is this library's: FreeType loads, hints and rasterises.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

int main(int argc, char ** argv) {
  FT_Library lib;
  FT_Face face;
  FT_UInt version;
  int ppem;
  int hinted;
  const char * text;
  int w;
  int h;
  int pen = 4;
  int base;
  unsigned char * img;
  FILE * out;

  if (argc < 7) {
    fprintf(stderr, "usage: %s <font> <ppem> <text> <hinted> <version> <out.pgm>\n", argv[0]);
    return 2;
  }
  ppem = atoi(argv[2]);
  text = argv[3];
  hinted = atoi(argv[4]);
  version = (FT_UInt)atoi(argv[5]);
  w = 8 + (int)strlen(text) * (ppem + 4);
  h = ppem * 2 + 6;
  base = 4 + (int)(ppem * 1.5);
  img = malloc((size_t)w * (size_t)h);
  memset(img, 255, (size_t)w * (size_t)h);
  if (FT_Init_FreeType(&lib)) return 1;
  FT_Property_Set(lib, "truetype", "interpreter-version", &version);
  if (FT_New_Face(lib, argv[1], 0, &face) || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem)) return 1;
  for (const char * c = text; *c; ++c) {
    FT_Int32 flags = FT_LOAD_RENDER | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT
        | FT_LOAD_TARGET_NORMAL | (hinted ? 0 : FT_LOAD_NO_HINTING);
    FT_GlyphSlot slot;
    int adv;

    if (FT_Load_Char(face, (FT_ULong)(unsigned char)*c, flags)) continue;
    slot = face->glyph;
    for (unsigned y = 0; y < slot->bitmap.rows; ++y) {
      for (unsigned x = 0; x < slot->bitmap.width; ++x) {
        int px = pen + slot->bitmap_left + (int)x;
        int py = base - slot->bitmap_top + (int)y;
        int v;

        if (px < 0 || px >= w || py < 0 || py >= h) continue;
        v = 255 - slot->bitmap.buffer[y * (unsigned)slot->bitmap.pitch + x];
        if (v < img[py * w + px]) img[py * w + px] = (unsigned char)v;
      }
    }
    if (hinted) {
      adv = (int)((slot->metrics.horiAdvance + 32) >> 6);
    }
    else {
      adv = (int)((slot->linearHoriAdvance + 0x8000) >> 16);   /* 16.16 pixels */
    }
    pen += adv;
  }
  out = fopen(argv[6], "wb");
  fprintf(out, "P5\n%d %d\n255\n", w, h);
  fwrite(img, 1, (size_t)w * (size_t)h, out);
  fclose(out);
  return 0;
}
