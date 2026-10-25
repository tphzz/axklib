// rawdisk tests. Portable parts run everywhere; the Linux device tests run when root and
// losetup are available and RAWDISK_TEST_LOOP_DEVICE=1 (they drive the production path against a loop device
// behind a fake USB sysfs tree).
#include "detail.hpp"
#include "rawdisk/block_device.hpp"
#include "remote.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

using namespace rawdisk;
namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

template <class T> static std::error_code err(const Result<T> &r) { return r ? std::error_code{} : r.error(); }

static fs::path temp_dir() {
    static fs::path dir = [] {
        auto d = fs::temp_directory_path() / std::format("rawdisk-tests-{}", std::random_device{}());
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

static fs::path make_image(const std::string &name, std::size_t bytes, unsigned char fill = 0) {
    const auto p = temp_dir() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::vector<char> data(bytes, static_cast<char>(fill));
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return p;
}

static std::vector<std::byte> pattern(std::size_t n, unsigned seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<std::byte>((i * 31 + seed * 7 + (i >> 9)) & 0xFF);
    return v;
}

// ---- in-memory duplex stream ---------------------------------------------------------------

struct Channel {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::byte> q;
    bool closed = false;
};

class MemStream final : public detail::Stream {
  public:
    MemStream(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out, std::shared_ptr<void> lifetime = {})
        : in_(std::move(in)), out_(std::move(out)), lifetime_(std::move(lifetime)) {}
    ~MemStream() override { shutdown(); }
    void shutdown() {
        for (auto *c : {in_.get(), out_.get()}) {
            std::lock_guard l(c->m);
            c->closed = true;
            c->cv.notify_all();
        }
    }
    void close_write() { // half-close, like a pipe whose writer is done
        std::lock_guard l(out_->m);
        out_->closed = true;
        out_->cv.notify_all();
    }
    bool throw_next_write = false;
    Result<void> read_exact(std::span<std::byte> buf) override {
        std::unique_lock l(in_->m);
        in_->cv.wait(l, [&] { return in_->q.size() >= buf.size() || in_->closed; });
        if (in_->q.size() < buf.size())
            return detail::fail(std::make_error_code(std::errc::broken_pipe));
        for (auto &b : buf) {
            b = in_->q.front();
            in_->q.pop_front();
        }
        return {};
    }
    Result<void> write_all(std::span<const std::byte> buf) override {
        if (std::exchange(throw_next_write, false))
            throw std::bad_alloc();
        std::lock_guard l(out_->m);
        if (out_->closed)
            return detail::fail(std::make_error_code(std::errc::broken_pipe));
        out_->q.insert(out_->q.end(), buf.begin(), buf.end());
        out_->cv.notify_all();
        return {};
    }

  private:
    std::shared_ptr<Channel> in_, out_;
    std::shared_ptr<void> lifetime_;
};

static std::pair<std::unique_ptr<MemStream>, std::unique_ptr<MemStream>>
make_pipe(std::shared_ptr<void> client_lifetime = {}) {
    auto a = std::make_shared<Channel>(), b = std::make_shared<Channel>();
    return {std::make_unique<MemStream>(a, b, std::move(client_lifetime)), std::make_unique<MemStream>(b, a)};
}

// Server whose "devices" are image files; DeviceInfo.path is the image path.
struct TestServer {
    std::unique_ptr<MemStream> server_side;
    std::thread thread;
    Result<void> result;

    explicit TestServer(std::unique_ptr<MemStream> s) : server_side(std::move(s)) {
        thread = std::thread([this] {
            result = detail::serve(
                *server_side, [](const DeviceInfo &info, Access access) -> Result<std::unique_ptr<BlockDevice>> {
                    if (info.model == "fail-generic")
                        return detail::fail(std::make_error_code(std::errc::no_such_file_or_directory));
                    if (info.model == "fail-system")
                        return detail::fail(std::error_code(1234, std::system_category()));
                    if (info.model == "identity-bound" &&
                        (info.carrier_id != "usb-parent" || info.connection_id != std::string(1000, 'D')))
                        return detail::fail(Errc::device_connection_changed);
                    return open_image(info.path, access, info.sector_size ? info.sector_size : 512);
                });
            server_side->shutdown();
        });
    }
    ~TestServer() {
        if (thread.joinable())
            thread.join();
    }
};

// ---- tests ---------------------------------------------------------------------------------

static void test_errors() {
    CHECK(make_error_code(Errc::device_busy) == std::errc::device_or_resource_busy);
    CHECK(make_error_code(Errc::elevation_cancelled) == std::errc::operation_canceled);
    CHECK(make_error_code(Errc::write_protected) == std::errc::read_only_file_system);
    CHECK(make_error_code(Errc::device_changed) == std::errc::no_such_device);
    CHECK(std::error_code(Errc::out_of_range) == Errc::out_of_range);
    CHECK(!make_error_code(Errc::internal_error).message().empty());
    CHECK(make_error_code(static_cast<Errc>(999)).message() == "unknown rawdisk error");
}

static void test_image_basic() {
    const auto path = make_image("basic.img", 512 * 100);
    auto img = open_image(path, Access::ReadWrite);
    CHECK(img.has_value());
    if (!img)
        return;
    auto &dev = **img;
    CHECK(dev.sector_count() == 100);
    CHECK(dev.writable());

    // Unaligned buffer (bounce path) and aligned buffer (direct path).
    std::vector<std::byte> raw(512 * 3 + 1);
    const std::span<std::byte> unaligned(raw.data() + 1, 512 * 3);
    const auto pat = pattern(unaligned.size(), 1);
    std::copy(pat.begin(), pat.end(), unaligned.begin());
    CHECK(dev.write(97, unaligned).has_value());
    AlignedBuffer back(512 * 3);
    CHECK(dev.read(97, back.span()).has_value());
    CHECK(std::memcmp(back.data(), pat.data(), pat.size()) == 0);

    CHECK(err(dev.write(98, unaligned)) == Errc::out_of_range);
    CHECK(err(dev.read(100, back.span().first(512))) == Errc::out_of_range);
    CHECK(err(dev.read(UINT64_MAX, back.span().first(512))) == Errc::out_of_range);
    CHECK(err(dev.write(0, unaligned.first(100))) == Errc::bad_length);
    CHECK(dev.read(0, {}).has_value()); // empty transfer is a no-op
    CHECK(dev.flush().has_value());

    // Multi-chunk transfer (> 1 MiB) through the bounce path.
    img->reset(); // img has a value here (checked above)
    const auto big = make_image("big.img", 3 << 20);
    auto b = open_image(big, Access::ReadWrite);
    CHECK(b.has_value());
    if (!b)
        return;
    std::vector<std::byte> braw((3 << 20) - 1024 + 3);
    std::span<std::byte> bs(braw.data() + 3, (3 << 20) - 1024);
    const auto bpat = pattern(bs.size(), 9);
    std::copy(bpat.begin(), bpat.end(), bs.begin());
    CHECK((*b)->write(2, bs).has_value());
    std::vector<std::byte> bback(bs.size() + 5);
    std::span<std::byte> bbs(bback.data() + 5, bs.size());
    CHECK((*b)->read(2, bbs).has_value());
    CHECK(std::memcmp(bbs.data(), bpat.data(), bpat.size()) == 0);
}

static void test_image_validation() {
    CHECK(err(open_image(temp_dir() / "missing.img", Access::ReadOnly)) == std::errc::no_such_file_or_directory);
    CHECK(!open_image(temp_dir(), Access::ReadOnly).has_value()); // a directory is never an image
    CHECK(err(open_image(make_image("odd.img", 1000), Access::ReadOnly)) == Errc::bad_length);
    CHECK(err(open_image(make_image("empty.img", 0), Access::ReadOnly)) == Errc::invalid_argument);
    CHECK(err(open_image(make_image("ss.img", 4096), Access::ReadOnly, 1000)) == Errc::invalid_argument);
    CHECK(err(open_image(make_image("ss2.img", 4096), Access::ReadOnly, 256)) == Errc::invalid_argument);
    CHECK(err(open_image("", Access::ReadOnly)) == Errc::invalid_argument);
#if !defined(_WIN32)
    CHECK(err(open_image("/dev/null", Access::ReadOnly)) == Errc::invalid_argument); // devices refused
    CHECK(open_image("/dev/zero", Access::ReadWrite).has_value() == false);
#endif

    const auto p = make_image("ro.img", 4096);
    auto ro = open_image(p, Access::ReadOnly);
    CHECK(ro.has_value());
    if (ro) {
        AlignedBuffer b(512);
        CHECK(err((*ro)->write(0, b.span())) == Errc::read_only_handle);
        CHECK((*ro)->flush().has_value());
        // Shared lock held: a writer must be refused, a second reader allowed.
        CHECK(err(open_image(p, Access::ReadWrite)) == Errc::device_busy);
        CHECK(open_image(p, Access::ReadOnly).has_value());
    }
    if (ro)
        ro->reset();
    auto rw = open_image(p, Access::ReadWrite);
    CHECK(rw.has_value());
    CHECK(err(open_image(p, Access::ReadOnly)) == Errc::device_busy);
    CHECK(err(open_image(p, Access::ReadWrite)) == Errc::device_busy);
}

static void test_threads() {
    const auto p = make_image("threads.img", 512 * 4096);
    auto img = open_image(p, Access::ReadWrite);
    CHECK(img.has_value());
    if (!img)
        return;
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            const auto pat = pattern(512 * 16, t);
            std::vector<std::byte> back(pat.size());
            for (int round = 0; round < 50; ++round) {
                const std::uint64_t lba = t * 512 + static_cast<unsigned>(round % 8) * 16;
                if (!(*img)->write(lba, pat))
                    ++bad;
                if (!(*img)->read(lba, back) || back != pat)
                    ++bad;
            }
        });
    }
    for (auto &t : ts)
        t.join();
    CHECK(bad == 0);
}

static void test_remote_roundtrip() {
    auto [client, server] = make_pipe();
    TestServer srv(std::move(server));
    auto session = detail::RemoteSession::start(std::move(client));
    CHECK(session.has_value());
    if (!session)
        return;

    const auto path = make_image("remote.img", 512 * 64);
    DeviceInfo info{.path = path.string(), .model = "m", .serial = "s", .size_bytes = 512 * 64, .sector_size = 512};
    auto dev = (*session)->open(info, Access::ReadWrite);
    CHECK(dev.has_value());
    if (!dev)
        return;
    CHECK((*dev)->sector_count() == 64);
    CHECK((*dev)->writable());

    CHECK(err((*dev)->verify_binding()) == Errc::not_a_usb_disk);
    CHECK(!(*session)->broken());

    const auto pat = pattern(512 * 10, 3);
    CHECK((*dev)->write(5, pat).has_value());
    std::vector<std::byte> back(pat.size());
    CHECK((*dev)->read(5, back).has_value());
    CHECK(back == pat);
    CHECK((*dev)->flush().has_value());
    CHECK(err((*dev)->read(60, back)) == Errc::out_of_range); // rejected client-side

    // Errors from the server arrive with their category intact; the session stays usable.
    DeviceInfo missing = info;
    missing.path = (temp_dir() / "nope.img").string();
    CHECK(err((*session)->open(missing, Access::ReadOnly)) == std::errc::no_such_file_or_directory);
    missing.model = "fail-system";
    CHECK(err((*session)->open(missing, Access::ReadOnly)) == std::error_code(1234, std::system_category()));
    // Lock conflict inside the server: second RW open of the same image.
    CHECK(err((*session)->open(info, Access::ReadWrite)) == Errc::device_busy);
    CHECK(!(*session)->broken());

    // Read-only handle through the helper.
    dev->reset(); // closes remotely, releasing the lock
    auto ro = (*session)->open(info, Access::ReadOnly);
    CHECK(ro.has_value());
    if (ro) {
        CHECK(!(*ro)->writable());
        CHECK(err((*ro)->write(0, pat)) == Errc::read_only_handle);
        CHECK((*ro)->read(5, back).has_value() && back == pat);
    }

    // Many concurrent users of one session.
    std::vector<std::unique_ptr<BlockDevice>> handles;
    for (int i = 0; i < 6; ++i) {
        auto h = (*session)->open(info, Access::ReadOnly);
        CHECK(h.has_value());
        if (h)
            handles.push_back(std::move(*h));
    }
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (auto &h : handles)
        ts.emplace_back([&, d = h.get()] {
            std::vector<std::byte> b(pat.size());
            for (int i = 0; i < 30; ++i)
                if (!d->read(5, b) || b != pat)
                    ++bad;
        });
    for (auto &t : ts)
        t.join();
    CHECK(bad == 0);
}

static void test_remote_connection_identity() {
    auto [client, server] = make_pipe();
    TestServer srv(std::move(server));
    auto session = detail::RemoteSession::start(std::move(client));
    CHECK(session.has_value());
    if (!session)
        return;
    DeviceInfo info{.path = make_image("remote-identity.img", 4096).string(),
                    .model = "identity-bound",
                    .serial = "test-serial",
                    .size_bytes = 4096,
                    .sector_size = 512,
                    .carrier_id = "usb-parent",
                    .connection_id = std::string(1000, 'D')};
    auto opened = (*session)->open(info, Access::ReadOnly);
    CHECK(opened.has_value());
    if (opened)
        opened->reset();
    const auto fresh = info;
    info.connection_id += ":new-arrival";
    CHECK(err((*session)->open(info, Access::ReadOnly)) == Errc::device_connection_changed);
    CHECK(!(*session)->broken());
    opened = (*session)->open(fresh, Access::ReadOnly);
    CHECK(opened.has_value());
    if (opened)
        opened->reset();
}

static void test_remote_handle_limit() {
    auto [client, server] = make_pipe();
    TestServer srv(std::move(server));
    auto session = detail::RemoteSession::start(std::move(client));
    CHECK(session.has_value());
    if (!session)
        return;
    const auto path = make_image("limit.img", 4096);
    DeviceInfo info{.path = path.string(), .model = {}, .serial = {}, .size_bytes = 4096, .sector_size = 512};
    std::vector<std::unique_ptr<BlockDevice>> open;
    for (std::size_t i = 0; i < detail::kMaxOpenHandles; ++i) {
        auto d = (*session)->open(info, Access::ReadOnly);
        CHECK(d.has_value());
        if (d)
            open.push_back(std::move(*d));
    }
    CHECK(err((*session)->open(info, Access::ReadOnly)) == Errc::too_many_handles);
    open.pop_back();
    CHECK((*session)->open(info, Access::ReadOnly).has_value());
}

// Sends raw bytes to a server and returns how serve() ended.
static Result<void> serve_raw(const std::vector<std::byte> &bytes) {
    auto [client, server] = make_pipe();
    TestServer srv(std::move(server));
    (void)client->write_all(bytes);
    client->close_write(); // EOF after the bytes; replies can still flow back
    srv.thread.join();
    return srv.result;
}

static std::vector<std::byte> frame(std::uint16_t op, std::uint32_t handle, std::vector<std::byte> payload,
                                    std::uint32_t len_override = UINT32_MAX) {
    const auto h = detail::encode_header(
        {op, handle, len_override == UINT32_MAX ? static_cast<std::uint32_t>(payload.size()) : len_override});
    std::vector<std::byte> v(h.begin(), h.end());
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

static std::vector<std::byte> hello() { return frame(1, 0, {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}); }

static void test_server_robustness() {
    // Clean hello then EOF: normal end.
    CHECK(serve_raw(hello()).has_value());
    // Nothing at all: normal end.
    CHECK(serve_raw({}).has_value());
    // Wrong magic.
    auto bad_magic = hello();
    bad_magic[0] = std::byte{0};
    CHECK(err(serve_raw(bad_magic)) == Errc::helper_protocol);
    // Absurd length is rejected before allocating anything.
    CHECK(err(serve_raw(frame(1, 0, {}, 0xFFFFFFFFu))) == Errc::helper_protocol);
    // Request before hello.
    CHECK(err(serve_raw(frame(3, 1, std::vector<std::byte>(12)))) == Errc::helper_protocol);
    // Wrong protocol version.
    CHECK(err(serve_raw(frame(1, 0, {std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}}))) ==
          Errc::helper_protocol);
    // Second hello, unknown op, truncated payload after a valid hello.
    auto twice = hello();
    auto h2 = hello();
    twice.insert(twice.end(), h2.begin(), h2.end());
    CHECK(err(serve_raw(twice)) == Errc::helper_protocol);
    auto unknown = hello();
    auto u = frame(99, 0, {});
    unknown.insert(unknown.end(), u.begin(), u.end());
    CHECK(err(serve_raw(unknown)) == Errc::helper_protocol);
    auto shortread = hello();
    auto s = frame(3, 1, std::vector<std::byte>(5)); // read needs 12 bytes
    shortread.insert(shortread.end(), s.begin(), s.end());
    CHECK(err(serve_raw(shortread)) == Errc::helper_protocol);
    // Open with a string length pointing past the payload.
    auto badopen = hello();
    std::vector<std::byte> op(1 + 8 + 4, std::byte{0});
    for (int i = 0; i < 4; ++i)
        op.push_back(std::byte{0xFF}); // path length 0xFFFFFFFF
    auto o = frame(2, 0, op);
    badopen.insert(badopen.end(), o.begin(), o.end());
    CHECK(err(serve_raw(badopen)) == Errc::helper_protocol);
    // Open with an embedded NUL in the path.
    auto nul = hello();
    std::vector<std::byte> np(1 + 8 + 4, std::byte{0});
    np.push_back(std::byte{3});
    np.push_back(std::byte{0});
    np.push_back(std::byte{0});
    np.push_back(std::byte{0});
    np.push_back(std::byte{'a'});
    np.push_back(std::byte{0});
    np.push_back(std::byte{'b'});
    for (int i = 0; i < 8; ++i)
        np.push_back(std::byte{0}); // empty model + serial
    np.push_back(std::byte{0});     // removable
    auto n = frame(2, 0, np);
    nul.insert(nul.end(), n.begin(), n.end());
    CHECK(err(serve_raw(nul)) == Errc::helper_protocol);

    // Fuzz: random garbage (sometimes behind a valid hello) never crashes or hangs the server.
    std::mt19937 rng(12345);
    int ended = 0;
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::byte> bytes = (i % 2) ? hello() : std::vector<std::byte>{};
        std::vector<std::byte> junk(rng() % 64);
        for (auto &b : junk)
            b = static_cast<std::byte>(rng());
        if (i % 3 == 0 && junk.size() >= 16) { // make some of them look like frames
            const auto hdr =
                detail::encode_header({static_cast<std::uint16_t>(1 + rng() % 7), static_cast<std::uint32_t>(rng() % 3),
                                       static_cast<std::uint32_t>(rng() % 40)});
            std::copy(hdr.begin(), hdr.end(), junk.begin());
        }
        bytes.insert(bytes.end(), junk.begin(), junk.end());
        (void)serve_raw(bytes);
        ++ended;
    }
    CHECK(ended == 3000);
}

// Fake server that answers with whatever we script, to test the client's defenses.
static Result<std::shared_ptr<detail::RemoteSession>> client_against(std::vector<std::byte> script,
                                                                     std::unique_ptr<MemStream> &server_keep,
                                                                     std::shared_ptr<void> client_lifetime = {}) {
    auto [client, server] = make_pipe(std::move(client_lifetime));
    (void)server->write_all(script);
    server_keep = std::move(server);
    return detail::RemoteSession::start(std::move(client));
}

static std::vector<std::byte> reply(std::uint16_t op, std::uint32_t handle, std::vector<std::byte> body) {
    return frame(static_cast<std::uint16_t>(op | detail::kReplyBit), handle, std::move(body));
}

static void test_client_robustness() {
    const std::vector<std::byte> ok_hello_body = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                                  std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    std::unique_ptr<MemStream> keep;
    // Server hangs up immediately.
    {
        auto [client, server] = make_pipe();
        server.reset();
        CHECK(err(detail::RemoteSession::start(std::move(client))) == Errc::elevation_failed);
    }
    // Reply with the wrong op.
    CHECK(err(client_against(reply(2, 0, ok_hello_body), keep)) == Errc::helper_protocol);
    // Reply with an unknown error category.
    CHECK(err(client_against(reply(1, 0, {std::byte{9}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}),
                             keep)) == Errc::helper_protocol);
    // Error status carrying a body.
    CHECK(err(client_against(
              reply(1, 0, {std::byte{3}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{7}}),
              keep)) == Errc::helper_protocol);
    // Wrong version in a successful hello.
    auto wrong = ok_hello_body;
    wrong[5] = std::byte{2};
    CHECK(err(client_against(reply(1, 0, wrong), keep)) == Errc::helper_protocol);

    // Good hello, then a lying open reply (sector size 7): session must become broken.
    auto script = reply(1, 0, ok_hello_body);
    std::vector<std::byte> open_body(5, std::byte{0});
    for (std::byte b : {std::byte{7}, std::byte{0}, std::byte{0}, std::byte{0}})
        open_body.push_back(b);
    for (int i = 0; i < 8; ++i)
        open_body.push_back(i == 0 ? std::byte{1} : std::byte{0});
    open_body.push_back(std::byte{0});
    auto o = reply(2, 5, open_body);
    script.insert(script.end(), o.begin(), o.end());
    auto s = client_against(script, keep);
    CHECK(s.has_value());
    if (s) {
        DeviceInfo info{.path = "x", .model = {}, .serial = {}, .size_bytes = 512, .sector_size = 512};
        CHECK(err((*s)->open(info, Access::ReadOnly)) == Errc::helper_protocol);
        CHECK((*s)->broken());
        CHECK(err((*s)->open(info, Access::ReadOnly)) == Errc::elevation_failed); // fails fast now
    }
}

static void test_broken_session_releases_transport() {
    enum class Failure { transport_eof, reply_opcode, open_geometry, verify_body };
    const std::vector<std::byte> ok_hello_body = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                                  std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const DeviceInfo info{.path = "x", .model = {}, .serial = {}, .size_bytes = 512, .sector_size = 512};
    for (const auto failure :
         {Failure::transport_eof, Failure::reply_opcode, Failure::open_geometry, Failure::verify_body}) {
        auto script = reply(1, 0, ok_hello_body);
        std::vector<std::byte> open_body(5 + 4 + 8 + 1, std::byte{0});
        open_body[6] = std::byte{2}; // 512-byte sectors
        open_body[9] = std::byte{1}; // one sector
        if (failure == Failure::open_geometry) {
            open_body[5] = std::byte{7};
            open_body[6] = std::byte{0};
        }
        const auto opened = reply(2, 5, open_body);
        script.insert(script.end(), opened.begin(), opened.end());
        if (failure == Failure::reply_opcode || failure == Failure::verify_body) {
            std::vector<std::byte> body(5, std::byte{0});
            if (failure == Failure::verify_body)
                body.push_back(std::byte{1});
            const auto verified = reply(failure == Failure::reply_opcode ? 6 : 7, 5, std::move(body));
            script.insert(script.end(), verified.begin(), verified.end());
        }

        auto lifetime = std::make_shared<int>(0);
        const std::weak_ptr<int> transport_lifetime = lifetime;
        std::unique_ptr<MemStream> keep;
        auto session = client_against(std::move(script), keep, std::move(lifetime));
        CHECK(session.has_value());
        if (!session)
            continue;
        CHECK(!transport_lifetime.expired());
        if (failure == Failure::transport_eof)
            keep->close_write();
        auto device = (*session)->open(info, Access::ReadOnly);
        if (failure == Failure::open_geometry) {
            CHECK(err(device) == Errc::helper_protocol);
        } else {
            CHECK(device.has_value());
            if (!device)
                continue;
            const auto expected = failure == Failure::transport_eof ? Errc::elevation_failed : Errc::helper_protocol;
            CHECK(err((*device)->verify_binding()) == expected);
        }

        CHECK((*session)->broken());
        CHECK(transport_lifetime.expired()); // Neither the session nor a remaining disk may retain the transport.
        CHECK(err((*session)->open(info, Access::ReadOnly)) == Errc::elevation_failed);
    }
}

static void test_failed_close_releases_transport() {
    enum class Failure { reported_error, malformed_body, exception };
    for (const auto failure : {Failure::reported_error, Failure::malformed_body, Failure::exception}) {
        auto lifetime = std::make_shared<int>(0);
        const std::weak_ptr<int> transport_lifetime = lifetime;
        auto [client, server] = make_pipe(std::move(lifetime));
        auto *client_stream = client.get();
        std::vector<std::byte> hello_body(9, std::byte{0});
        hello_body[5] = std::byte{1};
        CHECK(server->write_all(reply(1, 0, std::move(hello_body))).has_value());
        std::vector<std::byte> open_body(18, std::byte{0});
        open_body[6] = std::byte{2}; // 512-byte sectors
        open_body[9] = std::byte{1}; // one sector
        CHECK(server->write_all(reply(2, 5, std::move(open_body))).has_value());
        std::vector<std::byte> close_body(5, std::byte{0});
        if (failure == Failure::reported_error) {
            close_body[0] = std::byte{3}; // rawdisk error category
            close_body[1] = static_cast<std::byte>(Errc::device_busy);
        }
        if (failure == Failure::malformed_body)
            close_body.push_back(std::byte{1});
        CHECK(server->write_all(reply(6, 5, std::move(close_body))).has_value());
        auto session = detail::RemoteSession::start(std::move(client));
        CHECK(session.has_value());
        if (!session)
            continue;
        const DeviceInfo info{.path = "x", .model = {}, .serial = {}, .size_bytes = 512, .sector_size = 512};
        auto device = (*session)->open(info, Access::ReadOnly);
        CHECK(device.has_value());
        if (!device)
            continue;
        client_stream->throw_next_write = failure == Failure::exception;
        device->reset();
        CHECK((*session)->broken());
        CHECK(transport_lifetime.expired()); // Cached sessions must not pin a handle whose Close failed.
    }
}

#if defined(__linux__)

static std::string run(const std::string &cmd) {
    std::string out;
    if (FILE *p = ::popen((cmd + " 2>/dev/null").c_str(), "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, p))
            out += buf;
        ::pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

static void write_file(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s << "\n";
}

// Builds /sys-like tree: block/<name> -> devices/.../usb1/1-1/.../block/<name>, dev/block/M:m -> same.
static fs::path fake_sysfs(const std::string &name, const std::string &majmin, std::uint64_t sectors512,
                           const std::string &serial, const std::string &type = "0", bool usb = true) {
    const auto root = temp_dir() / std::format("sys-{}-{}-{}-{}", name, serial, sectors512, type);
    fs::remove_all(root);
    const auto usbdev = root / "devices/pci0000:00/0000:00:14.0/usb1/1-1";
    const auto scsi = usbdev / "1-1:1.0/host6/target6:0:0/6:0:0:0";
    const auto blk = scsi / "block" / name;
    if (usb) {
        write_file(usbdev / "idVendor", "058f");
        write_file(usbdev / "idProduct", "6366");
        write_file(usbdev / "serial", serial);
    }
    write_file(scsi / "type", type);
    write_file(scsi / "vendor", "Generic ");
    write_file(scsi / "model", "CF Reader       ");
    write_file(blk / "size", std::to_string(sectors512));
    write_file(blk / "removable", "1");
    write_file(blk / "queue/logical_block_size", "512");
    fs::create_directory_symlink("../..", blk / "device");
    fs::create_directories(root / "block");
    fs::create_directory_symlink(fs::relative(blk, root / "block"), root / "block" / name);
    fs::create_directories(root / "dev/block");
    fs::create_directory_symlink(fs::relative(blk, root / "dev/block"), root / "dev/block" / majmin);
    // A partition next to it that must never be listed.
    write_file(blk / (name + "p1") / "partition", "1");
    return root;
}

static void test_linux_enumeration() {
    auto root = fake_sysfs("sdz", "8:400", 2048, "SER1");
    detail::set_sysfs_root(root);
    auto list = list_usb_devices();
    CHECK(list.has_value() && list->size() == 1);
    if (list && list->size() == 1) {
        const auto &d = list->front();
        CHECK(d.path == "/dev/sdz");
        CHECK(d.model == "Generic CF Reader");
        CHECK(d.serial == "SER1");
        CHECK(d.size_bytes == 2048 * 512);
        CHECK(d.sector_size == 512);
        CHECK(d.removable);
    }
    CHECK(!detail::linux_describe_block(root / "block/sdz/sdzp1").has_value()); // partition
    detail::set_sysfs_root(fake_sysfs("sdy", "8:401", 2048, "S", "5"));         // CD-ROM
    CHECK(list_usb_devices()->empty());
    detail::set_sysfs_root(fake_sysfs("sdx", "8:402", 0, "S")); // no card
    CHECK(list_usb_devices()->empty());
    detail::set_sysfs_root(fake_sysfs("sdw", "8:403", 2048, "S", "0", false)); // not USB
    CHECK(list_usb_devices()->empty());
    detail::set_sysfs_root(temp_dir() / "does-not-exist");
    CHECK(!list_usb_devices().has_value());
    detail::set_sysfs_root("/sys");
}

// Full production path on a real block device (loop), behind a fake USB sysfs.
static void test_linux_loop_device() {
    const char *enabled = std::getenv("RAWDISK_TEST_LOOP_DEVICE");
    if (!enabled || std::string_view(enabled) != "1") {
        std::puts("SKIP linux loop-device test (opt in with RAWDISK_TEST_LOOP_DEVICE=1)");
        return;
    }
    if (::geteuid() != 0 || run("command -v losetup").empty()) {
        std::puts("SKIP linux loop-device test (needs root and losetup)");
        return;
    }
    const auto backing = make_image("loop.img", 512 * 4096);
    const std::string loop = run("losetup -f --show " + backing.string());
    if (!loop.starts_with("/dev/loop")) {
        std::puts("SKIP linux loop-device test (losetup failed)");
        return;
    }
    const std::string name = loop.substr(5);
    const std::string majmin = run("cat /sys/class/block/" + name + "/dev");
    auto root = fake_sysfs(name, majmin, 4096, "LOOPSER");
    detail::set_sysfs_root(root);

    auto list = list_usb_devices();
    CHECK(list.has_value() && list->size() == 1);
    if (list && list->size() == 1) {
        const DeviceInfo info = list->front();
        {
            auto dev = open_device(info, Access::ReadWrite);
            CHECK(dev.has_value());
            if (dev) {
                const auto pat = pattern(512 * 8, 5);
                std::vector<std::byte> raw(pat.size() + 1);
                std::span<std::byte> un(raw.data() + 1, pat.size());
                std::copy(pat.begin(), pat.end(), un.begin());
                CHECK((*dev)->write(100, un).has_value()); // O_DIRECT through the bounce buffer
                CHECK((*dev)->flush().has_value());
                AlignedBuffer back(pat.size());
                CHECK((*dev)->read(100, back.span()).has_value());
                CHECK(std::memcmp(back.data(), pat.data(), pat.size()) == 0);
                // Exclusive open: a second writer is refused while we hold it.
                CHECK(err(open_device(info, Access::ReadWrite)) == Errc::device_busy);
            }
        }
        // Data really reached the backing file.
        std::ifstream f(backing, std::ios::binary);
        f.seekg(100 * 512);
        std::vector<char> got(512 * 8);
        f.read(got.data(), static_cast<std::streamsize>(got.size()));
        const auto pat = pattern(512 * 8, 5);
        CHECK(std::memcmp(got.data(), pat.data(), got.size()) == 0);

        // Stale selection: serial changed (another card) -> device_changed.
        DeviceInfo stale = info;
        stale.serial = "OTHER";
        CHECK(err(open_device(stale, Access::ReadOnly)) == Errc::device_changed);
        // Reported size larger than the device: identity check on the opened fd fails.
        detail::set_sysfs_root(fake_sysfs(name, majmin, 8192, "LOOPSER"));
        auto big = list_usb_devices();
        CHECK(big && big->size() == 1 && err(open_device(big->front(), Access::ReadOnly)) == Errc::device_changed);
        detail::set_sysfs_root(root);

        // Write protection.
        run("blockdev --setro " + loop);
        const auto wp = open_device(info, Access::ReadWrite);
        CHECK(err(wp) == Errc::write_protected);
        if (err(wp) != Errc::write_protected)
            std::fprintf(stderr, "  write-protect open gave: %s\n", err(wp).message().c_str());
        const auto ro = open_device(info, Access::ReadOnly);
        CHECK(ro.has_value());
        if (!ro)
            std::fprintf(stderr, "  read-only open gave: %s\n", ro.error().message().c_str());
        run("blockdev --setrw " + loop);
    }
    detail::set_sysfs_root("/sys");
    run("losetup -d " + loop);
}
#endif

int main() {
    test_errors();
    test_image_basic();
    test_image_validation();
    test_threads();
    test_remote_roundtrip();
    test_remote_connection_identity();
    test_remote_handle_limit();
    test_broken_session_releases_transport();
    test_failed_close_releases_transport();
    test_server_robustness();
    test_client_robustness();
#if defined(__linux__)
    test_linux_enumeration();
    test_linux_loop_device();
#endif
    std::error_code ec;
    fs::remove_all(temp_dir(), ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
