#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "axklib/filesystem_edit.hpp"
#include "axklib/filesystem_transaction.hpp"
#include "axklib/sfs.hpp"

namespace axk::sfs_files {

struct Record {
    IndexRecord info;
    std::array<std::byte, 72> raw{};
    std::vector<std::byte> directory;
    bool changed{};
    bool directory_changed{};
    bool directory_renamed{};
    bool payload_changed{};
    bool deleted{};
};

struct State {
    std::shared_ptr<const RandomAccessReader> source;
    Partition partition;
    std::uint32_t sector_bytes{};
    std::uint32_t cluster_bytes{};
    SfsId root;
    std::vector<std::byte> bitmap;
    std::map<std::uint32_t, Record> records;
    std::set<std::uint32_t> occupied_slots;
    std::set<std::uint32_t> protected_records;
    std::vector<detail::FilesystemWritePatch> patches;
    CancellationToken cancellation;

    [[nodiscard]] std::uint64_t cluster_offset(std::uint32_t cluster) const;
    [[nodiscard]] Result<void> load_directory(Record &record);
    [[nodiscard]] Result<void> replace_payload(Record &record, std::shared_ptr<const RandomAccessReader> contents);
    [[nodiscard]] Result<std::vector<Extent>> allocate(std::uint32_t bytes, std::uint32_t attributes);
    [[nodiscard]] Result<void> release(Record &record);
    [[nodiscard]] Result<void> change_links(Record &record, int delta);
    [[nodiscard]] Result<void> encode(Record &record);
    [[nodiscard]] Result<Record *> create(bool directory);
    [[nodiscard]] Result<void> apply(const FilesystemEdit &edit);
    [[nodiscard]] Result<void> remove(Record &parent, std::size_t offset, bool recursive);
    [[nodiscard]] Result<void> finish();
};

[[nodiscard]] Error error(std::string message, ErrorCode code = ErrorCode::transaction_rejected);
[[nodiscard]] Result<void> check_path(const FilesystemPath &path);
[[nodiscard]] Result<State> open(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                                 const CancellationToken &cancellation);
void bytes_patch(State &state, std::uint64_t offset, std::vector<std::byte> bytes);
[[nodiscard]] Result<void> validate(const Container &container, PartitionIndex partition);

using PrepareEdits = std::function<Result<detail::PreparedFilesystemEdits>(std::shared_ptr<const RandomAccessReader>)>;
[[nodiscard]] Result<PublicationOutcome> publish(const std::filesystem::path &source,
                                                 const std::filesystem::path &destination, PartitionIndex partition,
                                                 std::span<const std::shared_ptr<const RandomAccessReader>> inputs,
                                                 const PrepareEdits &prepare, const CancellationToken &cancellation,
                                                 ProgressSink *progress);

} // namespace axk::sfs_files
