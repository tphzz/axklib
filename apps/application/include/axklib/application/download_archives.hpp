#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "axklib/application/filesystem.hpp"

namespace axk::app {

struct DownloadArchiveRef {
    std::string archive_id;
};

struct DownloadArchiveSnapshot {
    DownloadArchiveRef reference;
    std::string filename;
    std::uint64_t size_bytes{};
    std::size_t entry_count{};
    std::uint64_t expires_in_seconds{};
};

struct DownloadArchiveContent {
    DownloadArchiveSnapshot snapshot;
    std::shared_ptr<const axk::RandomAccessReader> reader;
};

class DownloadArchiveStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    DownloadArchiveStore(std::filesystem::path staging_directory, std::uint64_t maximum_total_bytes,
                         std::uint64_t maximum_archive_bytes, std::size_t maximum_entries,
                         std::chrono::seconds retention, Clock clock = std::chrono::steady_clock::now);
    ~DownloadArchiveStore();
    DownloadArchiveStore(DownloadArchiveStore &&) noexcept;
    DownloadArchiveStore &operator=(DownloadArchiveStore &&) noexcept;
    DownloadArchiveStore(const DownloadArchiveStore &) = delete;
    DownloadArchiveStore &operator=(const DownloadArchiveStore &) = delete;

    [[nodiscard]] Result<DownloadArchiveSnapshot> create(std::string owner_id, const Sandbox &sandbox,
                                                         const DirectoryRef &source);
    [[nodiscard]] Result<DownloadArchiveSnapshot> inspect(const DownloadArchiveRef &reference,
                                                          std::string_view owner_id);
    [[nodiscard]] Result<DownloadArchiveContent> open(const DownloadArchiveRef &reference, std::string_view owner_id);
    [[nodiscard]] Result<void> remove(const DownloadArchiveRef &reference, std::string_view owner_id);
    void cleanup();

  private:
    struct Implementation;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace axk::app
