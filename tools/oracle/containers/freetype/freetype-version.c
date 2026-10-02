/* What this oracle is, asked of the library rather than of the Containerfile.
 *
 * `FT_Library_Version` reports what the shared object actually is, which is the
 * claim `oracle_env.check_pin()` needs: the ARG in the Containerfile says what
 * the build was told to fetch, and an image is not its recipe.
 */
#include <stdio.h>

#include <ft2build.h>
#include FT_FREETYPE_H

int main(void) {
  FT_Library library;
  FT_Int major = 0;
  FT_Int minor = 0;
  FT_Int patch = 0;

  if (FT_Init_FreeType(&library) != 0) {
    fprintf(stderr, "FT_Init_FreeType failed\n");
    return 1;
  }
  FT_Library_Version(library, &major, &minor, &patch);
  printf("FreeType %d.%d.%d\n", (int)major, (int)minor, (int)patch);
  FT_Done_FreeType(library);
  return 0;
}
