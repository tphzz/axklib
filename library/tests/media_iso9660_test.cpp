#include "media_test_fixtures.hpp"

namespace {

class CountingReader final : public axk::RandomAccessReader {
  public:
    explicit CountingReader(std::vector<std::byte> bytes) : source_{std::move(bytes)} {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return source_.size(); }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        ++read_count_;
        bytes_read_ += destination.size();
        return source_.read_exact_at(offset, destination);
    }

    void reset_counts() const noexcept {
        read_count_ = 0;
        bytes_read_ = 0;
    }

    [[nodiscard]] std::uint64_t read_count() const noexcept { return read_count_; }
    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return bytes_read_; }

  private:
    axk::MemoryReader source_;
    mutable std::uint64_t read_count_{};
    mutable std::uint64_t bytes_read_{};
};

} // namespace

TEST(Iso9660Reader, LoadsYamahaScopeLabelsObjectsAndStructuredPaths) {
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "fixture.iso");
    ASSERT_TRUE(image) << image.error().message;
    EXPECT_EQ(image->volume_id(), "TESTVOL");
    EXPECT_TRUE(image->validation_issues().empty());
    auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    const auto &object = objects->front();
    EXPECT_EQ(object.logical_path, "GROUP/F001/F000");
    EXPECT_EQ(object.group_label.value, "Mapped Group");
    EXPECT_EQ(object.group_label.status, axk::LabelStatus::confirmed);
    EXPECT_EQ(object.volume_label.value, "Mapped Vol");
    EXPECT_EQ(object.volume_label.status, axk::LabelStatus::confirmed);
    const auto path = axk::structured_object_path(object);
    EXPECT_EQ(path.relative_path.generic_string(), "Mapped Group/Mapped Vol/SMPL/CD WAVE");

    const axk::MediaContainer media{*image};
    const auto catalog = axk::build_object_catalog(media);
    ASSERT_TRUE(catalog) << catalog.error().message;
    ASSERT_EQ(catalog->objects.size(), 1U);
    EXPECT_EQ(catalog->objects.front().raw_payload, object.raw_payload);
}

TEST(Iso9660Reader, ReadsBoundedFileRanges) {
    const auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "range.iso");
    ASSERT_TRUE(image) << image.error().message;
    const auto file = std::ranges::find(image->files(), "GROUP/F001/F000", &axk::IsoFile::path);
    ASSERT_NE(file, image->files().end());

    const auto range = image->read_file_range(*file, 0xacU, 4U);
    ASSERT_TRUE(range) << range.error().message;
    EXPECT_EQ(*range, (std::vector<std::byte>{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}}));
    const auto invalid = image->read_file_range(*file, file->size - 2U, 4U);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, axk::ErrorCode::out_of_bounds);
}

TEST(Iso9660Reader, MetadataInventorySkipsPcmAndMatchesCompleteCatalog) {
    constexpr std::size_t object_size = 1024U * 1024U;
    auto reader = std::make_shared<CountingReader>(iso_fixture_with_large_smpl(object_size));
    auto image = axk::IsoImage::open(reader, "large.iso");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{*image};

    reader->reset_counts();
    const auto complete = axk::build_media_inventory(media, axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(complete) << complete.error().message;
    const auto complete_bytes = reader->bytes_read();
    ASSERT_EQ(complete->catalog.objects.size(), 1U);
    ASSERT_EQ(complete->objects.size(), 1U);
    EXPECT_EQ(complete->catalog.objects.front().raw_payload.size(), object_size);
    EXPECT_TRUE(complete->raw_payloads_complete);

    reader->reset_counts();
    const auto metadata = axk::build_media_inventory(media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(metadata) << metadata.error().message;
    ASSERT_EQ(metadata->catalog.objects.size(), 1U);
    ASSERT_EQ(metadata->objects.size(), 1U);
    EXPECT_EQ(metadata->objects.front().size, object_size);
    EXPECT_TRUE(metadata->catalog.objects.front().raw_payload.empty());
    EXPECT_FALSE(metadata->raw_payloads_complete);
    EXPECT_LT(reader->bytes_read(), 4096U);
    EXPECT_LT(reader->bytes_read() * 100U, complete_bytes);

    const auto &complete_object = complete->catalog.objects.front();
    const auto &metadata_object = metadata->catalog.objects.front();
    EXPECT_EQ(metadata_object.key, complete_object.key);
    EXPECT_EQ(metadata_object.scope_key, complete_object.scope_key);
    EXPECT_EQ(metadata_object.placement->container_directory, complete_object.placement->container_directory);
    EXPECT_EQ(metadata_object.object.header.raw_type, complete_object.object.header.raw_type);
    EXPECT_EQ(metadata_object.object.header.name, complete_object.object.header.name);
    const auto *complete_smpl = std::get_if<axk::CurrentSmpl>(&complete_object.object.payload);
    const auto *metadata_smpl = std::get_if<axk::CurrentSmpl>(&metadata_object.object.payload);
    ASSERT_NE(complete_smpl, nullptr);
    ASSERT_NE(metadata_smpl, nullptr);
    EXPECT_EQ(metadata_smpl->sample_rate.value, complete_smpl->sample_rate.value);
    EXPECT_EQ(metadata_smpl->wave_length_frames.value, complete_smpl->wave_length_frames.value);

    const auto complete_graph = axk::build_relationship_graph(complete->catalog);
    const auto metadata_graph = axk::build_relationship_graph(metadata->catalog);
    EXPECT_EQ(metadata_graph.relationships.size(), complete_graph.relationships.size());
    EXPECT_EQ(metadata_graph.bitmap_comparisons.size(), complete_graph.bitmap_comparisons.size());
}

TEST(Iso9660Reader, RequiresCatalogedDsknameForAConfirmedGroupLabelButKeepsInventory) {
    auto fixture = iso_fixture();
    std::fill_n(fixture.begin() + 22U * 2048U + 32U, 32, std::byte{});
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "missing-dskname.iso");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->validation_issues().size(), 1U);
    EXPECT_EQ(image->validation_issues().front().code, "ISO_YAMAHA_DSKNAME_ROW_MISSING");
    auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().group_label.value, "GROUP");
    EXPECT_EQ(objects->front().group_label.status, axk::LabelStatus::raw_identifier);
    EXPECT_EQ(objects->front().volume_label.value, "Mapped Vol");
    EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::confirmed);
}

TEST(Iso9660Reader, RequiresDsknameToBeTheFinalCatalogRow) {
    auto fixture = iso_fixture();
    const auto catalog = fixture.begin() + 22U * 2048U;
    std::swap_ranges(catalog, catalog + 32U, catalog + 32U);
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "non-final-dskname.iso");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->validation_issues().size(), 1U);
    EXPECT_EQ(image->validation_issues().front().code, "ISO_YAMAHA_DSKNAME_ROW_NOT_FINAL");
    auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().group_label.value, "GROUP");
    EXPECT_EQ(objects->front().group_label.status, axk::LabelStatus::raw_identifier);
    EXPECT_EQ(objects->front().volume_label.value, "Mapped Vol");
    EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::confirmed);
}

TEST(Iso9660Reader, ReportsWrongDsknameTargetButKeepsInventory) {
    auto fixture = iso_fixture();
    ascii(fixture, 22U * 2048U + 50U, "F003");
    auto image =
        axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "wrong-dskname-target.iso");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->validation_issues().size(), 1U);
    EXPECT_EQ(image->validation_issues().front().code, "ISO_YAMAHA_DSKNAME_TARGET_INVALID");
    const axk::MediaContainer media{*image};
    ASSERT_EQ(media.validation_issues().size(), 1U);
    EXPECT_EQ(media.validation_issues().front().code, "ISO_YAMAHA_DSKNAME_TARGET_INVALID");
    auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().group_label.status, axk::LabelStatus::raw_identifier);
    EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::confirmed);
}

TEST(Iso9660Reader, RequiresVolumeIdInTheCatalogFilenameField) {
    auto fixture = iso_fixture();
    std::fill_n(fixture.begin() + 22U * 2048U + 18U, 11, std::byte{});
    ascii(fixture, 22U * 2048U + 10U, "F001");
    auto image =
        axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "misplaced-volume-id.iso");
    ASSERT_TRUE(image) << image.error().message;
    auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().volume_label.value, "CD WAVE");
    EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::navigation_aid);
}

TEST(Iso9660Reader, RejectsInvalidDescriptorAndOutOfRangeExtent) {
    auto invalid = iso_fixture();
    invalid[16U * 2048U + 1U] = std::byte{'X'};
    auto result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(invalid)), "invalid.iso");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::container_unrecognized);

    result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture(true)), "extent.iso");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::allocation_invalid_extent);

    auto duplicate = iso_fixture();
    constexpr std::size_t root = 18U * 2048U;
    std::ranges::copy_n(duplicate.begin() + root + 68U, 38, duplicate.begin() + root + 106U);
    result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(duplicate)), "duplicate.iso");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);

    auto traversal = iso_fixture();
    traversal[root + 68U + 33U] = std::byte{'/'};
    result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(traversal)), "traversal.iso");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);

    auto malformed = iso_fixture();
    malformed[18U * 2048U] = std::byte{20};
    result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(malformed)), "malformed.iso");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);
}

TEST(Iso9660Reader, KeepsReadableInventoryWhenDeclaredTailFileIsMissing) {
    auto image = iso_fixture(true);
    constexpr std::size_t pvd = 16U * 2048U;
    le32(image, pvd + 80U, 120U);
    be32(image, pvd + 84U, 120U);
    const auto iso = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(image)), "missing-tail.iso");
    ASSERT_TRUE(iso) << iso.error().message;
    const auto objects = iso->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    EXPECT_TRUE(objects->empty());
}

TEST(Iso9660Reader, MarksContentFallbackAsNavigationAid) {
    auto fixture = iso_fixture();
    std::fill_n(fixture.begin() + 22U * 2048U, 32, std::byte{});
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "fallback.iso");
    ASSERT_TRUE(image) << image.error().message;
    auto objects = image->objects();
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().volume_label.value, "CD WAVE");
    EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::navigation_aid);
    EXPECT_EQ(objects->front().volume_label.basis, "ISO directory path plus content-derived volume label fallback");
}

TEST(Iso9660Reader, DisambiguatesDuplicateContentTreeVolumesByRawIdentifier) {
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "duplicate.iso");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer container{*image};
    auto catalog = axk::build_object_catalog(container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    ASSERT_EQ(catalog->objects.size(), 1U);
    catalog->objects.front().placement->partition_name = " GROUP ";
    catalog->objects.front().placement->volume_name = " Mapped Volume ";
    catalog->objects.front().placement->container_directory = "GROUP/F001";
    auto duplicate = catalog->objects.front();
    duplicate.key = "duplicate";
    duplicate.sfs_id = axk::SfsId{2};
    duplicate.placement->volume_directory = axk::SfsId{2};
    duplicate.placement->container_directory = "GROUP/F002";
    catalog->objects.push_back(std::move(duplicate));

    const auto graph = axk::build_relationship_graph(*catalog);
    const auto tree = axk::build_content_tree(container, *catalog, graph);
    ASSERT_EQ(tree.roots.size(), 1U);
    EXPECT_EQ(tree.roots.front().display_name, "GROUP");
    ASSERT_EQ(tree.roots.front().children.size(), 2U);
    EXPECT_EQ(tree.roots.front().children[0].display_name, "Mapped Volume (F001)");
    EXPECT_EQ(tree.roots.front().children[1].display_name, "Mapped Volume (F002)");
}

TEST(Iso9660Reader, MarksOnlyUnknownActiveSampleBankMembersAsVolumeErrors) {
    auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "member.iso");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer container{*image};
    auto catalog = axk::build_object_catalog(container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    ASSERT_EQ(catalog->objects.size(), 1U);

    auto bank = catalog->objects.front();
    bank.key = "bank";
    bank.object.header.type = axk::ObjectType::sbnk;
    bank.object.header.name = "BANK";
    bank.object.payload = axk::CurrentSbnk{};
    auto program = bank;
    program.key = "program";
    program.object.header.type = axk::ObjectType::prog;
    program.object.header.name = "001";
    program.object.payload = axk::CurrentProg{};
    catalog->objects.push_back(std::move(bank));
    catalog->objects.push_back(std::move(program));

    axk::RelationshipGraph graph;
    axk::Relationship assignment;
    assignment.key = "assignment";
    assignment.source_key = "program";
    assignment.target_key = "bank";
    assignment.type = "PROG_ASSIGNMENT_TO_SBNK";
    assignment.quality = axk::RelationshipQuality::known;
    assignment.basis = "test";
    assignment.assignment_index = 0U;
    assignment.assignment_name = "BANK";
    assignment.assignment_state = axk::AssignmentState::active;
    graph.relationships.push_back(std::move(assignment));
    axk::Relationship member;
    member.key = "member";
    member.source_key = "bank";
    member.type = "SBNK_LEFT_MEMBER_TO_SMPL";
    member.quality = axk::RelationshipQuality::likely;
    member.basis = "test";
    graph.relationships.push_back(std::move(member));

    auto tree = axk::build_content_tree(container, *catalog, graph);
    ASSERT_EQ(tree.roots.size(), 1U);
    ASSERT_EQ(tree.roots.front().children.size(), 1U);
    EXPECT_EQ(tree.roots.front().children.front().display_name, "Mapped Vol");
    EXPECT_TRUE(tree.issues.empty());

    graph.relationships.back().quality = axk::RelationshipQuality::unknown;
    catalog->objects[1].placement.reset();
    tree = axk::build_content_tree(container, *catalog, graph);
    ASSERT_EQ(tree.issues.size(), 2U);
    EXPECT_EQ(tree.issues[0].code, "REL_SBNK_MEMBER_TARGET_MISSING");
    EXPECT_EQ(tree.issues[1].code, "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING");
}

TEST(Iso9660Reader, RejectsMultiExtentAndUsesOnlyPrimaryTreeNames) {
    auto multi_extent = iso_fixture();
    constexpr std::size_t object_record_flags = 20U * 2048U + 68U + 25U;
    multi_extent[object_record_flags] = std::byte{0x80};
    const auto unsupported =
        axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(multi_extent)), "multi-extent.iso");
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, axk::ErrorCode::unsupported_profile);

    auto hybrid = iso_fixture();
    auto supplementary = std::span{hybrid}.subspan(17U * 2048U, 2048U);
    supplementary[0] = std::byte{2};
    ascii(supplementary, 1, "CD001");
    supplementary[6] = std::byte{1};
    ascii(supplementary, 88, "%/E");
    constexpr std::size_t group_record = 18U * 2048U + 68U;
    hybrid[group_record] = std::byte{45};
    ascii(hybrid, group_record + 38U, "SP");
    const auto image = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(hybrid)), "hybrid.iso");
    ASSERT_TRUE(image) << image.error().message;
    EXPECT_NE(std::ranges::find(image->files(), "GROUP", &axk::IsoFile::path), image->files().end());
    EXPECT_EQ(std::ranges::find(image->files(), "Mapped Vol", &axk::IsoFile::path), image->files().end());
}
