#include <stdio.h>
#include <string.h>

#include "axklib/c/axk.h"

int main(int argc, char **argv) {
  axk_context *context = NULL;
  axk_image *image = NULL;
  axk_object_result *objects = NULL;
  int result = 1;
  if (argc != 2 || axk_context_create(&context) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_image_open(context, (axk_string_view){argv[1], strlen(argv[1])}, &image) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_image_objects(context, image, 0U, 1024U, &objects) != AXK_STATUS_OK)
    goto cleanup;
  for (uint64_t index = 0; index < axk_object_result_count(objects); ++index) {
    axk_object_info object;
    AXK_INIT_STRUCT(object);
    if (axk_object_result_at(objects, index, &object) != AXK_STATUS_OK)
      goto cleanup;
    printf("%.*s\t%.*s\n", (int)object.object_type.size, object.object_type.data,
           (int)object.object_name.size, object.object_name.data);
  }
  result = 0;
cleanup:
  axk_object_result_destroy(&objects);
  axk_image_close(&image);
  axk_context_destroy(&context);
  return result;
}
