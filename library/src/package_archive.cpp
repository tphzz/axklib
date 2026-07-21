#include "axklib/package_archive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <utility>

#include "axklib/utf8.hpp"

namespace axk::package_internal {
namespace {

constexpr std::uint32_t local_signature = 0x04034b50U;
constexpr std::uint32_t central_signature = 0x02014b50U;
constexpr std::uint32_t end_signature = 0x06054b50U;
constexpr std::uint16_t zip_version = 20U;
constexpr std::uint16_t utf8_flag = 0x0800U;
constexpr std::uint16_t stored_method = 0U;
constexpr std::uint16_t fixed_time = 0U;
constexpr std::uint16_t fixed_date = 0x0021U;
constexpr std::size_t local_header_size = 30U;
constexpr std::size_t central_header_size = 46U;
constexpr std::size_t end_header_size = 22U;

constexpr std::array<std::uint32_t, 64> sha256_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

Error archive_error(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}

bool fits_u16(std::size_t value) { return value <= std::numeric_limits<std::uint16_t>::max(); }

bool fits_u32(std::uint64_t value) { return value <= std::numeric_limits<std::uint32_t>::max(); }

void append_u16(std::vector<std::byte> &target, std::uint16_t value) {
    target.push_back(static_cast<std::byte>(value));
    target.push_back(static_cast<std::byte>(value >> 8U));
}

void append_u32(std::vector<std::byte> &target, std::uint32_t value) {
    append_u16(target, static_cast<std::uint16_t>(value));
    append_u16(target, static_cast<std::uint16_t>(value >> 16U));
}

std::optional<std::uint16_t> read_u16(std::span<const std::byte> source, std::size_t offset) {
    if (offset > source.size() || 2U > source.size() - offset)
        return std::nullopt;
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[offset + 1U]) << 8U);
}

std::optional<std::uint32_t> read_u32(std::span<const std::byte> source, std::size_t offset) {
    if (offset > source.size() || 4U > source.size() - offset)
        return std::nullopt;
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[offset + 1U])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[offset + 2U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[offset + 3U])) << 24U);
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' || !text::is_valid_utf8(path) ||
        path.contains('\\') || path.contains('\0')) {
        return false;
    }
    std::size_t begin{};
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto component = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        begin = end + 1U;
    }
    return true;
}

Result<void> validate_entries(const std::vector<ArchiveEntry> &entries, const ArchiveLimits &limits) {
    if (limits.maximum_archive_bytes < end_header_size)
        return std::unexpected{archive_error("configured package archive limit is too small")};
    if (entries.empty())
        return std::unexpected{archive_error("package archive has no entries")};
    if (entries.size() > limits.maximum_entries || !fits_u16(entries.size()))
        return std::unexpected{archive_error("package archive entry count exceeds the configured limit")};
    if (entries.front().path != "manifest.json")
        return std::unexpected{archive_error("manifest.json must be the first package archive entry")};

    std::set<std::string, std::less<>> paths;
    std::uint64_t total{};
    std::uint64_t archive_size = end_header_size;
    std::uint64_t directory_size{};
    std::string previous;
    for (const auto &entry : entries) {
        if (!safe_archive_path(entry.path) || !fits_u16(entry.path.size()))
            return std::unexpected{archive_error("package archive contains an invalid entry path")};
        if (!previous.empty() && entry.path <= previous)
            return std::unexpected{archive_error("package archive entries are not in lexical path order")};
        previous = entry.path;
        if (!paths.emplace(entry.path).second)
            return std::unexpected{archive_error("package archive contains a duplicate entry path")};
        if (entry.bytes.size() > limits.maximum_entry_bytes || !fits_u32(entry.bytes.size()))
            return std::unexpected{archive_error("package archive entry exceeds the configured size limit")};
        if (entry.path == "manifest.json" && entry.bytes.size() > limits.maximum_manifest_bytes)
            return std::unexpected{archive_error("package manifest exceeds the configured size limit")};
        if (entry.bytes.size() > limits.maximum_total_bytes - total)
            return std::unexpected{archive_error("package archive expanded size exceeds the configured limit")};
        total += entry.bytes.size();
        const auto entry_archive_size = static_cast<std::uint64_t>(local_header_size) + central_header_size +
                                        entry.path.size() * 2U + entry.bytes.size();
        if (entry_archive_size > limits.maximum_archive_bytes - archive_size)
            return std::unexpected{archive_error("package archive exceeds the configured archive limit")};
        archive_size += entry_archive_size;
        const auto directory_entry_size = static_cast<std::uint64_t>(central_header_size) + entry.path.size();
        if (directory_entry_size > limits.maximum_directory_bytes - directory_size)
            return std::unexpected{archive_error("package archive central directory exceeds the configured limit")};
        directory_size += directory_entry_size;
    }
    return {};
}

void compress_sha256_block(std::array<std::uint32_t, 8> &state, const std::array<std::byte, 64> &block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
        const auto offset = index * 4U;
        words[index] = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset])) << 24U) |
                       (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 1U])) << 16U) |
                       (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 2U])) << 8U) |
                       static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 3U]));
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
        const auto low =
            std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
        const auto high =
            std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + low + words[index - 7U] + high;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto temporary1 = h + sum1 + choice + sha256_constants[index] + words[index];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

class Sha256State {
  public:
    void update(std::span<const std::byte> bytes) {
        total_bytes_ += bytes.size();
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), block_.size() - block_size_);
            std::ranges::copy(bytes.first(count), block_.begin() + static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += count;
            bytes = bytes.subspan(count);
            if (block_size_ == block_.size()) {
                compress_sha256_block(state_, block_);
                block_.fill(std::byte{});
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] Sha256Digest finish() {
        const auto bit_length = total_bytes_ * 8U;
        block_[block_size_++] = std::byte{0x80};
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), std::byte{});
            compress_sha256_block(state_, block_);
            block_.fill(std::byte{});
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, std::byte{});
        for (std::size_t index = 0; index < 8U; ++index)
            block_[63U - index] = static_cast<std::byte>(bit_length >> (index * 8U));
        compress_sha256_block(state_, block_);

        Sha256Digest result{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            result[index * 4U] = static_cast<std::byte>(state_[index] >> 24U);
            result[index * 4U + 1U] = static_cast<std::byte>(state_[index] >> 16U);
            result[index * 4U + 2U] = static_cast<std::byte>(state_[index] >> 8U);
            result[index * 4U + 3U] = static_cast<std::byte>(state_[index]);
        }
        return result;
    }

  private:
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, 64> block_{};
    std::size_t block_size_{};
    std::uint64_t total_bytes_{};
};

} // namespace

Sha256Digest sha256(std::span<const std::byte> bytes) {
    Sha256State state;
    state.update(bytes);
    return state.finish();
}

Result<Sha256Digest> sha256_reader(const RandomAccessReader &reader, const CancellationToken &cancellation) {
    constexpr std::size_t chunk_size = 1024U * 1024U;
    std::vector<std::byte> buffer(chunk_size);
    Sha256State state;
    for (std::uint64_t offset = 0U; offset < reader.size();) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        const auto chunk = std::span<std::byte>{buffer}.first(count);
        if (const auto read = reader.read_exact_at(offset, chunk); !read)
            return std::unexpected{read.error()};
        state.update(chunk);
        offset += count;
    }
    return state.finish();
}

std::string hex_digest(const Sha256Digest &digest) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(alphabet[value >> 4U]);
        result.push_back(alphabet[value & 0x0fU]);
    }
    return result;
}

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= std::to_integer<std::uint8_t>(byte);
        for (std::size_t bit = 0; bit < 8U; ++bit)
            result = (result >> 1U) ^ (0xedb88320U & (0U - (result & 1U)));
    }
    return ~result;
}

Result<std::vector<std::byte>> write_archive(std::vector<ArchiveEntry> entries, const ArchiveLimits &limits) {
    std::ranges::sort(entries, {}, &ArchiveEntry::path);
    if (const auto valid = validate_entries(entries, limits); !valid)
        return std::unexpected{valid.error()};

    struct CentralEntry {
        const ArchiveEntry *entry;
        std::uint32_t checksum;
        std::uint32_t local_offset;
    };
    std::vector<CentralEntry> central;
    central.reserve(entries.size());
    std::vector<std::byte> result;
    for (const auto &entry : entries) {
        if (!fits_u32(result.size()))
            return std::unexpected{archive_error("package archive exceeds the ZIP32 offset range")};
        const auto size = static_cast<std::uint32_t>(entry.bytes.size());
        const auto checksum = crc32(entry.bytes);
        central.push_back({&entry, checksum, static_cast<std::uint32_t>(result.size())});
        append_u32(result, local_signature);
        append_u16(result, zip_version);
        append_u16(result, utf8_flag);
        append_u16(result, stored_method);
        append_u16(result, fixed_time);
        append_u16(result, fixed_date);
        append_u32(result, checksum);
        append_u32(result, size);
        append_u32(result, size);
        append_u16(result, static_cast<std::uint16_t>(entry.path.size()));
        append_u16(result, 0U);
        const auto path_bytes = std::as_bytes(std::span{entry.path});
        result.insert(result.end(), path_bytes.begin(), path_bytes.end());
        result.insert(result.end(), entry.bytes.begin(), entry.bytes.end());
    }

    if (!fits_u32(result.size()))
        return std::unexpected{archive_error("package archive exceeds the ZIP32 offset range")};
    const auto central_offset = static_cast<std::uint32_t>(result.size());
    for (const auto &item : central) {
        const auto size = static_cast<std::uint32_t>(item.entry->bytes.size());
        append_u32(result, central_signature);
        append_u16(result, zip_version);
        append_u16(result, zip_version);
        append_u16(result, utf8_flag);
        append_u16(result, stored_method);
        append_u16(result, fixed_time);
        append_u16(result, fixed_date);
        append_u32(result, item.checksum);
        append_u32(result, size);
        append_u32(result, size);
        append_u16(result, static_cast<std::uint16_t>(item.entry->path.size()));
        append_u16(result, 0U);
        append_u16(result, 0U);
        append_u16(result, 0U);
        append_u16(result, 0U);
        append_u32(result, 0U);
        append_u32(result, item.local_offset);
        const auto path_bytes = std::as_bytes(std::span{item.entry->path});
        result.insert(result.end(), path_bytes.begin(), path_bytes.end());
    }
    const auto central_size = result.size() - central_offset;
    if (!fits_u32(central_size))
        return std::unexpected{archive_error("package archive central directory exceeds ZIP32")};
    append_u32(result, end_signature);
    append_u16(result, 0U);
    append_u16(result, 0U);
    append_u16(result, static_cast<std::uint16_t>(central.size()));
    append_u16(result, static_cast<std::uint16_t>(central.size()));
    append_u32(result, static_cast<std::uint32_t>(central_size));
    append_u32(result, central_offset);
    append_u16(result, 0U);
    if (result.size() > limits.maximum_archive_bytes)
        return std::unexpected{archive_error("package archive exceeds the configured archive limit")};
    return result;
}

Result<ArchiveInspection> inspect_archive(const RandomAccessReader &reader, const CancellationToken &cancellation,
                                          const ArchiveLimits &limits) {
    const auto read_bytes = [&](std::uint64_t offset, std::size_t size) -> Result<std::vector<std::byte>> {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (offset > reader.size() || size > reader.size() - offset)
            return std::unexpected{archive_error("package archive metadata is truncated")};
        std::vector<std::byte> result(size);
        if (const auto read = reader.read_exact_at(offset, result); !read)
            return std::unexpected{read.error()};
        return result;
    };

    const auto archive_size = reader.size();
    if (archive_size > limits.maximum_archive_bytes)
        return std::unexpected{archive_error("package archive exceeds the configured archive limit")};
    if (archive_size < end_header_size)
        return std::unexpected{archive_error("package archive is truncated before the end record")};
    const auto end_offset = archive_size - end_header_size;
    auto end = read_bytes(end_offset, end_header_size);
    if (!end)
        return std::unexpected{end.error()};
    const auto signature = read_u32(*end, 0U);
    const auto disk = read_u16(*end, 4U);
    const auto central_disk = read_u16(*end, 6U);
    const auto disk_entries = read_u16(*end, 8U);
    const auto total_entries = read_u16(*end, 10U);
    const auto central_size = read_u32(*end, 12U);
    const auto central_offset = read_u32(*end, 16U);
    const auto comment_size = read_u16(*end, 20U);
    if (!signature || !disk || !central_disk || !disk_entries || !total_entries || !central_size || !central_offset ||
        !comment_size || *signature != end_signature || *disk != 0U || *central_disk != 0U ||
        *disk_entries != *total_entries || *comment_size != 0U) {
        return std::unexpected{archive_error("package archive has an invalid ZIP end record")};
    }
    if (*total_entries == 0U || *total_entries > limits.maximum_entries)
        return std::unexpected{archive_error("package archive entry count exceeds the configured limit")};
    if (*central_size > limits.maximum_directory_bytes)
        return std::unexpected{archive_error("package archive central directory exceeds the configured limit")};
    if (static_cast<std::uint64_t>(*central_offset) + *central_size != end_offset)
        return std::unexpected{archive_error("package archive central directory bounds are invalid")};
    auto central = read_bytes(*central_offset, *central_size);
    if (!central)
        return std::unexpected{central.error()};

    struct DirectoryEntry {
        std::string path;
        std::uint32_t checksum{};
        std::uint32_t size{};
        std::uint32_t local_offset{};
    };
    std::vector<DirectoryEntry> directory;
    directory.reserve(*total_entries);
    std::size_t cursor{};
    for (std::size_t index = 0; index < *total_entries; ++index) {
        if (cursor > central->size() || central_header_size > central->size() - cursor)
            return std::unexpected{archive_error("package archive central directory is truncated")};
        const auto central_sig = read_u32(*central, cursor);
        const auto made_by = read_u16(*central, cursor + 4U);
        const auto needed = read_u16(*central, cursor + 6U);
        const auto flags = read_u16(*central, cursor + 8U);
        const auto method = read_u16(*central, cursor + 10U);
        const auto time = read_u16(*central, cursor + 12U);
        const auto date = read_u16(*central, cursor + 14U);
        const auto checksum = read_u32(*central, cursor + 16U);
        const auto compressed = read_u32(*central, cursor + 20U);
        const auto expanded = read_u32(*central, cursor + 24U);
        const auto name_size = read_u16(*central, cursor + 28U);
        const auto extra_size = read_u16(*central, cursor + 30U);
        const auto entry_comment = read_u16(*central, cursor + 32U);
        const auto start_disk = read_u16(*central, cursor + 34U);
        const auto internal_attributes = read_u16(*central, cursor + 36U);
        const auto external_attributes = read_u32(*central, cursor + 38U);
        const auto local_offset = read_u32(*central, cursor + 42U);
        if (!central_sig || !made_by || !needed || !flags || !method || !time || !date || !checksum || !compressed ||
            !expanded || !name_size || !extra_size || !entry_comment || !start_disk || !internal_attributes ||
            !external_attributes || !local_offset || *central_sig != central_signature || *made_by != zip_version ||
            *needed != zip_version || *flags != utf8_flag || *method != stored_method || *time != fixed_time ||
            *date != fixed_date || *compressed != *expanded || *name_size == 0U || *extra_size != 0U ||
            *entry_comment != 0U || *start_disk != 0U || *internal_attributes != 0U || *external_attributes != 0U) {
            return std::unexpected{archive_error("package archive central entry violates the profile")};
        }
        const auto record_size = central_header_size + *name_size;
        if (record_size > central->size() - cursor)
            return std::unexpected{archive_error("package archive central entry name is truncated")};
        const auto name_bytes = std::span{*central}.subspan(cursor + central_header_size, *name_size);
        std::string path(reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size());
        if (!safe_archive_path(path))
            return std::unexpected{archive_error("package archive contains an invalid entry path")};
        directory.push_back({std::move(path), *checksum, *expanded, *local_offset});
        cursor += record_size;
    }
    if (cursor != central->size())
        return std::unexpected{archive_error("package archive central directory contains trailing data")};

    ArchiveInspection result;
    result.archive_size = archive_size;
    result.entries.reserve(directory.size());
    std::uint64_t total{};
    std::uint64_t expected_local_offset{};
    std::string previous;
    for (const auto &item : directory) {
        if (!previous.empty() && item.path <= previous)
            return std::unexpected{archive_error("package archive entries are not in lexical path order")};
        previous = item.path;
        if (item.local_offset != expected_local_offset)
            return std::unexpected{archive_error("package archive local entries are not contiguous")};
        if (item.size > limits.maximum_entry_bytes || item.size > limits.maximum_total_bytes - total)
            return std::unexpected{archive_error("package archive expanded size exceeds the configured limit")};
        if (item.path == "manifest.json" && item.size > limits.maximum_manifest_bytes)
            return std::unexpected{archive_error("package manifest exceeds the configured size limit")};
        total += item.size;

        auto local = read_bytes(item.local_offset, local_header_size);
        if (!local)
            return std::unexpected{local.error()};
        const auto local_sig = read_u32(*local, 0U);
        const auto needed = read_u16(*local, 4U);
        const auto flags = read_u16(*local, 6U);
        const auto method = read_u16(*local, 8U);
        const auto time = read_u16(*local, 10U);
        const auto date = read_u16(*local, 12U);
        const auto checksum = read_u32(*local, 14U);
        const auto compressed = read_u32(*local, 18U);
        const auto expanded = read_u32(*local, 22U);
        const auto name_size = read_u16(*local, 26U);
        const auto extra_size = read_u16(*local, 28U);
        if (!local_sig || !needed || !flags || !method || !time || !date || !checksum || !compressed || !expanded ||
            !name_size || !extra_size || *local_sig != local_signature || *needed != zip_version ||
            *flags != utf8_flag || *method != stored_method || *time != fixed_time || *date != fixed_date ||
            *checksum != item.checksum || *compressed != item.size || *expanded != item.size ||
            *name_size != item.path.size() || *extra_size != 0U) {
            return std::unexpected{archive_error("package archive local entry disagrees with its directory")};
        }
        auto local_name = read_bytes(static_cast<std::uint64_t>(item.local_offset) + local_header_size, *name_size);
        if (!local_name)
            return std::unexpected{local_name.error()};
        const std::string_view local_name_text(reinterpret_cast<const char *>(local_name->data()), local_name->size());
        if (local_name_text != item.path)
            return std::unexpected{archive_error("package archive local entry name disagrees")};
        const auto data_offset =
            static_cast<std::uint64_t>(item.local_offset) + local_header_size + static_cast<std::uint64_t>(*name_size);
        if (data_offset > *central_offset || item.size > *central_offset - data_offset)
            return std::unexpected{archive_error("package archive entry data is truncated")};
        result.entries.push_back({item.path, item.checksum, item.size, data_offset});
        if (item.path == "manifest.json") {
            if (item.size > limits.maximum_manifest_bytes)
                return std::unexpected{archive_error("package manifest exceeds the configured size limit")};
            auto manifest = read_bytes(data_offset, item.size);
            if (!manifest)
                return std::unexpected{manifest.error()};
            if (crc32(*manifest) != item.checksum)
                return std::unexpected{archive_error("package manifest CRC-32 mismatch")};
            result.manifest = {item.path, std::move(*manifest)};
        }
        expected_local_offset = data_offset + item.size;
    }
    if (expected_local_offset != *central_offset)
        return std::unexpected{archive_error("package archive contains undeclared local data")};
    if (result.entries.empty() || result.entries.front().path != "manifest.json" ||
        result.manifest.path != "manifest.json") {
        return std::unexpected{archive_error("manifest.json must be the first package archive entry")};
    }
    return result;
}

Result<std::vector<ArchiveEntry>> read_archive(std::span<const std::byte> archive, const ArchiveLimits &limits) {
    if (archive.size() > limits.maximum_archive_bytes)
        return std::unexpected{archive_error("package archive exceeds the configured archive limit")};
    if (archive.size() < end_header_size)
        return std::unexpected{archive_error("package archive is truncated before the end record")};
    const auto end_offset = archive.size() - end_header_size;
    const auto signature = read_u32(archive, end_offset);
    const auto disk = read_u16(archive, end_offset + 4U);
    const auto central_disk = read_u16(archive, end_offset + 6U);
    const auto disk_entries = read_u16(archive, end_offset + 8U);
    const auto total_entries = read_u16(archive, end_offset + 10U);
    const auto central_size = read_u32(archive, end_offset + 12U);
    const auto central_offset = read_u32(archive, end_offset + 16U);
    const auto comment_size = read_u16(archive, end_offset + 20U);
    if (!signature || !disk || !central_disk || !disk_entries || !total_entries || !central_size || !central_offset ||
        !comment_size || *signature != end_signature || *disk != 0U || *central_disk != 0U ||
        *disk_entries != *total_entries || *comment_size != 0U) {
        return std::unexpected{archive_error("package archive has an invalid ZIP end record")};
    }
    if (*total_entries == 0U || *total_entries > limits.maximum_entries)
        return std::unexpected{archive_error("package archive entry count exceeds the configured limit")};
    if (*central_size > limits.maximum_directory_bytes)
        return std::unexpected{archive_error("package archive central directory exceeds the configured limit")};
    if (static_cast<std::uint64_t>(*central_offset) + *central_size != end_offset)
        return std::unexpected{archive_error("package archive central directory bounds are invalid")};

    struct DirectoryEntry {
        std::string path;
        std::uint32_t checksum;
        std::uint32_t size;
        std::uint32_t local_offset;
    };
    std::vector<DirectoryEntry> directory;
    directory.reserve(*total_entries);
    std::size_t cursor = *central_offset;
    for (std::size_t index = 0; index < *total_entries; ++index) {
        if (cursor > end_offset || central_header_size > end_offset - cursor)
            return std::unexpected{archive_error("package archive central directory is truncated")};
        const auto central_sig = read_u32(archive, cursor);
        const auto made_by = read_u16(archive, cursor + 4U);
        const auto needed = read_u16(archive, cursor + 6U);
        const auto flags = read_u16(archive, cursor + 8U);
        const auto method = read_u16(archive, cursor + 10U);
        const auto time = read_u16(archive, cursor + 12U);
        const auto date = read_u16(archive, cursor + 14U);
        const auto checksum = read_u32(archive, cursor + 16U);
        const auto compressed = read_u32(archive, cursor + 20U);
        const auto expanded = read_u32(archive, cursor + 24U);
        const auto name_size = read_u16(archive, cursor + 28U);
        const auto extra_size = read_u16(archive, cursor + 30U);
        const auto entry_comment = read_u16(archive, cursor + 32U);
        const auto start_disk = read_u16(archive, cursor + 34U);
        const auto internal_attributes = read_u16(archive, cursor + 36U);
        const auto external_attributes = read_u32(archive, cursor + 38U);
        const auto local_offset = read_u32(archive, cursor + 42U);
        if (!central_sig || !made_by || !needed || !flags || !method || !time || !date || !checksum || !compressed ||
            !expanded || !name_size || !extra_size || !entry_comment || !start_disk || !internal_attributes ||
            !external_attributes || !local_offset || *central_sig != central_signature || *made_by != zip_version ||
            *needed != zip_version || *flags != utf8_flag || *method != stored_method || *time != fixed_time ||
            *date != fixed_date || *compressed != *expanded || *name_size == 0U || *extra_size != 0U ||
            *entry_comment != 0U || *start_disk != 0U || *internal_attributes != 0U || *external_attributes != 0U) {
            return std::unexpected{archive_error("package archive central entry violates the profile")};
        }
        const auto record_size = central_header_size + *name_size;
        if (record_size > end_offset - cursor)
            return std::unexpected{archive_error("package archive central entry name is truncated")};
        const auto name_bytes = archive.subspan(cursor + central_header_size, *name_size);
        std::string path(reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size());
        if (!safe_archive_path(path))
            return std::unexpected{archive_error("package archive contains an invalid entry path")};
        directory.push_back({std::move(path), *checksum, *expanded, *local_offset});
        cursor += record_size;
    }
    if (cursor != end_offset)
        return std::unexpected{archive_error("package archive central directory contains trailing data")};

    std::vector<ArchiveEntry> entries;
    entries.reserve(directory.size());
    std::uint64_t total{};
    std::size_t expected_local_offset{};
    std::string previous;
    for (const auto &item : directory) {
        if (!previous.empty() && item.path <= previous)
            return std::unexpected{archive_error("package archive entries are not in lexical path order")};
        previous = item.path;
        if (item.local_offset != expected_local_offset)
            return std::unexpected{archive_error("package archive local entries are not contiguous")};
        if (item.size > limits.maximum_entry_bytes || item.size > limits.maximum_total_bytes - total)
            return std::unexpected{archive_error("package archive expanded size exceeds the configured limit")};
        if (item.path == "manifest.json" && item.size > limits.maximum_manifest_bytes)
            return std::unexpected{archive_error("package manifest exceeds the configured size limit")};
        total += item.size;
        const auto local = static_cast<std::size_t>(item.local_offset);
        if (local > *central_offset || local_header_size > *central_offset - local)
            return std::unexpected{archive_error("package archive local entry is truncated")};
        const auto local_sig = read_u32(archive, local);
        const auto needed = read_u16(archive, local + 4U);
        const auto flags = read_u16(archive, local + 6U);
        const auto method = read_u16(archive, local + 8U);
        const auto time = read_u16(archive, local + 10U);
        const auto date = read_u16(archive, local + 12U);
        const auto checksum = read_u32(archive, local + 14U);
        const auto compressed = read_u32(archive, local + 18U);
        const auto expanded = read_u32(archive, local + 22U);
        const auto name_size = read_u16(archive, local + 26U);
        const auto extra_size = read_u16(archive, local + 28U);
        if (!local_sig || !needed || !flags || !method || !time || !date || !checksum || !compressed || !expanded ||
            !name_size || !extra_size || *local_sig != local_signature || *needed != zip_version ||
            *flags != utf8_flag || *method != stored_method || *time != fixed_time || *date != fixed_date ||
            *checksum != item.checksum || *compressed != item.size || *expanded != item.size ||
            *name_size != item.path.size() || *extra_size != 0U) {
            return std::unexpected{archive_error("package archive local entry disagrees with its directory")};
        }
        const auto data_offset = local + local_header_size + *name_size;
        if (data_offset > *central_offset || item.size > *central_offset - data_offset)
            return std::unexpected{archive_error("package archive entry data is truncated")};
        const auto name_bytes = archive.subspan(local + local_header_size, *name_size);
        const std::string_view local_name(reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size());
        if (local_name != item.path)
            return std::unexpected{archive_error("package archive local entry name disagrees")};
        const auto data = archive.subspan(data_offset, item.size);
        if (crc32(data) != item.checksum)
            return std::unexpected{archive_error("package archive entry CRC-32 mismatch")};
        entries.push_back({item.path, std::vector<std::byte>{data.begin(), data.end()}});
        expected_local_offset = data_offset + item.size;
    }
    if (expected_local_offset != *central_offset)
        return std::unexpected{archive_error("package archive contains undeclared local data")};
    if (entries.front().path != "manifest.json")
        return std::unexpected{archive_error("manifest.json must be the first package archive entry")};
    return entries;
}

} // namespace axk::package_internal
