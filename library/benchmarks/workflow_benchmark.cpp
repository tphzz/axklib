#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
    std::string name;
    double wall_ms{};
};

Measurement measure(std::string name, const std::function<void()> &operation) {
    const auto start = Clock::now();
    operation();
    const auto elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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

template <typename T>
const T &require(const axk::Result<T> &result, std::string_view label) {
    if (!result)
        throw std::runtime_error(std::string{label} + ": " +
                                 axk::render_error(result.error()));
    return *result;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 3) {
        std::cerr << "usage: axk_workflow_benchmark IMAGE OUTPUT_JSON\n";
        return 2;
    }
    const std::filesystem::path source{argv[1]};
    const std::filesystem::path output_json{argv[2]};
    const auto scratch = output_json.parent_path() / "benchmark-tmp";
    std::error_code filesystem_error;
    std::filesystem::remove_all(scratch, filesystem_error);
    std::filesystem::create_directories(scratch);
    std::vector<Measurement> measurements;

    axk::Container container;
    measurements.push_back(measure("cold_open", [&] {
        container = require(axk::open_image(source), "open");
    }));
    axk::ObjectCatalog catalog;
    measurements.push_back(measure("inventory", [&] {
        catalog = require(axk::build_object_catalog(container), "inventory");
    }));
    axk::RelationshipGraph graph;
    measurements.push_back(measure("relationships", [&] {
        graph = axk::build_relationship_graph(catalog);
    }));
    const auto waveform =
        std::ranges::find_if(catalog.objects, [](const auto &object) {
            return object.object.header.type == axk::ObjectType::smpl;
        });
    measurements.push_back(measure("preview", [&] {
        if (waveform == catalog.objects.end())
            throw std::runtime_error("benchmark fixture has no waveform");
        const auto decoded = require(axk::decode_waveform(container, *waveform),
                                     "decode waveform");
        static_cast<void>(
            require(axk::build_preview_envelope(decoded, 512U), "preview"));
    }));
    measurements.push_back(measure("exact_export", [&] {
        const auto plan = require(
            axk::build_export_plan(container, catalog, graph), "export plan");
        static_cast<void>(require(
            axk::write_export_audio(plan, scratch / "export"), "export"));
    }));

    const auto build = require(
        axk::parse_hds_build_manifest(
            R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"sample_banks":[]}]}]})"),
        "build manifest");
    measurements.push_back(measure("fresh_write", [&] {
        static_cast<void>(require(
            axk::write_hds_image(build, scratch / "fresh.hds"), "fresh write"));
    }));
    const auto alteration = require(
        axk::parse_alteration_manifest(
            R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"New Volume"}]})"),
        "alteration manifest");
    measurements.push_back(measure("alteration_plan", [&] {
        static_cast<void>(require(axk::plan_hds_alteration(source, alteration),
                                  "alteration plan"));
    }));
    measurements.push_back(measure("alteration_apply", [&] {
        static_cast<void>(
            require(axk::alter_hds(source, alteration, scratch / "altered.hds"),
                    "alteration apply"));
    }));

    nlohmann::ordered_json report = {
        {"schema_version", "1.0"},
        {"source_size_bytes", std::filesystem::file_size(source)},
        {"object_count", catalog.objects.size()},
        {"peak_memory_bytes", peak_memory_bytes()},
#if defined(__clang__)
        {"compiler", "clang"},
#elif defined(__GNUC__)
        {"compiler", "gcc"},
#elif defined(_MSC_VER)
        {"compiler", "msvc"},
#else
        {"compiler", "unknown"},
#endif
        {"metrics", nlohmann::ordered_json::array()},
    };
    const auto source_bytes = std::filesystem::file_size(source);
    for (const auto &measurement : measurements) {
        const auto bytes =
            measurement.name == "fresh_write" ? 1'048'576U : source_bytes;
        const auto throughput =
            measurement.wall_ms <= 0.0
                ? 0.0
                : static_cast<double>(bytes) / (measurement.wall_ms / 1000.0);
        report["metrics"].push_back({{"name", measurement.name},
                                     {"wall_ms", measurement.wall_ms},
                                     {"bytes_processed", bytes},
                                     {"bytes_per_second", throughput}});
    }
    std::filesystem::create_directories(output_json.parent_path());
    std::ofstream{output_json} << report.dump(2) << '\n';
    std::filesystem::remove_all(scratch, filesystem_error);
    return 0;
} catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
