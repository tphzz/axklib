#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "axklib/application/contracts.hpp"
#include "axklib/application/filesystem.hpp"

namespace axk::app {

enum class UploadKind : std::uint8_t { audio, package, manifest };
enum class UploadState : std::uint8_t { receiving, ready };

struct UploadCreateRequest {
    std::string owner_id;
    std::string filename;
    UploadKind kind{UploadKind::audio};
    std::string media_type;
    std::uint64_t declared_size{};
    std::optional<std::string> sha256;
};

struct UploadSnapshot {
    UploadRef reference;
    std::string filename;
    UploadKind kind{UploadKind::audio};
    std::string media_type;
    std::uint64_t declared_size{};
    std::uint64_t received_size{};
    UploadState state{UploadState::receiving};
    std::uint64_t expires_in_seconds{};
};

struct UploadCleanupSnapshot {
    bool healthy{true};
    std::uint64_t failed_deletions{};
    std::uint64_t orphan_count{};
    std::uint64_t orphan_bytes{};
    std::uint64_t reserved_bytes{};
};

class UploadLease {
  public:
    UploadLease() = default;
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    friend class UploadStore;
    std::filesystem::path path_;
    std::shared_ptr<void> guard_;
};

class UploadStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using RemoveFile = std::function<bool(const std::filesystem::path &, std::error_code &)>;

    UploadStore(std::filesystem::path staging_directory, std::uint64_t maximum_total_bytes,
                std::uint64_t maximum_upload_bytes, std::size_t maximum_uploads, std::size_t maximum_chunk_bytes,
                std::chrono::seconds retention, Clock clock = std::chrono::steady_clock::now,
                RemoveFile remove_file = {});
    ~UploadStore();
    UploadStore(UploadStore &&) noexcept;
    UploadStore &operator=(UploadStore &&) noexcept;
    UploadStore(const UploadStore &) = delete;
    UploadStore &operator=(const UploadStore &) = delete;

    [[nodiscard]] Result<UploadSnapshot> create(UploadCreateRequest request);
    [[nodiscard]] Result<UploadSnapshot> append(const UploadRef &reference, std::string_view owner_id,
                                                std::uint64_t offset, std::span<const std::byte> bytes);
    [[nodiscard]] Result<UploadSnapshot> complete(const UploadRef &reference, std::string_view owner_id);
    [[nodiscard]] Result<UploadSnapshot> inspect(const UploadRef &reference, std::string_view owner_id);
    [[nodiscard]] Result<std::filesystem::path> resolve(const UploadRef &reference, std::string_view owner_id);
    [[nodiscard]] Result<UploadLease> lease(const UploadRef &reference, std::string_view owner_id);
    [[nodiscard]] Result<FileRef> materialize(const UploadRef &reference, std::string_view owner_id,
                                              const Sandbox &sandbox, const FileRef &destination, bool overwrite);
    [[nodiscard]] Result<void> remove(const UploadRef &reference, std::string_view owner_id);
    void cleanup();
    [[nodiscard]] UploadCleanupSnapshot cleanup_snapshot();

    [[nodiscard]] std::size_t maximum_chunk_bytes() const noexcept;

  private:
    struct Implementation;
    std::shared_ptr<Implementation> implementation_;
};

[[nodiscard]] std::string_view upload_kind_name(UploadKind kind) noexcept;
[[nodiscard]] std::string_view upload_state_name(UploadState state) noexcept;

} // namespace axk::app
