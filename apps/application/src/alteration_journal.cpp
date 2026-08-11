#include "axklib/application/alteration_journal.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

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

#include "alteration_journal_io.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "private_storage.hpp"

namespace {

constexpr std::size_t maximum_journal_count = 128U;

axk::app::Error journal_error(std::string message, bool retryable = false) {
    return {"alteration_journal_unavailable", std::move(message), {}, retryable};
}

axk::app::Result<axk::PublicationOutcome> publish_file(const std::filesystem::path &path,
                                                       std::span<const std::byte> bytes) {
    auto publication = axk::detail::TemporaryPublication::create(
        path, [&](const axk::detail::TemporaryFileSink &sink) { return sink(bytes); });
    if (!publication)
        return std::unexpected(journal_error(publication.error().message));
    auto published = publication->publish(axk::detail::PublicationMode::create_only);
    if (!published)
        return std::unexpected(journal_error(published.error().message));
    return std::move(*published);
}

axk::app::Result<void> compare_patch(const axk::app::SandboxMutation &target,
                                     const axk::app::AlterationJournalPatch &patch, bool replacement,
                                     std::size_t chunk_bytes) {
    const auto &expected = replacement ? patch.replacement : patch.original;
    const auto maximum_chunk = std::max<std::size_t>(chunk_bytes, 1U);
    std::vector<std::byte> current(std::min(maximum_chunk, expected.size()));
    for (std::size_t offset = 0U; offset < expected.size();) {
        const auto size = std::min(maximum_chunk, expected.size() - offset);
        auto current_chunk = std::span{current}.first(size);
        if (auto read = target.read_exact_at(patch.offset + offset, current_chunk); !read)
            return std::unexpected(journal_error(read.error().message));
        if (!std::ranges::equal(current_chunk, std::span{expected}.subspan(offset, size)))
            return std::unexpected(journal_error("alteration target does not match its journal", true));
        offset += size;
    }
    return {};
}

std::filesystem::path commit_marker_path(const std::filesystem::path &path) {
    auto marker = path;
    marker += ".commit";
    return marker;
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

} // namespace

axk::app::AlterationJournalStore::AlterationJournalStore(std::filesystem::path directory,
                                                         std::uint64_t maximum_journal_bytes,
                                                         InterruptionHook interruption_hook,
                                                         std::size_t maximum_patch_write_bytes)
    : directory_(std::move(directory)), maximum_journal_bytes_(std::max<std::uint64_t>(maximum_journal_bytes, 1U)),
      interruption_hook_(std::move(interruption_hook)),
      maximum_patch_write_bytes_(std::max<std::size_t>(maximum_patch_write_bytes, 1U)) {
    const auto available = detail::prepare_private_directory(directory_).has_value();
    storage_available_.store(available, std::memory_order_relaxed);
    storage_ready_.store(available, std::memory_order_relaxed);
}

bool axk::app::AlterationJournalStore::storage_ready() const noexcept {
    return storage_ready_.load(std::memory_order_relaxed);
}

axk::app::Result<void> axk::app::AlterationJournalStore::apply(const std::shared_ptr<SandboxMutation> &target,
                                                               std::uint64_t image_size_bytes,
                                                               std::span<const AlterationJournalPatch> patches,
                                                               const CancellationToken &cancellation,
                                                               const std::function<Result<void>()> &validate) {
    if (!storage_ready())
        return std::unexpected(journal_error("alteration journal storage is not ready"));
    if (!target || target->size() != image_size_bytes)
        return std::unexpected(journal_error("alteration target size changed"));
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", checked.error().message});
    if (auto bound = target->verify_bound(); !bound)
        return std::unexpected(bound.error());
    for (const auto &patch : patches) {
        if (auto compared = compare_patch(*target, patch, false, maximum_patch_write_bytes_); !compared)
            return compared;
    }

    const auto target_identity = target->stable_identity();
    if (target_identity.empty())
        return std::unexpected(journal_error("alteration target identity is unavailable"));
    auto identifier = secure_random_hex(16U);
    if (!identifier)
        return std::unexpected(identifier.error());
    const auto path = directory_ / ("alteration-" + *identifier + ".axkjournal");
    const auto quarantine = [&] { storage_ready_.store(false, std::memory_order_relaxed); };
    auto journal_publication =
        journal_io::publish(path, target->reference(), target_identity, image_size_bytes, patches,
                            maximum_journal_bytes_, cancellation, maximum_patch_write_bytes_);
    if (!journal_publication)
        return std::unexpected{journal_publication.error()};
    if (journal_publication->durability != axk::PublicationDurability::confirmed) {
        quarantine();
        return std::unexpected(journal_error("alteration journal durability could not be confirmed", true));
    }
    const auto rollback = [&]() -> Result<void> {
        for (const auto &patch : std::views::reverse(patches)) {
            if (auto written = target->write_exact_at(patch.offset, patch.original); !written)
                return written;
        }
        return target->flush();
    };
    const auto rollback_and_remove = [&](bool remove_marker) -> Result<void> {
        if (auto restored = rollback(); !restored) {
            quarantine();
            return restored;
        }
        if (auto removed = remove_file(path, "alteration journal"); !removed) {
            quarantine();
            return removed;
        }
        if (!remove_marker)
            return {};
        const auto marker = commit_marker_path(path);
        std::error_code error;
        const auto exists = std::filesystem::exists(marker, error);
        if (error) {
            quarantine();
            return std::unexpected(journal_error("alteration commit marker cannot be inspected", true));
        }
        if (exists) {
            if (auto removed = remove_file(marker, "alteration commit marker"); !removed) {
                quarantine();
                return removed;
            }
        }
        return {};
    };
    std::size_t write_chunk_index{};
    for (std::size_t index = 0U; index < patches.size(); ++index) {
        const auto &patch = patches[index];
        std::size_t replacement_offset{};
        while (replacement_offset < patch.replacement.size()) {
            const auto chunk_size = std::min(maximum_patch_write_bytes_, patch.replacement.size() - replacement_offset);
            const auto chunk = std::span{patch.replacement}.subspan(replacement_offset, chunk_size);
            if (auto written = target->write_exact_at(patch.offset + replacement_offset, chunk); !written) {
                if (auto recovered = rollback_and_remove(false); !recovered)
                    return recovered;
                return written;
            }
            replacement_offset += chunk_size;
            if (interruption_hook_ && interruption_hook_("after-patch-chunk", write_chunk_index++)) {
                if (auto flushed = target->flush(); !flushed) {
                    quarantine();
                    return flushed;
                }
                quarantine();
                return std::unexpected(journal_error("simulated alteration interruption", true));
            }
        }
        if (interruption_hook_ && interruption_hook_("after-patch", index)) {
            if (auto flushed = target->flush(); !flushed) {
                quarantine();
                return flushed;
            }
            quarantine();
            return std::unexpected(journal_error("simulated alteration interruption", true));
        }
    }
    if (auto flushed = target->flush(); !flushed) {
        if (auto recovered = rollback_and_remove(false); !recovered)
            return recovered;
        return flushed;
    }
    if (auto bound = target->verify_bound(); !bound) {
        if (auto recovered = rollback_and_remove(false); !recovered)
            return recovered;
        return bound;
    }
    for (const auto &patch : patches) {
        if (auto compared = compare_patch(*target, patch, true, maximum_patch_write_bytes_); !compared) {
            if (auto recovered = rollback_and_remove(false); !recovered)
                return recovered;
            return compared;
        }
    }
    if (validate) {
        if (auto validated = validate(); !validated) {
            if (auto recovered = rollback_and_remove(false); !recovered)
                return recovered;
            return validated;
        }
        if (auto bound = target->verify_bound(); !bound) {
            if (auto recovered = rollback_and_remove(false); !recovered)
                return recovered;
            return bound;
        }
    }
    const auto marker = commit_marker_path(path);
    const auto &journal_checksum = journal_publication->file_checksum;
    auto marker_publication =
        publish_file(marker, std::as_bytes(std::span{journal_checksum.data(), journal_checksum.size()}));
    if (!marker_publication || marker_publication->durability != axk::PublicationDurability::confirmed) {
        if (auto recovered = rollback_and_remove(true); !recovered)
            return recovered;
        if (!marker_publication)
            return std::unexpected{marker_publication.error()};
        return std::unexpected(journal_error("alteration commit marker durability could not be confirmed", true));
    }
    if (interruption_hook_ && interruption_hook_("after-commit-marker", patches.size())) {
        quarantine();
        return {};
    }
    if (auto removed = remove_completed_journal(path); !removed) {
        quarantine();
        return {};
    }
    return {};
}

axk::app::Result<void> axk::app::AlterationJournalStore::recover(const Sandbox &sandbox) {
    if (!storage_available_.load(std::memory_order_relaxed))
        return std::unexpected(journal_error("alteration journal storage is not ready"));
    storage_ready_.store(false, std::memory_order_relaxed);
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
        auto journal = journal_io::inspect(path, maximum_journal_bytes_, maximum_patch_write_bytes_);
        if (!journal)
            return std::unexpected(journal.error());
        auto committed = journal_io::commit_marker_matches(path, journal->file_checksum);
        if (!committed)
            return std::unexpected(committed.error());
        auto target = sandbox.open_mutation(journal->target);
        if (!target)
            return std::unexpected(target.error());
        if ((*target)->size() != journal->image_size_bytes)
            return std::unexpected(journal_error("journal target size changed", true));
        if ((*target)->stable_identity() != journal->target_identity)
            return std::unexpected(journal_error("journal target identity changed", true));
        if (*committed) {
            if (auto compared = journal_io::compare_target(**target, path, *journal, true, maximum_patch_write_bytes_);
                !compared)
                return compared;
        } else {
            if (auto recognized =
                    journal_io::recognize_uncommitted_target(**target, path, *journal, maximum_patch_write_bytes_);
                !recognized)
                return recognized;
            if (auto restored =
                    journal_io::restore_original_bytes(**target, path, *journal, maximum_patch_write_bytes_);
                !restored)
                return restored;
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
    storage_ready_.store(true, std::memory_order_relaxed);
    return {};
}
