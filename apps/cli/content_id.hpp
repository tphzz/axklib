#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "axklib/error.hpp"
#include "axklib/wav_stream.hpp"

namespace axk::cli::detail {

struct ContentId {
    std::string algorithm;
    std::string digest_hex;
};

using WavContentIdProvider = std::function<Result<ContentId>(const audio_internal::WavSource &)>;

ContentId sha1_content_id(std::span<const std::byte> bytes);
Result<ContentId> sha1_wav_content_id(const audio_internal::WavSource &source);

class PooledPathAllocator final {
  public:
    explicit PooledPathAllocator(WavContentIdProvider provider = sha1_wav_content_id);

    Result<std::filesystem::path> allocate(const std::filesystem::path &selection_root, std::string_view kind,
                                           std::string_view safe_stem, const audio_internal::WavSource &source);

  private:
    struct Entry {
        ContentId id;
        audio_internal::WavSource source;
    };

    WavContentIdProvider provider_;
    std::map<std::filesystem::path, Entry> entries_;
};

} // namespace axk::cli::detail
