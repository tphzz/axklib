#include "content_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

namespace {

axk::app::Error digest_error(std::string message) { return {"hash_failed", std::move(message)}; }

axk::app::Error core_digest_error(const axk::Error &error) {
    if (error.code == axk::ErrorCode::operation_cancelled)
        return {"operation_cancelled", error.message};
    return digest_error(error.message);
}

} // namespace

axk::app::Result<std::string> axk::app::detail::reader_sha256(const RandomAccessReader &reader,
                                                              const CancellationToken &cancellation) {
    constexpr std::size_t chunk_size = 1024U * 1024U;
    std::vector<std::byte> buffer(chunk_size);
    const auto destroy_context = [](EVP_MD_CTX *context) { EVP_MD_CTX_free(context); };
    std::unique_ptr<EVP_MD_CTX, decltype(destroy_context)> digest{EVP_MD_CTX_new(), destroy_context};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        return std::unexpected(digest_error("could not initialize SHA-256"));

    for (std::uint64_t offset = 0U; offset < reader.size();) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected(core_digest_error(checked.error()));
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        const auto chunk = std::span<std::byte>{buffer}.first(count);
        if (const auto read = reader.read_exact_at(offset, chunk); !read)
            return std::unexpected(core_digest_error(read.error()));
        if (EVP_DigestUpdate(digest.get(), chunk.data(), chunk.size()) != 1)
            return std::unexpected(digest_error("could not update SHA-256"));
        offset += count;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size{};
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32U)
        return std::unexpected(digest_error("could not finish SHA-256"));
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<std::size_t>(size) * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(alphabet[bytes[index] >> 4U]);
        result.push_back(alphabet[bytes[index] & 0x0fU]);
    }
    return result;
}

axk::app::Result<std::string> axk::app::detail::file_sha256(const std::filesystem::path &path,
                                                            const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected(core_digest_error(reader.error()));
    return reader_sha256(**reader, cancellation);
}
