#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "axklib/c/axk.h"

int main(int argc, char **argv) {
  axk_context *context = NULL;
  axk_image *image = NULL;
  uint64_t count = 0U;
  int result = 1;
  if (argc != 3 || axk_context_create(&context) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_image_open(context, (axk_string_view){argv[1], strlen(argv[1])}, &image) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_image_export_audio(context, image, (axk_string_view){argv[2], strlen(argv[2])}, 0, 0,
                             &count) != AXK_STATUS_OK)
    goto cleanup;
  printf("wrote %" PRIu64 " files\n", count);
  result = 0;
cleanup:
  axk_image_close(&image);
  axk_context_destroy(&context);
  return result;
}
