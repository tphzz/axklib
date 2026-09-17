#include <array>
#include <format>
#include <utility>

#include "axklib/floppy_import.hpp"
#include "axklib/writer_internal.hpp"
#include "media_test_fixtures.hpp"

namespace {
axk::FatImage catalog_member(std::uint16_t index, bool final, std::string set = "FOLDER SET    ", bool gap = false,
                             bool overlap = false) {
    axk::detail::PreparedMediaImage image;
    auto wave = smpl_object();
    if (index <= 2U) {
        if (index == 1U)
            wave.resize(0xaeU);
        else
            wave.erase(wave.begin() + 0xac, wave.begin() + 0xae);
        be32(wave, 0x20U, 2U);
        be32(wave, 0x24U, index == 1U ? 0U : gap ? 3U : overlap ? 1U : 2U);
        image.objects.emplace_back(axk::ObjectType::smpl, "TEST", wave);
        image.objects.back().fat_filename = "TEST____.004";
    } else {
        for (const auto type : {"PROG", "SBNK", "SBAC", "SEQU"}) {
            auto object = smpl_object(type);
            ascii(object, 0x0cU, type);
            image.objects.emplace_back(axk::ObjectType::unknown, type, object);
            image.objects.back().fat_filename = std::format("{:_<8}.{:03}", type, image.objects.size() + 5U);
        }
    }
    const auto marker = final ? "A3000E.SYM" : "A3000F.SYM";
    image.retained_files.push_back({final ? "A3000E_S.005" : "A3000F_S.005", {}});
    image.floppy_catalog = axk::YamahaFloppyCatalog{set + std::format("{:02}", index),
                                                    {{5U, "\\" + std::string{marker}}},
                                                    {"\\OTHERS", "\\SMPL", "\\PROG", "\\SBNK", "\\SBAC", "\\SEQU"}};
    if (index <= 2U)
        image.floppy_catalog->files.push_back({4U, std::format(R"(\SMPL\TEST            {:02})", index)});
    else
        for (std::size_t offset = 0U; offset < image.objects.size(); ++offset) {
            const auto &name = image.objects[offset].name;
            image.floppy_catalog->files.push_back({static_cast<std::uint16_t>(offset + 6U), "\\" + name + "\\" + name});
        }
    const auto bytes = axk::detail::build_fat12_image(image, {});
    EXPECT_TRUE(bytes) << bytes.error().message;
    auto fat = axk::FatImage::open(std::make_shared<axk::MemoryReader>(bytes.value()), "member.img");
    EXPECT_TRUE(fat) << fat.error().message;
    return std::move(fat.value());
}

std::vector<axk::AxkObjectDirectoryEntry> unpack(const axk::FatImage &fat) {
    std::vector<axk::AxkObjectDirectoryEntry> entries;
    for (const auto &file : fat.files()) {
        auto bytes = fat.read_file(file);
        EXPECT_TRUE(bytes) << bytes.error().message;
        entries.push_back({file.path, std::make_shared<axk::MemoryReader>(std::move(bytes.value()))});
    }
    return entries;
}

axk::AxkObjectDirectory directory_member(std::uint16_t index, bool final, std::string set = "FOLDER SET    ",
                                         bool gap = false, bool overlap = false) {
    auto opened = axk::AxkObjectDirectory::open(unpack(catalog_member(index, final, set, gap, overlap)), "folder");
    EXPECT_TRUE(opened) << opened.error().message;
    return std::move(opened.value());
}
} // namespace

TEST(ObjectDirectorySet, SharesCatalogValidationAndAssemblyWithRawFloppies) {
    auto one = directory_member(1U, false);
    ASSERT_TRUE(one.disk_identity().trusted_for_disk_set);
    EXPECT_EQ(one.disk_identity().index, 1U);
    EXPECT_TRUE(one.validation_issues().empty());
    const auto partial = axk::AxkObjectDirectory::open_members({one});
    ASSERT_TRUE(partial) << partial.error().message;
    EXPECT_EQ(partial->stored_objects().size(), 1U);
    const auto two = axk::AxkObjectDirectory::open_members({one, directory_member(2U, false)});
    ASSERT_TRUE(two) << two.error().message;
    EXPECT_EQ(two->stored_objects().front().raw_payload, smpl_object());
    const auto complete =
        axk::AxkObjectDirectory::open_members({directory_member(3U, true), one, directory_member(2U, false)});
    ASSERT_TRUE(complete) << complete.error().message;
    ASSERT_EQ(complete->stored_objects().size(), 5U);
    ASSERT_EQ(complete->disk_members().size(), 3U);
    const auto raw =
        axk::FloppyDiskSet::open({catalog_member(1U, false), catalog_member(2U, false), catalog_member(3U, true)});
    ASSERT_TRUE(raw) << raw.error().message;
    const auto objects = raw->objects(axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), complete->stored_objects().size());
    for (std::size_t index = 0U; index < objects->size(); ++index)
        EXPECT_EQ((*objects)[index].raw_payload, complete->stored_objects()[index].raw_payload);
}

TEST(ObjectDirectorySet, ImportAssemblesUnorderedFoldersAndRequiresEveryCompanion) {
    const axk::FloppyImportDirectory one{"disk1", unpack(catalog_member(1U, false))};
    const axk::FloppyImportDirectory two{"disk2", unpack(catalog_member(2U, true))};
    const auto partial = axk::FloppyImportSource::open_directories({one});
    ASSERT_TRUE(partial) << partial.error().message;
    EXPECT_FALSE(partial->inspection().complete);
    EXPECT_EQ(partial->inspection().next_required_index, 2U);
    EXPECT_FALSE(partial->prepare({}));
    const auto later = axk::FloppyImportSource::open_directories({two});
    ASSERT_TRUE(later) << later.error().message;
    EXPECT_EQ(later->inspection().next_required_index, 1U);
    const auto complete = axk::FloppyImportSource::open_directories({two, one});
    ASSERT_TRUE(complete) << complete.error().message;
    EXPECT_TRUE(complete->inspection().complete);
    ASSERT_EQ(complete->inspection().objects.size(), 1U);
    const auto prepared = complete->prepare(std::array{complete->inspection().objects.front().key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes.front().raw_payload, smpl_object());
    EXPECT_FALSE(axk::FloppyImportSource::open_directories({one, one}));
    EXPECT_FALSE(
        axk::FloppyImportSource::open_directories({one, {"wrong", unpack(catalog_member(2U, true, "OTHER SET    "))}}));
    auto broken = one;
    for (auto &entry : broken.entries)
        if (entry.name == "YAMAHA.SYM")
            entry.reader = std::make_shared<axk::MemoryReader>(std::vector<std::byte>{std::byte{0xff}});
    EXPECT_FALSE(axk::FloppyImportSource::open_directories({broken}));
}

TEST(ObjectDirectorySet, RejectsMissingDuplicateWrongSetAndInvalidWaveRanges) {
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({directory_member(2U, true)}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({directory_member(1U, false), directory_member(1U, false)}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({directory_member(1U, false), directory_member(3U, true)}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members(
        {directory_member(1U, false), directory_member(2U, true, "OTHER SET     ")}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({directory_member(1U, true)}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members(
        {directory_member(1U, false), directory_member(2U, true, "FOLDER SET    ", true)}));
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members(
        {directory_member(1U, false), directory_member(2U, true, "FOLDER SET    ", false, true)}));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({directory_member(1U, false)}, {}, cancellation.token()));
}

TEST(ObjectDirectorySet, RetainsRecoveryWhenCatalogIsAbsentOrMalformed) {
    auto entries = unpack(catalog_member(1U, false));
    std::erase_if(entries, [](const auto &entry) { return entry.name == "YAMAHA.SYM"; });
    auto missing = axk::AxkObjectDirectory::open(entries, "missing");
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing->disk_identity().trusted_for_disk_set);
    entries.push_back({"YAMAHA.SYM", std::make_shared<axk::MemoryReader>(std::vector<std::byte>{std::byte{0xff}})});
    auto invalid = axk::AxkObjectDirectory::open(entries, "invalid");
    ASSERT_TRUE(invalid);
    EXPECT_FALSE(invalid->disk_identity().trusted_for_disk_set);
    ASSERT_EQ(invalid->validation_issues().size(), 1U);
    EXPECT_EQ(invalid->validation_issues().front().code, "FLOPPY_CATALOG_INVALID");
    EXPECT_FALSE(axk::AxkObjectDirectory::open_members({*invalid}));
}

TEST(ObjectDirectorySet, CountsSupportFilesInAggregateEntryAndPayloadLimits) {
    std::vector<axk::AxkObjectDirectory> members;
    for (std::uint16_t index = 1U; index <= 6U; ++index) {
        auto entries = unpack(catalog_member(index, index == 6U));
        for (std::size_t support = 0U; support < 200U; ++support)
            entries.push_back(
                {std::format("note-{}.txt", support), std::make_shared<axk::MemoryReader>(std::vector<std::byte>{})});
        auto member = axk::AxkObjectDirectory::open(std::move(entries), "folder");
        ASSERT_TRUE(member) << member.error().message;
        ASSERT_TRUE(member->disk_identity().trusted_for_disk_set);
        members.push_back(std::move(*member));
    }
    const auto too_many = axk::AxkObjectDirectory::open_members(std::move(members));
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error().code, axk::ErrorCode::io_unsupported_size);
    members.clear();
    for (std::uint16_t index = 1U; index <= 2U; ++index) {
        auto entries = unpack(catalog_member(index, index == 2U));
        entries.push_back(
            {"support.bin", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(8U * 1024U * 1024U))});
        auto member = axk::AxkObjectDirectory::open(std::move(entries), "folder");
        ASSERT_TRUE(member) << member.error().message;
        members.push_back(std::move(*member));
    }
    const auto too_large = axk::AxkObjectDirectory::open_members(std::move(members));
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().code, axk::ErrorCode::io_unsupported_size);
}
