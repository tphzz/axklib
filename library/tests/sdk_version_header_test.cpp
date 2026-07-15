#include "axklib/sdk/version.hpp"

static_assert(axk::version_major == AXKLIB_VERSION_MAJOR);
static_assert(axk::version_minor == AXKLIB_VERSION_MINOR);
static_assert(axk::version_patch == AXKLIB_VERSION_PATCH);
static_assert(axk::version_string[0] != '\0');

int sdk_version_header_test() { return axk::version_string[0] == '\0' ? 1 : 0; }
