#include "axklib/sdk/build_info.hpp"

int sdk_build_info_header_test() {
    const axk::build_info value{"local-unknown", "axklib-local-unknown", "", "local", "unknown", false, false};
    return value.source_identity[0] == 'l' ? 0 : 1;
}
