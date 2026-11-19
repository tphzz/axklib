#include "axklib/conversion/profiles.hpp"

#include <stdexcept>
#include <utility>

#include "axklib/conversion/a_series_to_tx16w/profile.hpp"
#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/conversion/typhoon_to_a_series/profile.hpp"

namespace axk::conversion {
Registry builtin_profiles() {
    Registry registry;
    if (auto added = registry.add(tx16w::a_series::profile()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(a_series_to_tx16w::profile()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(typhoon_to_a_series::profile()); !added)
        throw std::logic_error(added.error().message);
    return registry;
}

nlohmann::json profiles_json(const Registry &registry) {
    auto result = nlohmann::json::array();
    for (const auto &profile : registry.profiles())
        result.push_back({{"id", profile.id},
                          {"name", profile.name},
                          {"sourceFormat", profile.source_format},
                          {"destinationFormat", profile.destination_format},
                          {"outputKind", profile.output_kind == OutputKind::new_image ? "NEW_IMAGE" : "EXISTING_IMAGE"},
                          {"optionsSchema", profile.options_schema}});
    return {{"profiles", std::move(result)}};
}
} // namespace axk::conversion
