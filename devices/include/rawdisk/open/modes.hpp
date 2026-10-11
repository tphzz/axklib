// Built-in modes and the registry.
//
//   id                  steps
//   image               image (path, sector size)
//   raw                 device
//   scsi2sd-v6          device -> scsi2sd (auto) -> volume
//   scsi2sd-v5          device -> xml (file) -> volume
//   scsi2sd-v6-image    image (path) -> scsi2sd (auto) -> volume
//   scsi2sd-xml-image   image (path) -> xml (file, board) -> volume
#pragma once

#include "rawdisk/open/flow.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace rawdisk::open {

class Registry {
  public:
    // Errc::invalid_argument if the id is already registered or empty.
    Result<void> add(std::shared_ptr<const Mode> mode);
    std::shared_ptr<const Mode> find(std::string_view id) const noexcept;
    const std::vector<std::shared_ptr<const Mode>> &list() const noexcept { return modes_; }

  private:
    std::vector<std::shared_ptr<const Mode>> modes_;
};

struct SelectionLimits {
    std::optional<std::uint64_t> max_device_bytes{}; // raw mode only; layout carriers are not targets
    std::optional<std::uint64_t> max_volume_bytes{}; // physical SCSI2SD modes only
};

// No limits by default. Image-file modes are never filtered.
Registry builtin_modes(SelectionLimits limits = {});

} // namespace rawdisk::open
