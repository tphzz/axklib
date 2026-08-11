#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "axklib/application/filesystem.hpp"

namespace axk::app {

inline constexpr std::uint64_t default_maximum_alteration_journal_bytes =
    2ULL * 2'147'483'648ULL + 64ULL * 1024ULL * 1024ULL;

struct AlterationJournalPatch {
    std::uint64_t offset{};
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
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
                                     const std::function<Result<void>()> &validate = {});

  private:
    std::filesystem::path directory_;
    std::uint64_t maximum_journal_bytes_;
    InterruptionHook interruption_hook_;
    std::size_t maximum_patch_write_bytes_;
    std::atomic_bool storage_available_{};
    std::atomic_bool storage_ready_{};
};

} // namespace axk::app
