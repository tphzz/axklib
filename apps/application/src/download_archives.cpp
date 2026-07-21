#include "axklib/application/download_archives.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <mutex>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "axklib/application/secure_random.hpp"
#include "axklib/utf8.hpp"
#include "private_storage.hpp"

namespace {

constexpr std::size_t tar_block_size = 512U;

axk::app::Error archive_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

struct SourceEntry {
    std::string path;
    axk::app::SandboxTreeEntryKind kind{axk::app::SandboxTreeEntryKind::file};
    std::uint64_t size{};
    std::size_t source_index{};
};

bool split_tar_path(std::string_view path, std::string_view &name, std::string_view &prefix) {
    if (path.empty() || path.size() > 255U)
        return false;
    if (path.size() <= 100U) {
        name = path;
        prefix = {};
        return true;
    }
    auto split = path.rfind('/');
    while (split != std::string_view::npos) {
        if (split <= 155U && path.size() - split - 1U <= 100U) {
            prefix = path.substr(0U, split);
            name = path.substr(split + 1U);
            return !name.empty();
        }
        if (split == 0U)
            break;
        split = path.rfind('/', split - 1U);
    }
    return false;
}

bool write_octal(std::span<char> target, std::uint64_t value) {
    if (target.size() < 2U)
        return false;
    std::ranges::fill(target, '0');
    target.back() = '\0';
    auto first = target.data();
    auto last = target.data() + static_cast<std::ptrdiff_t>(target.size() - 1U);
    const auto converted = std::to_chars(first, last, value, 8);
    if (converted.ec != std::errc{})
        return false;
    const auto digits = static_cast<std::size_t>(converted.ptr - first);
    const auto width = target.size() - 1U;
    if (digits > width)
        return false;
    std::move_backward(first, converted.ptr, target.data() + static_cast<std::ptrdiff_t>(width));
    std::fill(first, first + static_cast<std::ptrdiff_t>(width - digits), '0');
    return true;
}

axk::app::Result<std::array<char, tar_block_size>> tar_header(const SourceEntry &entry) {
    std::string_view name;
    std::string_view prefix;
    if (!split_tar_path(entry.path, name, prefix))
        return std::unexpected(
            archive_error("archive_path_too_long", "directory entry does not fit the TAR path profile"));
    std::array<char, tar_block_size> header{};
    std::ranges::copy(name, header.begin());
    const auto directory = entry.kind == axk::app::SandboxTreeEntryKind::directory;
    if (!write_octal(std::span{header}.subspan(100U, 8U), directory ? 0755U : 0644U) ||
        !write_octal(std::span{header}.subspan(108U, 8U), 0U) ||
        !write_octal(std::span{header}.subspan(116U, 8U), 0U) ||
        !write_octal(std::span{header}.subspan(124U, 12U), directory ? 0U : entry.size) ||
        !write_octal(std::span{header}.subspan(136U, 12U), 0U)) {
        return std::unexpected(archive_error("archive_too_large", "directory entry does not fit the TAR size profile"));
    }
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = directory ? '5' : '0';
    constexpr std::string_view magic{"ustar\0", 6U};
    std::ranges::copy(magic, header.begin() + 257);
    header[263] = '0';
    header[264] = '0';
    std::ranges::copy(prefix, header.begin() + 345);
    std::uint64_t checksum{};
    for (const auto byte : header)
        checksum += static_cast<unsigned char>(byte);
    auto checksum_field = std::span{header}.subspan(148U, 8U);
    if (!write_octal(checksum_field.first(7U), checksum))
        return std::unexpected(archive_error("archive_failed", "TAR checksum cannot be represented"));
    checksum_field[6] = '\0';
    checksum_field[7] = ' ';
    return header;
}

std::uint64_t padded_size(std::uint64_t size) { return (size + tar_block_size - 1U) / tar_block_size * tar_block_size; }

std::string archive_filename(const axk::app::DirectoryRef &source) {
    const auto path = axk::text::path_from_utf8(source.relative_path);
    auto stem = path ? axk::text::path_to_utf8(path->filename()) : std::string{};
    if (stem.empty() || stem == ".")
        stem = source.root_id;
    return stem + ".tar";
}

} // namespace

struct axk::app::DownloadArchiveStore::Implementation {
    struct Entry {
        std::string owner_id;
        std::string filename;
        std::filesystem::path path;
        std::uint64_t size_bytes{};
        std::size_t entry_count{};
        std::chrono::steady_clock::time_point expires_at;
        std::size_t active_downloads{};
    };

    std::filesystem::path staging_directory;
    std::uint64_t maximum_total_bytes{};
    std::uint64_t maximum_archive_bytes{};
    std::size_t maximum_entries{};
    std::size_t maximum_depth{};
    std::size_t maximum_path_bytes{};
    std::chrono::seconds retention{};
    Clock clock;
    std::uint64_t reserved_bytes{};
    bool storage_ready{};
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;

    Implementation(std::filesystem::path directory, DownloadArchiveLimits limits,
                   std::chrono::seconds archive_retention, Clock archive_clock)
        : staging_directory(std::move(directory)), maximum_total_bytes(limits.maximum_total_bytes),
          maximum_archive_bytes(limits.maximum_archive_bytes), maximum_entries(limits.maximum_entries),
          maximum_depth(limits.maximum_depth), maximum_path_bytes(limits.maximum_path_bytes),
          retention(archive_retention), clock(std::move(archive_clock)) {}

    void cleanup_locked() {
        const auto now = clock();
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (iterator->second.expires_at > now || iterator->second.active_downloads != 0U) {
                ++iterator;
                continue;
            }
            std::error_code error;
            std::filesystem::remove(iterator->second.path, error);
            if (error) {
                ++iterator;
                continue;
            }
            reserved_bytes -= iterator->second.size_bytes;
            iterator = entries.erase(iterator);
        }
    }

    [[nodiscard]] DownloadArchiveSnapshot snapshot(std::string_view id, const Entry &entry) const {
        const auto remaining =
            entry.expires_at > clock()
                ? std::chrono::duration_cast<std::chrono::seconds>(entry.expires_at - clock()).count()
                : 0;
        return {{std::string{id}},
                entry.filename,
                entry.size_bytes,
                entry.entry_count,
                static_cast<std::uint64_t>(remaining)};
    }

    [[nodiscard]] Result<std::unordered_map<std::string, Entry>::iterator> owned(const DownloadArchiveRef &reference,
                                                                                 std::string_view owner_id) {
        cleanup_locked();
        const auto found = entries.find(reference.archive_id);
        if (found == entries.end() || found->second.owner_id != owner_id)
            return std::unexpected(archive_error("download_archive_not_found", "download archive does not exist"));
        return found;
    }
};

axk::app::DownloadArchiveStore::DownloadArchiveStore(std::filesystem::path staging_directory,
                                                     std::uint64_t maximum_total_bytes,
                                                     std::uint64_t maximum_archive_bytes, std::size_t maximum_entries,
                                                     std::chrono::seconds retention, Clock clock)
    : DownloadArchiveStore(std::move(staging_directory), {maximum_total_bytes, maximum_archive_bytes, maximum_entries},
                           retention, std::move(clock)) {}

axk::app::DownloadArchiveStore::DownloadArchiveStore(std::filesystem::path staging_directory,
                                                     DownloadArchiveLimits limits, std::chrono::seconds retention,
                                                     Clock clock)
    : implementation_(
          std::make_shared<Implementation>(std::move(staging_directory), limits, retention, std::move(clock))) {
    std::error_code error;
    const auto prepared = detail::prepare_private_directory(implementation_->staging_directory);
    implementation_->storage_ready = prepared.has_value();
    if (prepared) {
        for (const auto &entry : std::filesystem::directory_iterator{implementation_->staging_directory, error}) {
            if (error)
                break;
            if (entry.is_regular_file(error) &&
                (entry.path().extension() == ".tar" || entry.path().extension() == ".part"))
                std::filesystem::remove(entry.path(), error);
            if (error)
                break;
        }
    }
}

axk::app::DownloadArchiveStore::~DownloadArchiveStore() = default;
axk::app::DownloadArchiveStore::DownloadArchiveStore(DownloadArchiveStore &&) noexcept = default;
axk::app::DownloadArchiveStore &axk::app::DownloadArchiveStore::operator=(DownloadArchiveStore &&) noexcept = default;

bool axk::app::DownloadArchiveStore::storage_ready() const noexcept {
    return implementation_ != nullptr && implementation_->storage_ready;
}

axk::app::Result<axk::app::DownloadArchiveSnapshot>
axk::app::DownloadArchiveStore::create(std::string owner_id, const Sandbox &sandbox, const DirectoryRef &source) {
    if (owner_id.empty())
        return std::unexpected(archive_error("invalid_archive_request", "download archive owner is required"));
    if (!implementation_->storage_ready)
        return std::unexpected(
            archive_error("archive_storage_unavailable", "download archive staging directory is not private"));
    std::vector<SourceEntry> entries;
    std::uint64_t archive_size = tar_block_size * 2U;
    auto tree = sandbox.open_tree(source, {implementation_->maximum_entries, implementation_->maximum_archive_bytes,
                                           implementation_->maximum_depth, implementation_->maximum_path_bytes});
    if (!tree)
        return std::unexpected(tree.error());
    for (std::size_t index = 0U; index < tree->entries().size(); ++index) {
        const auto &entry = tree->entries()[index];
        std::string_view name;
        std::string_view prefix;
        if (!split_tar_path(entry.relative_path, name, prefix))
            return std::unexpected(
                archive_error("archive_path_too_long", "directory entry does not fit the TAR path profile"));
        const auto payload_size = entry.kind == axk::app::SandboxTreeEntryKind::file ? padded_size(entry.size) : 0U;
        if (payload_size > std::numeric_limits<std::uint64_t>::max() - archive_size - tar_block_size)
            return std::unexpected(archive_error("archive_too_large", "directory archive size overflows"));
        archive_size += tar_block_size + payload_size;
        entries.push_back({entry.relative_path, entry.kind, entry.size, index});
        if (entries.size() > implementation_->maximum_entries ||
            archive_size > implementation_->maximum_archive_bytes) {
            return std::unexpected(
                archive_error("download_archive_too_large", "directory archive exceeds configured limits"));
        }
    }
    std::ranges::sort(entries, {}, &SourceEntry::path);

    std::string id;
    std::filesystem::path final_path;
    {
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->cleanup_locked();
        if (archive_size > implementation_->maximum_total_bytes - implementation_->reserved_bytes)
            return std::unexpected(
                archive_error("download_archive_quota_exceeded", "download archive staging quota is exhausted", true));
        do {
            auto generated = secure_random_hex(32U);
            if (!generated)
                return std::unexpected(archive_error("archive_storage_unavailable", generated.error().message));
            id = std::move(*generated);
        } while (implementation_->entries.contains(id));
        final_path = implementation_->staging_directory / (id + ".tar");
        implementation_->reserved_bytes += archive_size;
    }

    const auto temporary_path = implementation_->staging_directory / (id + ".part");
    auto release_reservation = [&] {
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->reserved_bytes -= archive_size;
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    };
    if (const auto created = detail::create_private_file(temporary_path); !created) {
        release_reservation();
        return std::unexpected(archive_error("archive_storage_unavailable", created.error().message));
    }
    std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        release_reservation();
        return std::unexpected(archive_error("archive_storage_unavailable", "download archive cannot be created"));
    }
    std::vector<std::byte> buffer(1024U * 1024U);
    for (const auto &entry : entries) {
        const auto header = tar_header(entry);
        if (!header) {
            output.close();
            release_reservation();
            return std::unexpected(header.error());
        }
        output.write(header->data(), static_cast<std::streamsize>(header->size()));
        if (entry.kind == axk::app::SandboxTreeEntryKind::directory)
            continue;
        auto file = tree->open_file(entry.source_index);
        if (!file) {
            output.close();
            release_reservation();
            return std::unexpected(file.error());
        }
        std::uint64_t remaining = entry.size;
        std::uint64_t offset{};
        while (remaining != 0U) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            const auto read = file->reader->read_exact_at(offset, std::span{buffer}.first(count));
            if (!read) {
                output.close();
                release_reservation();
                return std::unexpected(
                    archive_error("archive_source_changed", "directory entry could not be archived"));
            }
            output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
            if (!output) {
                output.close();
                release_reservation();
                return std::unexpected(
                    archive_error("archive_storage_unavailable", "directory archive could not be written"));
            }
            remaining -= count;
            offset += count;
        }
        const std::array<char, tar_block_size> padding{};
        if (const auto unchanged = file->verify_unchanged(); !unchanged) {
            output.close();
            release_reservation();
            return std::unexpected(unchanged.error());
        }
        const auto padding_size = padded_size(entry.size) - entry.size;
        output.write(padding.data(), static_cast<std::streamsize>(padding_size));
    }
    const std::array<char, tar_block_size * 2U> end_blocks{};
    output.write(end_blocks.data(), static_cast<std::streamsize>(end_blocks.size()));
    output.close();
    if (!output) {
        release_reservation();
        return std::unexpected(archive_error("archive_storage_unavailable", "download archive could not be finalized"));
    }
    std::error_code error;
    std::filesystem::rename(temporary_path, final_path, error);
    if (error) {
        release_reservation();
        return std::unexpected(archive_error("archive_storage_unavailable", "download archive could not be published"));
    }

    const std::scoped_lock lock{implementation_->mutex};
    auto [position, inserted] = implementation_->entries.emplace(
        id, Implementation::Entry{owner_id, archive_filename(source), final_path, archive_size, entries.size(),
                                  implementation_->clock() + implementation_->retention, 0U});
    if (!inserted) {
        implementation_->reserved_bytes -= archive_size;
        std::filesystem::remove(final_path, error);
        return std::unexpected(archive_error("archive_storage_unavailable", "download archive ID collision"));
    }
    return implementation_->snapshot(position->first, position->second);
}

axk::app::Result<axk::app::DownloadArchiveSnapshot>
axk::app::DownloadArchiveStore::inspect(const DownloadArchiveRef &reference, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    return implementation_->snapshot(reference.archive_id, (**found).second);
}

axk::app::Result<axk::app::DownloadArchiveContent>
axk::app::DownloadArchiveStore::open(const DownloadArchiveRef &reference, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    (**found).second.expires_at = implementation_->clock() + implementation_->retention;
    auto reader = axk::FileReader::open((**found).second.path);
    if (!reader)
        return std::unexpected(archive_error("archive_storage_unavailable", reader.error().message));
    ++(**found).second.active_downloads;
    const auto archive_id = reference.archive_id;
    const auto implementation = implementation_;
    auto lease = std::shared_ptr<void>{implementation_.get(), [implementation, archive_id](void *) {
                                           const std::scoped_lock release_lock{implementation->mutex};
                                           if (const auto retained = implementation->entries.find(archive_id);
                                               retained != implementation->entries.end() &&
                                               retained->second.active_downloads != 0U) {
                                               --retained->second.active_downloads;
                                           }
                                       }};
    return DownloadArchiveContent{implementation_->snapshot(reference.archive_id, (**found).second),
                                  (**found).second.path, std::move(*reader), std::move(lease)};
}

axk::app::Result<void> axk::app::DownloadArchiveStore::remove(const DownloadArchiveRef &reference,
                                                              std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    if ((**found).second.active_downloads != 0U)
        return std::unexpected(archive_error("archive_in_use", "download archive is being transferred", true));
    std::error_code error;
    std::filesystem::remove((**found).second.path, error);
    if (error)
        return std::unexpected(archive_error("archive_storage_unavailable", "download archive cannot be removed"));
    implementation_->reserved_bytes -= (**found).second.size_bytes;
    implementation_->entries.erase(*found);
    return {};
}

void axk::app::DownloadArchiveStore::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}
