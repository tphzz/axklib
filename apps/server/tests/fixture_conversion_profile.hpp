#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "axklib/conversion/a_series/wave_reader.hpp"
#include "axklib/conversion/profiles.hpp"

namespace fixture {
inline constexpr char profile_id[] = "fixture.inspect-wave";

inline axk::conversion::Profile image_profile() {
    using namespace axk::conversion;
    return {"fixture.new-image",
            "Fixture image",
            "yamaha.tx16w",
            "test.image",
            {{"type", "object"}, {"additionalProperties", false}, {"properties", {{"wave", {{"type", "string"}}}}}},
            [](const Request &input, const axk::CancellationToken &,
               axk::ProgressSink *) -> axk::Result<std::shared_ptr<const PreparedPlan>> {
                const auto &objects = input.source->catalog().objects;
                const auto selected = input.options.contains("wave")
                                          ? std::ranges::find(objects, input.options.at("wave").get<std::string>(),
                                                              &axk::native::ObjectSummary::id)
                                          : (std::ranges::count(objects, "wave", &axk::native::ObjectSummary::type) == 1
                                                 ? std::ranges::find(objects, "wave", &axk::native::ObjectSummary::type)
                                                 : objects.end());
                const auto members = input.source->sources()->members();
                if (selected == objects.end() || selected->type != "wave" || members.size() != 1 || !members[0].reader)
                    return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::object,
                                                           "Expected one fixture disk and Wave"));
                const auto &reader = *members[0].reader;
                constexpr std::size_t overhead = 4096;
                if (reader.size() < 512 || reader.size() > 1474560 ||
                    reader.size() + overhead > input.limits.maximum_bytes)
                    return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::object,
                                                           "Fixture size exceeds its limit"));
                Report report{"fixture.new-image", input.fidelity, input.fidelity == Fidelity::best, {}, {}, {}};
                report.actions.push_back({"wave", selected->name, selected->id});
                report.issues.push_back({"L", "fixture.loss", Impact::loss, selected->id, {}, {}, {}, "Fixture loss"});
                NewImage image;
                if (report.executable) {
                    image.bytes.resize(static_cast<std::size_t>(reader.size()));
                    if (auto read = reader.read_exact_at(0, image.bytes); !read)
                        return std::unexpected(read.error());
                    image.bytes[400] ^= std::byte{0x5a};
                }
                const auto retained = image.bytes.capacity() + overhead;
                return std::make_shared<const PreparedPlan>(std::move(report), std::move(image), retained);
            },
            OutputKind::new_image};
}

inline axk::conversion::Registry conversion_profiles(bool enabled) {
    using namespace axk::conversion;
    auto registry = builtin_profiles();
    if (!enabled)
        return registry;
    Profile profile{profile_id,
                    "Inspect a Wave",
                    "yamaha.tx16w",
                    "yamaha.a-series",
                    {{"type", "object"},
                     {"required", {"reviewLabel"}},
                     {"additionalProperties", false},
                     {"properties", {{"reviewLabel", {{"type", "string"}}}}}},
                    [](const Request &request, const axk::CancellationToken &,
                       axk::ProgressSink *) -> axk::Result<std::shared_ptr<const PreparedPlan>> {
                        const auto &objects = request.source->catalog().objects;
                        if (objects.empty() || !request.options.is_object() || request.options.size() != 1U ||
                            !request.options.contains("reviewLabel") || !request.options.at("reviewLabel").is_string())
                            return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument,
                                                                   axk::ErrorCategory::object,
                                                                   "Expected source object and review label"));
                        Report report{profile_id, request.fidelity, true, {}, {}, {}};
                        report.actions.push_back({"inspection", request.options.at("reviewLabel").get<std::string>(),
                                                  objects.front().id, "inspect"});
                        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{},
                                                                    request.destination.reader->size(), 1024U);
                    }};
    if (auto added = registry.add(std::move(profile)); !added)
        throw std::logic_error(added.error().message);
    if (auto added = registry.add(image_profile()); !added)
        throw std::logic_error(added.error().message);
    Profile window{"fixture.a-series-window",
                   "Read selected Wave frames",
                   "yamaha.a-series",
                   "test.frames",
                   {{"type", "object"},
                    {"required", {"documentId", "waveIds", "first", "count"}},
                    {"properties",
                     {{"documentId", {{"type", "string"}}},
                      {"waveIds", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"first", {{"type", "integer"}, {"minimum", 0}}},
                      {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}}}},
                    {"additionalProperties", false}},
                   [](const Request &input, const axk::CancellationToken &token,
                      axk::ProgressSink *) -> axk::Result<std::shared_ptr<const PreparedPlan>> {
                       const auto ids = input.options.at("waveIds").get<std::vector<std::string>>();
                       auto waves = a_series_waves::select_waves(
                           input, input.options.at("documentId").get<std::string>(), ids, token);
                       if (!waves)
                           return std::unexpected(waves.error());
                       Report report{"fixture.a-series-window", input.fidelity, true, {}, {}, {}};
                       NewImage output;
                       for (const auto &wave : *waves) {
                           auto pcm = wave.read(input.options.at("first").get<std::uint64_t>(),
                                                input.options.at("count").get<std::size_t>(), token);
                           if (!pcm)
                               return std::unexpected(pcm.error());
                           report.actions.push_back(
                               {"wave", wave.name, wave.id, "inspect", {{"frames", nlohmann::json(*pcm).dump()}}});
                           for (const auto frame : *pcm) {
                               const auto bits = static_cast<std::uint16_t>(frame);
                               output.bytes.push_back(static_cast<std::byte>(bits & 0xffU));
                               output.bytes.push_back(static_cast<std::byte>(bits >> 8U));
                           }
                       }
                       return std::make_shared<const PreparedPlan>(std::move(report), std::move(output), 65536U);
                   },
                   OutputKind::new_image};
    if (auto added = registry.add(std::move(window)); !added)
        throw std::logic_error(added.error().message);
    return registry;
}
} // namespace fixture
