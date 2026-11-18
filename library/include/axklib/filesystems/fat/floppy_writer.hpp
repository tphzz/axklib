#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "axklib/io.hpp"

namespace axk::filesystems {

enum class FloppyLayout : std::uint8_t { dd_720k, hd_1440k };

struct FloppyCapacity {
    std::size_t disk_bytes{}, cluster_bytes{}, data_clusters{}, root_entries{};
};
[[nodiscard]] Result<FloppyCapacity> floppy_capacity(FloppyLayout layout);

struct FloppyFile {
    std::string name;
    std::variant<std::span<const std::byte>, std::reference_wrapper<const RandomAccessReader>> payload;
};

// Borrow the payloads for this call; directory order is the input order.
[[nodiscard]] Result<std::vector<std::byte>> build_floppy(FloppyLayout layout, std::span<const FloppyFile> files,
                                                          const CancellationToken &cancellation = {});

} // namespace axk::filesystems
