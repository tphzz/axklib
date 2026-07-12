#include "axklib/sdk.hpp"

int sdk_header_test() {
  axk::result<int> value{1};
  return value ? *value : 0;
}
