#include "axklib/sdk.hpp"

int sdk_header_test() {
  axk::result<int> value{1};
  axk::package_root_selector root;
  root.kind = axk::package_root_kind::sample;
  axk::package_import_request request;
  request.root_destinations.push_back({});
  return value && root.kind == axk::package_root_kind::sample &&
                 request.root_destinations.size() == 1U
             ? *value
             : 0;
}
