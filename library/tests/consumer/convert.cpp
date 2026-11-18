#include <iostream>
#include <string>
#include <vector>

#include "axklib/sdk.hpp"
#include "axklib/sdk/conversion.hpp"

int main(int argc, char **argv) {
    if (argc == 1) {
        const auto profiles = axk::conversion_profiles_json();
        if (!profiles)
            return 1;
        std::cout << *profiles << '\n';
        return 0;
    }
    if (argc < 4)
        return 2;
    axk::operation_context context;
    axk::conversion_request request;
    request.profile_id = argv[1];
    request.source_paths.emplace_back(argv[2]);
    request.options_json = argv[3];
    auto plan = axk::conversion_plan::prepare(request, context);
    if (!plan)
        return 3;
    const auto review = plan->report_json();
    if (!review)
        return 4;
    std::cout << *review << '\n';
    if (argc > 4 && !plan->apply(argv[4], std::vector<std::string>{argv + 5, argv + argc}, context))
        return 5;
    return 0;
}
