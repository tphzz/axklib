#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"

namespace axk::cli::detail {

struct ContentId {
    std::string algorithm;
    std::string digest_hex;
};

using ContentIdProvider = std::function<ContentId(std::span<const std::byte>)>;

ContentId sha1_content_id(std::span<const std::byte> bytes);

class PooledPathAllocator final {
  public:
    explicit PooledPathAllocator(ContentIdProvider provider = sha1_content_id);

    Result<std::filesystem::path> allocate(const std::filesystem::path &selection_root, std::string_view kind,
                                           std::string_view safe_stem, std::span<const std::byte> bytes);

  private:
    struct Entry {
        ContentId id;
        std::vector<std::byte> bytes;
    };

    ContentIdProvider provider_;
    std::map<std::filesystem::path, Entry> entries_;
};

} // namespace axk::cli::detail
