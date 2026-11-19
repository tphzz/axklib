#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "axklib/formats/a_series/authoring/bundle.hpp"

namespace axk::a_series::authoring {
struct FreshMedia {
    BuildManifestKind format;
    std::string volume_name;
    std::uint64_t size_bytes{};
    std::uint8_t partition_count{1}, partition_index{};
};
// Writes an independent, named A-series volume from materialized native audio. No file inputs.
[[nodiscard]] Result<std::vector<std::byte>> build_media(Bundle bundle, const FreshMedia &target,
                                                         std::uint64_t maximum_bytes,
                                                         const CancellationToken &cancellation);
[[nodiscard]] Result<std::vector<std::byte>> build_hds(VolumeSpec volume, std::span<ImportedAudio> audio,
                                                       const FreshMedia &target, const CancellationToken &cancellation);
} // namespace axk::a_series::authoring
