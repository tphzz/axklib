#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    std::size_t bin_count{};
    const std::string_view bins{argv[3]};
    const auto parsed =
        std::from_chars(bins.data(), bins.data() + bins.size(), bin_count);
    if (parsed.ec != std::errc{} || parsed.ptr != bins.data() + bins.size() ||
        bin_count == 0U)
        return 2;
    const auto container = axk::open_image(std::filesystem::path{argv[1]});
    if (!container) {
        std::cerr << axk::render_error(container.error()) << '\n';
        return 1;
    }
    const auto catalog = axk::build_object_catalog(*container);
    if (!catalog) {
        std::cerr << axk::render_error(catalog.error()) << '\n';
        return 1;
    }
    const std::string_view object_key{argv[2]};
    const auto object = std::ranges::find(catalog->objects, object_key,
                                          &axk::ObjectSnapshot::key);
    if (object == catalog->objects.end())
        return 1;
    const auto waveform = axk::decode_waveform(*container, *object);
    if (!waveform) {
        std::cerr << axk::render_error(waveform.error()) << '\n';
        return 1;
    }
    const auto preview = axk::build_preview_envelope(*waveform, bin_count);
    if (!preview) {
        std::cerr << axk::render_error(preview.error()) << '\n';
        return 1;
    }
    auto bins_json = nlohmann::ordered_json::array();
    for (const auto &bin : preview->bins)
        bins_json.push_back({bin.minimum, bin.maximum});
    std::cout << nlohmann::ordered_json{{"schema_version", "1.0"},
                                        {"object_key", object_key},
                                        {"frame_count", preview->frame_count},
                                        {"bins", std::move(bins_json)}}
                     .dump()
              << '\n';
    return 0;
}
