#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/publication.hpp"

namespace axk::app::journal_io {

struct PatchLocation {
    std::uint64_t target_offset{};
    std::uint64_t size{};
    std::uint64_t original_file_offset{};
    std::uint64_t replacement_file_offset{};
};

struct Inspection {
    FileRef target;
    std::string target_identity;
    std::uint64_t image_size_bytes{};
    std::vector<PatchLocation> patches;
    std::string file_checksum;
};

struct Publication {
    PublicationDurability durability{PublicationDurability::unconfirmed};
    std::string file_checksum;
    std::uint64_t size_bytes{};
};

[[nodiscard]] Result<std::uint64_t> encoded_size(const FileRef &target, std::string_view target_identity,
                                                 std::span<const AlterationJournalPatch> patches);

[[nodiscard]] Result<Publication> publish(const std::filesystem::path &path, const FileRef &target,
                                          std::string_view target_identity, std::uint64_t image_size_bytes,
                                          std::span<const AlterationJournalPatch> patches,
                                          std::uint64_t maximum_journal_bytes, const CancellationToken &cancellation,
                                          std::size_t chunk_bytes);

[[nodiscard]] Result<Inspection> inspect(const std::filesystem::path &path, std::uint64_t maximum_journal_bytes,
                                         std::size_t chunk_bytes);

[[nodiscard]] Result<bool> commit_marker_matches(const std::filesystem::path &journal_path,
                                                 std::string_view file_checksum);

[[nodiscard]] Result<void> compare_target(const SandboxMutation &target, const std::filesystem::path &journal_path,
                                          const Inspection &journal, bool replacement, std::size_t chunk_bytes);

[[nodiscard]] Result<void> recognize_uncommitted_target(const SandboxMutation &target,
                                                        const std::filesystem::path &journal_path,
                                                        const Inspection &journal, std::size_t chunk_bytes);

[[nodiscard]] Result<void> restore_original_bytes(SandboxMutation &target, const std::filesystem::path &journal_path,
                                                  const Inspection &journal, std::size_t chunk_bytes);

} // namespace axk::app::journal_io
