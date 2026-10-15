#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/formats/a_series/document.hpp"
#include "native_document_state.hpp"

namespace axk::app::formats::a_series {

struct SessionState final : NativeDocumentState, NativeAudioProvider {
    [[nodiscard]] static Result<std::unique_ptr<SessionState>>
    create(std::shared_ptr<const axk::formats::a_series::Document> document, const ImageSourceRef &source,
           diagnostic_sink *diagnostics = nullptr);
    [[nodiscard]] std::string_view format_id() const noexcept override { return "yamaha.a-series"; }
    [[nodiscard]] std::string_view profile() const noexcept override { return "a-series"; }
    [[nodiscard]] std::vector<std::string> available_operations(bool source_writable,
                                                                bool device_writable) const override;
    [[nodiscard]] Result<nlohmann::ordered_json> object_detail(const ImageDocumentContext &session,
                                                               std::string_view object_id) const override;
    [[nodiscard]] NativeFilesystemBindings filesystem_bindings() const override;
    [[nodiscard]] Result<void> preserve_identity(const NativeDocumentState &previous) override;
    [[nodiscard]] const NativeAudioProvider *audio() const noexcept override { return this; }
    std::shared_ptr<const axk::formats::a_series::Document> document;
    ImageSourceRef source_reference;
    std::shared_ptr<const MediaContainer> media;
    std::unordered_map<std::string, const MediaObjectDescriptor *> descriptors_by_id;
    std::unordered_map<std::string, const ObjectSnapshot *> snapshots_by_id;
    std::unordered_map<std::string, WaveformStatus> waveform_status_by_id;
    std::unordered_map<std::string, std::uint64_t> waveform_cluster_counts_by_id;
    std::vector<CatalogIssue> catalog_issues;
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, VolumeCapacityReport>> volume_capacity_cache;

    [[nodiscard]] Result<native::PcmMember> prepare_member(std::string object_id, std::string role,
                                                           const CurrentSbnkMember *sample_member,
                                                           native::PcmReadWindow read_window) const;
    [[nodiscard]] Result<native::PcmSource> prepare_source(std::string_view object_id,
                                                           native::PcmReadWindow read_window) const override;
    [[nodiscard]] Result<std::vector<std::byte>> read_object_range(std::string_view object_id, std::uint64_t offset,
                                                                   std::size_t size,
                                                                   const CancellationToken &cancellation) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_member_pcm(const native::PcmMember &member,
                                                                 std::uint64_t first_frame, std::size_t frame_count,
                                                                 const CancellationToken &cancellation) const override;
};

[[nodiscard]] inline SessionState &state(NativeDocumentState &document) {
    return dynamic_cast<SessionState &>(document);
}
[[nodiscard]] inline const SessionState &state(const NativeDocumentState &document) {
    return dynamic_cast<const SessionState &>(document);
}

} // namespace axk::app::formats::a_series
