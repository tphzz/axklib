#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "axklib/application/filesystem.hpp"

namespace axk::server::detail {

struct DownloadReadHooks {
    std::function<void()> after_open_before_read;
};

[[nodiscard]] app::Result<std::vector<std::byte>>
read_verified_download(const app::SandboxFile &file, std::uint64_t offset, std::uint64_t length,
                       std::shared_ptr<const DownloadReadHooks> hooks = {});

} // namespace axk::server::detail
