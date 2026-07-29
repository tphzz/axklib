#include "sdk_internal.hpp"

#include <filesystem>
#include <thread>
#include <utility>
#include <variant>

#include "axklib/alteration.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"

namespace axk {
using namespace sdk_internal;

struct build_plan::impl {
    std::variant<HdsBuildManifest, MediaBuildManifest> manifest;
    std::vector<PartitionGeometry> geometry;
    MediaBuildLimits media_limits;
    std::thread::id owner;
};

build_plan::build_plan() = default;
build_plan::~build_plan() = default;
build_plan::build_plan(build_plan &&) noexcept = default;
build_plan &build_plan::operator=(build_plan &&) noexcept = default;

result<build_plan> build_plan::from_manifest(const std::string &utf8_manifest_path, operation_context &context) {
    return from_manifest(utf8_manifest_path, media_build_limits{}, context);
}

result<build_plan> build_plan::from_manifest(const std::string &utf8_manifest_path, const media_build_limits &limits,
                                             operation_context &context) {
    return protect<build_plan>([&]() -> result<build_plan> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto path = checked_path(utf8_manifest_path, "build manifest path");
        if (!path)
            return path.error();
        build_plan output;
        if (auto manifest = load_hds_build_manifest(*path); manifest) {
            auto geometry = plan_hds_geometry(*manifest);
            if (!geometry)
                return public_error(geometry.error());
            output.impl_ = std::make_unique<impl>(
                impl{std::move(*manifest), std::move(*geometry), {}, std::this_thread::get_id()});
        } else {
            auto media_manifest = load_media_build_manifest(*path);
            if (!media_manifest)
                return public_error(media_manifest.error());
            const MediaBuildLimits core_limits{limits.maximum_object_bytes, limits.maximum_aggregate_payload_bytes,
                                               limits.maximum_output_bytes};
            auto planned = plan_media_build(*media_manifest, core_limits, context.impl_->cancellation.token());
            if (!planned)
                return public_error(planned.error());
            output.impl_ =
                std::make_unique<impl>(impl{std::move(*media_manifest), {}, core_limits, std::this_thread::get_id()});
        }
        return output;
    });
}

plan_summary build_plan::summary() const noexcept {
    if (!impl_)
        return {};
    if (const auto *hds = std::get_if<HdsBuildManifest>(&impl_->manifest))
        return {impl_->geometry.size(), 0U, hds->size_bytes, true};
    const auto &media = std::get<MediaBuildManifest>(impl_->manifest);
    return {0U, 0U, media.format == MediaImageFormat::fat12_floppy ? 1'474'560U : 0U, true};
}

result<void> build_plan::apply(const std::string &utf8_output_path, const write_options &options,
                               operation_context &context) {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("build plan is not initialized");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (impl_->owner != std::this_thread::get_id())
            return invalid_argument("build plan used from a different thread");
        auto path = checked_path(utf8_output_path, "output image path");
        if (!path)
            return path.error();
        if (const auto *hds = std::get_if<HdsBuildManifest>(&impl_->manifest)) {
            auto written = write_hds_image(*hds, *path, options.overwrite, context.impl_->cancellation.token());
            if (!written)
                return public_error(written.error());
        } else {
            auto written = write_media_image(std::get<MediaBuildManifest>(impl_->manifest), *path, options.overwrite,
                                             impl_->media_limits, context.impl_->cancellation.token());
            if (!written)
                return public_error(written.error());
        }
        return {};
    });
}

result<plan_summary> alteration::inspect(const std::string &utf8_source_path, const std::string &utf8_manifest_path,
                                         operation_context &context) {
    return protect<plan_summary>([&]() -> result<plan_summary> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "source image path");
        if (!source)
            return source.error();
        auto manifest_path = checked_path(utf8_manifest_path, "alteration manifest path");
        if (!manifest_path)
            return manifest_path.error();
        auto manifest = load_alteration_manifest(*manifest_path);
        if (!manifest)
            return public_error(manifest.error());
        auto inspection =
            inspect_hds_alteration(*source, *manifest, context.impl_->cancellation.token(), context.impl_.get());
        if (!inspection)
            return public_error(inspection.error());
        return plan_summary{0U, inspection->operations.size(), 0U, !inspection->operations.empty()};
    });
}

result<void> alteration::apply(const std::string &utf8_source_path, const std::string &utf8_manifest_path,
                               const std::string &utf8_output_path, const write_options &options,
                               operation_context &context) {
    return protect<void>([&]() -> result<void> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "source image path");
        if (!source)
            return source.error();
        auto manifest_path = checked_path(utf8_manifest_path, "alteration manifest path");
        if (!manifest_path)
            return manifest_path.error();
        auto manifest = load_alteration_manifest(*manifest_path);
        if (!manifest)
            return public_error(manifest.error());
        auto output = checked_path(utf8_output_path, "output image path");
        if (!output)
            return output.error();
        if (!options.overwrite && std::filesystem::exists(*output))
            return error{error_code::io_open_failed, error_category::io, "output image already exists", {}};
        auto altered = alter_hds(*source, *manifest, *output, context.impl_->cancellation.token(), context.impl_.get(),
                                 options.overwrite);
        if (!altered)
            return public_error(altered.error());
        return {};
    });
}

} // namespace axk
