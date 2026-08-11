#include "validation_operations_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/utf8.hpp"

namespace axk::app::validation_operations_internal {

axk::ReportRow export_validation_issue(std::string severity, std::string code, std::string message, std::string scope,
                                       const std::filesystem::path &source, std::string object_key = {}) {
    return {{"severity", std::move(severity)},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"scope", std::move(scope)},
            {"source_path", axk::text::path_to_utf8(source)},
            {"sampler_path", ""},
            {"object_key", std::move(object_key)},
            {"quality", "Known"},
            {"basis", "validation"},
            {"recommended_next_check", ""}};
}

std::optional<std::uint32_t> little_u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 4U > bytes.size())
        return std::nullopt;
    return std::to_integer<std::uint8_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

std::optional<std::uint16_t> little_u16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 2U > bytes.size())
        return std::nullopt;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U));
}

struct ExportWavHeader {
    std::uint64_t sample_rate{};
    std::uint64_t channels{};
    std::uint64_t sample_width_bytes{};
    std::uint64_t frames{};
};

std::expected<ExportWavHeader, std::string> parse_export_wav(const axk::RandomAccessReader &reader) {
    constexpr std::uint64_t riff_header_size = 12U;
    constexpr std::size_t maximum_chunks = 1024U;
    if (reader.size() < riff_header_size)
        return std::unexpected("truncated RIFF header");
    std::array<std::byte, riff_header_size> riff{};
    if (const auto read = reader.read_exact_at(0U, riff); !read)
        return std::unexpected(read.error().message);
    if (std::string_view{reinterpret_cast<const char *>(riff.data()), 4U} != "RIFF" ||
        std::string_view{reinterpret_cast<const char *>(riff.data() + 8U), 4U} != "WAVE") {
        return std::unexpected("invalid RIFF/WAVE signature");
    }
    const auto riff_size = little_u32(riff, 4U);
    if (!riff_size || static_cast<std::uint64_t>(*riff_size) + 8U > reader.size())
        return std::unexpected("RIFF size exceeds the retained file");
    const auto riff_end = static_cast<std::uint64_t>(*riff_size) + 8U;

    std::optional<ExportWavHeader> format;
    std::optional<std::uint32_t> data_size;
    std::uint64_t offset = riff_header_size;
    for (std::size_t chunk_index = 0U; offset < riff_end && chunk_index < maximum_chunks; ++chunk_index) {
        if (riff_end - offset < 8U)
            return std::unexpected("truncated chunk header");
        std::array<std::byte, 8U> chunk{};
        if (const auto read = reader.read_exact_at(offset, chunk); !read)
            return std::unexpected(read.error().message);
        const auto size = little_u32(chunk, 4U);
        if (!size)
            return std::unexpected("invalid chunk size");
        const auto payload_offset = offset + 8U;
        const auto padded_size = static_cast<std::uint64_t>(*size) + (*size & 1U);
        if (padded_size > riff_end - payload_offset)
            return std::unexpected("chunk exceeds the RIFF container");
        const std::string_view identifier{reinterpret_cast<const char *>(chunk.data()), 4U};
        if (identifier == "fmt ") {
            if (format || *size < 16U)
                return std::unexpected(format ? "duplicate format chunk" : "truncated format chunk");
            std::array<std::byte, 16U> bytes{};
            if (const auto read = reader.read_exact_at(payload_offset, bytes); !read)
                return std::unexpected(read.error().message);
            const auto encoding = little_u16(bytes, 0U);
            const auto channels = little_u16(bytes, 2U);
            const auto sample_rate = little_u32(bytes, 4U);
            const auto byte_rate = little_u32(bytes, 8U);
            const auto block_align = little_u16(bytes, 12U);
            const auto bits_per_sample = little_u16(bytes, 14U);
            if (!encoding || *encoding != 1U)
                return std::unexpected("WAV is not integer PCM");
            if (!channels || *channels == 0U || *channels > 2U)
                return std::unexpected("invalid channel count");
            if (!sample_rate || *sample_rate == 0U)
                return std::unexpected("invalid sample rate");
            if (!bits_per_sample || (*bits_per_sample != 8U && *bits_per_sample != 16U))
                return std::unexpected("invalid sample width");
            const auto sample_width = static_cast<std::uint16_t>(*bits_per_sample / 8U);
            const auto expected_alignment = static_cast<std::uint32_t>(*channels) * sample_width;
            const auto expected_byte_rate = static_cast<std::uint64_t>(*sample_rate) * expected_alignment;
            if (!block_align || *block_align != expected_alignment || !byte_rate || *byte_rate != expected_byte_rate)
                return std::unexpected("inconsistent PCM format geometry");
            format = ExportWavHeader{*sample_rate, *channels, sample_width, 0U};
        } else if (identifier == "data") {
            if (data_size)
                return std::unexpected("duplicate data chunk");
            data_size = *size;
        }
        offset = payload_offset + padded_size;
    }
    if (offset != riff_end)
        return std::unexpected("WAV chunk count exceeds the validation limit");
    if (!format || !data_size)
        return std::unexpected(!format ? "missing format chunk" : "missing data chunk");
    const auto block_align = format->channels * format->sample_width_bytes;
    if (block_align == 0U || *data_size % block_align != 0U)
        return std::unexpected("data chunk is not frame-aligned");
    format->frames = *data_size / block_align;
    return *format;
}

std::optional<std::string> normalized_export_path(std::string_view raw_path, const std::filesystem::path &sidecar,
                                                  bool relative_to_export_root) {
    const auto decoded = axk::text::path_from_utf8(raw_path);
    if (!decoded || decoded->empty() || decoded->is_absolute() || decoded->has_root_name() ||
        decoded->has_root_directory()) {
        return std::nullopt;
    }
    const auto combined = (relative_to_export_root ? std::filesystem::path{} : sidecar.parent_path()) / *decoded;
    const auto normalized = combined.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() || normalized.has_root_name() ||
        normalized.has_root_directory() || *normalized.begin() == "..") {
        return std::nullopt;
    }
    return axk::text::path_to_utf8(normalized);
}

std::expected<Json, std::string> parse_export_json(const axk::app::SandboxTree &tree, std::size_t index,
                                                   const axk::app::SandboxTreeEntry &file) {
    constexpr std::uint64_t maximum_sidecar_bytes = 64U * 1024U * 1024U;
    if (file.size > maximum_sidecar_bytes)
        return std::unexpected("sidecar exceeds the 64 MiB validation limit");
    auto opened = tree.open_file(index);
    if (!opened)
        return std::unexpected(opened.error().message);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.size));
    if (const auto read = opened->reader->read_exact_at(0U, bytes); !read)
        return std::unexpected(read.error().message);
    if (const auto unchanged = opened->verify_unchanged(); !unchanged)
        return std::unexpected(unchanged.error().message);
    try {
        return Json::parse(reinterpret_cast<const char *>(bytes.data()),
                           reinterpret_cast<const char *>(bytes.data() + bytes.size()));
    } catch (const std::exception &error) {
        return std::unexpected(error.what());
    }
}

std::vector<axk::ReportRow> validate_export_directory(const axk::app::SandboxTree &tree) {
    std::vector<axk::ReportRow> issues;
    std::map<std::string, std::size_t, std::less<>> by_path;
    for (std::size_t index = 0U; index < tree.entries().size(); ++index) {
        if (tree.entries()[index].kind == axk::app::SandboxTreeEntryKind::file)
            by_path.emplace(tree.entries()[index].relative_path, index);
    }
    for (std::size_t index = 0U; index < tree.entries().size(); ++index) {
        const auto &file = tree.entries()[index];
        if (file.kind != axk::app::SandboxTreeEntryKind::file)
            continue;
        const auto sidecar_path = axk::text::path_from_utf8(file.relative_path);
        if (!sidecar_path || sidecar_path->extension() != ".json" || sidecar_path->filename() == "schema_index.json" ||
            std::ranges::find(*sidecar_path, "_schemas") != sidecar_path->end()) {
            continue;
        }
        const auto record_result = parse_export_json(tree, index, file);
        if (!record_result) {
            issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_BAD_JSON",
                                                     "Sidecar JSON could not be parsed: " + record_result.error(),
                                                     "sidecar", *sidecar_path));
            continue;
        }
        const auto &record = *record_result;
        if (!record.is_object())
            continue;
        const auto schema = record.value("schema", std::string{});
        if (schema == "axklib.volume_graph.v1") {
            const auto inspect_path = [&](const Json &value, std::string_view object_key) {
                if (!value.is_string())
                    return;
                if (!normalized_export_path(value.get<std::string>(), *sidecar_path, false)) {
                    issues.push_back(export_validation_issue("error", "EXPORT_VOLUME_GRAPH_PATH_ESCAPE",
                                                             "Volume graph WAV path must be relative and stay inside "
                                                             "the export root.",
                                                             "sidecar", *sidecar_path, std::string{object_key}));
                }
            };
            if (record.contains("objects") && record["objects"].is_object() && record["objects"].contains("smpl") &&
                record["objects"]["smpl"].is_array()) {
                for (const auto &wave_data : record["objects"]["smpl"])
                    inspect_path(wave_data.value("wav_path", Json{}), wave_data.value("object_key", std::string{}));
            }
            continue;
        }
        if (schema != "axklib.wave_sidecar.v1") {
            if (record.contains("wav_path") || schema.starts_with("axklib.wave_sidecar.")) {
                issues.push_back(export_validation_issue(
                    "error", "EXPORT_SIDECAR_UNSUPPORTED_SCHEMA",
                    "Wave sidecar must use the current axklib.wave_sidecar.v1 schema.", "sidecar", *sidecar_path));
            }
            continue;
        }
        auto object_key = std::string{};
        static constexpr std::array sections{"identity",   "audio",      "playback", "relationships",
                                             "parameters", "conversion", "origin"};
        std::vector<std::string> missing;
        for (const auto section : sections) {
            if (!record.contains(section))
                missing.emplace_back(section);
        }
        if (record.contains("identity") && record["identity"].is_object())
            object_key = record["identity"].value("object_key", object_key);
        if (!missing.empty()) {
            std::string section_names;
            for (const auto &section : missing) {
                if (!section_names.empty())
                    section_names += ", ";
                section_names += section;
            }
            issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_MISSING_FIELD",
                                                     "Sidecar missing required sections: " + section_names, "sidecar",
                                                     *sidecar_path, object_key));
        }
        if (!record.contains("audio") || !record["audio"].is_object())
            continue;
        const auto &header = record["audio"];
        const auto normalized = normalized_export_path(header.value("wav_path", std::string{}), *sidecar_path, true);
        if (!normalized) {
            issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_PATH_ESCAPE",
                                                     "Sidecar audio.wav_path must be relative and stay inside the "
                                                     "export root.",
                                                     "sidecar", *sidecar_path, object_key));
            continue;
        }
        const auto wav_path = axk::text::path_from_utf8(*normalized).value_or(std::filesystem::path{});
        const auto wav_key = axk::text::path_to_utf8(wav_path);
        const auto wav_file = by_path.find(wav_key);
        if (wav_file == by_path.end()) {
            issues.push_back(export_validation_issue("error", "EXPORT_WAV_MISSING",
                                                     "Referenced WAV does not exist: " + wav_key, "export",
                                                     *sidecar_path, object_key));
            continue;
        }
        auto opened_wav = tree.open_file(wav_file->second);
        if (!opened_wav) {
            issues.push_back(export_validation_issue("error", "EXPORT_WAV_CHANGED",
                                                     "Referenced WAV changed during validation", "export", wav_path,
                                                     object_key));
            continue;
        }
        const auto observed_header = parse_export_wav(*opened_wav->reader);
        const auto unchanged = opened_wav->verify_unchanged();
        if (!observed_header || !unchanged) {
            issues.push_back(
                export_validation_issue("error", "EXPORT_WAV_BAD_HEADER",
                                        "Referenced WAV has an invalid WAVE header: " +
                                            (observed_header ? unchanged.error().message : observed_header.error()),
                                        "export", wav_path, object_key));
            continue;
        }
        const std::array observed{
            std::pair{"sample_rate", observed_header->sample_rate},
            std::pair{"channels", observed_header->channels},
            std::pair{"sample_width_bytes", observed_header->sample_width_bytes},
            std::pair{"frames", observed_header->frames},
        };
        for (const auto &[name, value] : observed) {
            if (header.contains(name) && header[name].is_number_integer() &&
                header[name].get<std::uint64_t>() != value) {
                issues.push_back(export_validation_issue(
                    "error", "EXPORT_WAV_HEADER_MISMATCH",
                    std::format("{} sidecar={} wav={}", name, header[name].get<std::uint64_t>(), value), "export",
                    wav_path, object_key));
            }
        }
    }
    return issues;
}

} // namespace axk::app::validation_operations_internal
