#include "axklib/sdk.hpp"

int sdk_header_test() {
    axk::result<int> value{1};
    axk::package_root_selector root;
    root.kind = axk::package_root_kind::wave_data;
    axk::package_import_request request;
    request.root_destinations.push_back({});
    request.program_slot_assignments.push_back({0U, "program", 1U});
    axk::package_program_slot_placement_info placement;
    placement.mode = "contiguous";
    return value && root.kind == axk::package_root_kind::wave_data && request.root_destinations.size() == 1U &&
                   request.program_slot_assignments.size() == 1U && placement.mode == "contiguous"
               ? *value
               : 0;
}
