#include "local_operation_projection.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace axk::cli::local_projection {
namespace {

schema::info_v1::NodeOutput info_node_output(const nlohmann::json &node) {
    schema::info_v1::NodeOutput result{
        .node_id = node.at("nodeId").get<std::string>(),
        .node_type = node.at("nodeType").get<std::string>(),
        .display_name = node.at("displayName").get<std::string>(),
        .object_key = node.at("objectKey").get<std::string>(),
        .object_type = node.at("objectType").get<std::string>(),
        .count = node.at("count").is_null() ? std::nullopt
                                            : std::optional<std::uint64_t>{node.at("count").get<std::uint64_t>()},
        .details = node.at("details").get<std::vector<std::string>>(),
        .quality = node.at("quality").get<std::string>(),
        .basis = node.at("basis").get<std::string>(),
        .notes = node.at("notes").get<std::string>(),
        .selector_path = node.at("selectorPath").get<std::string>(),
        .children = {},
    };
    for (const auto &child : node.at("children"))
        result.children.push_back(info_node_output(child));
    return result;
}

} // namespace

schema::info_v1::InfoOutput info_output(const nlohmann::json &service_result) {
    schema::info_v1::InfoOutput result;
    for (const auto &tree : service_result.at("trees")) {
        schema::info_v1::TreeOutput projected{
            .source_path_utf8 = tree.at("sourcePath").get<std::string>(),
            .container_kind = tree.at("containerKind").get<std::string>(),
            .detected_format = tree.at("detectedFormat").get<std::string>(),
            .object_count = tree.at("objectCount").get<std::uint64_t>(),
            .object_counts = tree.at("objectCounts").get<std::map<std::string, std::uint64_t>>(),
            .recovery = tree.at("recovery").is_null()
                            ? std::nullopt
                            : std::optional<std::string>{tree.at("recovery").get<std::string>()},
            .roots = {},
            .issues = {},
        };
        for (const auto &root : tree.at("roots"))
            projected.roots.push_back(info_node_output(root));
        for (const auto &issue : tree.at("issues")) {
            projected.issues.push_back({.code = issue.at("code").get<std::string>(),
                                        .severity = issue.at("severity").get<std::string>(),
                                        .message = issue.at("message").get<std::string>(),
                                        .source_path_utf8 = issue.at("sourcePath").get<std::string>(),
                                        .sampler_path = issue.at("samplerPath").get<std::string>(),
                                        .object_key = issue.at("objectKey").get<std::string>()});
        }
        result.trees.push_back(std::move(projected));
    }
    for (const auto &error : service_result.at("loadErrors")) {
        result.load_errors.push_back({.path_utf8 = error.at("path").get<std::string>(),
                                      .error_code = error.at("errorCode").get<std::uint64_t>(),
                                      .message = error.at("message").get<std::string>(),
                                      .original_exception = error.at("originalException").get<std::string>()});
    }
    return result;
}

schema::operations_v1::OperationOutput operation_output(const nlohmann::json &operation) {
    auto type = operation.at("type").get<std::string>();
    std::ranges::transform(type, type.begin(), [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
    });
    schema::operations_v1::OperationOutput result{
        .id = operation.at("id").get<std::string>(),
        .type = std::move(type),
        .partition_index = operation.at("partitionIndex").get<std::uint8_t>(),
        .volume_name = operation.at("volumeName").get<std::string>(),
        .object_name = operation.at("objectName").get<std::string>(),
        .removed_sfs_ids = operation.at("removedSfsIds").get<std::vector<std::uint32_t>>(),
        .inserted_sfs_ids = operation.at("insertedSfsIds").get<std::vector<std::uint32_t>>(),
        .freed_clusters = operation.at("freedClusters").get<std::uint64_t>(),
        .allocated_clusters = operation.at("allocatedClusters").get<std::uint64_t>(),
        .audio_import = std::nullopt,
    };
    if (!operation.at("audioImport").is_null()) {
        const auto &audio = operation.at("audioImport");
        result.audio_import = schema::operations_v1::AudioImportOutput{
            .source_path_utf8 = audio.at("sourcePath").get<std::string>(),
            .source_format = audio.at("sourceFormat").get<std::string>(),
            .source_subtype = audio.at("sourceSubtype").get<std::string>(),
            .source_channels = audio.at("sourceChannels").get<std::uint8_t>(),
            .source_sample_rate = audio.at("sourceSampleRate").get<std::uint32_t>(),
            .output_sample_rate = audio.at("outputSampleRate").get<std::uint32_t>(),
            .source_sample_width_bits = audio.at("sourceSampleWidthBits").get<std::uint8_t>(),
            .output_sample_width_bits = audio.at("outputSampleWidthBits").get<std::uint8_t>(),
            .output_frames = audio.at("outputFrames").get<std::uint64_t>(),
            .resampled = audio.at("resampled").get<bool>(),
            .quantized = audio.at("quantized").get<bool>(),
            .sample_width_converted = audio.at("sampleWidthConverted").get<bool>(),
            .dither_algorithm = audio.at("ditherAlgorithm").get<std::string>(),
            .split_stereo = audio.at("splitStereo").get<bool>(),
            .clipped_samples = audio.at("clippedSamples").get<std::uint64_t>(),
        };
    }
    return result;
}

} // namespace axk::cli::local_projection
