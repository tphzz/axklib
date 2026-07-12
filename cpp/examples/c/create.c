#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "axklib/c/axk.h"

int main(int argc, char **argv) {
  axk_context *context = NULL;
  uint64_t partitions = 0U;
  int result = 1;
  if (argc != 3 || axk_context_create(&context) != AXK_STATUS_OK)
    goto cleanup;
  if (axk_hds_create(context, (axk_string_view){argv[1], strlen(argv[1])},
                     (axk_string_view){argv[2], strlen(argv[2])}, 0, &partitions) != AXK_STATUS_OK)
    goto cleanup;
  printf("created %" PRIu64 " partitions\n", partitions);
  result = 0;
cleanup:
  axk_context_destroy(&context);
  return result;
}
