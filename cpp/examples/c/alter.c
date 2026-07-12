#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "axklib/c/axk.h"

int main(int argc, char **argv) {
  axk_context *context = NULL;
  uint64_t operations = 0U;
  int applied = 0;
  int result = 1;
  if (argc != 4 || axk_context_create(&context) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_hds_alter(context, (axk_string_view){argv[1], strlen(argv[1])},
                    (axk_string_view){argv[2], strlen(argv[2])},
                    (axk_string_view){argv[3], strlen(argv[3])}, &operations,
                    &applied) != AXK_STATUS_OK)
    goto cleanup;
  printf("applied %" PRIu64 " operations\n", operations);
  result = applied ? 0 : 2;
cleanup:
  axk_context_destroy(&context);
  return result;
}
