#include "axklib/application/alteration_journal.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

#include <hash-library/sha256.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "private_storage.hpp"

namespace {

constexpr std::array<std::byte, 8> journal_magic{
    std::byte{'A'}, std::byte{'X'}, std::byte{'K'}, std::byte{'J'},
    std::byte{'N'}, std::byte{'L'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::byte prepared_state{0U};
constexpr std::size_t checksum_size = 64U;
constexpr std::size_t maximum_journal_count = 128U;

axk::app::Error journal_error(std::string message, bool retryable = false) {
    return {"alteration_journal_unavailable", std::move(message), {}, retryable};
}

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
}

void append_u64(std::vector<std::byte> &bytes, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index)
        bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
}

axk::app::Result<std::uint32_t> read_u32(std::span<const std::byte> bytes, std::size_t &offset) {
    if (offset > bytes.size() || 4U > bytes.size() - offset)
        return std::unexpected(journal_error("alteration journal is truncated"));
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index)
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    offset += 4U;
    return value;
}

axk::app::Result<std::uint64_t> read_u64(std::span<const std::byte> bytes, std::size_t &offset) {
    if (offset > bytes.size() || 8U > bytes.size() - offset)
        return std::unexpected(journal_error("alteration journal is truncated"));
    std::uint64_t value{};
    for (std::size_t index = 0U; index < 8U; ++index)
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    offset += 8U;
    return value;
}

std::string checksum(std::span<const std::byte> bytes) {
    SHA256 hash;
    hash.add(bytes.data(), bytes.size());
    return hash.getHash();
}

struct Journal {
    std::byte state{prepared_state};
    axk::app::FileRef target;
    std::uint64_t image_size_bytes{};
    std::vector<axk::app::AlterationJournalPatch> patches;
};

axk::app::Result<std::vector<std::byte>> encode_journal(const Journal &journal, std::size_t maximum_bytes) {
    if (journal.target.root_id.size() > std::numeric_limits<std::uint32_t>::max() ||
        journal.target.relative_path.size() > std::numeric_limits<std::uint32_t>::max() ||
        journal.patches.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(journal_error("alteration journal metadata exceeds supported limits"));
    }
    std::vector<std::byte> bytes;
    bytes.reserve(std::min<std::size_t>(maximum_bytes, 1024U * 1024U));
    bytes.insert(bytes.end(), journal_magic.begin(), journal_magic.end());
    bytes.push_back(journal.state);
    bytes.insert(bytes.end(), 7U, std::byte{0U});
    append_u32(bytes, static_cast<std::uint32_t>(journal.target.root_id.size()));
    append_u32(bytes, static_cast<std::uint32_t>(journal.target.relative_path.size()));
    append_u64(bytes, journal.image_size_bytes);
    append_u32(bytes, static_cast<std::uint32_t>(journal.patches.size()));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(journal.target.root_id.data()),
                 reinterpret_cast<const std::byte *>(journal.target.root_id.data() + journal.target.root_id.size()));
    bytes.insert(
        bytes.end(), reinterpret_cast<const std::byte *>(journal.target.relative_path.data()),
        reinterpret_cast<const std::byte *>(journal.target.relative_path.data() + journal.target.relative_path.size()));
    for (const auto &patch : journal.patches) {
        if (patch.original.size() != patch.replacement.size())
            return std::unexpected(journal_error("alteration patch changes the image size"));
        append_u64(bytes, patch.offset);
        append_u64(bytes, patch.original.size());
        if (bytes.size() > maximum_bytes || patch.original.size() > maximum_bytes - bytes.size())
            return std::unexpected(journal_error("alteration journal exceeds its configured byte limit"));
        bytes.insert(bytes.end(), patch.original.begin(), patch.original.end());
        if (bytes.size() > maximum_bytes || patch.replacement.size() > maximum_bytes - bytes.size())
            return std::unexpected(journal_error("alteration journal exceeds its configured byte limit"));
        bytes.insert(bytes.end(), patch.replacement.begin(), patch.replacement.end());
    }
    const auto digest = checksum(bytes);
    if (bytes.size() > maximum_bytes || digest.size() > maximum_bytes - bytes.size())
        return std::unexpected(journal_error("alteration journal exceeds its configured byte limit"));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(digest.data()),
                 reinterpret_cast<const std::byte *>(digest.data() + digest.size()));
    return bytes;
}

axk::app::Result<Journal> decode_journal(std::span<const std::byte> bytes, std::size_t maximum_bytes) {
    if (bytes.size() > maximum_bytes || bytes.size() < 36U + checksum_size ||
        !std::ranges::equal(journal_magic, bytes.first(journal_magic.size()))) {
        return std::unexpected(journal_error("alteration journal has an invalid header"));
    }
    const auto payload = bytes.first(bytes.size() - checksum_size);
    const std::string stored_checksum{reinterpret_cast<const char *>(bytes.data() + payload.size()), checksum_size};
    if (checksum(payload) != stored_checksum)
        return std::unexpected(journal_error("alteration journal checksum does not match"));
    Journal journal;
    journal.state = bytes[journal_magic.size()];
    if (journal.state != prepared_state)
        return std::unexpected(journal_error("alteration journal has an invalid state"));
    std::size_t offset = 16U;
    const auto root_size = read_u32(payload, offset);
    const auto path_size = read_u32(payload, offset);
    const auto image_size = read_u64(payload, offset);
    const auto patch_count = read_u32(payload, offset);
    if (!root_size || !path_size || !image_size || !patch_count)
        return std::unexpected(root_size ? path_size ? image_size ? patch_count.error() : image_size.error()
                                                     : path_size.error()
                                         : root_size.error());
    if (offset > payload.size() || *root_size > payload.size() - offset)
        return std::unexpected(journal_error("alteration journal root is truncated"));
    journal.target.root_id.assign(reinterpret_cast<const char *>(payload.data() + offset), *root_size);
    offset += *root_size;
    if (offset > payload.size() || *path_size > payload.size() - offset)
        return std::unexpected(journal_error("alteration journal path is truncated"));
    journal.target.relative_path.assign(reinterpret_cast<const char *>(payload.data() + offset), *path_size);
    offset += *path_size;
    journal.image_size_bytes = *image_size;
    journal.patches.reserve(*patch_count);
    for (std::uint32_t index = 0U; index < *patch_count; ++index) {
        const auto patch_offset = read_u64(payload, offset);
        const auto patch_size = read_u64(payload, offset);
        if (!patch_offset || !patch_size || *patch_size > std::numeric_limits<std::size_t>::max())
            return std::unexpected(patch_offset ? patch_size.error() : patch_offset.error());
        const auto size = static_cast<std::size_t>(*patch_size);
        if (offset > payload.size() || size > payload.size() - offset)
            return std::unexpected(journal_error("alteration journal original bytes are truncated"));
        std::vector<std::byte> original(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                        payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        if (offset > payload.size() || size > payload.size() - offset)
            return std::unexpected(journal_error("alteration journal replacement bytes are truncated"));
        std::vector<std::byte> replacement(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                           payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        journal.patches.push_back({*patch_offset, std::move(original), std::move(replacement)});
    }
    if (offset != payload.size())
        return std::unexpected(journal_error("alteration journal contains trailing payload bytes"));
    return journal;
}

axk::app::Result<void> write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(journal_error("alteration journal cannot be opened"));
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output)
        return std::unexpected(journal_error("alteration journal cannot be written"));
    const auto flushed = axk::detail::flush_file_to_disk(path);
    if (!flushed)
        return std::unexpected(journal_error(flushed.error().message));
    return {};
}

axk::app::Result<void> write_text_file(const std::filesystem::path &path, std::string_view value) {
    return write_file(path, std::as_bytes(std::span{value}));
}

axk::app::Result<std::vector<std::byte>> read_file(const std::filesystem::path &path, std::size_t maximum_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_bytes || size > std::numeric_limits<std::size_t>::max())
        return std::unexpected(journal_error("alteration journal size is invalid"));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input{path, std::ios::binary};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input)
        return std::unexpected(journal_error("alteration journal cannot be read"));
    return bytes;
}

axk::app::Result<void> compare_patch(const axk::app::SandboxMutation &target,
                                     const axk::app::AlterationJournalPatch &patch, bool replacement) {
    std::vector<std::byte> current(patch.original.size());
    if (auto read = target.read_exact_at(patch.offset, current); !read)
        return std::unexpected(journal_error(read.error().message));
    const auto &expected = replacement ? patch.replacement : patch.original;
    if (!std::ranges::equal(current, expected))
        return std::unexpected(journal_error("alteration target does not match its journal", true));
    return {};
}

std::filesystem::path commit_marker_path(const std::filesystem::path &path) {
    auto marker = path;
    marker += ".commit";
    return marker;
}

axk::app::Result<void> publish_journal(const std::filesystem::path &temporary, const std::filesystem::path &path) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) == 0)
        return std::unexpected(journal_error("alteration journal cannot be committed"));
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0)
        return std::unexpected(journal_error("alteration journal cannot be committed"));
    const auto directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory < 0)
        return std::unexpected(journal_error("alteration journal directory cannot be synchronized"));
    const auto synchronized = ::fsync(directory);
    const auto sync_error = errno;
    const auto closed = ::close(directory);
    if (synchronized != 0 || closed != 0) {
        errno = sync_error;
        return std::unexpected(journal_error("alteration journal directory cannot be synchronized"));
    }
#endif
    return {};
}

axk::app::Result<void> remove_file(const std::filesystem::path &path, std::string_view description) {
    std::error_code error;
    if (!std::filesystem::remove(path, error) || error)
        return std::unexpected(journal_error(std::string{description} + " cannot be removed", true));
#if !defined(_WIN32)
    const auto directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory < 0)
        return std::unexpected(journal_error(std::string{description} + " directory cannot be synchronized", true));
    const auto synchronized = ::fsync(directory);
    const auto closed = ::close(directory);
    if (synchronized != 0 || closed != 0)
        return std::unexpected(journal_error(std::string{description} + " directory cannot be synchronized", true));
#endif
    return {};
}

axk::app::Result<void> remove_completed_journal(const std::filesystem::path &path) {
    const auto marker = commit_marker_path(path);
    if (auto removed = remove_file(path, "completed alteration journal"); !removed)
        return removed;
    return remove_file(marker, "alteration commit marker");
}

axk::app::Result<bool> is_committed(const std::filesystem::path &path, std::span<const std::byte> journal_bytes) {
    const auto marker = commit_marker_path(path);
    std::error_code error;
    if (!std::filesystem::exists(marker, error))
        return error ? std::unexpected(journal_error("alteration commit marker cannot be inspected"))
                     : axk::app::Result<bool>{false};
    auto marker_bytes = read_file(marker, checksum_size);
    if (!marker_bytes || marker_bytes->size() != checksum_size)
        return false;
    const std::string stored{reinterpret_cast<const char *>(marker_bytes->data()), marker_bytes->size()};
    return stored == checksum(journal_bytes);
}

} // namespace

axk::app::AlterationJournalStore::AlterationJournalStore(std::filesystem::path directory,
                                                         std::size_t maximum_journal_bytes,
                                                         InterruptionHook interruption_hook)
    : directory_(std::move(directory)), maximum_journal_bytes_(std::max<std::size_t>(maximum_journal_bytes, 1U)),
      interruption_hook_(std::move(interruption_hook)) {
    storage_ready_.store(detail::prepare_private_directory(directory_).has_value(), std::memory_order_relaxed);
}

bool axk::app::AlterationJournalStore::storage_ready() const noexcept {
    return storage_ready_.load(std::memory_order_relaxed);
}

axk::app::Result<void> axk::app::AlterationJournalStore::apply(const std::shared_ptr<SandboxMutation> &target,
                                                               std::uint64_t image_size_bytes,
                                                               std::span<const AlterationJournalPatch> patches,
                                                               const CancellationToken &cancellation) {
    if (!storage_ready())
        return std::unexpected(journal_error("alteration journal storage is not ready"));
    if (!target || target->size() != image_size_bytes)
        return std::unexpected(journal_error("alteration target size changed"));
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", checked.error().message});
    if (auto bound = target->verify_bound(); !bound)
        return std::unexpected(bound.error());
    for (const auto &patch : patches) {
        if (auto compared = compare_patch(*target, patch, false); !compared)
            return compared;
    }

    Journal journal{prepared_state, target->reference(), image_size_bytes, {patches.begin(), patches.end()}};
    auto encoded = encode_journal(journal, maximum_journal_bytes_);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto identifier = secure_random_hex(16U);
    if (!identifier)
        return std::unexpected(identifier.error());
    const auto temporary = directory_ / ("alteration-" + *identifier + ".tmp");
    const auto path = directory_ / ("alteration-" + *identifier + ".axkjournal");
    if (auto created = detail::create_private_file(temporary); !created)
        return std::unexpected(created.error());
    if (auto written = write_file(temporary, *encoded); !written) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return written;
    }
    if (auto published = publish_journal(temporary, path); !published) {
        std::error_code filesystem_error;
        std::filesystem::remove(temporary, filesystem_error);
        return published;
    }

    const auto rollback = [&]() -> Result<void> {
        for (const auto &patch : std::views::reverse(patches)) {
            if (auto written = target->write_exact_at(patch.offset, patch.original); !written)
                return written;
        }
        return target->flush();
    };
    for (std::size_t index = 0U; index < patches.size(); ++index) {
        const auto &patch = patches[index];
        if (auto written = target->write_exact_at(patch.offset, patch.replacement); !written) {
            if (auto restored = rollback(); restored)
                static_cast<void>(remove_file(path, "alteration journal"));
            return written;
        }
        if (interruption_hook_ && interruption_hook_("after-patch", index)) {
            if (auto flushed = target->flush(); !flushed)
                return flushed;
            return std::unexpected(journal_error("simulated alteration interruption", true));
        }
    }
    if (auto flushed = target->flush(); !flushed) {
        if (auto restored = rollback(); restored)
            static_cast<void>(remove_file(path, "alteration journal"));
        return flushed;
    }
    if (auto bound = target->verify_bound(); !bound) {
        if (auto restored = rollback(); restored)
            static_cast<void>(remove_file(path, "alteration journal"));
        return bound;
    }
    for (const auto &patch : patches) {
        if (auto compared = compare_patch(*target, patch, true); !compared) {
            if (auto restored = rollback(); restored)
                static_cast<void>(remove_file(path, "alteration journal"));
            return compared;
        }
    }
    const auto marker = commit_marker_path(path);
    if (auto created = detail::create_private_file(marker); !created) {
        if (auto restored = rollback(); !restored)
            return restored;
        static_cast<void>(remove_file(path, "alteration journal"));
        return std::unexpected(created.error());
    }
    const auto journal_checksum = checksum(*encoded);
    if (auto written = write_text_file(marker, journal_checksum); !written) {
        if (auto restored = rollback(); !restored)
            return restored;
        static_cast<void>(remove_file(path, "alteration journal"));
        static_cast<void>(remove_file(marker, "alteration commit marker"));
        return written;
    }
    if (interruption_hook_ && interruption_hook_("after-commit-marker", patches.size()))
        return std::unexpected(journal_error("simulated alteration interruption", true));
    if (auto removed = remove_completed_journal(path); !removed)
        storage_ready_.store(false, std::memory_order_relaxed);
    return {};
}

axk::app::Result<void> axk::app::AlterationJournalStore::recover(const Sandbox &sandbox) {
    if (!storage_ready())
        return std::unexpected(journal_error("alteration journal storage is not ready"));
    std::vector<std::filesystem::path> journals;
    std::vector<std::filesystem::path> markers;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{directory_, error}, end; !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == ".axkjournal")
            journals.push_back(iterator->path());
        const auto filename = iterator->path().filename();
        if (iterator->is_regular_file(error) && filename.extension() == ".commit" &&
            filename.stem().extension() == ".axkjournal")
            markers.push_back(iterator->path());
        if (journals.size() + markers.size() > maximum_journal_count)
            return std::unexpected(journal_error("too many alteration journals require recovery"));
    }
    if (error)
        return std::unexpected(journal_error("alteration journal directory cannot be enumerated"));
    std::ranges::sort(journals);
    for (const auto &path : journals) {
        auto bytes = read_file(path, maximum_journal_bytes_);
        if (!bytes)
            return std::unexpected(bytes.error());
        auto journal = decode_journal(*bytes, maximum_journal_bytes_);
        if (!journal)
            return std::unexpected(journal.error());
        auto committed = is_committed(path, *bytes);
        if (!committed)
            return std::unexpected(committed.error());
        auto target = sandbox.open_mutation(journal->target);
        if (!target)
            return std::unexpected(target.error());
        if ((*target)->size() != journal->image_size_bytes)
            return std::unexpected(journal_error("journal target size changed", true));
        if (*committed) {
            for (const auto &patch : journal->patches) {
                if (auto compared = compare_patch(**target, patch, true); !compared)
                    return compared;
            }
        } else {
            for (const auto &patch : journal->patches) {
                std::vector<std::byte> current(patch.original.size());
                if (auto read = (*target)->read_exact_at(patch.offset, current); !read)
                    return std::unexpected(journal_error(read.error().message));
                if (!std::ranges::equal(current, patch.original) && !std::ranges::equal(current, patch.replacement)) {
                    return std::unexpected(journal_error("uncommitted journal target changed unexpectedly", true));
                }
            }
            for (const auto &patch : std::views::reverse(journal->patches)) {
                if (auto written = (*target)->write_exact_at(patch.offset, patch.original); !written)
                    return written;
            }
            if (auto flushed = (*target)->flush(); !flushed)
                return flushed;
        }
        if (*committed) {
            if (auto removed = remove_completed_journal(path); !removed)
                return removed;
        } else {
            if (auto removed = remove_file(path, "alteration journal"); !removed)
                return removed;
            const auto marker = commit_marker_path(path);
            std::error_code marker_error;
            if (std::filesystem::exists(marker, marker_error)) {
                if (marker_error)
                    return std::unexpected(journal_error("alteration commit marker cannot be inspected"));
                if (auto removed = remove_file(marker, "alteration commit marker"); !removed)
                    return removed;
            }
        }
    }
    for (const auto &marker : markers) {
        std::error_code marker_error;
        if (!std::filesystem::exists(marker, marker_error)) {
            if (marker_error)
                return std::unexpected(journal_error("alteration commit marker cannot be inspected"));
            continue;
        }
        auto journal_path = marker;
        journal_path.replace_filename(marker.stem());
        std::error_code journal_error_code;
        const auto journal_exists = std::filesystem::exists(journal_path, journal_error_code);
        if (journal_error_code)
            return std::unexpected(journal_error("alteration journal cannot be inspected"));
        if (!journal_exists) {
            if (auto removed = remove_file(marker, "orphan alteration commit marker"); !removed)
                return removed;
        }
    }
    return {};
}
