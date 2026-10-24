// Platform-neutral client/server for doing raw I/O in a separate (elevated) process.
// Windows uses it over a named pipe for on-demand UAC elevation; the tests run it over an
// in-memory stream on every platform.
//
// Wire format (little-endian), every message = 16-byte header + payload:
//   u32 magic 'RDSK' | u16 op (| 0x8000 in replies) | u16 reserved=0 | u32 handle | u32 payload length
// Reply payload starts with a status: u8 category (0 ok, 1 generic, 2 system, 3 rawdisk) + i32 value.
// The server treats every byte it receives as untrusted: any malformed request ends the session.
#pragma once

#include "rawdisk/block_device.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace rawdisk::detail {

inline constexpr std::uint32_t kMagic = 0x4B534452; // "RDSK"
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kStatusSize = 5;
inline constexpr std::uint16_t kReplyBit = 0x8000;
inline constexpr std::uint32_t kMaxIoBytes = 8u << 20; // per read/write request
inline constexpr std::uint32_t kMaxPayload = kMaxIoBytes + 64;
inline constexpr std::size_t kMaxString = 4096;
inline constexpr std::size_t kMaxOpenHandles = 16;

enum class Op : std::uint16_t { hello = 1, open = 2, read = 3, write = 4, flush = 5, close = 6, verify = 7 };

struct Header {
    std::uint16_t op = 0;
    std::uint32_t handle = 0;
    std::uint32_t len = 0;
};
std::array<std::byte, kHeaderSize> encode_header(const Header &h);
Result<Header> decode_header(std::span<const std::byte, kHeaderSize> bytes);

// Reliable byte stream (pipe, socket, ...). read_exact must fail on EOF.
class Stream {
  public:
    virtual ~Stream() = default;
    virtual Result<void> read_exact(std::span<std::byte> out) = 0;
    virtual Result<void> write_all(std::span<const std::byte> in) = 0;
};

// Client side. Thread-safe; one request in flight at a time. After any transport or protocol
// failure the session is "broken" and every further call fails fast.
class RemoteSession : public std::enable_shared_from_this<RemoteSession> {
    struct Private {};

  public:
    RemoteSession(Private, std::unique_ptr<Stream> stream);
    static Result<std::shared_ptr<RemoteSession>> start(std::unique_ptr<Stream> stream);

    Result<std::unique_ptr<BlockDevice>> open(const DeviceInfo &info, Access access);
    bool broken() const noexcept { return broken_; }

    // Used by the remote BlockDevice.
    Result<void> read(std::uint32_t handle, std::uint64_t lba, std::span<std::byte> out);
    Result<void> write(std::uint32_t handle, std::uint64_t lba, std::span<const std::byte> in);
    Result<void> flush(std::uint32_t handle);
    Result<void> verify_binding(std::uint32_t handle);
    void close(std::uint32_t handle) noexcept;

  private:
    struct Reply {
        std::uint32_t handle = 0;
        std::vector<std::byte> body; // after the status
    };
    Result<Reply> call(Op op, std::uint32_t handle, std::span<const std::byte> payload,
                       std::span<const std::byte> data = {});
    std::unexpected<std::error_code> mark_broken(std::error_code ec) noexcept;

    std::mutex mutex_;
    std::unique_ptr<Stream> stream_;
    std::atomic<bool> broken_{false};
};

using Opener = std::function<Result<std::unique_ptr<BlockDevice>>(const DeviceInfo &, Access)>;

// Server side: runs until the client disconnects (returns success) or sends something
// malformed (returns Errc::helper_protocol). Closes all devices on return.
Result<void> serve(Stream &stream, const Opener &opener) noexcept;

} // namespace rawdisk::detail
