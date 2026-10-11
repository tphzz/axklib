// Tests for open flows, modes and locators. Image files pose as USB devices (FakeDevices), so
// every mode runs headless and deterministically.
#include "rawdisk/open/blocks.hpp"
#include "rawdisk/open/flow.hpp"
#include "rawdisk/open/locator.hpp"
#include "rawdisk/open/modes.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <utility>

using namespace rawdisk;
using namespace rawdisk::open;

struct FakeDevice {
    DeviceInfo info;
    std::filesystem::path image;
};

class FakeDevices final : public DeviceSource {
  public:
    std::vector<FakeDevice> devices;
    int opens = 0;
    std::vector<Access> accesses; // every open, in order
    std::weak_ptr<int> held;      // token handed out by hold()
    int rw_opens_while_held = 0, rw_opens = 0;
    std::function<void()> before_rw_open; // one-shot media change during exclusive reopen
    std::shared_ptr<void> hold() override {
        auto token = std::make_shared<int>(0);
        held = token;
        return token;
    }
    Result<std::vector<DeviceInfo>> list() override {
        std::vector<DeviceInfo> out;
        for (const auto &d : devices)
            out.push_back(d.info);
        return out;
    }
    Result<std::shared_ptr<BlockDevice>> open(const DeviceInfo &info, Access access) override {
        for (const auto &d : devices)
            if (d.info == info) { // like open_device: identity must still match
                ++opens;
                accesses.push_back(access);
                if (access == Access::ReadWrite) {
                    ++rw_opens;
                    rw_opens_while_held += held.expired() ? 0 : 1;
                    auto before = std::exchange(before_rw_open, {});
                    if (before)
                        before();
                }
                auto img = open_image(d.image, access, info.sector_size);
                if (!img)
                    return std::unexpected(img.error());
                return std::shared_ptr<BlockDevice>(std::move(*img));
            }
        return std::unexpected(make_error_code(Errc::device_changed));
    }
};

static FakeDevice fake(const std::filesystem::path &image, std::string serial, std::string path = "/dev/sdz") {
    const auto size = std::filesystem::file_size(image);
    return {{.path = std::move(path),
             .model = "Generic CF Reader",
             .serial = std::move(serial),
             .size_bytes = size,
             .sector_size = 512,
             .removable = true},
            image};
}

static std::shared_ptr<const Mode> mode(std::string_view id) { return builtin_modes().find(id); }

static const Option *option(const Step &s, std::string_view field, std::string_view id) {
    const Field *f = s.field(field);
    if (!f)
        return nullptr;
    for (const auto &o : f->options)
        if (o.id == id)
            return &o;
    return nullptr;
}

// Card: disk 0 (512 B) at 2048, disk 4 (2048 B) at 20480, disk 5 (256 B, unsupported),
// disk 6 reaching the card end (covers the configuration: danger).
static std::filesystem::path v6_card(const std::string &name) {
    return make_card(
        name, 65536,
        v6_block({{.id = 0, .start = 2048, .sectors = 16384, .bps = 512, .vendor = "SEAGATE", .product = "ST225N"},
                  {.id = 4, .start = 20480, .sectors = 4096, .bps = 2048},
                  {.id = 5, .start = 40000, .sectors = 10, .bps = 256},
                  {.id = 6, .start = 60000, .sectors = 9999, .bps = 512}}),
        65534);
}

static void test_registry() {
    const Registry r = builtin_modes();
    CHECK(r.list().size() == 6);
    for (const char *id : {"image", "raw", "scsi2sd-v6", "scsi2sd-v5", "scsi2sd-v6-image", "scsi2sd-xml-image"})
        CHECK(r.find(id) != nullptr);
    Registry copy = r;
    CHECK(err(copy.add(r.find("raw"))) == Errc::invalid_argument); // duplicate id
    CHECK(err(copy.add(nullptr)) == Errc::invalid_argument);
    CHECK(copy.add(make_mode({"custom", "Custom", ""},
                             [] {
                                 std::vector<std::unique_ptr<Block>> v;
                                 v.push_back(select_image());
                                 return v;
                             }))
              .has_value());
    CHECK(copy.find("custom") != nullptr);
}

static void test_image_mode() {
    const auto p = make_card("image & query=value.img", 4096);
    auto flow = Flow::start(mode("image"), Access::ReadOnly);
    CHECK(flow.has_value());
    if (!flow)
        return;
    Flow &f = **flow;
    CHECK(!f.ready() && f.step().id == "image" && f.step_total() == 1);
    CHECK(f.step().field("path") && f.step().field("path")->kind == FieldKind::file);
    CHECK(f.step().field("sector_size") && f.step().field("sector_size")->default_value == "512");
    CHECK(err(f.submit({})) == Errc::flow_missing_answer);
    auto e = f.submit({{"path", (test_dir() / "missing.img").string()}});
    CHECK(!e && e.error().field == "path" && e.error().code == std::errc::no_such_file_or_directory);
    CHECK(err(f.submit({{"path", p.string()}, {"sector_size", "3"}})) == Errc::flow_stale_selection);
    CHECK(err(f.submit({{"path", p.string()}, {"bogus", "1"}})) == Errc::flow_invalid_answer);
    CHECK(f.submit({{"path", p.string()}}).has_value());
    CHECK(f.ready() && f.preview().sector_count == 4096 && f.preview().sector_size == 512);
    const auto locator = parse_locator(f.locator());
    const Answers expected{{"image.path", p.string()}, {"image.sector_size", "512"}};
    CHECK(locator && locator->mode == "image" && locator->answers == expected);
    auto o = f.open();
    CHECK(o.has_value() && o->device->sector_count() == 4096 && !o->device->writable());
    o = {};
    flow = {};

    // A ReadWrite image flow discovers read-only: another reader is still allowed until open().
    auto w = Flow::start(mode("image"), Access::ReadWrite);
    CHECK(w && (*w)->submit({{"path", p.string()}}).has_value());
    CHECK(open_image(p, Access::ReadOnly).has_value());
    auto wo = (*w)->open();
    CHECK(wo.has_value() && wo->device->writable());
    CHECK(err(open_image(p, Access::ReadOnly)) == Errc::device_busy);
    wo = {};

    // 4096-byte sectors: the size must fit.
    auto g = Flow::start(mode("image"), Access::ReadOnly);
    auto bad = (*g)->submit({{"path", make_card("odd.img", 3).string()}, {"sector_size", "1024"}});
    CHECK(!bad && bad.error().field == "sector_size" && bad.error().code == Errc::bad_length);
}

static void test_raw_mode() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(make_card("raw.img", 2048), "SER1"));
    auto flow = Flow::start(mode("raw"), Access::ReadWrite, {src});
    CHECK(flow.has_value());
    if (!flow)
        return;
    Flow &f = **flow;
    const Field *dev = f.step().field("device");
    CHECK(dev && dev->options.size() == 1 && f.step().refreshable);
    const std::string key = dev->options[0].id;
    CHECK(key == device_key(src->devices[0].info) && key.find("SER1") != std::string::npos);
    CHECK(f.submit({{"device", key}}).has_value() && f.ready());
    CHECK(f.preview().access == Access::ReadWrite && f.preview().sector_count == 2048);
    // Discovery never opens for writing, even in a ReadWrite flow.
    CHECK(!src->accesses.empty() && std::ranges::all_of(src->accesses, [](Access a) { return a == Access::ReadOnly; }));
    const int before = src->opens;
    auto o = f.open();
    CHECK(o.has_value() && o->device->writable());
    CHECK(src->opens == before + 1); // reopened for writing in the final pass
    CHECK(src->accesses.back() == Access::ReadWrite);
    // Privileges were held while reopening (one UAC prompt on Windows), and released afterwards.
    CHECK(src->rw_opens == 1 && src->rw_opens_while_held == 1 && src->held.expired());
    if (o) {
        std::vector<std::byte> w(512, std::byte{9});
        CHECK(o->device->write(0, w).has_value());
    }

    // Identical readers without serial are told apart by path.
    auto two = std::make_shared<FakeDevices>();
    two->devices.push_back(fake(make_card("a.img", 64), "", "/dev/sdx"));
    two->devices.push_back(fake(make_card("b.img", 64), "", "/dev/sdy"));
    auto g = Flow::start(mode("raw"), Access::ReadOnly, {two});
    CHECK(g && (*g)->step().field("device")->options.size() == 2);
    CHECK(g && (*g)->step().field("device")->options[0].id.ends_with("@/dev/sdx"));
}

static void test_v6_mode_read() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(v6_card("v6.img"), "ABC"));
    auto flow = Flow::start(mode("scsi2sd-v6"), Access::ReadOnly, {src});
    CHECK(flow.has_value());
    if (!flow)
        return;
    Flow &f = **flow;
    CHECK(f.submit({{"device", device_key(src->devices[0].info)}}).has_value());
    // The configuration step had no fields: applied automatically, now at the volume step.
    CHECK(!f.ready() && f.step().id == "volume" && f.step_number() == 3);
    CHECK(option(f.step(), "volume", "0") && option(f.step(), "volume", "0")->enabled);
    CHECK(option(f.step(), "volume", "0")->label == "SCSI ID 0: SEAGATE ST225N");
    const Option *o5 = option(f.step(), "volume", "5");
    CHECK(o5 && !o5->enabled && o5->disabled_reason.find("not a multiple of 512") != std::string::npos);
    CHECK(option(f.step(), "volume", "6") && option(f.step(), "volume", "6")->enabled); // read-only: fine
    CHECK(!f.step().field("acknowledge"));
    CHECK(err(f.submit({{"volume", "5"}})) == Errc::flow_invalid_answer);
    CHECK(f.submit({{"volume", "4"}}).has_value() && f.ready());
    CHECK(f.preview().sector_size == 2048 && f.preview().sector_count == 4096);
    CHECK(f.preview().description.starts_with("SCSI ID 4: ACME DISK on Generic CF Reader"));
    auto o = f.open();
    CHECK(o.has_value());
    if (o) {
        AlignedBuffer b(2048);
        CHECK(o->device->read(0, b.span()).has_value() &&
              std::to_integer<unsigned>(b.data()[0]) == pattern_at(20480, 0));
    }
    // Locator round trip.
    auto l = parse_locator(f.locator());
    CHECK(l && l->mode == "scsi2sd-v6" && l->answers.at("volume.volume") == "4" && l->answers.size() == 2);
    auto again = open::open(f.locator(), Access::ReadOnly, builtin_modes(), {src});
    CHECK(again.has_value() && again->device->sector_size() == 2048);
}

static void test_v6_mode_write() {
    auto src = std::make_shared<FakeDevices>();
    const auto p = v6_card("v6w.img");
    src->devices.push_back(fake(p, "ABC"));
    const std::string dev = device_key(src->devices[0].info);

    auto flow = Flow::start(mode("scsi2sd-v6"), Access::ReadWrite, {src});
    Flow &f = **flow;
    CHECK(f.submit({{"device", dev}}).has_value());
    const Field *ack = f.step().field("acknowledge");
    CHECK(ack && !ack->required);
    CHECK(option(f.step(), "volume", "6") && has_severity(option(f.step(), "volume", "6")->notices, Severity::danger));
    auto no_ack = f.submit({{"volume", "6"}});
    CHECK(!no_ack && no_ack.error().field == "acknowledge" && no_ack.error().code == Errc::flow_missing_answer);
    CHECK(f.submit({{"volume", "6"}, {"acknowledge", "yes"}}).has_value());
    auto o = f.open();
    CHECK(o.has_value() && o->device->writable());
    if (o) {
        const std::uint64_t last = o->device->sector_count();
        std::vector<std::byte> w(1024, std::byte{0x77});
        CHECK(err(o->device->write(last - 2, w)) == Errc::layout_conflict); // config sectors stay protected
        CHECK(o->device->write(last - 4, w).has_value());
    }
    o = {}; // release before re-reading
    auto reread =
        open::open("scsi2sd-v6?device.device=" + std::string() + "x", Access::ReadOnly, builtin_modes(), {src});
    CHECK(!reread && reread.error().step == "device" && reread.error().code == Errc::flow_stale_selection);
    // Disk 0 without danger needs no acknowledgement.
    auto d0 = run(mode("scsi2sd-v6"), {{"device.device", dev}, {"volume.volume", "0"}}, Access::ReadWrite, {src});
    CHECK(d0.has_value());
}

static void test_card_swapped_before_open() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(v6_card("swap1.img"), "ONE"));
    auto flow = Flow::start(mode("scsi2sd-v6"), Access::ReadWrite, {src});
    Flow &f = **flow;
    CHECK(f.submit({{"device", device_key(src->devices[0].info)}}).has_value());
    CHECK(f.submit({{"volume", "0"}}).has_value() && f.ready());
    src->devices[0] = fake(v6_card("swap2.img"), "TWO"); // another card, same reader
    auto o = f.open();
    CHECK(!o && o.error().step == "device");
    CHECK(!f.ready() && f.step().id == "device"); // the flow went back to where the user must act
    CHECK(f.submit({{"device", device_key(src->devices[0].info)}}).has_value());
    CHECK(f.submit({{"volume", "0"}}).has_value());
    CHECK(f.open().has_value());

    // Layout changed between discovery and open: detected by the shape check.
    auto src2 = std::make_shared<FakeDevices>();
    const auto p = v6_card("relayout.img");
    src2->devices.push_back(fake(p, "L"));
    auto g = Flow::start(mode("scsi2sd-v6"), Access::ReadWrite, {src2});
    CHECK((*g)->submit({{"device", device_key(src2->devices[0].info)}}).has_value());
    CHECK((*g)->submit({{"volume", "0"}}).has_value());
    {
        auto blocked = open_image(p, Access::ReadWrite);
        CHECK(!blocked && blocked.error() == Errc::device_busy);
    }
    // Discovery holds a shared lock. Change the card only in the gap before the final reopen.
    int layout_changes = 0;
    src2->before_rw_open = [&] {
        auto file = open_image(p, Access::ReadWrite);
        CHECK(file.has_value());
        if (!file)
            return;
        const auto moved = v6_block({{.id = 0, .start = 4096, .sectors = 16384, .bps = 512}});
        auto written = (*file)->write(65534, std::as_bytes(std::span(moved)));
        CHECK(written.has_value());
        if (!written)
            return;
        auto flushed = (*file)->flush();
        CHECK(flushed.has_value());
        if (flushed)
            ++layout_changes;
    };
    auto o2 = (*g)->open();
    CHECK(layout_changes == 1);
    CHECK(!src2->before_rw_open);
    CHECK(!o2 && (o2.error().code == Errc::device_changed) && o2.error().step == "scsi2sd");
}

static void test_v6_no_config() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(make_card("blank.img", 8192), "B"));
    auto flow = Flow::start(mode("scsi2sd-v6"), Access::ReadOnly, {src});
    Flow &f = **flow;
    CHECK(f.submit({{"device", device_key(src->devices[0].info)}}).has_value());
    CHECK(f.step().id == "scsi2sd" && f.step().field("use_default") &&
          has_notice(f.step().notices, "s2s.firmware_default"));
    auto no = f.submit({{"use_default", "no"}});
    CHECK(!no && no.error().code == Errc::layout_not_found && no.error().field == "use_default");
    CHECK(err(f.submit({{"use_default", "maybe"}})) == Errc::flow_invalid_answer);
    CHECK(f.submit({{"use_default", "yes"}}).has_value() && f.step().id == "volume");
    CHECK(option(f.step(), "volume", "0") && option(f.step(), "volume", "0")->enabled);
    // Back skips nothing here (both previous steps had fields).
    CHECK(f.can_go_back() && f.back().has_value() && f.step().id == "scsi2sd");
    CHECK(f.back().has_value() && f.step().id == "device" && !f.can_go_back());
    CHECK(err(f.back()) == Errc::flow_invalid_answer);

    // Cancelled scan (no config -> the last 8 MiB are searched).
    CancellationSource stop;
    stop.cancel();
    auto g = Flow::start(mode("scsi2sd-v6"), Access::ReadOnly, {src});
    CHECK(err((*g)->submit({{"device", device_key(src->devices[0].info)}}, stop.token())) ==
          std::errc::operation_canceled);
}

static void test_v5_mode() {
    auto src = std::make_shared<FakeDevices>();
    const auto card = make_card("v5.img", 8192); // V5: nothing on the card
    src->devices.push_back(fake(card, "V5"));
    const auto xml = test_dir() / "board.xml";
    {
        std::ofstream(xml) << R"(<SCSI2SD><SCSITarget id="2"><enabled>true</enabled><sdSectorStart>100</sdSectorStart>
            <scsiSectors>8092</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget></SCSI2SD>)";
    }
    const auto junk = test_dir() / "junk.xml";
    {
        std::ofstream(junk) << "not xml";
    }

    auto flow = Flow::start(mode("scsi2sd-v5"), Access::ReadWrite, {src});
    Flow &f = **flow;
    CHECK(f.submit({{"device", device_key(src->devices[0].info)}}).has_value());
    CHECK(f.step().id == "xml" && f.step().field("file") && !f.step().field("board")); // board fixed to V5
    auto bad = f.submit({{"file", junk.string()}});
    CHECK(!bad && bad.error().field == "file" && bad.error().message == "not a valid scsi2sd-util layout file");
    CHECK(f.submit({{"file", xml.string()}}).has_value() && f.step().id == "volume");
    // V5: reaching the card end is fine, no protection, no danger.
    CHECK(option(f.step(), "volume", "2") && option(f.step(), "volume", "2")->notices.empty() &&
          !f.step().field("acknowledge"));
    CHECK(f.submit({{"volume", "2"}}).has_value());
    auto o = f.open();
    CHECK(o.has_value());
    if (o) {
        std::vector<std::byte> w(512, std::byte{1});
        CHECK(o->device->write(o->device->sector_count() - 1, w).has_value()); // last card sector: allowed on V5
    }
    // Image composition with a board choice (default v6: last 2 sectors protected).
    o = {};
    // Disk 2 reaches the card end, i.e. covers the V6 configuration sectors: danger, needs acknowledgement.
    auto unacked =
        run(mode("scsi2sd-xml-image"),
            {{"image.path", card.string()}, {"xml.file", xml.string()}, {"volume.volume", "2"}}, Access::ReadWrite);
    CHECK(!unacked && unacked.error().field == "acknowledge");
    auto img = run(mode("scsi2sd-xml-image"),
                   {{"image.path", card.string()},
                    {"xml.file", xml.string()},
                    {"volume.volume", "2"},
                    {"volume.acknowledge", "yes"}},
                   Access::ReadWrite);
    CHECK(img.has_value());
    if (img) {
        std::vector<std::byte> w(512, std::byte{1});
        CHECK(err(img->device->write(img->device->sector_count() - 1, w)) == Errc::layout_conflict);
    }
}

static void test_run_errors() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(v6_card("run.img"), "R"));
    const std::string dev = device_key(src->devices[0].info);
    auto missing = run(mode("scsi2sd-v6"), {{"device.device", dev}}, Access::ReadOnly, {src});
    CHECK(!missing && missing.error().code == Errc::flow_missing_answer && missing.error().step == "volume" &&
          missing.error().field == "volume");
    auto unknown = run(mode("scsi2sd-v6"), {{"device.device", dev}, {"volume.volume", "0"}, {"volume.extra", "1"}},
                       Access::ReadOnly, {src});
    CHECK(!unknown && unknown.error().code == Errc::flow_invalid_answer);
    auto disabled = run(mode("scsi2sd-v6"), {{"device.device", dev}, {"volume.volume", "5"}}, Access::ReadOnly, {src});
    CHECK(!disabled && disabled.error().code == Errc::flow_invalid_answer && disabled.error().field == "volume");
    auto gone = run(mode("scsi2sd-v6"), {{"device.device", "usb:x:y:1:512"}}, Access::ReadOnly, {src});
    CHECK(!gone && gone.error().code == Errc::flow_stale_selection && gone.error().step == "device");
    auto bad_mode = open::open("nope?a.b=c", Access::ReadOnly);
    CHECK(!bad_mode && bad_mode.error().code == Errc::locator_invalid);
}

static void test_locator_syntax() {
    Locator l{"scsi2sd-v5", {{"xml.file", "/tmp/fixture/my board&co=1.xml"}, {"device.device", "usb:A%20B:S:1:512"}}};
    const std::string s = format_locator(l);
    CHECK(s == "scsi2sd-v5?device.device=usb:A%2520B:S:1:512&xml.file=/tmp/fixture/my%20board%26co%3D1.xml");
    auto back = parse_locator(s);
    CHECK(back.has_value() && back->mode == l.mode && back->answers == l.answers);
    CHECK(parse_locator("raw").has_value() && parse_locator("raw")->answers.empty());
    CHECK(parse_locator("image?image.path=%C3%A4.img")->answers.at("image.path") == "\xC3\xA4.img");
    for (const char *bad : {"", "Raw", "raw?", "raw?a=b", "raw?.b=c", "raw?a.=c", "raw?a.b.c=d", "raw?a.b=c&",
                            "raw?a.b=c&a.b=d", "raw?a.b=%4", "raw?a.b=%zz", "raw?a.b=%00", "raw?a.b=x y", "raw?a.b=x+y",
                            "raw?a.b=x=y", "raw?A.b=c", "ra w", "raw?a.b"})
        CHECK(err(parse_locator(bad)) == Errc::locator_invalid);
    CHECK(err(parse_locator(std::string(9000, 'a'))) == Errc::locator_invalid);
    std::mt19937 rng(3);
    const std::string alphabet = "abz09-._~:/@?&=%+ AZ\x01\xff";
    int ok = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string t;
        for (std::size_t k = rng() % 40; k-- > 0;)
            t += alphabet[rng() % alphabet.size()];
        if (auto p = parse_locator(t)) {
            ++ok;
            CHECK(parse_locator(format_locator(*p)).value().answers == p->answers); // canonical form reparses
        }
    }
    CHECK(ok >= 0);
}

static void test_refresh() {
    auto src = std::make_shared<FakeDevices>();
    auto flow = Flow::start(mode("raw"), Access::ReadOnly, {src});
    CHECK(flow && (*flow)->step().field("device")->options.empty() &&
          has_notice((*flow)->step().notices, "device.none"));
    src->devices.push_back(fake(make_card("hot.img", 64), "HOT"));
    CHECK((*flow)->refresh().has_value() && (*flow)->step().field("device")->options.size() == 1);
}

static void test_selection_limits() {
    auto src = std::make_shared<FakeDevices>();
    src->devices.push_back(fake(make_card("limit.img", 64), "LIMIT"));
    src->devices[0].info.size_bytes = 9'000'000'000;
    const auto limited = builtin_modes({.max_device_bytes = 9'000'000'000, .max_volume_bytes = 8'388'608});
    auto f = Flow::start(limited.find("raw"), Access::ReadWrite, {src});
    CHECK(f && (*f)->step().field("device")->options.size() == 1);
    ++src->devices[0].info.size_bytes;
    CHECK((*f)->refresh().has_value());
    CHECK((*f)->step().field("device")->options.empty());
    CHECK(has_notice((*f)->step().notices, "device.filtered"));
    CHECK(!run(limited.find("raw"), {{"device.device", device_key(src->devices[0].info)}}, Access::ReadWrite, {src}));
    CHECK(src->opens == 0); // supplied locators cannot bypass the cap
    auto unlimited = Flow::start(mode("raw"), Access::ReadOnly, {src});
    CHECK(unlimited && (*unlimited)->step().field("device")->options.size() == 1);

    src->devices = {fake(v6_card("limit-v6.img"), "CARRIER")};
    const auto cap = builtin_modes({.max_device_bytes = 1024, .max_volume_bytes = 8'388'608});
    auto v6 = Flow::start(cap.find("scsi2sd-v6"), Access::ReadWrite, {src});
    CHECK(v6 && (*v6)->step().field("device")->options.size() == 1); // carrier is not capped
    CHECK((*v6)->submit({{"device", device_key(src->devices[0].info)}}).has_value());
    CHECK(option((*v6)->step(), "volume", "0") != nullptr); // exactly the cap
    CHECK((*v6)->submit({{"volume", "0"}}).has_value());
    CHECK((*v6)->preview().usb == src->devices[0].info);
    CHECK((*v6)->preview().offset_bytes == 2048 * 512);
    auto opened = (*v6)->open();
    CHECK(opened && opened->offset_bytes == 2048 * 512 && opened->usb == src->devices[0].info);
    opened = {};
    v6 = {};
    const auto smaller = builtin_modes({.max_volume_bytes = 8'388'607});
    auto too_big =
        run(smaller.find("scsi2sd-v6"), {{"device.device", device_key(src->devices[0].info)}, {"volume.volume", "0"}},
            Access::ReadOnly, {src});
    CHECK(!too_big);
    // File images remain unrestricted, including image-backed virtual disks.
    CHECK(run(cap.find("image"), {{"image.path", src->devices[0].image.string()}}, Access::ReadOnly).has_value());

    const auto xml = test_dir() / "limit-v5.xml";
    {
        std::ofstream(xml) << R"(<SCSI2SD><SCSITarget id="1"><enabled>true</enabled><sdSectorStart>100</sdSectorStart>
            <scsiSectors>16384</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget></SCSI2SD>)";
    }
    const Answers v5_answers{
        {"device.device", device_key(src->devices[0].info)}, {"xml.file", xml.string()}, {"volume.volume", "1"}};
    CHECK(run(cap.find("scsi2sd-v5"), v5_answers, Access::ReadOnly, {src}).has_value());
    CHECK(!run(smaller.find("scsi2sd-v5"), v5_answers, Access::ReadOnly, {src})); // no invented V5 geometry/truncation

    src->devices = {fake(make_card("reopen-limit.img", 64), "REOPEN")};
    const auto small = builtin_modes({.max_device_bytes = 64 * 512});
    auto replay = Flow::start(small.find("raw"), Access::ReadWrite, {src});
    CHECK(replay && (*replay)->submit({{"device", device_key(src->devices[0].info)}}).has_value());
    src->before_rw_open = [&] { std::filesystem::resize_file(src->devices[0].image, 128 * 512); };
    CHECK(!(*replay)->open()); // final opened geometry cannot bypass the cap either
}

int main() {
    test_registry();
    test_image_mode();
    test_raw_mode();
    test_v6_mode_read();
    test_v6_mode_write();
    test_card_swapped_before_open();
    test_v6_no_config();
    test_v5_mode();
    test_run_errors();
    test_locator_syntax();
    test_refresh();
    test_selection_limits();
    return finish_tests();
}
