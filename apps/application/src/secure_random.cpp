#include "axklib/application/secure_random.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

axk::app::Error random_error(std::string message) { return {"secure_random_failed", std::move(message)}; }

axk::app::Result<void> fill_random(std::span<std::byte> output) {
#if defined(_WIN32)
    if (output.size() > std::numeric_limits<ULONG>::max())
        return std::unexpected(random_error("secure random request exceeds the platform limit"));
    const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data()),
                                        static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0)
        return std::unexpected(random_error("operating-system random generation failed"));
    return {};
#elif defined(__linux__)
    std::size_t offset{};
    while (offset < output.size()) {
        const auto count = ::getrandom(output.data() + offset, output.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(random_error("operating-system random generation failed"));
        }
        if (count == 0)
            return std::unexpected(random_error("operating-system random generation returned no data"));
        offset += static_cast<std::size_t>(count);
    }
    return {};
#elif defined(__APPLE__)
    arc4random_buf(output.data(), output.size());
    return {};
#else
    const auto descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return std::unexpected(random_error("operating-system random source is unavailable"));
    std::size_t offset{};
    while (offset < output.size()) {
        const auto count = ::read(descriptor, output.data() + offset, output.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(descriptor);
            return std::unexpected(random_error("operating-system random generation failed"));
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    return {};
#endif
}

} // namespace

axk::app::Result<std::string> axk::app::secure_random_hex(std::size_t byte_count) {
    if (byte_count == 0U || byte_count > std::numeric_limits<std::size_t>::max() / 2U)
        return std::unexpected(random_error("secure random byte count is invalid"));
    std::vector<std::byte> bytes(byte_count);
    if (auto filled = fill_random(bytes); !filled)
        return std::unexpected(filled.error());

    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(byte_count * 2U);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}
