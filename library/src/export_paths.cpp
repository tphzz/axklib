#include "axklib/export_paths.hpp"

#include <array>
#include <system_error>

namespace axk::audio_internal {
namespace {

bool beneath(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty())
        return candidate == root;
    if (relative.is_absolute())
        return false;
    return *relative.begin() != "..";
}

Result<void> validate_plan_path(const std::filesystem::path &output_directory,
                                std::span<const std::filesystem::path> parts) {
    auto resolved = resolve_export_destination(output_directory, parts);
    if (!resolved)
        return std::unexpected{resolved.error()};
    return {};
}

} // namespace

Result<std::filesystem::path> resolve_export_destination(const std::filesystem::path &output_directory,
                                                         std::span<const std::filesystem::path> relative_parts) {
    std::error_code error;
    const auto root = std::filesystem::absolute(output_directory, error).lexically_normal();
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "audio export root cannot be normalized")};
    }
    auto destination = root;
    for (const auto &part : relative_parts) {
        if (part.is_absolute() || part.has_root_name() || part.has_root_directory()) {
            return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                              "audio export plan contains a rooted output path")};
        }
        destination /= part;
    }
    destination = destination.lexically_normal();
    if (!beneath(root, destination)) {
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                          "audio export plan escapes the output directory")};
    }
    return destination;
}

Result<void> validate_export_plan_paths(const ExportPlan &plan, const std::filesystem::path &output_directory) {
    for (const auto &volume : plan.volumes) {
        const std::array volume_parts{volume.relative_root};
        if (auto valid = validate_plan_path(output_directory, volume_parts); !valid)
            return valid;
        for (const auto &waveform : volume.waveforms) {
            const std::array parts{volume.relative_root, waveform.relative_wav_path};
            if (auto valid = validate_plan_path(output_directory, parts); !valid)
                return valid;
        }
        for (const auto &sample : volume.samples) {
            if (sample.rendered_wav_path) {
                const std::array parts{volume.relative_root, *sample.rendered_wav_path};
                if (auto valid = validate_plan_path(output_directory, parts); !valid)
                    return valid;
            }
            for (const auto &member : sample.members) {
                const std::array parts{volume.relative_root, member.relative_wav_path};
                if (auto valid = validate_plan_path(output_directory, parts); !valid)
                    return valid;
            }
        }
    }
    for (const auto &scope : plan.unresolved_wave_data) {
        const std::array scope_parts{scope.relative_root};
        if (auto valid = validate_plan_path(output_directory, scope_parts); !valid)
            return valid;
        for (const auto &waveform : scope.waveforms) {
            const std::array parts{scope.relative_root, waveform.relative_wav_path};
            if (auto valid = validate_plan_path(output_directory, parts); !valid)
                return valid;
        }
    }
    return {};
}

} // namespace axk::audio_internal
