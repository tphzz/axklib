#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/filesystem_import.hpp"
#include "axklib/filesystem_transaction.hpp"
#include "axklib/media.hpp"
#include "media_ex5_fixture.hpp"

namespace {
std::vector<std::byte> plain_fixture(bool removable = false) {
    const auto ex5 = ex5_fixture();
    std::vector<std::byte> bytes(ex5.begin() + boot_offset, ex5.end());
    if (!removable)
        ascii(bytes, 3U, "MSDOS5.0");
    le16(bytes, 19U, 0U);
    le32(bytes, 32U, static_cast<std::uint32_t>(bytes.size() / sector_bytes));
    return bytes;
}
std::shared_ptr<axk::MemoryReader> input(std::size_t size) {
    return std::make_shared<axk::MemoryReader>(std::vector<std::byte>(size, std::byte{0x6a}));
}
std::vector<std::byte> read(const axk::RandomAccessReader &reader) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(reader.size()));
    EXPECT_TRUE(reader.read_exact_at(0U, bytes));
    return bytes;
}
constexpr auto plain_root = root_offset - boot_offset;
constexpr auto plain_data = data_offset - boot_offset;
void set_plain_fat(std::vector<std::byte> &bytes, std::uint16_t cluster, std::uint16_t next) {
    for (const auto offset : {fat_offset - boot_offset, fat_offset - boot_offset + fat_bytes})
        le16(bytes, offset + static_cast<std::size_t>(cluster) * 2U, next);
}
std::vector<std::byte> long_name_fixture() {
    auto bytes = plain_fixture();
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(plain_data), 32U,
                bytes.begin() + static_cast<std::ptrdiff_t>(plain_data + 32U));
    auto entry = std::span{bytes}.subspan(plain_data, 32U);
    std::ranges::fill(entry, std::byte{});
    entry[0] = std::byte{0x41};
    entry[11] = std::byte{0x0f};
    constexpr std::array<std::size_t, 13> characters{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    constexpr std::string_view name = "Demo one.s1a";
    for (std::size_t i = 0; i < characters.size(); ++i)
        le16(bytes, plain_data + characters[i],
             i < name.size()    ? static_cast<std::uint16_t>(name[i])
             : i == name.size() ? std::uint16_t{}
                                : std::uint16_t{0xffffU});
    std::uint8_t checksum{};
    for (const auto byte : std::span{bytes}.subspan(plain_data + 32U, 11U))
        checksum =
            static_cast<std::uint8_t>(((checksum & 1U) << 7U) + (checksum >> 1U) + std::to_integer<std::uint8_t>(byte));
    entry[13] = static_cast<std::byte>(checksum);
    return bytes;
}

TEST(FatFiles, PreparesEmptyDirectoriesAndStreamedFilesForAllVolumeProfiles) {
    for (const auto &bytes : {plain_fixture(), plain_fixture(true), ex5_fixture()}) {
        const auto source = std::make_shared<axk::MemoryReader>(bytes);
        const std::vector<axk::FilesystemEdit> edits{
            axk::CreateFilesystemDirectory{{"NEW"}},
            axk::PutFilesystemFile{{"NEW", "EMPTY.BIN"}, input(0)},
            axk::PutFilesystemFile{{"NEW", "DATA.BIN"}, input(1300)},
        };
        const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(read(*source), bytes);
        const auto image = axk::FatImage::open(plan->preview);
        ASSERT_TRUE(image) << image.error().message;
        ASSERT_EQ(image->directories().size(), 2U);
        ASSERT_EQ(image->files().size(), 3U);
        for (const auto &[name, size] :
             std::vector<std::pair<std::string, std::size_t>>{{"NEW/EMPTY.BIN", 0}, {"NEW/DATA.BIN", 1300}}) {
            const auto file = std::ranges::find(image->files(), name, &axk::FatFile::path);
            ASSERT_NE(file, image->files().end());
            EXPECT_EQ(image->read_file(*file).value(), std::vector<std::byte>(size, std::byte{0x6a}));
            if (size == 0U) {
                EXPECT_EQ(file->first_cluster, 0U);
            }
        }
        const auto kept = std::ranges::find(image->files(), "DEMOS/DEMO1.S1A", &axk::FatFile::path);
        ASSERT_NE(kept, image->files().end());
        const auto original = axk::FatImage::open(source).value();
        EXPECT_EQ(image->read_file(*kept).value(), original.read_file(original.files().front()).value());
    }
}

TEST(FatFiles, KeepsEveryMbrByteOutsideTheSelectedPartitionUnchanged) {
    const auto volume = plain_fixture();
    constexpr std::size_t start = 63U * sector_bytes;
    std::vector<std::byte> bytes(start + 2U * volume.size() + 512U, std::byte{0x5a});
    std::fill_n(bytes.begin(), 512U, std::byte{});
    bytes[510U] = std::byte{0x55};
    bytes[511U] = std::byte{0xaa};
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const auto offset = start + slot * volume.size();
        bytes[446U + slot * 16U + 4U] = std::byte{0x06};
        le32(bytes, 446U + slot * 16U + 8U, static_cast<std::uint32_t>(offset / 512U));
        le32(bytes, 446U + slot * 16U + 12U, static_cast<std::uint32_t>(volume.size() / 512U));
        std::copy(volume.begin(), volume.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"NEW.BIN"}, input(0)}};
    const auto plan =
        axk::detail::prepare_fat_file_edits(std::make_shared<axk::MemoryReader>(bytes), axk::PartitionIndex{1}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto after = read(*plan->preview);
    const auto selected = start + volume.size();
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(selected), after.begin()));
    EXPECT_TRUE(std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(selected + volume.size()), bytes.end(),
                           after.begin() + static_cast<std::ptrdiff_t>(selected + volume.size())));
    for (const auto &patch : plan->patches) {
        EXPECT_GE(patch.offset, selected);
        EXPECT_LE(patch.offset + patch.size, selected + volume.size());
    }
}

TEST(FatFiles, MergesDirectoriesAndSupportsExplicitReplacementAndRecursiveDeletion) {
    const auto source = std::make_shared<axk::MemoryReader>(plain_fixture());
    const std::vector<axk::FilesystemEdit> edits{
        axk::CreateFilesystemDirectory{{"DEMOS"}},
        axk::PutFilesystemFile{{"DEMOS", "DEMO1.S1A"}, input(12)},
        axk::PutFilesystemFile{{"DEMOS", "DEMO1.S1A"}, input(1), axk::FileConflict::replace},
    };
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto fat = axk::FatImage::open(plan->preview).value();
    ASSERT_EQ(fat.files().size(), 1U);
    EXPECT_EQ(fat.read_file(fat.files().front()).value(), std::vector<std::byte>(1, std::byte{0x6a}));
    const std::array<axk::FilesystemEdit, 1> refused{axk::RemoveFilesystemEntry{{"DEMOS"}, false}};
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(plan->preview, axk::PartitionIndex{0}, refused));
    const std::array<axk::FilesystemEdit, 1> remove{axk::RemoveFilesystemEntry{{"DEMOS"}, true}};
    const auto deleted = axk::detail::prepare_fat_file_edits(plan->preview, axk::PartitionIndex{0}, remove);
    ASSERT_TRUE(deleted) << deleted.error().message;
    EXPECT_TRUE(axk::FatImage::open(deleted->preview)->files().empty());
    EXPECT_TRUE(axk::FatImage::open(deleted->preview)->directories().empty());
}

TEST(FatFiles, ReplacementPreservesLongNamesAttributesAndEveryOtherEntryByte) {
    auto bytes = long_name_fixture();
    const auto offset = plain_data + 32U;
    bytes[offset + 11U] = std::byte{0x26};
    for (const auto i : {12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 22U, 23U, 24U, 25U})
        bytes[offset + i] = static_cast<std::byte>(i);
    const std::array<axk::FilesystemEdit, 1> edits{
        axk::PutFilesystemFile{{"DEMOS", "demo1.s1a"}, input(1300), axk::FileConflict::replace}};
    const auto plan =
        axk::detail::prepare_fat_file_edits(std::make_shared<axk::MemoryReader>(bytes), axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto after = read(*plan->preview);
    EXPECT_TRUE(std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(plain_data),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + 26U),
                           after.begin() + static_cast<std::ptrdiff_t>(plain_data)));
    EXPECT_EQ(axk::FatImage::open(plan->preview)->files().front().size, 1300U);
}

TEST(FatFiles, DeletionRemovesOnlyMatchingLongNameRecordsAndShortEntry) {
    const auto bytes = long_name_fixture();
    const std::array<axk::FilesystemEdit, 1> edits{axk::RemoveFilesystemEntry{{"DEMOS", "DEMO1.S1A"}, false}};
    const auto plan =
        axk::detail::prepare_fat_file_edits(std::make_shared<axk::MemoryReader>(bytes), axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto after = read(*plan->preview);
    for (std::size_t i = plain_data; i < plain_data + 64U; ++i)
        EXPECT_EQ(after[i], i == plain_data || i == plain_data + 32U ? std::byte{0xe5} : bytes[i]);
    EXPECT_TRUE(std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(plain_data + 64U), bytes.end(),
                           after.begin() + static_cast<std::ptrdiff_t>(plain_data + 64U)));
    EXPECT_TRUE(axk::FatImage::open(plan->preview)->files().empty());
}

TEST(FatFiles, RejectsMalformedLongNamesWithoutChangingSource) {
    for (const auto offset : {0U, 12U, 13U, 26U, 32U}) {
        auto bytes = long_name_fixture();
        bytes[plain_data + offset] = std::byte{};
        if (offset == 12U || offset == 26U)
            bytes[plain_data + offset] = std::byte{1};
        const auto source = std::make_shared<axk::MemoryReader>(bytes);
        const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"NEW.BIN"}, input(0)}};
        const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
        if (offset == 0U) {
            // An end marker makes subsequent bytes inactive, not malformed.
            ASSERT_TRUE(plan) << plan.error().message;
        } else {
            EXPECT_FALSE(plan) << offset;
        }
        EXPECT_EQ(read(*source), bytes);
    }
}

TEST(FatFiles, GrowsSubdirectoriesAndAuthorsSelfAndParentEntries) {
    const auto bytes = plain_fixture();
    std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"NEW"}},
                                           axk::CreateFilesystemDirectory{{"NEW", "CHILD"}}};
    for (std::size_t i = 0; i < 32U; ++i)
        edits.emplace_back(axk::PutFilesystemFile{{"NEW", std::format("F{}.BIN", i)}, input(0)});
    const auto plan =
        axk::detail::prepare_fat_file_edits(std::make_shared<axk::MemoryReader>(bytes), axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto fat = axk::FatImage::open(plan->preview).value();
    const auto parent = std::ranges::find(fat.directories(), "NEW", &axk::FatDirectory::path);
    const auto child = std::ranges::find(fat.directories(), "NEW/CHILD", &axk::FatDirectory::path);
    ASSERT_NE(parent, fat.directories().end());
    ASSERT_NE(child, fat.directories().end());
    EXPECT_EQ(parent->clusters.size(), 3U);
    const auto after = read(*plan->preview);
    const auto parent_start = plain_data + (parent->clusters.front() - 2U) * sector_bytes;
    const auto child_start = plain_data + (child->clusters.front() - 2U) * sector_bytes;
    EXPECT_EQ(after[parent_start], std::byte{'.'});
    EXPECT_EQ(after[parent_start + 32U], std::byte{'.'});
    EXPECT_EQ(after[parent_start + 33U], std::byte{'.'});
    EXPECT_EQ(after[parent_start + 58U], std::byte{});
    EXPECT_EQ(after[child_start + 58U], static_cast<std::byte>(parent->clusters.front()));
    EXPECT_EQ(fat.files().size(), 33U);
}

TEST(FatFiles, RejectsFullRootAndReusesDeletedRootSlots) {
    std::vector<axk::FilesystemEdit> edits;
    for (std::size_t i = 0; i < 15U; ++i)
        edits.emplace_back(axk::PutFilesystemFile{{std::format("F{}.BIN", i)}, input(0)});
    const auto source = std::make_shared<axk::MemoryReader>(plain_fixture());
    auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    edits.emplace_back(axk::PutFilesystemFile{{"FULL.BIN"}, input(0)});
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits));
    const std::array<axk::FilesystemEdit, 2> reuse{axk::RemoveFilesystemEntry{{"F0.BIN"}, false},
                                                   axk::PutFilesystemFile{{"REUSED.BIN"}, input(0)}};
    plan = axk::detail::prepare_fat_file_edits(plan->preview, axk::PartitionIndex{0}, reuse);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(axk::FatImage::open(plan->preview)->files().size(), 16U);
}

TEST(FatFiles, LeavesOrphanAndBadClustersAllocatedAndRejectsExhaustion) {
    auto bytes = plain_fixture();
    for (std::uint16_t cluster = 4U; cluster < 4098U; ++cluster)
        if (cluster != 7U && cluster != 12U && cluster != 20U)
            set_plain_fat(bytes, cluster, cluster % 2U ? 0xfff7U : 0xffffU);
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"FRAG.BIN"}, input(700)}};
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto fat = axk::FatImage::open(plan->preview).value();
    const auto file = std::ranges::find(fat.files(), "FRAG.BIN", &axk::FatFile::path);
    ASSERT_NE(file, fat.files().end());
    EXPECT_EQ(file->clusters, (std::vector<std::uint16_t>{12U, 20U}));
    EXPECT_EQ(fat.read_file(*file).value(), std::vector<std::byte>(700U, std::byte{0x6a}));
    const std::array<axk::FilesystemEdit, 1> full{axk::PutFilesystemFile{{"FULL.BIN"}, input(1300)}};
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, full));
    EXPECT_EQ(read(*source), bytes);
}

TEST(FatFiles, RejectsReadOnlyReplacementAndRecursiveDeletionButAllowsSkip) {
    auto bytes = plain_fixture();
    bytes[plain_data + 11U] = std::byte{0x21};
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    for (const auto &edit : std::vector<axk::FilesystemEdit>{
             axk::PutFilesystemFile{{"DEMOS", "DEMO1.S1A"}, input(1), axk::FileConflict::replace},
             axk::RemoveFilesystemEntry{{"DEMOS"}, true}}) {
        EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, std::span{&edit, 1U}));
    }
    const std::array<axk::FilesystemEdit, 1> skip{axk::PutFilesystemFile{{"DEMOS", "DEMO1.S1A"}, input(1)}};
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, skip);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_TRUE(plan->patches.empty());
    EXPECT_EQ(read(*plan->preview), bytes);
}

TEST(FatFiles, RejectsInvalidNamesCollisionsAndCancellation) {
    const auto bytes = plain_fixture();
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    for (const auto name : {"lower.bin", "TOOLONGNAME.BIN", "A.LONG", "A.B.C", "../X", "A.", "A B"}) {
        const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{name}, input(0)}};
        EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits)) << name;
    }
    const std::array<axk::FilesystemEdit, 1> collision{
        axk::PutFilesystemFile{{"DEMOS"}, input(1), axk::FileConflict::replace}};
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, collision));
    const std::array<axk::FilesystemEdit, 1> valid{axk::PutFilesystemFile{{"OK.BIN"}, input(0)}};
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, valid, cancellation.token()));
    EXPECT_EQ(read(*source), bytes);
}

TEST(FatFiles, ValidatesReservedEntriesAndHighClusterWordsBeforeEditing) {
    for (const auto mode : {0, 1, 2}) {
        auto bytes = plain_fixture();
        if (mode == 0)
            set_plain_fat(bytes, 0U, 0xfff0U);
        if (mode == 1)
            set_plain_fat(bytes, 1U, 0x7fffU);
        if (mode == 2)
            le16(bytes, plain_data + 20U, 1U);
        const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"NEW.BIN"}, input(0)}};
        EXPECT_FALSE(axk::detail::prepare_fat_file_edits(std::make_shared<axk::MemoryReader>(bytes),
                                                         axk::PartitionIndex{0}, edits));
    }
}

TEST(FatFiles, LaterAllocationReuseReplacesEarlierPatches) {
    const auto source = std::make_shared<axk::MemoryReader>(plain_fixture());
    const std::vector<axk::FilesystemEdit> edits{
        axk::PutFilesystemFile{{"TEMP.BIN"}, input(1300)},
        axk::RemoveFilesystemEntry{{"TEMP.BIN"}, false},
        axk::CreateFilesystemDirectory{{"NEW"}},
        axk::PutFilesystemFile{{"NEW", "END.BIN"},
                               std::make_shared<axk::MemoryReader>(std::vector<std::byte>(1200U, std::byte{0x3f}))},
    };
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto fat = axk::FatImage::open(plan->preview).value();
    const auto file = std::ranges::find(fat.files(), "NEW/END.BIN", &axk::FatFile::path);
    ASSERT_NE(file, fat.files().end());
    EXPECT_EQ(fat.read_file(*file).value(), std::vector<std::byte>(1200U, std::byte{0x3f}));
    for (std::size_t i = 1; i < plan->patches.size(); ++i)
        EXPECT_LE(plan->patches[i - 1U].offset + plan->patches[i - 1U].size, plan->patches[i].offset);
}

TEST(FatFiles, KeepsContiguousInputRangesCompactInsteadOfOnePatchPerCluster) {
    const auto source = std::make_shared<axk::MemoryReader>(plain_fixture());
    const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"LARGE.BIN"}, input(100000U)}};
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_LE(plan->patches.size(), 8U);
}

TEST(FatFiles, RejectsIncorrectParentEntryWithoutRepairingIt) {
    auto bytes = plain_fixture();
    ascii(bytes, plain_data + 32U, "..         ");
    bytes[plain_data + 43U] = std::byte{0x10};
    le16(bytes, plain_data + 58U, 2U);
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"NEW.BIN"}, input(0)}};
    EXPECT_FALSE(axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits));
    EXPECT_EQ(read(*source), bytes);
}

TEST(FatFiles, AllocatesHighClusterNumbersOnlyUnderTheirEx5Profiles) {
    for (const auto hd : {false, true}) {
        constexpr std::size_t large_fat_bytes = 256U * sector_bytes;
        const auto clusters = hd ? 65533U : 65525U;
        const auto first_fat = hd ? fat_offset : 512U;
        const auto root = first_fat + 2U * large_fat_bytes;
        const auto data = root + sector_bytes;
        std::vector<std::byte> bytes(data + clusters * sector_bytes);
        const auto small = ex5_fixture();
        if (hd) {
            std::copy_n(small.begin(), fat_offset, bytes.begin());
            le16(bytes, boot_offset + 19U, static_cast<std::uint16_t>(clusters));
            le16(bytes, boot_offset + 22U, 256U);
            le32(bytes, boot_offset + 32U, clusters + 512U);
        } else {
            std::copy_n(small.begin() + static_cast<std::ptrdiff_t>(boot_offset), 512U, bytes.begin());
            le16(bytes, 14U, 1U);
            le16(bytes, 19U, 0U);
            le16(bytes, 22U, 256U);
            le32(bytes, 32U, static_cast<std::uint32_t>(bytes.size() / sector_bytes));
        }
        for (const auto copy : {first_fat, first_fat + large_fat_bytes}) {
            le16(bytes, copy, 0xfff8U);
            for (std::size_t cluster = 1U; cluster <= clusters; ++cluster)
                le16(bytes, copy + cluster * 2U, 0xffffU);
        }
        const auto source = std::make_shared<axk::MemoryReader>(std::move(bytes));
        const std::array<axk::FilesystemEdit, 1> edits{axk::PutFilesystemFile{{"HIGH.BIN"}, input(1U)}};
        const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(plan) << plan.error().message;
        const auto fat = axk::FatImage::open(plan->preview).value();
        ASSERT_EQ(fat.files().size(), 1U);
        EXPECT_EQ(fat.files().front().first_cluster, clusters + 1U);
        EXPECT_EQ(fat.read_file(fat.files().front()).value(), std::vector<std::byte>(1U, std::byte{0x6a}));
    }
}

TEST(FatFiles, ReviewsOrderedImportsWithoutAllocatingOrChangingBytes) {
    using Action = axk::FilesystemImportAction;
    const auto bytes = plain_fixture();
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::vector<axk::FilesystemImportEntry> entries{
        {{"demos"}, true, 0U},
        {{"demos", "demo1.s1a"}, false, 12U},
        {{"demos", "demo1.s1a"}, false, 13U, axk::FileConflict::replace},
        {{"NEW"}, true, 0U},
        {{"NEW", "EMPTY.BIN"}, false, 0U},
        {{"new", "empty.bin"}, false, 10U},
        {{"new", "empty.bin"}, false, 11U, axk::FileConflict::replace},
        {{"NEW", "EMPTY.BIN", "NO.BIN"}, false, 0U},
        {{"NEW", "lower.bin"}, false, 0U},
        {{"DEMOS"}, false, 0U},
        {{"MISSING", "NO.BIN"}, false, 0U},
        {{"NEW", "BIG.BIN"}, false, 4000000000U},
    };
    const auto review = axk::inspect_fat_file_import(source, axk::PartitionIndex{0}, entries);
    ASSERT_TRUE(review) << review.error().message;
    const std::vector<Action> expected{Action::merge_directory,  Action::skip_file,   Action::replace_file,
                                       Action::create_directory, Action::create_file, Action::skip_file,
                                       Action::replace_file,     Action::conflict,    Action::conflict,
                                       Action::conflict,         Action::conflict,    Action::create_file};
    ASSERT_EQ(review->size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ((*review)[i].action, expected[i]) << i;
    EXPECT_EQ((*review)[1].existing_size_bytes, 700U);
    EXPECT_EQ((*review)[2].existing_size_bytes, 700U);
    EXPECT_EQ((*review)[5].existing_size_bytes, 0U);
    EXPECT_EQ((*review)[6].existing_size_bytes, 0U);
    EXPECT_FALSE((*review)[5].issue.empty());
    EXPECT_EQ(read(*source), bytes);
}

TEST(FatFiles, ReviewsReadonlyConflictsAndRejectsInvalidRequests) {
    using Action = axk::FilesystemImportAction;
    auto bytes = plain_fixture();
    bytes[plain_data + 11U] = std::byte{0x21};
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::vector<axk::FilesystemImportEntry> entries{
        {{"DEMOS", "DEMO1.S1A"}, false, 1U},
        {{"DEMOS", "DEMO1.S1A"}, false, 1U, axk::FileConflict::replace},
        {{"BAD"}, true, 1U},
        {{"TOO.BIG"}, false, 0x100000000ULL},
        {{"BAD.BIN"}, false, 0U, static_cast<axk::FileConflict>(255U)},
        {{".."}, true, 0U},
    };
    const auto review = axk::inspect_fat_file_import(source, axk::PartitionIndex{0}, entries);
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_EQ(review->front().action, Action::skip_file);
    for (std::size_t i = 1; i < review->size(); ++i)
        EXPECT_EQ((*review)[i].action, Action::conflict) << i;
    EXPECT_FALSE(axk::inspect_fat_file_import(source, axk::PartitionIndex{0}, {}));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::inspect_fat_file_import(source, axk::PartitionIndex{0}, entries, cancellation.token()));
    EXPECT_EQ(read(*source), bytes);
}
} // namespace
