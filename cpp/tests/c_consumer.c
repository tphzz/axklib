#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "axklib/c_api.h"

#define ASSERT_VERSIONED_LAYOUT(type)                                                         \
  _Static_assert(offsetof(type, struct_size) == 0U, #type " struct_size offset changed");     \
  _Static_assert(offsetof(type, abi_version) == sizeof(uint32_t),                             \
                 #type " abi_version offset changed")

ASSERT_VERSIONED_LAYOUT(axk_content_node);
ASSERT_VERSIONED_LAYOUT(axk_object_info);
ASSERT_VERSIONED_LAYOUT(axk_validation_summary);
ASSERT_VERSIONED_LAYOUT(axk_preview_bin);
ASSERT_VERSIONED_LAYOUT(axk_error_info);
ASSERT_VERSIONED_LAYOUT(axk_write_options);
ASSERT_VERSIONED_LAYOUT(axk_plan_summary);
ASSERT_VERSIONED_LAYOUT(axk_progress_event);

#define REQUIRE(expression)                                                                 \
  do {                                                                                      \
    if (!(expression)) {                                                                    \
      fprintf(stderr, "C ABI consumer check failed at line %d: %s\n", __LINE__, #expression); \
      return __LINE__;                                                                      \
    }                                                                                       \
  } while (0)

int main(int argc, char** argv) {
  axk_context* context = NULL;
  axk_image* image = NULL;
  axk_node_result* roots = NULL;
  axk_content_node root;
  axk_validation_summary validation;
  REQUIRE(argc == 2);
  AXK_INIT_STRUCT(root);
  AXK_INIT_STRUCT(validation);
  REQUIRE(axk_abi_version() == AXK_ABI_VERSION);
  REQUIRE(axk_context_create(&context) == AXK_STATUS_OK);
  REQUIRE(context != NULL);
  REQUIRE(axk_image_open(context, (axk_string_view){argv[1], strlen(argv[1])}, &image) ==
          AXK_STATUS_OK);
  REQUIRE(image != NULL);
  REQUIRE(axk_image_content_children(
              context, image, (axk_string_view){NULL, 0U}, 0U, 32U, &roots) == AXK_STATUS_OK);
  REQUIRE(axk_node_result_total_count(roots) == 1U);
  REQUIRE(axk_node_result_count(roots) == 1U);
  REQUIRE(axk_node_result_at(roots, 0U, &root) == AXK_STATUS_OK);
  REQUIRE(root.type.size == strlen("partition"));
  REQUIRE(memcmp(root.type.data, "partition", root.type.size) == 0);
  REQUIRE(root.child_count == 1U);
  REQUIRE(axk_image_validation_summary(context, image, &validation) == AXK_STATUS_OK);
  REQUIRE(validation.valid == 1);
  REQUIRE(validation.object_count == 17U);
  REQUIRE(axk_node_result_destroy(&roots) == AXK_STATUS_OK);
  REQUIRE(roots == NULL);
  REQUIRE(axk_node_result_destroy(&roots) == AXK_STATUS_OK);
  REQUIRE(axk_image_close(&image) == AXK_STATUS_OK);
  REQUIRE(image == NULL);
  REQUIRE(axk_image_close(&image) == AXK_STATUS_OK);
  REQUIRE(axk_context_destroy(&context) == AXK_STATUS_OK);
  REQUIRE(context == NULL);
  REQUIRE(axk_context_destroy(&context) == AXK_STATUS_OK);
  return 0;
}
