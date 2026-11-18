#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "axklib/filesystems/view.hpp"

namespace axk {
class FatImage;
}

namespace axk::filesystems {

[[nodiscard]] Result<Root> open_fat(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                    const CancellationToken &cancellation = {});
[[nodiscard]] Result<Root> view_fat(std::shared_ptr<const FatImage> disk, const CancellationToken &cancellation = {});

// Files and native browsing use the same member-local physical entry IDs.
[[nodiscard]] std::string fat_root_id(std::size_t partition);
[[nodiscard]] std::string fat_entry_id(std::string_view root, bool directory, std::uint64_t directory_offset);
[[nodiscard]] Result<Root> browse_fat(std::shared_ptr<const FatImage> disk, std::size_t partition,
                                      const CancellationToken &cancellation = {});

} // namespace axk::filesystems
