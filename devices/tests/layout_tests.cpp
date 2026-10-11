// Tests for slices, volumes and the SCSI2SD typed API, on card images.
#include "rawdisk/scsi2sd.hpp"
#include "rawdisk/slice.hpp"
#include "rawdisk/volume.hpp"
#include "test_util.hpp"

#include <utility>

using namespace rawdisk;
namespace s2s = rawdisk::scsi2sd;

static void test_slice() {
    auto parent = open_shared(make_card("slice.img", 4096));
    CHECK(parent != nullptr);
    if (!parent)
        return;
    auto s = make_slice(parent, 100, 10, 1024, {.writable = true, .protected_ranges = {{110, 112}}});
    CHECK(s.has_value());
    if (!s)
        return;
    CHECK((*s)->sector_size() == 1024 && (*s)->sector_count() == 10 && (*s)->writable());
    AlignedBuffer b(1024);
    CHECK((*s)->read(1, b.span()).has_value());
    CHECK(std::to_integer<unsigned>(b.data()[0]) == pattern_at(102, 0));
    CHECK(std::to_integer<unsigned>(b.data()[512]) == pattern_at(103, 0));
    std::vector<std::byte> w(2048, std::byte{0x42});
    CHECK(err((*s)->write(4, w)) == Errc::layout_conflict); // 108..111 touches 110..111: nothing written
    CHECK((*s)->read(4, b.span()).has_value() && std::to_integer<unsigned>(b.data()[0]) == pattern_at(108, 0));
    CHECK((*s)->write(2, w).has_value()); // 104..107
    CHECK(err((*s)->read(9, w)) == Errc::out_of_range);

    // Slice of a slice: second sector of the slice above = parent sectors 102..103.
    auto inner = make_slice(*s, 1, 1, 1024);
    CHECK(inner.has_value() && (*inner)->read(0, b.span()).has_value() &&
          std::to_integer<unsigned>(b.data()[0]) == pattern_at(102, 0));

    CHECK(err(make_slice(parent, 0, 1, 256)) == Errc::invalid_argument);
    CHECK(err(make_slice(parent, 0, 1, 768)) == Errc::invalid_argument); // not a multiple of 512
    CHECK(err(make_slice(parent, 4095, 2, 512)) == Errc::out_of_range);
    CHECK(err(make_slice(parent, UINT64_MAX, 1, 512)) == Errc::out_of_range);
    CHECK(err(make_slice(parent, 0, UINT64_MAX, 1024)) == Errc::out_of_range);
    CHECK(err(make_slice(parent, 0, 0, 512)) == Errc::invalid_argument);
    CHECK(err(make_slice(nullptr, 0, 1, 512)) == Errc::invalid_argument);
    parent.reset();
    s->reset();
    inner->reset();
    auto ro = open_shared(test_dir() / "slice.img", Access::ReadOnly);
    CHECK(err(make_slice(ro, 0, 1, 512, {.writable = true})) == Errc::read_only_handle);
}

static VolumeSet sample_set() {
    VolumeSet set;
    set.scheme = "test";
    set.parent_sector_size = 512;
    set.parent_sectors = 4096;
    set.protected_ranges = {{4094, 4096}};
    set.volumes = {
        {.id = "a", .label = "A", .range = {0, 1000}, .sector_size = 512, .sector_count = 1000},
        {.id = "b", .label = "B", .range = {900, 1900}, .sector_size = 512, .sector_count = 1000},
        {.id = "c", .label = "C", .range = {2000, 4096}, .sector_size = 2048, .sector_count = 524},
        {.id = "u", .label = "U", .range = {3000, 3001}, .sector_size = 256, .sector_count = 0, .usable = false},
        {.id = "d", .label = "D1", .range = {10, 11}, .sector_size = 512, .sector_count = 1},
        {.id = "d", .label = "D2", .range = {12, 13}, .sector_size = 512, .sector_count = 1}};
    return set;
}

static void test_volume() {
    auto parent = open_shared(make_card("vol.img", 4096));
    const VolumeSet set = sample_set();
    CHECK(set.find("a") != nullptr && set.find("d") == nullptr && set.find("x") == nullptr);
    CHECK(err(open_volume(parent, set, "x", Access::ReadOnly)) == Errc::invalid_argument);
    CHECK(err(open_volume(parent, set, "d", Access::ReadOnly)) == Errc::layout_invalid);   // ambiguous
    CHECK(err(open_volume(parent, set, "u", Access::ReadOnly)) == Errc::layout_invalid);   // unusable
    CHECK(err(open_volume(parent, set, "a", Access::ReadWrite)) == Errc::layout_conflict); // overlaps b
    CHECK(err(check_writable(set, "a")) == Errc::layout_conflict);
    CHECK(check_writable(set, "a", {.allow_overlap = true}).has_value());
    CHECK(open_volume(parent, set, "a", Access::ReadOnly).has_value());
    // c overlaps u (unusable volumes still occupy space).
    CHECK(err(check_writable(set, "c")) == Errc::layout_conflict);
    auto c = open_volume(parent, set, "c", Access::ReadWrite, {.allow_overlap = true});
    CHECK(c.has_value() && (*c)->sector_size() == 2048);
    if (c) {
        std::vector<std::byte> w(2048, std::byte{1});
        CHECK(err((*c)->write(523, w)) == Errc::layout_conflict); // parent 4092..4095 hits the protected range
        CHECK((*c)->write(522, w).has_value());
    }
    auto c2 = open_volume(parent, set, "c", Access::ReadWrite, {.allow_overlap = true, .protect = false});
    CHECK(c2.has_value());
    // A set for another geometry is refused.
    VolumeSet other = set;
    other.parent_sectors = 8192;
    CHECK(err(open_volume(parent, other, "a", Access::ReadOnly)) == Errc::device_changed);
    CHECK(err(open_volume(nullptr, set, "a", Access::ReadOnly)) == Errc::invalid_argument);

    std::vector<Notice> ns = {{Severity::warning, "x.w", "w"}};
    CHECK(has_notice(ns, "x.w") && !has_notice(ns, "x.y"));
    CHECK(has_severity(ns, Severity::warning) && !has_severity(ns, Severity::danger));
}

static std::vector<std::byte> bytes(const std::vector<unsigned char> &v) {
    std::vector<std::byte> b(v.size());
    std::memcpy(b.data(), v.data(), v.size());
    return b;
}

static void test_v6_codec() {
    auto raw = bytes(v6_block({{.id = 3, .start = 2048, .sectors = 1000, .bps = 1024, .vendor = "SEAGATE"},
                               {.id = 5, .enabled = false, .start = 9, .sectors = 9}}));
    auto c = s2s::v6::decode(raw);
    CHECK(c.has_value());
    if (!c)
        return;
    const auto &t = c->targets[0];
    CHECK(t.enabled && t.scsi_id == 3 && t.sd_sector_start == 2048 && t.scsi_sectors == 1000 &&
          t.bytes_per_sector == 1024);
    CHECK(t.vendor == "SEAGATE" && t.product == "DISK" && t.index == 0);
    CHECK(!c->targets[1].enabled && c->targets[1].scsi_id == 5);
    CHECK(c->board.selection_delay == 255 && c->board.flags6 == s2s::kFlag6Terminator);
    const auto back = s2s::v6::encode(*c);
    CHECK(std::equal(back.begin(), back.end(), raw.begin()));

    // Edits are applied, everything else is kept.
    auto edited = *c;
    edited.targets[0].scsi_sectors = 77;
    edited.targets[0].vendor = "QUANTUM-LONGER-THAN-8";
    const auto e = s2s::v6::encode(edited);
    auto again = s2s::v6::decode(e);
    CHECK(again && again->targets[0].scsi_sectors == 77 && again->targets[0].vendor == "QUANTUM-" &&
          again->targets[0].product == "DISK");

    // Bit-exact for arbitrary content (reserved bytes, undefined flag bits, odd strings).
    std::mt19937 rng(99);
    int exact = 0;
    for (int i = 0; i < 2000; ++i) {
        std::vector<std::byte> r(1024);
        for (auto &x : r)
            x = static_cast<std::byte>(rng());
        std::memcpy(r.data(), "BCFG", 4);
        auto d = s2s::v6::decode(r);
        if (!d)
            continue;
        const auto en = s2s::v6::encode(*d);
        exact += std::equal(en.begin(), en.end(), r.begin()) ? 1 : 0;
    }
    CHECK(exact == 2000);

    raw[0] = std::byte{'X'};
    CHECK(err(s2s::v6::decode(raw)) == Errc::layout_not_found);
    CHECK(err(s2s::v6::decode(std::span(raw).first(1000))) == Errc::layout_invalid);

    const auto def = s2s::v6::default_config();
    CHECK(def.targets[0].enabled && def.targets[0].scsi_sectors == 4194303 && def.targets[0].bytes_per_sector == 512);
    CHECK(s2s::v6::config_lba(8192) == 8190u && !s2s::v6::config_lba(1));
    CHECK(s2s::v6::misplaced_save_lba(8192) == 7168u && !s2s::v6::misplaced_save_lba(1000));
}

static void test_v6_discovery() {
    // Primary only.
    {
        auto card = open_shared(make_card("p.img", 8192, v6_block({{.id = 0, .start = 0, .sectors = 4000}}), 8190),
                                Access::ReadOnly);
        auto raw = s2s::v6::read_config(*card, 8190);
        CHECK(raw.has_value() && std::memcmp(raw->data(), "BCFG", 4) == 0);
        auto d = s2s::v6::discover(*card);
        CHECK(d.has_value() && d->from_card() && d->candidates.size() == 1);
        CHECK(d && d->candidates[0].kind == s2s::v6::CandidateKind::primary && d->volumes.volumes.size() == 1);
        CHECK(d && d->notices.empty() && d->volumes.protected_ranges.size() == 1);
    }
    // Only the misplaced-save copy: the firmware uses its default.
    {
        auto card = open_shared(
            make_card("m.img", 8192, v6_block({{.id = 2, .start = 0, .sectors = 100}}), 8192 - 1024), Access::ReadOnly);
        auto d = s2s::v6::discover(*card);
        CHECK(d.has_value() && !d->from_card());
        CHECK(d && d->candidates.size() == 1 && d->candidates[0].kind == s2s::v6::CandidateKind::misplaced_save);
        CHECK(d && has_notice(d->notices, "s2s.firmware_default") && has_notice(d->notices, "s2s.misplaced_copy"));
        CHECK(d && d->volumes.volumes.size() == 1 && d->volumes.volumes[0].id == "0"); // the default disk
        CHECK(d && has_notice(d->volumes.notices, "s2s.firmware_default"));
    }
    // Elsewhere: used, with a danger notice, and its position is protected too.
    {
        auto card = open_shared(make_card("e.img", 20000, v6_block({{.id = 4, .start = 0, .sectors = 100}}), 15000),
                                Access::ReadOnly);
        auto d = s2s::v6::discover(*card);
        CHECK(d.has_value() && d->from_card() && d->candidates[*d->used].lba == 15000);
        CHECK(d && has_notice(d->notices, "s2s.config_elsewhere"));
        CHECK(d && d->volumes.protected_ranges.size() == 2);
        // Scan disabled: not found, default used.
        auto n = s2s::v6::discover(*card, {.scan = s2s::v6::ScanOptions::Scan::never});
        CHECK(n.has_value() && !n->from_card());
        // Cancelled scan.
        CancellationSource stop;
        stop.cancel();
        CHECK(err(s2s::v6::discover(*card, {.stop = stop.token()})) == std::errc::operation_canceled);
    }
    // Primary plus a different stale copy.
    {
        auto p = make_card("pm.img", 8192, v6_block({{.id = 0, .start = 0, .sectors = 100}}), 8190);
        {
            std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
            auto stale = v6_block({{.id = 0, .start = 0, .sectors = 50, .bps = 1024}});
            f.seekp((8192 - 1024) * 512);
            f.write(reinterpret_cast<const char *>(stale.data()), 1024);
        }
        auto card = open_shared(p, Access::ReadOnly);
        auto d = s2s::v6::discover(*card);
        CHECK(d.has_value() && d->from_card() && d->candidates.size() == 2 &&
              has_notice(d->notices, "s2s.misplaced_copy"));
        auto all = s2s::v6::find_configs(*card, {.scan = s2s::v6::ScanOptions::Scan::always});
        CHECK(all.has_value() && all->size() == 2);
    }
    // Wrong sector size.
    auto big = open_image(make_card("4k.img", 64), Access::ReadOnly, 4096);
    CHECK(big.has_value() && err(s2s::v6::discover(**big)) == Errc::invalid_argument);
}

static void test_v6_volumes_and_views() {
    const auto p = make_card("views.img", 8192,
                             v6_block({{.id = 1, .start = 100, .sectors = 1000, .bps = 512},
                                       {.id = 2, .start = 2000, .sectors = 500, .bps = 2048},
                                       {.id = 3, .start = 6000, .sectors = 9999, .bps = 512},
                                       {.id = 4, .start = 1500, .sectors = 10, .bps = 256}}),
                             8190);
    auto card = open_shared(p);
    auto d = s2s::v6::discover(*card);
    CHECK(d.has_value());
    if (!d)
        return;
    const VolumeSet &set = d->volumes;
    CHECK(set.scheme == "scsi2sd-v6" && set.volumes.size() == 4);
    const Volume *v3 = set.find("3");
    CHECK(v3 && has_notice(v3->notices, "s2s.truncated") && has_notice(v3->notices, "s2s.covers_config") &&
          has_notice(v3->notices, "s2s.covers_misplaced_save") && v3->usable);
    const Volume *v4 = set.find("4");
    CHECK(v4 && !v4->usable && has_notice(v4->notices, "s2s.sector_size_unsupported"));
    CHECK((set.find("2") && set.find("2")->sector_count == 500 && set.find("2")->range == Range{2000, 4000}));

    auto a = open_volume(card, set, "1", Access::ReadWrite);
    CHECK(a.has_value());
    if (a) {
        std::vector<std::byte> w(512 * 3, std::byte{0xAB});
        CHECK((*a)->write(997, w).has_value());
        CHECK(err((*a)->write(998, w)) == Errc::out_of_range);
    }
    auto b = open_volume(card, set, "2", Access::ReadOnly);
    CHECK(b.has_value());
    if (b) {
        AlignedBuffer s(2048);
        CHECK((*b)->read(1, s.span()).has_value() && std::to_integer<unsigned>(s.data()[0]) == pattern_at(2004, 0));
    }
    auto c = open_volume(card, set, "3", Access::ReadWrite);
    CHECK(c.has_value());
    if (c) {
        const std::uint64_t last = (*c)->sector_count();
        CHECK(last == 2192);
        std::vector<std::byte> w(512 * 4, std::byte{0x11});
        CHECK(err((*c)->write(last - 4, w)) == Errc::layout_conflict);
        CHECK((*c)->write(last - 6, w).has_value());
    }
    auto again = s2s::v6::discover(*card);
    CHECK(again.has_value() && again->from_card()); // the configuration survived
    CHECK(err(open_volume(card, set, "4", Access::ReadOnly)) == Errc::layout_invalid);
    CHECK(err(open_volume(card, set, "6", Access::ReadOnly)) == Errc::invalid_argument);

    // Warnings on a crafted configuration.
    s2s::v6::Config cfg = s2s::v6::default_config();
    cfg.targets[0] = {
        .index = 0, .scsi_id = 0, .enabled = true, .sd_sector_start = 0, .scsi_sectors = 5000, .bytes_per_sector = 512};
    cfg.targets[1] = {.index = 1,
                      .scsi_id = 0,
                      .enabled = true,
                      .sd_sector_start = 4000,
                      .scsi_sectors = 100,
                      .bytes_per_sector = 512};
    cfg.targets[2] = {.index = 2,
                      .scsi_id = 5,
                      .enabled = true,
                      .sd_sector_start = 99999,
                      .scsi_sectors = 10,
                      .bytes_per_sector = 512};
    cfg.targets[3] = {
        .index = 3, .scsi_id = 6, .enabled = true, .sd_sector_start = 0, .scsi_sectors = 0, .bytes_per_sector = 512};
    const auto vs = s2s::v6::to_volumes(cfg, 8192);
    CHECK(vs.volumes.size() == 4);
    CHECK(has_notice(vs.volumes[0].notices, "s2s.overlap") && has_notice(vs.volumes[0].notices, "s2s.duplicate_id"));
    CHECK(!vs.volumes[0].usable && !vs.volumes[1].usable);
    CHECK(has_notice(vs.volumes[2].notices, "s2s.beyond_card_end") && !vs.volumes[2].usable);
    CHECK(has_notice(vs.volumes[3].notices, "s2s.no_size") && !vs.volumes[3].usable);
    CHECK(has_notice(vs.notices, "s2s.no_volumes"));
}

static void test_xml() {
    const std::string text = R"(<?xml version="1.0"?>
<SCSI2SD>
  <!-- comment with <tags> inside -->
  <S2S_BoardCfg><enableTerminator>true</enableTerminator><selectionDelay>255</selectionDelay><blank/></S2S_BoardCfg>
  <SCSITarget id="1">
    <enabled>true</enabled>
    <quirks>apple</quirks>
    <deviceType>0x0</deviceType>
    <sdSectorStart>2048</sdSectorStart>
    <scsiSectors>4000</scsiSectors>
    <bytesPerSector>512</bytesPerSector>
    <vendor> SEAGATE</vendor>
    <prodId> ST225N &amp; Co</prodId>
  </SCSITarget>
  <SCSITarget id="2"><enabled>false</enabled></SCSITarget>
  <SCSITarget id="3"><enabled>true</enabled><sdSectorStart>0x1800</sdSectorStart><scsiSectors>1024</scsiSectors><bytesPerSector>1024</bytesPerSector></SCSITarget>
</SCSI2SD>)";
    auto c = s2s::xml::parse(text);
    CHECK(c.has_value());
    if (!c)
        return;
    CHECK(c->board.size() == 3 && c->board[0].first == "enableTerminator" && c->board[0].second == "true" &&
          c->board[2].first == "blank");
    CHECK(c->targets.size() == 3 && c->targets[0].scsi_id == 1 && c->targets[0].product == "ST225N & Co");
    CHECK(!c->targets[1].enabled && c->targets[2].sd_sector_start == 0x1800 && c->targets[2].bytes_per_sector == 1024);

    const auto v6 = s2s::xml::to_volumes(*c, 8192, s2s::xml::Board::v6);
    CHECK(v6.scheme == "scsi2sd-xml" && v6.volumes.size() == 2 && v6.protected_ranges.size() == 1);
    CHECK(v6.find("3") && has_notice(v6.find("3")->notices, "s2s.covers_config"));
    const auto v5 = s2s::xml::to_volumes(*c, 8192, s2s::xml::Board::v5);
    CHECK(v5.protected_ranges.empty() && v5.find("3") && v5.find("3")->notices.empty());

    for (
        const char *bad :
        {"", "<SCSI2SD></SCSI2SD>", "<SCSI2SD><!-- x <SCSITarget id=\"1\"></SCSITarget></SCSI2SD>",
         R"(<SCSI2SD><SCSITarget id="9"><enabled>false</enabled></SCSITarget></SCSI2SD>)",
         R"(<SCSI2SD><SCSITarget id="1"><enabled>true</enabled><scsiSectors>5</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget></SCSI2SD>)",
         R"(<SCSI2SD><SCSITarget id="1"><enabled>yes</enabled></SCSITarget></SCSI2SD>)",
         R"(<SCSI2SD><SCSITarget id="1"><enabled>true</enabled><sdSectorStart>99999999999</sdSectorStart><scsiSectors>5</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget></SCSI2SD>)",
         R"(<SCSI2SD><S2S_BoardCfg><a>1</S2S_BoardCfg><SCSITarget id="1"><enabled>false</enabled></SCSITarget></SCSI2SD>)"})
        CHECK(err(s2s::xml::parse(bad)) == Errc::layout_invalid);
    CHECK(err(s2s::xml::load(test_dir() / "missing.xml")) == std::errc::no_such_file_or_directory);
    CHECK(!s2s::xml::load(test_dir()).has_value()); // a directory

    const std::string disabled = "<SCSITarget id='0'><enabled>false</enabled></SCSITarget>";
    const auto v5_xml = s2s::xml::parse("<SCSI2SD><BoardConfig><selectionDelay>255</selectionDelay>"
                                        "</BoardConfig><SCSITarget id='0'><enabled>true</enabled>"
                                        "<sdSectorStart>0x800</sdSectorStart><scsiSectors>100</scsiSectors>"
                                        "<bytesPerSector>512</bytesPerSector><prodId>A&#32;B</prodId>"
                                        "<modePages>AA==</modePages></SCSITarget></SCSI2SD>");
    CHECK(v5_xml && v5_xml->board.size() == 1 && v5_xml->targets[0].product == "A B");
    for (const auto &bad : std::vector<std::string>{
             "<SCSI2SDwrong>" + disabled + "</SCSI2SDwrong>", "<SCSI2SD>" + disabled + "</SCSI2SD>trailing",
             "<SCSI2SD><wrapper>" + disabled + "</wrapper></SCSI2SD>",
             "<SCSI2SD><SCSITarget id='0' id='1'><enabled>false</enabled></SCSITarget></SCSI2SD>",
             "<!DOCTYPE SCSI2SD [<!ENTITY x 'false'>]><SCSI2SD><SCSITarget id='0'>"
             "<enabled>&x;</enabled></SCSITarget></SCSI2SD>",
             "<!DOCTYPE SCSI2SD SYSTEM 'file:///etc/passwd'><SCSI2SD>" + disabled + "</SCSI2SD>",
             "<SCSI2SD><SCSITarget id='0'><enabled>false</enabled><enabled>false</enabled>"
             "</SCSITarget></SCSI2SD>",
             "<SCSI2SD><SCSITarget id='0'><enabled>false</enabled><prodId>&unknown;</prodId>"
             "</SCSITarget></SCSI2SD>",
             "<SCSI2SD>" + disabled + "<!--" + std::string(262144, 'x') + "--></SCSI2SD>"})
        CHECK(err(s2s::xml::parse(bad)) == Errc::layout_invalid);
    std::string deep = "<SCSI2SD>";
    for (int depth = 0; depth < 40; ++depth)
        deep += "<other>";
    for (int depth = 0; depth < 40; ++depth)
        deep += "</other>";
    CHECK(err(s2s::xml::parse(deep + disabled + "</SCSI2SD>")) == Errc::layout_invalid);
    const std::string enabled_target =
        "<SCSITarget id='1'><enabled>true</enabled><sdSectorStart>2048</sdSectorStart>"
        "<scsiSectors>100</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget>";
    CHECK(err(s2s::xml::parse("<SCSI2SD>" + enabled_target + enabled_target + "</SCSI2SD>")) == Errc::layout_invalid);
    CHECK(s2s::xml::parse("<SCSI2SD>" + disabled + disabled + "</SCSI2SD>"));
    CHECK(err(s2s::xml::parse("<SCSI2SD>" + disabled + "<wrapper><SCSITarget id=\"1\"/></wrapper></SCSI2SD>")) ==
          Errc::layout_invalid);
    CHECK(err(s2s::xml::parse("<SCSI2SD>" + disabled + "<wrapper><BoardConfig/></wrapper></SCSI2SD>")) ==
          Errc::layout_invalid);
    CHECK(err(s2s::xml::parse("<SCSI2SD>" + disabled + "<SCSI2SD/></SCSI2SD>")) == Errc::layout_invalid);
    CHECK(err(s2s::xml::parse("<SCSI2SD>" + disabled + disabled + disabled + disabled + disabled + disabled + disabled +
                              disabled + disabled + "</SCSI2SD>")) == Errc::layout_invalid);
    for (const auto &[field, value] : {std::pair{"scsiSectors", "4294967296"},
                                       {"sdSectorStart", "-1"},
                                       {"bytesPerSector", "65536"},
                                       {"bytesPerSector", "8193"},
                                       {"sectorsPerTrack", "256"},
                                       {"headsPerCylinder", "256"}})
        CHECK(err(s2s::xml::parse("<SCSI2SD><SCSITarget id=\"0\"><enabled>false</enabled><" + std::string(field) + ">" +
                                  value + "</" + field + "></SCSITarget></SCSI2SD>")) == Errc::layout_invalid);

    std::mt19937 rng(7);
    for (int i = 0; i < 3000; ++i) {
        std::string m = text;
        for (int k = 0; k < 5; ++k)
            m[rng() % m.size()] = static_cast<char>(rng() % 128);
        if (i % 3 == 0)
            m.resize(rng() % m.size());
        (void)s2s::xml::parse(m);
    }
    CHECK(true);
}

static void test_layout_write_policy() {
    auto config = s2s::v6::default_config();
    auto copy = config;
    CHECK(s2s::same_target_layout(config.targets, copy.targets));
    copy.targets[0].sd_sector_start = 12U;
    CHECK(!s2s::same_target_layout(config.targets, copy.targets));
    copy = config;
    copy.targets[0].device_type = static_cast<std::uint8_t>(s2s::DeviceType::optical);
    copy.targets[0].bytes_per_sector = 512U;
    copy.targets[0].scsi_sectors = 100U;
    const auto volumes = s2s::v6::to_volumes(copy, 200U);
    CHECK(volumes.find("0") != nullptr && volumes.find("0")->read_only);
    CHECK(err(check_writable(volumes, "0", {.allow_overlap = true})) == Errc::read_only_handle);
    CHECK(!s2s::same_target_layout(config.targets, copy.targets));
}

int main() {
    test_layout_write_policy();
    test_slice();
    test_volume();
    test_v6_codec();
    test_v6_discovery();
    test_v6_volumes_and_views();
    test_xml();
    return finish_tests();
}
