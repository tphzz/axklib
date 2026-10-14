#pragma once

#include <memory>
#include <string>

#include "axklib/filesystems/view.hpp"

namespace axk {
class FatImage;
}

namespace axk::filesystems {

[[nodiscard]] Result<Root> open_fat(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                    const CancellationToken &cancellation = {});
[[nodiscard]] Result<Root> view_fat(std::shared_ptr<const FatImage> disk, const CancellationToken &cancellation = {});

} // namespace axk::filesystems
