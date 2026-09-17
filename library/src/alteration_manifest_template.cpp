#include "axklib/alteration.hpp"

#include "axklib/utf8.hpp"

#include <nlohmann/json.hpp>

#include "axklib/file_publication.hpp"

namespace axk {
namespace {

using OrderedJson = nlohmann::ordered_json;

Error transaction_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

} // namespace

Result<std::string> serialize_alteration_manifest_template() {
    try {
        OrderedJson operation = OrderedJson::object();
        operation["id"] = "rename-waveform";
        operation["type"] = "rename_waveform";
        operation["partition_index"] = 0;
        operation["volume_name"] = "Volume";
        operation["waveform_name"] = "Old Wave";
        operation["new_waveform_name"] = "New Wave";

        OrderedJson manifest = OrderedJson::object();
        manifest["schema_version"] = alteration_manifest_schema_version;
        manifest["operations"] = OrderedJson::array({std::move(operation)});
        return manifest.dump(2) + "\n";
    } catch (const OrderedJson::exception &error) {
        return std::unexpected{
            transaction_error(std::string{"could not serialize alteration manifest template: "} + error.what())};
    }
}

Result<PublicationOutcome> write_alteration_manifest_template(const std::filesystem::path &output_path,
                                                              bool overwrite) {
    auto serialized = serialize_alteration_manifest_template();
    if (!serialized)
        return std::unexpected{serialized.error()};

    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                       "refusing to replace existing alteration manifest: " + text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not inspect alteration manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not create alteration manifest output directory")};
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
