#include "axklib/writer.hpp"

#include "axklib/utf8.hpp"

#include <nlohmann/json.hpp>

#include "axklib/file_publication.hpp"

namespace axk {
namespace {

using OrderedJson = nlohmann::ordered_json;

Error manifest_error(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}

OrderedJson empty_volume(std::string_view name) {
    OrderedJson result = OrderedJson::object();
    result["name"] = name;
    result["waveforms"] = OrderedJson::array();
    result["samples"] = OrderedJson::array();
    return result;
}

OrderedJson authored_starter_volume(std::string_view name) {
    OrderedJson result = OrderedJson::object();
    result["name"] = name;
    result["waveforms"] =
        OrderedJson::array({{{"id", "tone"}, {"name", "Authored Tone"}, {"path", "tone.wav"}, {"root_key", 60}}});
    result["samples"] = OrderedJson::array({{{"name", "Authored Tone"},
                                             {"waveform_id", "tone"},
                                             {"root_key", 60},
                                             {"key_low", 60},
                                             {"key_high", 60},
                                             {"level", 100}}});
    return result;
}

Result<OrderedJson> manifest_template(BuildManifestKind kind) {
    OrderedJson result = OrderedJson::object();
    result["schema_version"] = build_manifest_schema_version;
    switch (kind) {
    case BuildManifestKind::hds: {
        result["size_bytes"] = 536'870'912;
        OrderedJson partition = OrderedJson::object();
        partition["name"] = "New Partition";
        partition["volumes"] = OrderedJson::array();
        result["partitions"] = OrderedJson::array({std::move(partition)});
        return result;
    }
    case BuildManifestKind::fat12_floppy:
        result["format"] = "fat12_floppy";
        result["authored_volume"] = authored_starter_volume("FAT ROOT");
        return result;
    case BuildManifestKind::iso9660: {
        result["format"] = "iso9660";
        OrderedJson iso = OrderedJson::object();
        iso["volume_id"] = "AXK_AUDIO";
        iso["raw_group"] = "46DEF120";
        iso["group_name"] = "NEW GROUP";
        iso["raw_volume"] = "F001";
        iso["volume_name"] = "NEW VOLUME";
        result["iso"] = std::move(iso);
        result["authored_volume"] = empty_volume("NEW VOLUME");
        return result;
    }
    }
    return std::unexpected{manifest_error("unknown build manifest template kind")};
}

} // namespace

Result<std::string> serialize_build_manifest_template(BuildManifestKind kind) {
    try {
        auto value = manifest_template(kind);
        if (!value)
            return std::unexpected{value.error()};
        return value->dump(2) + "\n";
    } catch (const OrderedJson::exception &error) {
        return std::unexpected{
            manifest_error(std::string{"could not serialize build manifest template: "} + error.what())};
    }
}

Result<PublicationOutcome> write_build_manifest_template(BuildManifestKind kind,
                                                         const std::filesystem::path &output_path, bool overwrite) {
    auto serialized = serialize_build_manifest_template(kind);
    if (!serialized)
        return std::unexpected{serialized.error()};

    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                       "refusing to replace existing build manifest: " + text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not inspect build manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not create build manifest output directory")};
    }
    auto temporary = detail::TemporaryPublication::create(output_path, [&](const detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{serialized->data(), serialized->size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = temporary->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    return std::move(*published);
}

} // namespace axk
