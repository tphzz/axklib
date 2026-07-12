#include "axklib/c/axk.h"

int main(void) {
  axk_context *context = 0;
  if (axk_abi_version_major() != 1U || axk_context_create(&context) != AXK_STATUS_OK)
    return 1;
  return axk_context_destroy(&context) == AXK_STATUS_OK ? 0 : 2;
}
