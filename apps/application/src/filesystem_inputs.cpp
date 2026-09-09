#include "filesystem_inputs.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "content_digest.hpp"

namespace axk::app::filesystem_inputs {
namespace {
using Json = nlohmann::json;
Error changed() {
    return {
        "filesystem_input_changed", "An import source changed. Inspect the inputs again before importing.", {}, true};
}
} // namespace

Result<OpenedInput> open(const Json &source, std::string_view owner, const Sandbox &sandbox, UploadStore &uploads) {
    if (!source.is_object() || source.size() != 1U)
        return std::unexpected(Error{"invalid_request", "Choose exactly one fileRef or uploadRef input"});
    if (source.contains("fileRef")) {
        const auto &value = source.at("fileRef");
        const FileRef ref{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
        auto opened = sandbox.open_file(ref);
        if (!opened)
            return std::unexpected(opened.error());
        auto verify = [&sandbox, ref, revision = opened->revision,
                       verify_handle = opened->verify_unchanged]() -> Result<void> {
            if (auto result = verify_handle(); !result)
                return std::unexpected(changed());
            const auto current = sandbox.open_file(ref);
            if (!current || current->revision != revision)
                return std::unexpected(changed());
            return {};
        };
        return OpenedInput{std::move(opened->reader), std::move(opened->revision), {}, std::move(verify)};
    }
    if (source.contains("uploadRef")) {
        const auto id = source.at("uploadRef").at("uploadId").get<std::string>();
        auto lease = uploads.lease({id}, owner);
        if (!lease)
            return std::unexpected(lease.error());
        auto reader = FileReader::open(lease->path());
        if (!reader)
            return std::unexpected(Error{"filesystem_input_failed", reader.error().message});
        return OpenedInput{std::move(*reader), "upload:" + id, std::move(*lease), []() -> Result<void> { return {}; }};
    }
    return std::unexpected(Error{"invalid_request", "Choose a fileRef or uploadRef input"});
}

Result<Json> OpenedInput::snapshot(const CancellationToken &cancellation) const {
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", "Input inspection was cancelled"});
    if (reader->size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(Error{"filesystem_input_too_large", "File input exceeds the filesystem size field"});
    if (auto checked = verify_identity(); !checked)
        return std::unexpected(checked.error());
    auto hash = detail::reader_sha256(*reader, cancellation);
    if (!hash)
        return std::unexpected(hash.error());
    if (auto checked = verify_identity(); !checked)
        return std::unexpected(checked.error());
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", "Input inspection was cancelled"});
    return Json{{"revision", revision}, {"sizeBytes", reader->size()}, {"sha256", std::move(*hash)}};
}

Result<void> OpenedInput::verify(const Json &expected, const CancellationToken &cancellation) const {
    if (!expected.is_object() || expected.size() != 3U || !expected.contains("revision") ||
        !expected.at("revision").is_string() || expected.at("revision").get_ref<const std::string &>().empty() ||
        expected.at("revision").get_ref<const std::string &>().size() > 512U || !expected.contains("sha256") ||
        !expected.at("sha256").is_string() || !expected.contains("sizeBytes") ||
        !expected.at("sizeBytes").is_number_integer() || expected.at("sizeBytes") < 0 ||
        expected.at("sizeBytes") > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(Error{"invalid_request", "A complete reviewed input snapshot is required"});
    const auto &hash = expected.at("sha256").get_ref<const std::string &>();
    if (hash.size() != 64U ||
        !std::ranges::all_of(hash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected(Error{"invalid_request", "Input SHA-256 must contain 64 lowercase hexadecimal digits"});
    if (expected.at("revision") != revision || expected.at("sizeBytes") != reader->size())
        return std::unexpected(changed());
    const auto current = snapshot(cancellation);
    if (!current)
        return std::unexpected(current.error());
    if (*current != expected)
        return std::unexpected(changed());
    return {};
}

Result<void> bind_inspection(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads) {
    if (registry.is_implemented("filesystem.inputs.inspect"))
        return {};
    auto bound = registry.bind(
        "filesystem.inputs.inspect",
        [&sandbox, &uploads](const Json &request, const OperationContext &context) -> Result<Json> {
            try {
                const auto &inputs = request.at("inputs");
                if (!inputs.is_array() || inputs.empty() || inputs.size() > 10000U)
                    return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 import inputs"});
                Json result = Json::array();
                for (const auto &source : inputs) {
                    if (auto checked = context.cancellation.check(); !checked)
                        return std::unexpected(Error{"operation_cancelled", "Input inspection was cancelled"});
                    if (context.progress)
                        context.progress->report({ProgressPhase::reading, result.size(), inputs.size(),
                                                  "Inspecting import inputs", std::nullopt});
                    auto input = open(source, context.owner_id, sandbox, uploads);
                    if (!input)
                        return std::unexpected(input.error());
                    auto snapshot = input->snapshot(context.cancellation);
                    if (!snapshot)
                        return std::unexpected(snapshot.error());
                    result.push_back({{"source", source}, {"snapshot", std::move(*snapshot)}});
                }
                return Json{{"inputs", std::move(result)}};
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Input inspection request is incomplete or malformed"});
            }
        });
    if (!bound)
        return bound;
    return registry.bind_path_accesses(
        "filesystem.inputs.inspect",
        [](const Json &request, const OperationContext &) -> Result<std::vector<PathAccess>> {
            try {
                std::vector<PathAccess> accesses;
                for (const auto &source : request.at("inputs")) {
                    if (source.contains("fileRef")) {
                        const auto &ref = source.at("fileRef");
                        accesses.push_back(
                            {{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                             PathAccessMode::shared});
                    }
                }
                return accesses;
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Import input reference is malformed"});
            }
        });
}
} // namespace axk::app::filesystem_inputs
