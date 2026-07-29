#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/package.hpp"
#include "axklib/relationship.hpp"

namespace axk::package_internal {

inline constexpr std::string_view schema_version = "1.0";
inline constexpr std::uint64_t maximum_package_file_bytes = 512U * 1024U * 1024U;

struct WaveformDigests {
    std::string semantic;
    std::string audio;
};

[[nodiscard]] bool portable_inactive_program_relationship(const Relationship &relationship);
[[nodiscard]] Error package_error(std::string message, ErrorCode code = ErrorCode::manifest_invalid);
[[nodiscard]] std::vector<std::byte> string_bytes(std::string_view value);
[[nodiscard]] std::string digest_text(std::string_view value);
[[nodiscard]] std::string object_type_name(ObjectType type);
[[nodiscard]] std::optional<ObjectType> parse_object_type(std::string_view value);
[[nodiscard]] std::string object_format_name(ObjectFormat format);
[[nodiscard]] std::string media_kind_name(MediaKind kind);
[[nodiscard]] std::optional<PackageRootKind> parse_root_kind(std::string_view value);
[[nodiscard]] std::optional<PackageKind> parse_package_kind(std::string_view value);
[[nodiscard]] PackageKind package_kind_for_root(PackageRootKind kind);
[[nodiscard]] ObjectType root_object_type(PackageRootKind kind);
[[nodiscard]] std::string lower_extension(std::string_view filename);
[[nodiscard]] bool recognized_extension(std::string_view extension);
[[nodiscard]] bool closure_relationship(std::string_view role);
[[nodiscard]] Result<std::optional<WaveformDigests>> waveform_digests(const DecodedObject &decoded,
                                                                      std::span<const std::byte> raw_payload,
                                                                      std::string_view normalized_digest);
[[nodiscard]] std::string edge_id(std::string_view source, std::string_view target, std::string_view role,
                                  std::uint32_t ordinal);

template <std::ranges::input_range Roots> PackageKind derive_kind(const Roots &roots) {
    const auto first = std::ranges::begin(roots);
    if (first == std::ranges::end(roots))
        return PackageKind::bundle;
    const auto root_kind = first->kind;
    if (!std::ranges::all_of(roots, [root_kind](const auto &root) { return root.kind == root_kind; }))
        return PackageKind::bundle;
    return package_kind_for_root(root_kind);
}

} // namespace axk::package_internal
