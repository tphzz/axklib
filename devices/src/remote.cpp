#include "remote.hpp"
#include "detail.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rawdisk::detail {
namespace {

// ---- encoding ------------------------------------------------------------------------------

class Writer {
  public:
    explicit Writer(std::vector<std::byte> &b) : b_(b) {}
    void u8(std::uint8_t v) { b_.push_back(std::byte{v}); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void i32(std::int32_t v) { le(static_cast<std::uint32_t>(v), 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        for (char c : s)
            b_.push_back(static_cast<std::byte>(c));
    }

  private:
    void le(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            b_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
    std::vector<std::byte> &b_;
};

// Bounds-checked reader: any underflow or invalid field latches ok() == false.
class Reader {
  public:
    explicit Reader(std::span<const std::byte> d) : d_(d) {}
    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return le(8); }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok_ || n > kMaxString || n > remaining()) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char *>(d_.data() + pos_), n);
        pos_ += n;
        if (s.find('\0') != std::string::npos)
            ok_ = false; // would silently truncate a path
        return s;
    }
    std::span<const std::byte> rest() {
        const auto r = d_.subspan(std::min(pos_, d_.size()));
        pos_ = d_.size();
        return r;
    }
    bool ok() const { return ok_; }
    bool done() const { return ok_ && pos_ == d_.size(); }

  private:
    std::size_t remaining() const { return d_.size() - pos_; }
    std::uint64_t le(std::size_t n) {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i)
            v |= std::uint64_t{std::to_integer<std::uint8_t>(d_[pos_ + i])} << (8 * i);
        pos_ += n;
        return v;
    }
    std::span<const std::byte> d_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

enum : std::uint8_t { kCatOk = 0, kCatGeneric = 1, kCatSystem = 2, kCatRawdisk = 3 };

std::pair<std::uint8_t, std::int32_t> encode_error(std::error_code ec) {
    if (!ec)
        return {kCatOk, 0};
    if (ec.category() == std::generic_category())
        return {kCatGeneric, ec.value()};
    if (ec.category() == std::system_category())
        return {kCatSystem, ec.value()};
    if (ec.category() == rawdisk_category())
        return {kCatRawdisk, ec.value()};
    const auto cond = ec.default_error_condition(); // foreign category: send its portable meaning
    if (cond.category() == std::generic_category() && cond.value() != 0)
        return {kCatGeneric, cond.value()};
    return {kCatRawdisk, static_cast<std::int32_t>(Errc::internal_error)};
}

std::optional<std::error_code> decode_error(std::uint8_t cat, std::int32_t v) {
    switch (cat) {
    case kCatOk:
        if (v == 0)
            return std::error_code{};
        return std::nullopt;
    case kCatGeneric:
        if (v == 0)
            return std::nullopt;
        return std::error_code(v, std::generic_category());
    case kCatSystem:
        if (v == 0)
            return std::nullopt;
        return std::error_code(v, std::system_category());
    case kCatRawdisk:
        if (v < 1 || v > static_cast<std::int32_t>(Errc::internal_error))
            return std::nullopt;
        return make_error_code(static_cast<Errc>(v));
    default:
        return std::nullopt;
    }
}

// ---- client-side device ----------------------------------------------------------------------

class RemoteDisk final : public BlockDevice {
  public:
    RemoteDisk(std::shared_ptr<RemoteSession> s, std::uint32_t h, std::uint32_t ss, std::uint64_t count, bool writable)
        : session_(std::move(s)), handle_(h), ss_(ss), count_(count), writable_(writable) {}
    ~RemoteDisk() override { session_->close(handle_); }

    std::uint32_t sector_size() const noexcept override { return ss_; }
    std::uint64_t sector_count() const noexcept override { return count_; }
    bool writable() const noexcept override { return writable_; }
    bool uses_elevation_helper() const noexcept override { return true; }

  protected:
    Result<void> read_raw(std::uint64_t off, std::span<std::byte> out) override {
        return session_->read(handle_, off / ss_, out);
    }
    Result<void> write_raw(std::uint64_t off, std::span<const std::byte> in) override {
        return session_->write(handle_, off / ss_, in);
    }
    Result<void> flush_raw() override { return session_->flush(handle_); }
    Result<void> verify_binding_raw() override { return session_->verify_binding(handle_); }

  private:
    std::shared_ptr<RemoteSession> session_;
    std::uint32_t handle_;
    std::uint32_t ss_;
    std::uint64_t count_;
    bool writable_;
};

} // namespace

std::array<std::byte, kHeaderSize> encode_header(const Header &h) {
    std::vector<std::byte> v;
    v.reserve(kHeaderSize);
    Writer w(v);
    w.u32(kMagic);
    w.u16(h.op);
    w.u16(0);
    w.u32(h.handle);
    w.u32(h.len);
    std::array<std::byte, kHeaderSize> out{};
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

Result<Header> decode_header(std::span<const std::byte, kHeaderSize> bytes) {
    Reader r(bytes);
    const std::uint32_t magic = r.u32();
    Header h;
    h.op = r.u16();
    const std::uint16_t reserved = r.u16();
    h.handle = r.u32();
    h.len = r.u32();
    if (!r.done() || magic != kMagic || reserved != 0 || h.len > kMaxPayload)
        return fail(Errc::helper_protocol);
    return h;
}

// ---- client ----------------------------------------------------------------------------------

RemoteSession::RemoteSession(Private, std::unique_ptr<Stream> stream) : stream_(std::move(stream)) {}

Result<std::shared_ptr<RemoteSession>> RemoteSession::start(std::unique_ptr<Stream> stream) {
    if (!stream)
        return fail(Errc::invalid_argument);
    auto session = std::make_shared<RemoteSession>(Private{}, std::move(stream));
    std::vector<std::byte> p;
    Writer(p).u32(kProtocolVersion);
    auto reply = session->call(Op::hello, 0, p);
    if (!reply)
        return fail(reply.error());
    Reader r(reply->body);
    if (r.u32() != kProtocolVersion || !r.done())
        return session->mark_broken(make_error_code(Errc::helper_protocol));
    return session;
}

std::unexpected<std::error_code> RemoteSession::mark_broken(std::error_code ec) noexcept {
    broken_ = true;
    return std::unexpected(ec);
}

Result<RemoteSession::Reply> RemoteSession::call(Op op, std::uint32_t handle, std::span<const std::byte> payload,
                                                 std::span<const std::byte> data) {
    std::lock_guard lock(mutex_);
    if (broken_)
        return fail(Errc::elevation_failed);
    const std::size_t len = payload.size() + data.size();
    if (len > kMaxPayload)
        return fail(Errc::invalid_argument);

    // A failed transfer leaves the stream mid-message: the session can never be used again.
    auto transport = [&](std::error_code ec) {
        return mark_broken(ec == Errc::helper_timeout ? ec : make_error_code(Errc::elevation_failed));
    };

    const auto opcode = static_cast<std::uint16_t>(op);
    const auto hdr = encode_header({opcode, handle, static_cast<std::uint32_t>(len)});
    if (auto r = stream_->write_all(hdr); !r)
        return transport(r.error());
    if (!payload.empty()) {
        if (auto r = stream_->write_all(payload); !r)
            return transport(r.error());
    }
    if (!data.empty()) {
        if (auto r = stream_->write_all(data); !r)
            return transport(r.error());
    }

    std::array<std::byte, kHeaderSize> rh{};
    if (auto r = stream_->read_exact(rh); !r)
        return transport(r.error());
    const auto h = decode_header(rh);
    if (!h || h->op != (opcode | kReplyBit) || h->len < kStatusSize || (op != Op::open && h->handle != handle))
        return mark_broken(make_error_code(Errc::helper_protocol));

    Reply reply{h->handle, std::vector<std::byte>(h->len)};
    if (auto r = stream_->read_exact(reply.body); !r)
        return transport(r.error());

    Reader status(std::span<const std::byte>(reply.body).first(kStatusSize));
    const std::uint8_t cat = status.u8();
    const std::int32_t value = status.i32();
    const auto ec = decode_error(cat, value);
    if (!ec || (*ec && h->len != kStatusSize))
        return mark_broken(make_error_code(Errc::helper_protocol));
    if (*ec)
        return fail(*ec); // a normal, reported failure: the session stays usable

    reply.body.erase(reply.body.begin(), reply.body.begin() + kStatusSize);
    return reply;
}

Result<std::unique_ptr<BlockDevice>> RemoteSession::open(const DeviceInfo &info, Access access) {
    if (info.path.size() > kMaxString || info.model.size() > kMaxString || info.serial.size() > kMaxString ||
        info.carrier_id.size() > kMaxString || info.connection_id.size() > kMaxString)
        return fail(Errc::invalid_argument);
    std::vector<std::byte> p;
    Writer w(p);
    w.u8(access == Access::ReadWrite ? 1 : 0);
    w.u64(info.size_bytes);
    w.u32(info.sector_size);
    w.str(info.path);
    w.str(info.model);
    w.str(info.serial);
    w.u8(info.removable ? 1 : 0);
    w.str(info.carrier_id);
    w.str(info.connection_id);

    auto reply = call(Op::open, 0, p);
    if (!reply)
        return fail(reply.error());

    Reader r(reply->body);
    const std::uint32_t ss = r.u32();
    const std::uint64_t count = r.u64();
    const bool writable = r.u8() != 0;
    if (!r.done() || reply->handle == 0 || !valid_sector_size(ss) || count == 0 ||
        count > std::numeric_limits<std::uint64_t>::max() / ss || writable != (access == Access::ReadWrite))
        return mark_broken(make_error_code(Errc::helper_protocol));

    return std::make_unique<RemoteDisk>(shared_from_this(), reply->handle, ss, count, writable);
}

Result<void> RemoteSession::read(std::uint32_t handle, std::uint64_t lba, std::span<std::byte> out) {
    if (out.empty() || out.size() > kMaxIoBytes)
        return fail(Errc::invalid_argument);
    std::vector<std::byte> p;
    Writer w(p);
    w.u64(lba);
    w.u32(static_cast<std::uint32_t>(out.size()));
    auto reply = call(Op::read, handle, p);
    if (!reply)
        return fail(reply.error());
    if (reply->body.size() != out.size())
        return mark_broken(make_error_code(Errc::helper_protocol));
    std::memcpy(out.data(), reply->body.data(), out.size());
    return {};
}

Result<void> RemoteSession::write(std::uint32_t handle, std::uint64_t lba, std::span<const std::byte> in) {
    if (in.empty() || in.size() > kMaxIoBytes)
        return fail(Errc::invalid_argument);
    std::vector<std::byte> p;
    Writer(p).u64(lba);
    auto reply = call(Op::write, handle, p, in);
    if (!reply)
        return fail(reply.error());
    if (!reply->body.empty())
        return mark_broken(make_error_code(Errc::helper_protocol));
    return {};
}

Result<void> RemoteSession::flush(std::uint32_t handle) {
    auto reply = call(Op::flush, handle, {});
    if (!reply)
        return fail(reply.error());
    if (!reply->body.empty())
        return mark_broken(make_error_code(Errc::helper_protocol));
    return {};
}

void RemoteSession::close(std::uint32_t handle) noexcept {
    try {
        (void)call(Op::close, handle, {});
    } catch (...) {
    }
}

Result<void> RemoteSession::verify_binding(std::uint32_t handle) {
    auto reply = call(Op::verify, handle, {});
    if (!reply)
        return fail(reply.error());
    if (!reply->body.empty())
        return mark_broken(make_error_code(Errc::helper_protocol));
    return {};
}

// ---- server ----------------------------------------------------------------------------------

Result<void> serve(Stream &stream, const Opener &opener) noexcept {
    return guard([&]() -> Result<void> {
        std::map<std::uint32_t, std::unique_ptr<BlockDevice>> handles;
        std::uint32_t next_handle = 1;
        bool greeted = false;
        std::vector<std::byte> payload;
        std::vector<std::byte> reply; // status + body

        auto send = [&](std::uint16_t op, std::uint32_t handle, std::error_code ec) -> Result<void> {
            if (ec)
                reply.resize(kStatusSize); // failures carry no body
            const auto [cat, value] = encode_error(ec);
            std::vector<std::byte> st;
            Writer w(st);
            w.u8(cat);
            w.i32(value);
            std::copy(st.begin(), st.end(), reply.begin());
            const auto hdr = encode_header(
                {static_cast<std::uint16_t>(op | kReplyBit), handle, static_cast<std::uint32_t>(reply.size())});
            if (auto r = stream.write_all(hdr); !r)
                return r;
            return stream.write_all(reply);
        };

        for (;;) {
            std::array<std::byte, kHeaderSize> hb{};
            if (!stream.read_exact(hb))
                return {}; // client went away: normal end of session
            const auto h = decode_header(hb);
            if (!h)
                return fail(h.error()); // not our protocol: drop the connection unanswered
            payload.resize(h->len);
            if (auto r = stream.read_exact(payload); !r)
                return fail(r.error());

            const std::uint16_t op = h->op;
            reply.assign(kStatusSize, std::byte{0});
            Reader in(payload);
            auto violation = [&]() -> Result<void> {
                (void)send(op, h->handle, make_error_code(Errc::helper_protocol));
                return fail(Errc::helper_protocol);
            };

            if (!greeted) {
                const std::uint32_t version = in.u32();
                if (op != static_cast<std::uint16_t>(Op::hello) || !in.done() || version != kProtocolVersion)
                    return violation();
                greeted = true;
                Writer(reply).u32(kProtocolVersion);
                if (auto r = send(op, 0, {}); !r)
                    return r;
                continue;
            }

            std::error_code ec;
            std::uint32_t reply_handle = h->handle;
            const auto found = handles.find(h->handle);

            switch (static_cast<Op>(op)) {
            case Op::open: {
                const std::uint8_t access = in.u8();
                DeviceInfo info;
                info.size_bytes = in.u64();
                info.sector_size = in.u32();
                info.path = in.str();
                info.model = in.str();
                info.serial = in.str();
                const std::uint8_t removable = in.u8();
                info.carrier_id = in.str();
                info.connection_id = in.str();
                if (!in.done() || access > 1 || removable > 1 || h->handle != 0)
                    return violation();
                info.removable = removable == 1;
                reply_handle = 0;
                if (handles.size() >= kMaxOpenHandles) {
                    ec = Errc::too_many_handles;
                    break;
                }
                auto dev = opener(info, access == 1 ? Access::ReadWrite : Access::ReadOnly);
                if (!dev) {
                    ec = dev.error();
                    break;
                }
                if (!*dev) {
                    ec = Errc::internal_error;
                    break;
                }
                while (next_handle == 0 || handles.contains(next_handle))
                    ++next_handle;
                reply_handle = next_handle++;
                Writer w(reply);
                w.u32((*dev)->sector_size());
                w.u64((*dev)->sector_count());
                w.u8((*dev)->writable() ? 1 : 0);
                handles.emplace(reply_handle, std::move(*dev));
                break;
            }
            case Op::read: {
                const std::uint64_t lba = in.u64();
                const std::uint32_t bytes = in.u32();
                if (!in.done())
                    return violation();
                if (found == handles.end() || bytes == 0 || bytes > kMaxIoBytes) {
                    ec = Errc::invalid_argument;
                    break;
                }
                reply.resize(kStatusSize + bytes);
                if (auto r = found->second->read(lba, std::span(reply).subspan(kStatusSize)); !r)
                    ec = r.error();
                break;
            }
            case Op::write: {
                const std::uint64_t lba = in.u64();
                const auto data = in.rest();
                if (!in.ok())
                    return violation();
                if (found == handles.end() || data.empty() || data.size() > kMaxIoBytes) {
                    ec = Errc::invalid_argument;
                    break;
                }
                if (auto r = found->second->write(lba, data); !r)
                    ec = r.error();
                break;
            }
            case Op::verify:
                if (!in.done())
                    return violation();
                if (found == handles.end()) {
                    ec = Errc::invalid_argument;
                    break;
                }
                if (auto r = found->second->verify_binding(); !r)
                    ec = r.error();
                break;
            case Op::flush:
                if (!in.done())
                    return violation();
                if (found == handles.end()) {
                    ec = Errc::invalid_argument;
                    break;
                }
                if (auto r = found->second->flush(); !r)
                    ec = r.error();
                break;
            case Op::close:
                if (!in.done())
                    return violation();
                if (found == handles.end()) {
                    ec = Errc::invalid_argument;
                    break;
                }
                handles.erase(found);
                break;
            default: // includes a second hello
                return violation();
            }

            if (auto r = send(op, reply_handle, ec); !r)
                return r;
        }
    });
}

} // namespace rawdisk::detail
