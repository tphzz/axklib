#include "package_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/package_archive.hpp"

namespace axk::package_internal {

Error package_error(std::string message, ErrorCode code) {
    return make_error(code, ErrorCategory::manifest, std::move(message));
}

std::vector<std::byte> string_bytes(std::string_view value) {
    const auto source = std::as_bytes(std::span{value});
    return {source.begin(), source.end()};
}

std::string digest_text(std::string_view value) { return hex_digest(sha256(string_bytes(value))); }

std::string object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<ObjectType> parse_object_type(std::string_view value) {
    if (value == "SMPL")
        return ObjectType::smpl;
    if (value == "SBNK")
        return ObjectType::sbnk;
    if (value == "SBAC")
        return ObjectType::sbac;
    if (value == "PROG")
        return ObjectType::prog;
    if (value == "SEQU")
        return ObjectType::sequ;
    if (value == "PRF3")
        return ObjectType::prf3;
    return std::nullopt;
}

std::string object_format_name(ObjectFormat format) {
    switch (format) {
    case ObjectFormat::current:
        return "current";
    case ObjectFormat::alternating_byte:
        return "alternating-byte";
    case ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string media_kind_name(MediaKind kind) {
    switch (kind) {
    case MediaKind::sfs:
        return "sfs";
    case MediaKind::fat12_floppy:
        return "fat12-floppy";
    case MediaKind::fat12_floppy_set:
        return "fat12-floppy-set";
    case MediaKind::iso9660:
        return "iso9660";
    case MediaKind::standalone_object:
        return "standalone-object";
    case MediaKind::axk_object_directory:
        return "axk-object-directory";
    }
    return "unknown";
}

std::optional<PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return PackageRootKind::volume;
    if (value == "prog")
        return PackageRootKind::prog;
    if (value == "sbac")
        return PackageRootKind::sbac;
    if (value == "sbnk")
        return PackageRootKind::sbnk;
    if (value == "smpl")
        return PackageRootKind::smpl;
    if (value == "sequ")
        return PackageRootKind::sequ;
    return std::nullopt;
}

std::optional<PackageKind> parse_package_kind(std::string_view value) {
    if (value == "volume")
        return PackageKind::volume;
    if (value == "program")
        return PackageKind::program;
    if (value == "sbac")
        return PackageKind::sbac;
    if (value == "sbnk")
        return PackageKind::sbnk;
    if (value == "smpl")
        return PackageKind::smpl;
    if (value == "sequence")
        return PackageKind::sequence;
    if (value == "bundle")
        return PackageKind::bundle;
    return std::nullopt;
}

PackageKind package_kind_for_root(PackageRootKind kind) {
    switch (kind) {
    case PackageRootKind::volume:
        return PackageKind::volume;
    case PackageRootKind::prog:
        return PackageKind::program;
    case PackageRootKind::sbac:
        return PackageKind::sbac;
    case PackageRootKind::sbnk:
        return PackageKind::sbnk;
    case PackageRootKind::smpl:
        return PackageKind::smpl;
    case PackageRootKind::sequ:
        return PackageKind::sequence;
    }
    return PackageKind::bundle;
}

ObjectType root_object_type(PackageRootKind kind) {
    switch (kind) {
    case PackageRootKind::prog:
        return ObjectType::prog;
    case PackageRootKind::sbac:
        return ObjectType::sbac;
    case PackageRootKind::sbnk:
        return ObjectType::sbnk;
    case PackageRootKind::smpl:
        return ObjectType::smpl;
    case PackageRootKind::sequ:
        return ObjectType::sequ;
    case PackageRootKind::volume:
        return ObjectType::unknown;
    }
    return ObjectType::unknown;
}

std::string lower_extension(std::string_view filename) {
    const auto slash = filename.find_last_of("/\\");
    const auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string result{filename.substr(dot)};
    std::ranges::transform(result, result.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

bool recognized_extension(std::string_view extension) {
    constexpr std::array extensions{".axkvol", ".axkprg", ".axksbac", ".axksbnk", ".axksmpl", ".axkseq", ".axkpkg"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

bool closure_relationship(std::string_view role) {
    return role == "SBNK_LEFT_MEMBER_TO_SMPL" || role == "SBNK_RIGHT_MEMBER_TO_SMPL" || role == "SBAC_SLOT_TO_SBNK" ||
           role == "PROG_ASSIGNMENT_TO_SBAC" || role == "PROG_ASSIGNMENT_TO_SBNK";
}

Result<std::optional<WaveformDigests>> waveform_digests(const DecodedObject &decoded,
                                                        std::span<const std::byte> raw_payload,
                                                        std::string_view normalized_digest) {
    using Json = nlohmann::json;
    const auto *smpl = std::get_if<CurrentSmpl>(&decoded.payload);
    if (smpl == nullptr)
        return std::optional<WaveformDigests>{};
    MediaObject media_object;
    media_object.decoded = decoded;
    media_object.raw_payload.assign(raw_payload.begin(), raw_payload.end());
    auto waveform = decode_waveform(media_object);
    if (!waveform)
        return std::unexpected{waveform.error()};
    const auto audio = hex_digest(sha256(waveform->pcm));
    const auto optional_u64 = [](const std::optional<std::uint64_t> &value) -> Json {
        return value ? Json(*value) : Json(nullptr);
    };
    const Json semantic{{"audio_sha256", audio},
                        {"decoded_sample_width_bytes", waveform->format.sample_width_bytes},
                        {"fine_tune_cents", smpl->fine_tune_cents.value},
                        {"frame_count", waveform->frame_count},
                        {"loop_end_frame_exclusive", optional_u64(smpl->loop_end_frame_exclusive)},
                        {"loop_end_frame_inclusive", optional_u64(smpl->loop_end_frame_inclusive)},
                        {"loop_length_frames", smpl->loop_length_frames.value},
                        {"loop_mode", smpl->loop_mode.value},
                        {"loop_start_frame", smpl->loop_start_frame.value},
                        {"name", decoded.header.name},
                        {"normalized_sha256", normalized_digest},
                        {"object_format", object_format_name(decoded.format)},
                        {"root_key", smpl->root_key.value},
                        {"sample_rate", smpl->sample_rate.value},
                        {"schema", "axklib-smpl-semantic-v1"},
                        {"source_wave_name", smpl->source_wave_name.value},
                        {"stored_pcm_bytes", smpl->stored_pcm_bytes},
                        {"stored_pcm_offset", smpl->stored_pcm_offset},
                        {"stored_sample_width_bytes", smpl->stored_sample_width_bytes.value},
                        {"wave_length_frames", smpl->wave_length_frames.value}};
    return std::optional{WaveformDigests{digest_text(semantic.dump() + '\n'), audio}};
}

std::string edge_id(std::string_view source, std::string_view target, std::string_view role, std::uint32_t ordinal) {
    std::string identity;
    const auto append = [&](std::string_view value) {
        identity += std::format("{}:", value.size());
        identity.append(value);
        identity.push_back(';');
    };
    append(source);
    append(target);
    append(role);
    append(std::to_string(ordinal));
    return "e-" + digest_text(identity);
}

} // namespace axk::package_internal
