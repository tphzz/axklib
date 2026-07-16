#include "axklib/application/content_id.hpp"

#include <algorithm>
#include <format>

#include <hash-library/sha1.h>

#include "axklib/utf8.hpp"

namespace axk::app {
namespace {

constexpr std::size_t pooled_suffix_length = 12U;

bool valid_sha1(const ContentId &id) {
    return id.algorithm == "sha1" && id.digest_hex.size() == 40U && std::ranges::all_of(id.digest_hex, [](char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

Error content_id_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::internal, std::move(message));
}

} // namespace

ContentId sha1_content_id(std::span<const std::byte> bytes) {
    SHA1 hash;
    hash.add(bytes.data(), bytes.size());
    return {.algorithm = "sha1", .digest_hex = hash.getHash()};
}

axk::Result<ContentId> sha1_wav_content_id(const audio_internal::WavSource &source) {
    SHA1 hash;
    const auto streamed =
        audio_internal::stream_wav(source, [&](std::span<const std::byte> bytes) -> axk::Result<void> {
            hash.add(bytes.data(), bytes.size());
            return {};
        });
    if (!streamed)
        return std::unexpected{streamed.error()};
    return ContentId{.algorithm = "sha1", .digest_hex = hash.getHash()};
}

PooledPathAllocator::PooledPathAllocator(WavContentIdProvider provider) : provider_{std::move(provider)} {}

axk::Result<std::filesystem::path> PooledPathAllocator::allocate(const std::filesystem::path &selection_root,
                                                                 std::string_view kind, std::string_view safe_stem,
                                                                 const audio_internal::WavSource &source) {
    const auto id = provider_(source);
    if (!id)
        return std::unexpected{id.error()};
    if (!valid_sha1(*id))
        return std::unexpected{content_id_error("content identifier provider returned invalid SHA-1")};

    const auto target = std::filesystem::path{"_samples"} / kind /
                        std::format("{}__{}.wav", safe_stem, id->digest_hex.substr(0U, pooled_suffix_length));
    if (const auto existing = entries_.find(target); existing != entries_.end()) {
        auto equal = audio_internal::equal_wav(existing->second.source, source);
        if (!equal)
            return std::unexpected{equal.error()};
        if (existing->second.id.digest_hex != id->digest_hex || !*equal) {
            return std::unexpected{
                content_id_error("distinct WAV contents share pooled export path " + axk::text::path_to_utf8(target))};
        }
    } else {
        entries_.emplace(target, Entry{*id, source});
    }

    std::error_code error;
    auto relative = std::filesystem::relative(target, selection_root, error);
    return error ? target : relative;
}

} // namespace axk::app
