#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "rawdisk/block_device.hpp"

namespace rawdisk {

[[nodiscard]] Result<std::filesystem::path> carrier_lease_directory() noexcept;
[[nodiscard]] Result<std::shared_ptr<BlockDevice>>
open_owned_device(const DeviceInfo &info, Access access,
                  std::shared_ptr<AuthorizationSession> authorization = {}) noexcept;

// Every process accessing the same carrier must use the same private lease directory.
// This coordinates application instances; it does not replace OS device/volume claims.
[[nodiscard]] Result<std::shared_ptr<void>> claim_carrier(const std::filesystem::path &directory,
                                                          std::string_view carrier_id) noexcept;

} // namespace rawdisk
