#include "axklib/application/write_operations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/alteration_transaction.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"
#include <nlohmann/json.hpp>

#include "content_digest.hpp"

#include "write_operations_internal.hpp"

using namespace axk::app::write_operations_internal;
using axk::app::detail::file_sha256;
using axk::app::detail::reader_sha256;

axk::app::Result<void> axk::app::bind_manifest_operations(OperationRegistry &registry) {
    if (!registry.is_implemented("create.manifest")) {
        auto bound = registry.bind("create.manifest", [](const Json &input, const OperationContext &) {
            std::string kind_text;
            try {
                kind_text = input.at("kind").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "kind is required"))};
            }
            auto kind = parse_build_kind(kind_text);
            if (!kind)
                return Result<Json>{std::unexpected(kind.error())};
            auto serialized = axk::serialize_build_manifest_template(*kind);
            if (!serialized)
                return Result<Json>{std::unexpected(core_error(serialized.error()))};
            try {
                return Result<Json>{Json{{"schemaVersion", "1.0"},
                                         {"kind", kind_text},
                                         {"manifest", Json::parse(*serialized)},
                                         {"canonicalJson", *serialized},
                                         {"choices", manifest_choices(*kind)},
                                         {"documentation", "/formats/generated-image-writing/"}}};
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("manifest_serialization", "starter manifest is invalid JSON"))};
            }
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("alter.manifest")) {
        auto bound = registry.bind("alter.manifest", [](const Json &, const OperationContext &) {
            auto serialized = axk::serialize_alteration_manifest_template();
            if (!serialized)
                return Result<Json>{std::unexpected(core_error(serialized.error()))};
            try {
                return Result<Json>{Json{{"schemaVersion", "1.0"},
                                         {"kind", "ALTERATION"},
                                         {"manifest", Json::parse(*serialized)},
                                         {"canonicalJson", *serialized},
                                         {"choices", alteration_manifest_choices()},
                                         {"documentation", "/output-contracts/writer-and-alteration/"}}};
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("manifest_serialization", "starter manifest is invalid JSON"))};
            }
        });
        if (!bound)
            return bound;
    }
    return {};
}

axk::Result<axk::app::PreparedLocalBuildManifest>
axk::app::prepare_local_build_manifest(std::string_view kind, const std::filesystem::path &manifest_path) {
    auto manifest_kind = parse_build_kind(kind);
    if (!manifest_kind) {
        return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::manifest,
                                               manifest_kind.error().message));
    }
    std::vector<std::filesystem::path> paths;
    if (*manifest_kind == axk::BuildManifestKind::hds) {
        auto manifest = axk::load_hds_build_manifest(manifest_path);
        if (!manifest)
            return std::unexpected(manifest.error());
        paths = external_paths(*manifest);
    } else {
        auto manifest = axk::load_media_build_manifest(manifest_path);
        if (!manifest)
            return std::unexpected(manifest.error());
        const auto expected_format = *manifest_kind == axk::BuildManifestKind::fat12_floppy
                                         ? axk::MediaImageFormat::fat12_floppy
                                         : axk::MediaImageFormat::iso9660;
        if (manifest->format != expected_format) {
            return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::manifest,
                                                   "manifest format does not match image build"));
        }
        paths = external_paths(*manifest);
    }

    return prepare_local_manifest_document(manifest_path, paths);
}

axk::Result<axk::app::PreparedLocalBuildManifest>
axk::app::prepare_local_alteration_manifest(const std::filesystem::path &manifest_path) {
    auto manifest = axk::load_alteration_manifest(manifest_path);
    if (!manifest)
        return std::unexpected(manifest.error());
    const auto paths = external_paths(*manifest);
    return prepare_local_manifest_document(manifest_path, paths);
}
