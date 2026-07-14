#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/sfs.hpp"

namespace axk {

struct MediaObject;

struct PcmFormat {
    std::uint16_t channels{1};
    std::uint16_t sample_width_bytes{};
    std::uint32_t sample_rate{};
};

struct Waveform {
    std::string object_key;
    std::filesystem::path source_path;
    PartitionIndex partition;
    SfsId sfs_id;
    std::string name;
    PcmFormat format;
    std::uint64_t frame_count{};
    std::uint16_t stored_sample_width_bytes{};
    std::uint32_t stored_payload_size{};
    std::string stored_payload_transform;
    bool alternating_byte_payload_detected{};
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::uint32_t loop_start{};
    std::uint32_t loop_length{};
    std::vector<std::byte> pcm;
};

struct PreviewBin {
    std::int32_t minimum{};
    std::int32_t maximum{};
};

struct PreviewEnvelope {
    std::uint64_t frame_count{};
    std::vector<PreviewBin> bins;
};

struct StereoRenderDecision {
    bool renderable{};
    std::string reason_code;
    std::uint64_t output_frame_count{};
    std::uint64_t left_padding_frames{};
    std::uint64_t right_padding_frames{};
};

AXK_API Result<Waveform> decode_waveform(const Container &container, const ObjectSnapshot &snapshot,
                                         const CancellationToken &cancellation = {});
AXK_API Result<Waveform> decode_waveform(const MediaObject &object);
AXK_API Result<std::vector<std::byte>> wav_bytes(const Waveform &waveform);
AXK_API Result<void> write_wav_atomic(const std::filesystem::path &path, const Waveform &waveform,
                                      bool overwrite = false);
AXK_API Result<PreviewEnvelope> build_preview_envelope(const Waveform &waveform, std::size_t bin_count);
AXK_API StereoRenderDecision stereo_render_decision(const Waveform &left, const Waveform &right);
AXK_API Result<Waveform> render_stereo(const Waveform &left, const Waveform &right);

} // namespace axk
