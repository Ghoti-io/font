/* FreeType's reading of an sfnt's `EBLC`/`EBDT` strikes, as comparable lines.
 *
 * Runs inside the pinned oracle image. `eblc_diff.py` runs this, the fontTools
 * adapter, and `examples/font-bitmap --strikes`, and compares them line for
 * line: every line is `<key> <value>`, so a disagreement names a field.
 *
 * Four things about this reference, which are facts about FreeType rather than
 * about the comparison:
 *
 *   * **`FT_LOAD_SBITS_ONLY` is why this is a reading of `EBDT` and not of an
 *     outline.** Both fonts in the original population carry `glyf` as well,
 *     and FreeType prefers an embedded bitmap at a matching ppem and silently
 *     scales an outline when there is none. A driver that loaded with
 *     `FT_LOAD_RENDER` would therefore report pixels for every glyph of every
 *     strike, agree with nothing, and look like a disagreement in this library.
 *     With this flag a glyph that has no `EBDT` entry *fails to load*, which is
 *     the answer we want to compare.
 *
 *   * **A glyph this strike does not carry and a glyph whose data is broken are
 *     one answer here.** FreeType has both `Invalid_Argument` and
 *     `Invalid_File_Format` in `ttsbit.c`, so the split looked available - and
 *     over the one font in the population that has a broken glyph it is not:
 *     mona.ttf's last glyph, whose offset pair runs backwards, comes back from
 *     FreeType as `Invalid_Argument`, the same code as a glyph no subtable
 *     covers. So what this reference can corroborate is the *set of glyphs that
 *     produce pixels* - which it does, 7,224 of 7,225 per strike, agreeing with
 *     this library exactly - and not the absent/corrupt classification M11
 *     draws. The per-glyph state printed below is therefore `present` or
 *     `uncarried`, and `census coarse state` says so, so that the differential
 *     coarsens a stated thing rather than scoring a wording difference 21,672
 *     times.
 *
 *   * **No per-strike bit depth is reachable.** FreeType exposes a strike as an
 *     `FT_Bitmap_Size`, which carries ppem and no depth, and converts depths 2,
 *     4 and 8 to 8-bit gray on the way out. The depth printed per glyph below is
 *     therefore recovered from `pixel_mode` and `num_grays`, and the keys this
 *     reference cannot answer at all are named in `census unanswered` lines so
 *     that the differential skips a stated list rather than silently comparing
 *     nothing.
 *
 *   * **`FT_IS_SCALABLE` is the axis this whole driver turns on**, and it decides
 *     two different questions in opposite directions. Both arms of it were a bug
 *     in the first draft, and both bugs printed plausible numbers.
 *
 *     *The baseline* comes from `FT_Select_Size`, which goes through
 *     `tt_size_select`: a **scalable** face gets `FT_Select_Metrics`, so
 *     `size->metrics.ascender` is `hhea`'s ascender scaled to the ppem and the
 *     strike's own `sbitLineMetrics` are never consulted; only a bitmap-only face
 *     reaches `load_strike_metrics` and reports the strike. The first draft
 *     printed `size->metrics` either way and said `ascent 11` for a mona.ttf
 *     strike whose byte says 10 - not a disagreement about `EBLC`, but one reader
 *     answering a different question in the same units. So the baseline is
 *     printed for a bitmap-only face and disclaimed for a scalable one.
 *
 *     *Absence* goes the other way. `tt_sbit_decoder_load_image` returns
 *     `Missing_Bitmap` for a glyph no index subtable covers, and for a scalable
 *     face `FT_LOAD_SBITS_ONLY` turns that into `Invalid_Argument` and a failed
 *     load, which is what this driver reads. For a **bitmap-only** face
 *     `ttgload.c` intercepts it first - "a missing glyph in a bitmap-only font is
 *     assumed whitespace that needs to be constructed using metrics data from
 *     `hmtx`" - and returns a *successful* 0x0 bitmap carrying the `hmtx`
 *     advance. So on such a face FreeType cannot report absence at all, and a
 *     glyph the strike does not carry is indistinguishable from one it carries
 *     with an empty box. `census coarse empty-vs-absent` says so and the
 *     differential merges exactly those two states for that face and no other.
 *
 *     Note that `FT_IS_SCALABLE` is FreeType's judgement and not "has a `glyf`
 *     table": Wine's system.ttf has one and is bitmap-only by this test, which is
 *     how 191 glyphs of its 18 ppem strike came back as whitespace the table does
 *     not contain. Reading the flag is the only way to know which arm applies.
 *
 *   * **A zero advance is filled in from `hmtx`, so it is not this table's.**
 *     `ttgload.c` sets the advance from the sbit and then, twenty lines later,
 *     does `if ( !glyph->metrics.horiAdvance && glyph->linearHoriAdvance )` and
 *     replaces it with the scaled outline's. That is the right answer for a
 *     renderer and the wrong one for a differential: 266 glyphs across uming.ttc,
 *     titr.ttf, system.ttf, tahoma.ttf and ms_sans_serif.ttf state an advance of
 *     zero, fontTools reports zero, and FreeType reports what the outline would
 *     advance. `census fills zero-advance` says so, and the differential then
 *     allows exactly that shape - our zero against a non-zero here - and nothing
 *     else about an advance. The fontTools arm is what keeps that exemption
 *     honest, because an advance this library reported as zero *wrongly* would
 *     disagree with the reference that does read the table's own value.
 *
 *   * **Nothing here reads `EBSC`.** FreeType has no `EBSC` support, so a face
 *     whose scaled strikes live there reports only the strikes `EBLC` states.
 *     fontTools is the reference for that table.
 *
 * Usage: freetype-strikes <font> [face-index] [stride]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H

/* The one load flag that makes this a reading of the bitmap tables. See above. */
#define LOAD_FLAGS (FT_LOAD_SBITS_ONLY | FT_LOAD_NO_AUTOHINT)

/* What a glyph turned out to be. */
enum state { STATE_PRESENT, STATE_ABSENT, STATE_CORRUPT };

static enum state classify(FT_Error error) {
  if (error == 0) {
    return STATE_PRESENT;
  }
  /* `Invalid_Argument` is the `EBLC` walk finding no entry for the glyph, which
   * is "this strike does not carry it". Anything else is data that was found and
   * did not parse. Collapsing the two would make this reference unable to
   * disagree with M11, which is one of the three reasons it exists. */
  // Both codes mean "there is no sbit here". `FT_LOAD_SBITS_ONLY` turns the
  // decoder's `Missing_Bitmap` into `Invalid_Argument` on the way out of
  // `ttgload.c`, and which one surfaces depends on the path; neither is a
  // statement that the data was bad.
  if (error == FT_Err_Invalid_Argument || error == FT_Err_Missing_Bitmap) {
    return STATE_ABSENT;
  }
  return STATE_CORRUPT;
}

/* `present` or `uncarried`: the distinction FreeType can actually make. See the
 * file comment - the two error codes exist and the font that would separate them
 * does not separate them. */
static const char * name_of(enum state state) {
  return state == STATE_PRESENT ? "present" : "uncarried";
}

/* The bit depth, recovered rather than read: see the file comment. */
static int depth_of(const FT_Bitmap * bitmap) {
  if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
    return 1;
  }
  switch (bitmap->num_grays) {
    case 4:   return 2;
    case 16:  return 4;
    case 256: return 8;
    default:  return 0;
  }
}

/* One row of a monochrome bitmap as `#` and `.`, truncated to the width.
 *
 * Truncated for the same reason the fontTools adapter truncates: the bits past
 * the width are nobody's promise. A negative pitch means the rows run upwards in
 * the buffer, which FreeType is allowed to do and which no font here does - so it
 * is handled rather than asserted, because an assertion that never fires and a
 * branch that is never taken are equally untested and the branch is at least
 * correct if it ever is.
 */
static void print_row(const FT_Bitmap * bitmap, unsigned int y) {
  int pitch = bitmap->pitch;
  const unsigned char * row = bitmap->buffer;
  unsigned int x;

  if (pitch < 0) {
    row += (size_t)(-pitch) * (bitmap->rows - 1 - y);
  }
  else {
    row += (size_t)pitch * y;
  }
  for (x = 0; x < bitmap->width; ++x) {
    unsigned int set;

    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
      set = (row[x >> 3] >> (7 - (x & 7))) & 1u;
    }
    else {
      set = row[x] != 0;
    }
    fputc(set ? '#' : '.', stdout);
  }
}

static int report(const char * path, int index, int stride) {
  FT_Library library;
  FT_Face face;
  FT_Error error;
  FT_Long faces;
  FT_Long glyphs;
  int strike;

  if (FT_Init_FreeType(&library) != 0) {
    fprintf(stderr, "FT_Init_FreeType failed\n");
    return 1;
  }
  error = FT_New_Face(library, path, index, &face);
  if (error != 0) {
    fprintf(stderr, "FT_New_Face(%s, %d): error %d\n", path, index,
        (int)error);
    FT_Done_FreeType(library);
    return 1;
  }
  faces = face->num_faces;
  glyphs = face->num_glyphs;

  printf("faces %ld\n", (long)faces);
  printf("glyphs %ld\n", (long)glyphs);
  printf("strikes %d\n", (int)face->num_fixed_sizes);

  /* The keys this reference has no way to answer, printed rather than left out.
   * A differential that skipped them by a list of its own would stop skipping
   * them the day FreeType grew an accessor and nobody would notice; a
   * differential that skips what the reference *says* it cannot answer follows
   * the reference, and a reference that stops disclaiming a key is immediately
   * compared on it. */
  printf("census unanswered strike depth\n");
  printf("census unanswered strike absent\n");
  printf("census unanswered strike corrupt\n");
  printf("census coarse state\n");
  printf("census fills zero-advance\n");
  // `EBSC` is read by nothing here: FreeType has no support for the table, so a
  // face whose scaled sizes live there reports only the strikes `EBLC` states.
  // Declared rather than silently absent, so that the differential skips a stated
  // list - and so that a FreeType which grows EBSC support is compared on it.
  printf("census unanswered scaled\n");
  printf("census unanswered scale ppem_x\n");
  printf("census unanswered scale ppem_y\n");
  printf("census unanswered scale sub_ppem_x\n");
  printf("census unanswered scale sub_ppem_y\n");
  printf("census unanswered scale sub_index\n");
  printf("census unanswered scale ascent\n");
  printf("census unanswered scale descent\n");
  /* Whether the baseline below means the strike or the outline. See the file
   * comment; this is the fact the next two lines depend on, so it is printed. */
  printf("census outlines %d\n", FT_IS_SCALABLE(face) ? 1 : 0);
  if (FT_IS_SCALABLE(face)) {
    printf("census unanswered strike ascent\n");
    printf("census unanswered strike descent\n");
  }
  else {
    // See the file comment: on a bitmap-only face a glyph the strike does not
    // carry arrives as a successful empty bitmap with an `hmtx` advance, so
    // absence and an empty box are one answer here. The counts below are affected
    // the same way, so they are disclaimed too rather than printed as facts.
    printf("census coarse empty-vs-absent\n");
    printf("census unanswered strike present\n");
  }

  for (strike = 0; strike < face->num_fixed_sizes; ++strike) {
    FT_Bitmap_Size * size = &face->available_sizes[strike];
    long present = 0;
    long absent = 0;
    long corrupt = 0;
    FT_Long glyph;

    error = FT_Select_Size(face, strike);
    if (error != 0) {
      fprintf(stderr, "FT_Select_Size(%d): error %d\n", strike, (int)error);
      FT_Done_Face(face);
      FT_Done_FreeType(library);
      return 1;
    }
    printf("strike %d ppem_x %ld\n", strike, (long)(size->x_ppem >> 6));
    printf("strike %d ppem_y %ld\n", strike, (long)(size->y_ppem >> 6));
    /* The strike's own `sbitLineMetrics`, and *only* when the face has no
     * outlines for `tt_size_select` to prefer. See the file comment. */
    if (!FT_IS_SCALABLE(face)) {
      printf("strike %d ascent %ld\n", strike,
          (long)(face->size->metrics.ascender >> 6));
      printf("strike %d descent %ld\n", strike,
          (long)(face->size->metrics.descender >> 6));
    }

    /* Every glyph, whatever the stride: the three counts are what the whole
     * index subtable walk decides, and a subtable read at the wrong offset moves
     * a count that a sampled pixel would step over. */
    for (glyph = 0; glyph < glyphs; ++glyph) {
      switch (classify(FT_Load_Glyph(face, (FT_UInt)glyph, LOAD_FLAGS))) {
        case STATE_PRESENT: ++present; break;
        case STATE_ABSENT:  ++absent;  break;
        default:            ++corrupt; break;
      }
    }
    /* `present` is compared; `absent` and `corrupt` are disclaimed above and
     * printed anyway, because "FreeType put the odd glyph in absent" is the fact
     * that explains the disclaimer and a reader of this output should see it. */
    printf("strike %d present %ld\n", strike, present);
    printf("strike %d absent %ld\n", strike, absent);
    printf("strike %d corrupt %ld\n", strike, corrupt);

    for (glyph = 0; glyph < glyphs; ++glyph) {
      enum state state;
      FT_GlyphSlot slot;
      unsigned int y;

      /* Every stride'th glyph **and the last**: the final entry of an offset
       * array is the one a producer gets wrong, so a stride that stepped over it
       * would leave the one glyph that matters unsampled. */
      if (glyph % stride != 0 && glyph + 1 != glyphs) {
        continue;
      }
      state = classify(FT_Load_Glyph(face, (FT_UInt)glyph, LOAD_FLAGS));
      printf("g %ld %d state %s\n", (long)glyph, strike, name_of(state));
      if (state != STATE_PRESENT) {
        continue;
      }
      slot = face->glyph;
      if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        /* Unreachable with FT_LOAD_SBITS_ONLY, and checked anyway: if it ever
         * fires, this driver has started reporting a scaled outline as a strike's
         * pixels and every number it prints is about a different table. */
        fprintf(stderr, "glyph %ld of strike %d came back as format 0x%x, not a "
            "bitmap - FT_LOAD_SBITS_ONLY is not doing what this driver assumes\n",
            (long)glyph, strike, (unsigned)slot->format);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
      }
      printf("g %ld %d box %u %u %d %d %ld %d\n", (long)glyph, strike,
          slot->bitmap.width, slot->bitmap.rows,
          slot->bitmap_left, slot->bitmap_top,
          (long)(slot->metrics.horiAdvance >> 6),
          depth_of(&slot->bitmap));
      for (y = 0; y < slot->bitmap.rows; ++y) {
        printf("g %ld %d row %u ", (long)glyph, strike, y);
        print_row(&slot->bitmap, y);
        fputc('\n', stdout);
      }
    }
  }

  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}

int main(int argc, char ** argv) {
  int index = 0;
  int stride = 1;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [face-index] [stride]\n", argv[0]);
    return 2;
  }
  if (argc > 2) {
    index = atoi(argv[2]);
  }
  if (argc > 3) {
    stride = atoi(argv[3]);
    if (stride < 1) {
      stride = 1;
    }
  }
  return report(argv[1], index, stride);
}
