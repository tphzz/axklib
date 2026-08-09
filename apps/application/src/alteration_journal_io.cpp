#include "alteration_journal_io.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>

#include <hash-library/sha256.h>

#include "axklib/file_publication.hpp"

namespace {

constexpr std::array<std::byte, 8> journal_magic{
    std::byte{'A'}, std::byte{'X'}, std::byte{'K'}, std::byte{'J'},
    std::byte{'N'}, std::byte{'L'}, std::byte{'0'}, std::byte{'2'},
};
constexpr std::byte prepared_state{0U};
constexpr std::uint64_t header_size = 40U;
constexpr std::uint64_t checksum_size = 64U;
constexpr std::uint64_t maximum_metadata_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t maximum_patch_count = 1'000'000U;

axk::app::Error journal_error(std::string message, bool retryable = false) {
    return {"alteration_journal_unavailable", std::move(message), {}, retryable};
}

axk::app::Error capacity_error(std::uint64_t required, std::uint64_t maximum) {
    return {
        "alteration_journal_capacity",
        std::format("alteration journal requires {} bytes but its configured limit is {} bytes", required, maximum)};
}

axk::app::Error storage_capacity_error(std::uint64_t required, std::uint64_t available) {
    return {"alteration_journal_capacity",
            std::format("alteration journal storage requires {} available bytes but only {} bytes are available",
                        required, available)};
}

bool checked_add(std::uint64_t &value, std::uint64_t added) {
    if (added > std::numeric_limits<std::uint64_t>::max() - value)
        return false;
    value += added;
    return true;
}

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

void write_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index)
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index)
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value{};
    for (std::size_t index = 0U; index < 8U; ++index)
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

std::filesystem::path marker_path(const std::filesystem::path &journal_path) {
    auto marker = journal_path;
    marker += ".commit";
    return marker;
}

class Reader final {
  public:
    static axk::app::Result<Reader> open(const std::filesystem::path &path, std::uint64_t maximum_bytes) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maximum_bytes ||
            size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            return std::unexpected(journal_error("alteration journal size is invalid"));
        std::ifstream input{path, std::ios::binary};
        if (!input)
            return std::unexpected(journal_error("alteration journal cannot be opened"));
        return Reader{std::move(input), size};
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

    axk::app::Result<void> read_at(std::uint64_t offset, std::span<std::byte> bytes) {
        if (offset > size_ || bytes.size() > size_ - offset ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
            bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return std::unexpected(journal_error("alteration journal read is out of range"));
        }
        if (bytes.empty())
            return {};
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input_.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input_)
            return std::unexpected(journal_error("alteration journal cannot be read"));
        return {};
    }

  private:
    Reader(std::ifstream input, std::uint64_t size) : input_(std::move(input)), size_(size) {}

    std::ifstream input_;
    std::uint64_t size_{};
};

axk::app::Result<void> check_cancelled(const axk::CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(axk::app::Error{"operation_cancelled", checked.error().message});
    return {};
}

axk::app::Result<void> append(axk::detail::TemporaryPublication &publication, SHA256 &payload_hash, SHA256 &file_hash,
                              std::span<const std::byte> bytes) {
    if (auto written = publication.append(bytes); !written)
        return std::unexpected(journal_error(written.error().message));
    payload_hash.add(bytes.data(), bytes.size());
    file_hash.add(bytes.data(), bytes.size());
    return {};
}

axk::app::Result<std::string> read_string(Reader &reader, std::uint64_t &offset, std::uint32_t size,
                                          std::uint64_t payload_size) {
    if (offset > payload_size || size > payload_size - offset)
        return std::unexpected(journal_error("alteration journal metadata is truncated"));
    std::string value(size, '\0');
    if (auto read = reader.read_at(offset, std::as_writable_bytes(std::span{value})); !read)
        return std::unexpected(read.error());
    offset += size;
    return value;
}

axk::app::Result<void> read_journal_chunk(Reader &reader, std::uint64_t offset, std::span<std::byte> bytes) {
    if (auto read = reader.read_at(offset, bytes); !read)
        return std::unexpected(read.error());
    return {};
}

} // namespace

axk::app::Result<std::uint64_t> axk::app::journal_io::encoded_size(const FileRef &target,
                                                                   std::string_view target_identity,
                                                                   std::span<const AlterationJournalPatch> patches) {
    if (target.root_id.size() > std::numeric_limits<std::uint32_t>::max() ||
        target.relative_path.size() > std::numeric_limits<std::uint32_t>::max() ||
        target_identity.size() > std::numeric_limits<std::uint32_t>::max() || patches.size() > maximum_patch_count) {
        return std::unexpected(journal_error("alteration journal metadata exceeds supported limits"));
    }
    std::uint64_t size = header_size + checksum_size;
    if (!checked_add(size, target.root_id.size()) || !checked_add(size, target.relative_path.size()) ||
        !checked_add(size, target_identity.size())) {
        return std::unexpected(journal_error("alteration journal size exceeds supported limits"));
    }
    for (const auto &patch : patches) {
        if (patch.original.size() != patch.replacement.size())
            return std::unexpected(journal_error("alteration patch changes the image size"));
        if (!checked_add(size, 16U) || !checked_add(size, patch.original.size()) ||
            !checked_add(size, patch.replacement.size())) {
            return std::unexpected(journal_error("alteration journal size exceeds supported limits"));
        }
    }
    return size;
}

axk::app::Result<axk::app::journal_io::Publication>
axk::app::journal_io::publish(const std::filesystem::path &path, const FileRef &target,
                              std::string_view target_identity, std::uint64_t image_size_bytes,
                              std::span<const AlterationJournalPatch> patches, std::uint64_t maximum_journal_bytes,
                              const CancellationToken &cancellation, std::size_t chunk_bytes) {
    auto required = encoded_size(target, target_identity, patches);
    if (!required)
        return std::unexpected(required.error());
    if (*required > maximum_journal_bytes)
        return std::unexpected(capacity_error(*required, maximum_journal_bytes));
    for (const auto &patch : patches) {
        if (patch.offset > image_size_bytes || patch.original.size() > image_size_bytes - patch.offset)
            return std::unexpected(journal_error("alteration patch exceeds the image boundary"));
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled)
        return std::unexpected(cancelled.error());
    std::error_code space_error;
    const auto space = std::filesystem::space(path.parent_path(), space_error);
    std::uint64_t storage_required = *required;
    if (!checked_add(storage_required, checksum_size))
        return std::unexpected(journal_error("alteration journal size exceeds supported limits"));
    if (space_error)
        return std::unexpected(journal_error("alteration journal storage capacity cannot be inspected"));
    if (space.available < storage_required)
        return std::unexpected(storage_capacity_error(storage_required, space.available));

    auto publication = axk::detail::TemporaryPublication::create(path);
    if (!publication)
        return std::unexpected(journal_error(publication.error().message));
    SHA256 payload_hash;
    SHA256 file_hash;
    std::array<std::byte, header_size> header{};
    std::ranges::copy(journal_magic, header.begin());
    header[journal_magic.size()] = prepared_state;
    write_u32(header, 16U, static_cast<std::uint32_t>(target.root_id.size()));
    write_u32(header, 20U, static_cast<std::uint32_t>(target.relative_path.size()));
    write_u32(header, 24U, static_cast<std::uint32_t>(target_identity.size()));
    write_u64(header, 28U, image_size_bytes);
    write_u32(header, 36U, static_cast<std::uint32_t>(patches.size()));
    if (auto written = append(*publication, payload_hash, file_hash, header); !written)
        return std::unexpected(written.error());
    const std::array strings{std::string_view{target.root_id}, std::string_view{target.relative_path}, target_identity};
    for (const auto value : strings) {
        if (auto written = append(*publication, payload_hash, file_hash, std::as_bytes(std::span{value})); !written)
            return std::unexpected(written.error());
    }
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    for (const auto &patch : patches) {
        std::array<std::byte, 16> descriptor{};
        write_u64(descriptor, 0U, patch.offset);
        write_u64(descriptor, 8U, patch.original.size());
        if (auto written = append(*publication, payload_hash, file_hash, descriptor); !written)
            return std::unexpected(written.error());
        const std::array payloads{std::span<const std::byte>{patch.original},
                                  std::span<const std::byte>{patch.replacement}};
        for (const auto payload : payloads) {
            for (std::size_t offset = 0U; offset < payload.size();) {
                if (auto cancelled = check_cancelled(cancellation); !cancelled)
                    return std::unexpected(cancelled.error());
                const auto size = std::min(maximum_chunk, payload.size() - offset);
                if (auto written = append(*publication, payload_hash, file_hash, payload.subspan(offset, size));
                    !written) {
                    return std::unexpected(written.error());
                }
                offset += size;
            }
        }
    }
    const auto payload_checksum = payload_hash.getHash();
    const auto checksum_bytes = std::as_bytes(std::span{payload_checksum});
    if (auto written = publication->append(checksum_bytes); !written)
        return std::unexpected(journal_error(written.error().message));
    file_hash.add(checksum_bytes.data(), checksum_bytes.size());
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected(journal_error(flushed.error().message));
    auto published = publication->publish(axk::detail::PublicationMode::create_only);
    if (!published)
        return std::unexpected(journal_error(published.error().message));
    return Publication{published->durability, file_hash.getHash(), *required};
}

axk::app::Result<axk::app::journal_io::Inspection> axk::app::journal_io::inspect(const std::filesystem::path &path,
                                                                                 std::uint64_t maximum_journal_bytes,
                                                                                 std::size_t chunk_bytes) {
    auto reader = Reader::open(path, maximum_journal_bytes);
    if (!reader)
        return std::unexpected(reader.error());
    if (reader->size() < header_size + checksum_size)
        return std::unexpected(journal_error("alteration journal has an invalid header"));
    const auto payload_size = reader->size() - checksum_size;
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    std::vector<std::byte> buffer(std::min<std::uint64_t>(maximum_chunk, reader->size()));
    SHA256 payload_hash;
    SHA256 file_hash;
    for (std::uint64_t offset = 0U; offset < reader->size();) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader->size() - offset));
        auto bytes = std::span{buffer}.first(size);
        if (auto read = reader->read_at(offset, bytes); !read)
            return std::unexpected(read.error());
        file_hash.add(bytes.data(), bytes.size());
        if (offset < payload_size) {
            const auto payload_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(size, payload_size - offset));
            payload_hash.add(bytes.data(), payload_bytes);
        }
        offset += size;
    }
    std::array<std::byte, checksum_size> stored_checksum_bytes{};
    if (auto read = reader->read_at(payload_size, stored_checksum_bytes); !read)
        return std::unexpected(read.error());
    const std::string stored_checksum{reinterpret_cast<const char *>(stored_checksum_bytes.data()),
                                      stored_checksum_bytes.size()};
    if (payload_hash.getHash() != stored_checksum)
        return std::unexpected(journal_error("alteration journal checksum does not match"));

    std::array<std::byte, header_size> header{};
    if (auto read = reader->read_at(0U, header); !read)
        return std::unexpected(read.error());
    if (!std::ranges::equal(journal_magic, std::span{header}.first(journal_magic.size())) ||
        header[journal_magic.size()] != prepared_state) {
        return std::unexpected(journal_error("alteration journal has an invalid header"));
    }
    const auto root_size = read_u32(header, 16U);
    const auto path_size = read_u32(header, 20U);
    const auto identity_size = read_u32(header, 24U);
    const auto image_size = read_u64(header, 28U);
    const auto patch_count = read_u32(header, 36U);
    std::uint64_t metadata_size{};
    if (!checked_add(metadata_size, root_size) || !checked_add(metadata_size, path_size) ||
        !checked_add(metadata_size, identity_size) || metadata_size > maximum_metadata_bytes ||
        patch_count > maximum_patch_count) {
        return std::unexpected(journal_error("alteration journal metadata exceeds supported limits"));
    }
    std::uint64_t offset = header_size;
    auto root = read_string(*reader, offset, root_size, payload_size);
    auto relative_path = read_string(*reader, offset, path_size, payload_size);
    auto identity = read_string(*reader, offset, identity_size, payload_size);
    if (!root || !relative_path || !identity) {
        if (!root)
            return std::unexpected(root.error());
        if (!relative_path)
            return std::unexpected(relative_path.error());
        return std::unexpected(identity.error());
    }
    Inspection inspection{
        {std::move(*root), std::move(*relative_path)}, std::move(*identity), image_size, {}, file_hash.getHash()};
    inspection.patches.reserve(patch_count);
    for (std::uint32_t index = 0U; index < patch_count; ++index) {
        if (offset > payload_size || 16U > payload_size - offset)
            return std::unexpected(journal_error("alteration journal patch descriptor is truncated"));
        std::array<std::byte, 16> descriptor{};
        if (auto read = reader->read_at(offset, descriptor); !read)
            return std::unexpected(read.error());
        offset += descriptor.size();
        const auto target_offset = read_u64(descriptor, 0U);
        const auto size = read_u64(descriptor, 8U);
        if (target_offset > image_size || size > image_size - target_offset)
            return std::unexpected(journal_error("alteration journal patch exceeds the image boundary"));
        const auto original_offset = offset;
        if (!checked_add(offset, size) || offset > payload_size)
            return std::unexpected(journal_error("alteration journal original bytes are truncated"));
        const auto replacement_offset = offset;
        if (!checked_add(offset, size) || offset > payload_size)
            return std::unexpected(journal_error("alteration journal replacement bytes are truncated"));
        inspection.patches.push_back({target_offset, size, original_offset, replacement_offset});
    }
    if (offset != payload_size)
        return std::unexpected(journal_error("alteration journal contains trailing payload bytes"));
    return inspection;
}

axk::app::Result<bool> axk::app::journal_io::commit_marker_matches(const std::filesystem::path &journal_path,
                                                                   std::string_view file_checksum) {
    const auto marker = marker_path(journal_path);
    std::error_code error;
    if (!std::filesystem::exists(marker, error))
        return error ? std::unexpected(journal_error("alteration commit marker cannot be inspected"))
                     : Result<bool>{false};
    auto reader = Reader::open(marker, checksum_size);
    if (!reader || reader->size() != checksum_size)
        return false;
    std::array<std::byte, checksum_size> bytes{};
    if (auto read = reader->read_at(0U, bytes); !read)
        return false;
    return std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()} == file_checksum;
}

axk::app::Result<void> axk::app::journal_io::compare_target(const SandboxMutation &target,
                                                            const std::filesystem::path &journal_path,
                                                            const Inspection &journal, bool replacement,
                                                            std::size_t chunk_bytes) {
    auto reader = Reader::open(journal_path, std::numeric_limits<std::uint64_t>::max());
    if (!reader)
        return std::unexpected(reader.error());
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    std::vector<std::byte> expected(maximum_chunk);
    std::vector<std::byte> current(maximum_chunk);
    for (const auto &patch : journal.patches) {
        for (std::uint64_t offset = 0U; offset < patch.size;) {
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_chunk, patch.size - offset));
            auto expected_chunk = std::span{expected}.first(size);
            auto current_chunk = std::span{current}.first(size);
            const auto journal_offset =
                (replacement ? patch.replacement_file_offset : patch.original_file_offset) + offset;
            if (auto read = read_journal_chunk(*reader, journal_offset, expected_chunk); !read)
                return read;
            if (auto read = target.read_exact_at(patch.target_offset + offset, current_chunk); !read)
                return std::unexpected(journal_error(read.error().message));
            if (!std::ranges::equal(current_chunk, expected_chunk))
                return std::unexpected(journal_error("alteration target does not match its journal", true));
            offset += size;
        }
    }
    return {};
}

axk::app::Result<void> axk::app::journal_io::recognize_uncommitted_target(const SandboxMutation &target,
                                                                          const std::filesystem::path &journal_path,
                                                                          const Inspection &journal,
                                                                          std::size_t chunk_bytes) {
    auto reader = Reader::open(journal_path, std::numeric_limits<std::uint64_t>::max());
    if (!reader)
        return std::unexpected(reader.error());
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    std::vector<std::byte> original(maximum_chunk);
    std::vector<std::byte> replacement(maximum_chunk);
    std::vector<std::byte> current(maximum_chunk);
    for (const auto &patch : journal.patches) {
        for (std::uint64_t offset = 0U; offset < patch.size;) {
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_chunk, patch.size - offset));
            auto original_chunk = std::span{original}.first(size);
            auto replacement_chunk = std::span{replacement}.first(size);
            auto current_chunk = std::span{current}.first(size);
            if (auto read = read_journal_chunk(*reader, patch.original_file_offset + offset, original_chunk); !read)
                return read;
            if (auto read = read_journal_chunk(*reader, patch.replacement_file_offset + offset, replacement_chunk);
                !read) {
                return read;
            }
            if (auto read = target.read_exact_at(patch.target_offset + offset, current_chunk); !read)
                return std::unexpected(journal_error(read.error().message));
            for (std::size_t index = 0U; index < size; ++index) {
                if (current_chunk[index] != original_chunk[index] && current_chunk[index] != replacement_chunk[index]) {
                    return std::unexpected(journal_error("uncommitted journal target changed unexpectedly", true));
                }
            }
            offset += size;
        }
    }
    return {};
}

axk::app::Result<void> axk::app::journal_io::restore_original_bytes(SandboxMutation &target,
                                                                    const std::filesystem::path &journal_path,
                                                                    const Inspection &journal,
                                                                    std::size_t chunk_bytes) {
    auto reader = Reader::open(journal_path, std::numeric_limits<std::uint64_t>::max());
    if (!reader)
        return std::unexpected(reader.error());
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    std::vector<std::byte> original(maximum_chunk);
    for (const auto &patch : std::views::reverse(journal.patches)) {
        for (std::uint64_t offset = 0U; offset < patch.size;) {
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_chunk, patch.size - offset));
            auto chunk = std::span{original}.first(size);
            if (auto read = read_journal_chunk(*reader, patch.original_file_offset + offset, chunk); !read)
                return read;
            if (auto written = target.write_exact_at(patch.target_offset + offset, chunk); !written)
                return written;
            offset += size;
        }
    }
    return target.flush();
}
