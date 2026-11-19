#include "axklib/conversion/profiles.hpp"

#include <stdexcept>
#include <utility>

#include "axklib/conversion/a_series/wave_reader.hpp"
#include "axklib/conversion/tx16w/wave_disk_writer.hpp"
#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/conversion/typhoon_to_a_series/profile.hpp"

namespace axk::conversion {
Registry builtin_profiles() {
    Registry registry;
    if (auto added = registry.add(tx16w::a_series::profile()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(a_series_waves::reader()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(tx16w_wave_disk::writer()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(tx16w_wave_disk::set_writer()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(typhoon_to_a_series::profile()); !added)
        throw std::logic_error(added.error().message);
    for (auto profiles : {tx16w::a_series::fresh_profiles(), typhoon_to_a_series::fresh_profiles()})
        for (auto &profile : profiles)
            if (auto added = registry.add(std::move(profile)); !added)
                throw std::logic_error(added.error().message);
    return registry;
}

nlohmann::json profiles_json(const Registry &registry) {
    auto result = nlohmann::json::array();
    for (const auto &profile : registry.profiles()) {
        nlohmann::json route = nullptr;
        if (const auto &r = profile.route)
            route = {{"targetLabel", r->target_label},
                     {"medium", r->medium},
                     {"intents", r->intents},
                     {"editorCapabilities", r->editor_capabilities},
                     {"priority", r->priority}};
        result.push_back({{"id", profile.id},
                          {"name", profile.name},
                          {"sourceFormat", profile.source_format},
                          {"destinationFormat", profile.destination_format},
                          {"outputKind", output_kind_name(profile.output_kind)},
                          {"optionsSchema", profile.options_schema},
                          {"route", std::move(route)}});
    }
    return {{"profiles", std::move(result)}};
}
} // namespace axk::conversion
