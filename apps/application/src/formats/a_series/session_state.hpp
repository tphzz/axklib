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

struct PcmMember {
    std::string object_id;
    std::string role;
    std::uint16_t output_width{};
    std::uint32_t sample_rate{};
    std::uint64_t stored_frame_count{};
    std::uint64_t playback_start_frame{};
    std::uint64_t playback_length_frames{};
    std::uint64_t source_loop_start{};
    std::uint64_t source_loop_length{};
    std::uint64_t physical_first_frame{};
    std::uint64_t frame_count{};
    std::uint64_t loop_start{};
    std::uint64_t loop_length{};
};

struct PcmSource {
    std::vector<PcmMember> members;
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::vector<std::string> warnings;
};

enum class PcmReadWindow : std::uint8_t { stored_pcm, playback };

struct AuditionEntry {
    ImageAudition descriptor;
    std::vector<PcmSource> sources;
    std::chrono::steady_clock::time_point last_access;
};

struct SessionState final : NativeDocumentState {
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
    void clear_transient_state() noexcept override { auditions.clear(); }
    std::shared_ptr<const axk::formats::a_series::Document> document;
    ImageSourceRef source_reference;
    std::shared_ptr<const MediaContainer> media;
    std::unordered_map<std::string, const MediaObjectDescriptor *> descriptors_by_id;
    std::unordered_map<std::string, const ObjectSnapshot *> snapshots_by_id;
    std::unordered_map<std::string, WaveformStatus> waveform_status_by_id;
    std::unordered_map<std::string, std::uint64_t> waveform_cluster_counts_by_id;
    std::vector<CatalogIssue> catalog_issues;
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, VolumeCapacityReport>> volume_capacity_cache;
    std::unordered_map<std::string, AuditionEntry> auditions;

    [[nodiscard]] Result<PcmMember> prepare_member(std::string object_id, std::string role,
                                                   const CurrentSbnkMember *sample_member,
                                                   PcmReadWindow read_window) const;
    [[nodiscard]] Result<PcmSource> prepare_source(std::string_view object_id, PcmReadWindow read_window,
                                                   std::span<const ImageRelationshipItem> relationships) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_object_range(std::string_view object_id, std::uint64_t offset,
                                                                   std::size_t size,
                                                                   const CancellationToken &cancellation) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_member_pcm(const PcmMember &member, std::uint64_t first_frame,
                                                                 std::size_t frame_count,
                                                                 const CancellationToken &cancellation) const;
};

[[nodiscard]] inline SessionState &state(NativeDocumentState &document) {
    return dynamic_cast<SessionState &>(document);
}
[[nodiscard]] inline const SessionState &state(const NativeDocumentState &document) {
    return dynamic_cast<const SessionState &>(document);
}

} // namespace axk::app::formats::a_series
