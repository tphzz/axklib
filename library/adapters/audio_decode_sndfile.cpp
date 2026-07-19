#include "audio_import_internal.hpp"

#include "axklib/utf8.hpp"

#include <limits>
#include <memory>

#include <sndfile.h>

namespace axk::audio_import_detail {
namespace {

Error audio_error(std::string message) {
    return make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, std::move(message));
}

std::string format_name(int format) {
    switch (format & SF_FORMAT_TYPEMASK) {
    case SF_FORMAT_WAV:
        return "WAV";
    case SF_FORMAT_AIFF:
        return "AIFF";
    case SF_FORMAT_FLAC:
        return "FLAC";
    default:
        return "UNKNOWN";
    }
}

std::string subtype_name(int format) {
    switch (format & SF_FORMAT_SUBMASK) {
    case SF_FORMAT_PCM_U8:
        return "PCM_U8";
    case SF_FORMAT_PCM_S8:
        return "PCM_S8";
    case SF_FORMAT_PCM_16:
        return "PCM_16";
    case SF_FORMAT_PCM_24:
        return "PCM_24";
    case SF_FORMAT_PCM_32:
        return "PCM_32";
    case SF_FORMAT_FLOAT:
        return "FLOAT";
    case SF_FORMAT_DOUBLE:
        return "DOUBLE";
    default:
        return "UNKNOWN";
    }
}

std::uint8_t sample_width_bits(int subtype) {
    switch (subtype) {
    case SF_FORMAT_PCM_U8:
    case SF_FORMAT_PCM_S8:
        return 8U;
    case SF_FORMAT_PCM_16:
        return 16U;
    case SF_FORMAT_PCM_24:
        return 24U;
    case SF_FORMAT_PCM_32:
    case SF_FORMAT_FLOAT:
        return 32U;
    case SF_FORMAT_DOUBLE:
        return 64U;
    default:
        return 0U;
    }
}

using SndfileHandle = std::unique_ptr<SNDFILE, decltype(&sf_close)>;

struct VirtualInput {
    const RandomAccessReader *reader{};
    std::uint64_t offset{};
};

sf_count_t virtual_size(void *data) {
    const auto &input = *static_cast<VirtualInput *>(data);
    return input.reader->size() > static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max())
               ? -1
               : static_cast<sf_count_t>(input.reader->size());
}

sf_count_t virtual_seek(sf_count_t offset, int whence, void *data) {
    auto &input = *static_cast<VirtualInput *>(data);
    const auto size = input.reader->size();
    std::uint64_t base{};
    if (whence == SEEK_CUR) {
        base = input.offset;
    } else if (whence == SEEK_END) {
        base = size;
    } else if (whence != SEEK_SET) {
        return -1;
    }
    if ((offset < 0 && static_cast<std::uint64_t>(-(offset + 1)) + 1U > base) ||
        (offset >= 0 && static_cast<std::uint64_t>(offset) > size - std::min(size, base))) {
        return -1;
    }
    input.offset = offset < 0 ? base - (static_cast<std::uint64_t>(-(offset + 1)) + 1U)
                              : base + static_cast<std::uint64_t>(offset);
    return input.offset > static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max())
               ? -1
               : static_cast<sf_count_t>(input.offset);
}

sf_count_t virtual_read(void *destination, sf_count_t count, void *data) {
    if (count <= 0)
        return 0;
    auto &input = *static_cast<VirtualInput *>(data);
    const auto available = input.reader->size() - std::min(input.reader->size(), input.offset);
    const auto requested = static_cast<std::uint64_t>(count);
    const auto size = std::min(available, requested);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return 0;
    auto bytes = std::span{static_cast<std::byte *>(destination), static_cast<std::size_t>(size)};
    if (const auto read = input.reader->read_exact_at(input.offset, bytes); !read)
        return 0;
    input.offset += size;
    return static_cast<sf_count_t>(size);
}

sf_count_t virtual_write(const void *, sf_count_t, void *) { return 0; }

sf_count_t virtual_tell(void *data) {
    const auto &input = *static_cast<VirtualInput *>(data);
    return input.offset > static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max())
               ? -1
               : static_cast<sf_count_t>(input.offset);
}

SF_VIRTUAL_IO virtual_io{virtual_size, virtual_seek, virtual_read, virtual_write, virtual_tell};

struct SndfileSource {
    std::unique_ptr<VirtualInput> input;
    SndfileHandle handle{nullptr, &sf_close};

    [[nodiscard]] SNDFILE *get() const noexcept { return handle.get(); }
    explicit operator bool() const noexcept { return handle != nullptr; }
};

Result<SndfileSource> open_sndfile(const std::filesystem::path &path, SF_INFO &info) {
#ifdef _WIN32
    SndfileSource source{nullptr, SndfileHandle{sf_wchar_open(path.c_str(), SFM_READ, &info), &sf_close}};
#else
    const auto path_text = path.string();
    SndfileSource source{nullptr, SndfileHandle{sf_open(path_text.c_str(), SFM_READ, &info), &sf_close}};
#endif
    if (!source) {
        return std::unexpected{
            audio_error("cannot decode audio source " + text::path_to_utf8(path) + ": " + sf_strerror(nullptr))};
    }
    return source;
}

Result<SndfileSource> open_sndfile(const RandomAccessReader &reader, SF_INFO &info) {
    auto input = std::make_unique<VirtualInput>(VirtualInput{&reader, 0U});
    SndfileHandle handle{sf_open_virtual(&virtual_io, SFM_READ, &info, input.get()), &sf_close};
    SndfileSource source{std::move(input), std::move(handle)};
    if (!source)
        return std::unexpected{
            audio_error("cannot decode retained audio source: " + std::string{sf_strerror(nullptr)})};
    return source;
}

template <typename Source>
Result<SndfileSource> open_verified(const Source &source_input, const SourceAudioInfo &expected, SF_INFO &info) {
    auto source = open_sndfile(source_input, info);
    if (!source)
        return std::unexpected{source.error()};
    if (info.frames < 0 || static_cast<std::uint64_t>(info.frames) != expected.frames || info.channels < 0 ||
        static_cast<std::size_t>(info.channels) != expected.channels || info.samplerate < 0 ||
        static_cast<std::uint32_t>(info.samplerate) != expected.sample_rate) {
        return std::unexpected{audio_error("audio source metadata changed while it was being read")};
    }
    return source;
}

} // namespace

Result<SourceAudioInfo> inspect_sndfile(const std::filesystem::path &path,
                                        std::optional<std::size_t> expected_channels) {
    SF_INFO info{};
    auto source = open_sndfile(path, info);
    if (!source)
        return std::unexpected{source.error()};
    if (info.channels < 1 || info.channels > 2 ||
        (expected_channels && static_cast<std::size_t>(info.channels) != *expected_channels)) {
        return std::unexpected{audio_error("audio source channel count does not match the import")};
    }
    if (info.frames <= 0 || info.samplerate <= 0) {
        return std::unexpected{audio_error("audio source must contain frames at a valid sample rate")};
    }
    const auto channels = static_cast<std::size_t>(info.channels);
    if (static_cast<std::uint64_t>(info.frames) > std::numeric_limits<std::size_t>::max() / channels) {
        return std::unexpected{audio_error("audio source is too large")};
    }
    const auto subtype = info.format & SF_FORMAT_SUBMASK;
    const auto format = format_name(info.format);
    const auto subtype_text = subtype_name(info.format);
    const auto width_bits = sample_width_bits(subtype);
    if (format == "UNKNOWN" || subtype_text == "UNKNOWN" || width_bits == 0U) {
        return std::unexpected{
            audio_error("audio source uses a compressed or unsupported container or sample encoding")};
    }
    return SourceAudioInfo{
        .format = format,
        .subtype = subtype_text,
        .channels = channels,
        .frames = static_cast<std::size_t>(info.frames),
        .sample_rate = static_cast<std::uint32_t>(info.samplerate),
        .sample_width_bits = width_bits,
        .is_pcm8 = subtype == SF_FORMAT_PCM_U8 || subtype == SF_FORMAT_PCM_S8,
        .is_pcm16 = subtype == SF_FORMAT_PCM_16,
        .reduces_precision = subtype != SF_FORMAT_PCM_U8 && subtype != SF_FORMAT_PCM_S8 && subtype != SF_FORMAT_PCM_16,
    };
}

Result<SourceAudioInfo> inspect_sndfile(const RandomAccessReader &reader,
                                        std::optional<std::size_t> expected_channels) {
    SF_INFO info{};
    auto source = open_sndfile(reader, info);
    if (!source)
        return std::unexpected{source.error()};
    if (info.channels < 1 || info.channels > 2 ||
        (expected_channels && static_cast<std::size_t>(info.channels) != *expected_channels)) {
        return std::unexpected{audio_error("audio source channel count does not match the import")};
    }
    if (info.frames <= 0 || info.samplerate <= 0)
        return std::unexpected{audio_error("audio source must contain frames at a valid sample rate")};
    const auto channels = static_cast<std::size_t>(info.channels);
    if (static_cast<std::uint64_t>(info.frames) > std::numeric_limits<std::size_t>::max() / channels)
        return std::unexpected{audio_error("audio source is too large")};
    const auto subtype = info.format & SF_FORMAT_SUBMASK;
    const auto format = format_name(info.format);
    const auto subtype_text = subtype_name(info.format);
    const auto width_bits = sample_width_bits(subtype);
    if (format == "UNKNOWN" || subtype_text == "UNKNOWN" || width_bits == 0U)
        return std::unexpected{
            audio_error("audio source uses a compressed or unsupported container or sample encoding")};
    return SourceAudioInfo{.format = format,
                           .subtype = subtype_text,
                           .channels = channels,
                           .frames = static_cast<std::size_t>(info.frames),
                           .sample_rate = static_cast<std::uint32_t>(info.samplerate),
                           .sample_width_bits = width_bits,
                           .is_pcm8 = subtype == SF_FORMAT_PCM_U8 || subtype == SF_FORMAT_PCM_S8,
                           .is_pcm16 = subtype == SF_FORMAT_PCM_16,
                           .reduces_precision = subtype != SF_FORMAT_PCM_U8 && subtype != SF_FORMAT_PCM_S8 &&
                                                subtype != SF_FORMAT_PCM_16};
}

Result<std::vector<std::int16_t>> decode_sndfile_pcm16(const std::filesystem::path &path,
                                                       const SourceAudioInfo &expected) {
    SF_INFO info{};
    auto source = open_verified(path, expected, info);
    if (!source)
        return std::unexpected{source.error()};
    std::vector<std::int16_t> samples(expected.frames * expected.channels);
    const auto read = sf_readf_short(source->get(), samples.data(), info.frames);
    if (read != info.frames) {
        return std::unexpected{audio_error("audio source ended before its declared frame count")};
    }
    return samples;
}

Result<std::vector<std::int16_t>> decode_sndfile_pcm16(const RandomAccessReader &reader,
                                                       const SourceAudioInfo &expected) {
    SF_INFO info{};
    auto source = open_verified(reader, expected, info);
    if (!source)
        return std::unexpected{source.error()};
    std::vector<std::int16_t> samples(expected.frames * expected.channels);
    const auto read = sf_readf_short(source->get(), samples.data(), info.frames);
    if (read != info.frames)
        return std::unexpected{audio_error("audio source ended before its declared frame count")};
    return samples;
}

Result<std::vector<double>> decode_sndfile_float64(const std::filesystem::path &path, const SourceAudioInfo &expected) {
    SF_INFO info{};
    auto source = open_verified(path, expected, info);
    if (!source)
        return std::unexpected{source.error()};
    std::vector<double> samples(expected.frames * expected.channels);
    const auto read = sf_readf_double(source->get(), samples.data(), info.frames);
    if (read != info.frames) {
        return std::unexpected{audio_error("audio source ended before its declared frame count")};
    }
    return samples;
}

Result<std::vector<double>> decode_sndfile_float64(const RandomAccessReader &reader, const SourceAudioInfo &expected) {
    SF_INFO info{};
    auto source = open_verified(reader, expected, info);
    if (!source)
        return std::unexpected{source.error()};
    std::vector<double> samples(expected.frames * expected.channels);
    const auto read = sf_readf_double(source->get(), samples.data(), info.frames);
    if (read != info.frames)
        return std::unexpected{audio_error("audio source ended before its declared frame count")};
    return samples;
}

} // namespace axk::audio_import_detail
