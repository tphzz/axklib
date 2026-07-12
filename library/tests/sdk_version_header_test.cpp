#include "axklib/sdk/version.hpp"

static_assert(axk::version_major == 0);
static_assert(axk::version_minor == 1);
static_assert(axk::version_patch == 0);

int sdk_version_header_test() { return axk::version_string[0] == '0' ? 1 : 0; }
