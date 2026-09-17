#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/application/filesystem.hpp"
#include "axklib/io.hpp"

namespace axk::app {

inline constexpr std::uint64_t default_maximum_alteration_journal_bytes =
    2ULL * 2'147'483'648ULL + 64ULL * 1024ULL * 1024ULL;

// Owned small metadata or a borrowed range of an immutable, shared input.
// Reader-backed bytes are frozen in the journal before the target is modified.
class AlterationJournalBytes {
  public:
    AlterationJournalBytes() : AlterationJournalBytes(std::vector<std::byte>{}) {}
    AlterationJournalBytes(std::initializer_list<std::byte> bytes)
        : AlterationJournalBytes(std::vector<std::byte>{bytes}) {}
    AlterationJournalBytes(std::vector<std::byte> bytes)
        : reader_(std::make_shared<MemoryReader>(std::move(bytes))), size_(reader_->size()) {}
    AlterationJournalBytes(std::shared_ptr<const RandomAccessReader> reader, std::uint64_t offset, std::uint64_t size)
        : reader_(std::move(reader)), offset_(offset), size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const {
        if (!reader_ || offset_ > reader_->size() || size_ > reader_->size() - offset_ || offset > size_ ||
            bytes.size() > size_ - offset)
            return std::unexpected(Error{"alteration_journal_unavailable", "alteration input range is invalid"});
        if (auto read = reader_->read_exact_at(offset_ + offset, bytes); !read)
            return std::unexpected(Error{"alteration_journal_unavailable", read.error().message});
        return {};
    }

  private:
    std::shared_ptr<const RandomAccessReader> reader_;
    std::uint64_t offset_{};
    std::uint64_t size_{};
};

struct AlterationJournalPatch {
    std::uint64_t offset{};
    AlterationJournalBytes original;
    AlterationJournalBytes replacement;
};

class AlterationJournalStore {
  public:
    using InterruptionHook = std::function<bool(std::string_view, std::size_t)>;

    explicit AlterationJournalStore(std::filesystem::path directory,
                                    std::uint64_t maximum_journal_bytes = default_maximum_alteration_journal_bytes,
                                    InterruptionHook interruption_hook = {},
                                    std::size_t maximum_patch_write_bytes = 1024U * 1024U);

    [[nodiscard]] bool storage_ready() const noexcept;
    [[nodiscard]] Result<void> recover(const Sandbox &sandbox);
    [[nodiscard]] Result<void> apply(const std::shared_ptr<SandboxMutation> &target, std::uint64_t image_size_bytes,
                                     std::span<const AlterationJournalPatch> patches,
                                     const CancellationToken &cancellation = {},
                                     const std::function<Result<void>()> &validate = {},
                                     const std::function<void()> &on_rollback_verified = {});

  private:
    std::filesystem::path directory_;
    std::uint64_t maximum_journal_bytes_;
    InterruptionHook interruption_hook_;
    std::size_t maximum_patch_write_bytes_;
    std::atomic_bool storage_available_{};
    std::atomic_bool storage_ready_{};
};

} // namespace axk::app
