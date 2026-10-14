#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/application/device_sessions.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/media.hpp"
#include "axklib/native/source.hpp"
#include "image_filesystem_internal.hpp"

namespace axk::app {

struct ImageSourceMember {
    std::string id;
    ImageSourceRef reference;
    std::filesystem::path canonical_path;
    std::string format;
    std::string revision;
    std::vector<std::string> entry_revisions;
    std::shared_ptr<const RandomAccessReader> reader;
    std::shared_ptr<const MediaContainer> media;
    std::vector<filesystems::Root> roots;
    std::function<Result<void>()> verify_unchanged;
    std::function<Result<void>(const CancellationToken &, ProgressSink *)> seal;
    std::shared_ptr<PathReservationCoordinator::Lease> reservation;
    std::optional<detail::ImageFilesystemIndex> filesystem_index;
    std::optional<std::string> content_fingerprint;
    std::mutex fingerprint_mutex;
};

struct AdmittedImageSources {
    std::vector<std::shared_ptr<ImageSourceMember>> members;
    std::shared_ptr<const native::SourceSet> native_sources;
};

// Admission owns storage and identity, never sampler recognition or decoding.
[[nodiscard]] Result<AdmittedImageSources>
admit_image_sources(const Sandbox &sandbox, PathReservationCoordinator *reservations, DeviceSessionManager *devices,
                    std::span<const ImageSourceRef> references, std::string_view owner_id,
                    const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr,
                    diagnostic_sink *diagnostics = nullptr);

} // namespace axk::app
