#include "axklib/sdk/result.hpp"

int sdk_result_header_test() {
  axk::result<void> value;
  return value ? 1 : 0;
}
