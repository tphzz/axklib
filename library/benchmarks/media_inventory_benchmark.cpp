#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include <nlohmann/json.hpp>

#include "axklib/media.hpp"
#include "axklib/relationship.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
    std::string name;
    double wall_ms{};
};

struct Profile {
    std::string name;
    std::vector<Measurement> measurements;
    std::size_t object_count{};
    std::size_t relationship_count{};
};

Measurement measure(std::string name, const std::function<void()> &operation) {
    const auto start = Clock::now();
    operation();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return {std::move(name), elapsed};
}

std::uint64_t peak_memory_bytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0U;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#else
    return 0U;
#endif
}

template <typename T> T require(axk::Result<T> result, std::string_view label) {
    if (!result)
        throw std::runtime_error(std::string{label} + ": " + axk::render_error(result.error()));
    return std::move(*result);
}

Profile run_profile(const std::filesystem::path &source, std::string name, axk::MediaObjectReadMode mode) {
    Profile profile;
    profile.name = std::move(name);
    std::optional<axk::MediaContainer> media;
    profile.measurements.push_back(
        measure("open", [&] { media.emplace(require(axk::open_media(source), "open media")); }));

    std::optional<axk::MediaInventory> inventory;
    profile.measurements.push_back(measure("inventory", [&] {
        inventory.emplace(require(axk::build_media_inventory(*media, mode), "build media inventory"));
    }));

    std::optional<axk::RelationshipGraph> graph;
    profile.measurements.push_back(
        measure("relationships", [&] { graph.emplace(axk::build_relationship_graph(inventory->catalog)); }));
    profile.object_count = inventory->objects.size();
    profile.relationship_count = graph->relationships.size();
    return profile;
}

nlohmann::ordered_json profile_json(const Profile &profile) {
    nlohmann::ordered_json metrics = nlohmann::ordered_json::array();
    for (const auto &measurement : profile.measurements)
        metrics.push_back({{"name", measurement.name}, {"wall_ms", measurement.wall_ms}});
    return {{"name", profile.name},
            {"object_count", profile.object_count},
            {"relationship_count", profile.relationship_count},
            {"metrics", std::move(metrics)}};
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 3) {
        std::cerr << "usage: axk_media_inventory_benchmark IMAGE OUTPUT_JSON\n";
        return 2;
    }
    const std::filesystem::path source{argv[1]};
    const std::filesystem::path output_json{argv[2]};
    const auto metadata = run_profile(source, "decoded_metadata", axk::MediaObjectReadMode::decoded_metadata);
    const auto complete = run_profile(source, "complete", axk::MediaObjectReadMode::complete);

    nlohmann::ordered_json report = {
        {"schema_version", "1.0"},
        {"source_size_bytes", std::filesystem::file_size(source)},
        {"peak_memory_bytes", peak_memory_bytes()},
        {"profiles", nlohmann::ordered_json::array({profile_json(metadata), profile_json(complete)})},
    };
    if (!output_json.parent_path().empty())
        std::filesystem::create_directories(output_json.parent_path());
    std::ofstream{output_json} << report.dump(2) << '\n';
    return 0;
} catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
