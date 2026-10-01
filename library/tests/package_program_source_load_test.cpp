#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/package.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

#include "../src/package_import_support.hpp"

namespace {

class PackageProgramSourceLoad : public testing::TestWithParam<std::uint32_t> {
  protected:
    axk::ObjectSnapshot program;
    axk::ObjectSnapshot source;
    axk::PortablePackage package;
    axk::PackageNode sample;
    axk::PackageRootDestination destination;

    void SetUp() override {
        program.key = "iso:program";
        program.scope_key = "iso:test";
        program.object.header.type = axk::ObjectType::prog;
        program.object.header.raw_type = "PROG";
        program.object.header.name = "008";
        program.object.format = axk::ObjectFormat::current;
        axk::CurrentProg payload;
        payload.program_name = "Source Loader";
        axk::ProgAssignment assignment;
        assignment.name = "Source Sample";
        assignment.kind = 0x10U;
        assignment.raw_handle = GetParam();
        payload.assignments.push_back(std::move(assignment));
        program.object.payload = std::move(payload);
        program.placement.emplace();
        program.placement->container_directory = "G001/F001";
        program.placement_resolution = axk::PlacementResolution::exact;

        source.key = "iso:source";
        source.scope_key = program.scope_key;
        source.object.header.type = axk::ObjectType::smpl;
        source.object.header.raw_type = "SMPL";
        source.object.header.name = "Source Sample";
        source.placement = program.placement;
        source.placement_resolution = axk::PlacementResolution::exact;

        sample.node_id = "incoming-sample";
        sample.object_type = "SBNK";
        sample.name = "Source Sample";
        destination.group_name = "Group";
        destination.volume_name = "Target";
        destination.raw_group = "G001";
        destination.raw_volume = "F001";
    }

    std::vector<axk::package_import_internal::Candidate> candidates() const {
        return {{&package, &sample, &destination, sample.name, "sample-identity", {}, {}}};
    }

    std::vector<axk::package_import_internal::ExistingObject> existing() const {
        return {{&program, {}, {}, {}}, {&source, {}, {}, {}}};
    }

    void expect_source_load() const {
        const std::vector<const axk::ObjectSnapshot *> snapshots{&program, &source};
        const auto graph = axk::build_relationship_graph(snapshots);
        const auto row = std::ranges::find_if(graph.relationships, [&](const auto &relationship) {
            return relationship.source_key == program.key && relationship.assignment_index == 0U;
        });
        ASSERT_NE(row, graph.relationships.end());
        EXPECT_EQ(row->assignment_state, axk::AssignmentState::source_load_assignment);
        EXPECT_EQ(row->quality, axk::RelationshipQuality::likely);
        EXPECT_EQ(row->target_key, source.key);
    }
};

TEST_P(PackageProgramSourceLoad, RejectsRetargetingToAnIncomingSameNameSample) {
    expect_source_load();
    ASSERT_FALSE(HasFatalFailure());
    auto incoming = candidates();
    const auto retained = existing();
    axk::PackageImportPlan plan;

    const auto planned = axk::package_import_internal::plan_program_assignment_adjustments(incoming, retained, plan);

    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_FALSE(plan.valid());
    ASSERT_EQ(plan.conflicts.size(), 1U);
    EXPECT_EQ(plan.conflicts.front().node_id, sample.node_id);
    EXPECT_EQ(plan.conflicts.front().raw_group, destination.raw_group);
    EXPECT_EQ(plan.conflicts.front().raw_volume, destination.raw_volume);
    EXPECT_TRUE(plan.program_assignment_adjustments.empty());
    EXPECT_EQ(std::get<axk::CurrentProg>(program.object.payload).assignments.front().raw_handle, GetParam());
}

TEST_P(PackageProgramSourceLoad, PreservesTheRowWhenIncomingSampleUsesAnotherName) {
    expect_source_load();
    ASSERT_FALSE(HasFatalFailure());
    auto incoming = candidates();
    incoming.front().destination_name = "Other Sample";
    const auto retained = existing();
    axk::PackageImportPlan plan;

    const auto planned = axk::package_import_internal::plan_program_assignment_adjustments(incoming, retained, plan);

    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_TRUE(plan.valid());
    EXPECT_TRUE(plan.program_assignment_adjustments.empty());
}

TEST_P(PackageProgramSourceLoad, PreservesTheRowWhenIncomingSampleUsesAnotherVolume) {
    expect_source_load();
    ASSERT_FALSE(HasFatalFailure());
    destination.raw_volume = "F002";
    auto incoming = candidates();
    const auto retained = existing();
    axk::PackageImportPlan plan;

    const auto planned = axk::package_import_internal::plan_program_assignment_adjustments(incoming, retained, plan);

    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_TRUE(plan.valid());
    EXPECT_TRUE(plan.program_assignment_adjustments.empty());
}

TEST_P(PackageProgramSourceLoad, DoesNotTreatSourceLoadToBankAsADirectBankAssignment) {
    source.object.header.type = axk::ObjectType::sbac;
    source.object.header.raw_type = "SBAC";
    source.object.format = axk::ObjectFormat::current;
    source.object.payload = axk::CurrentSbac{};
    expect_source_load();
    ASSERT_FALSE(HasFatalFailure());
    const axk::ObjectCatalog catalog{{program, source}, {}};
    const auto graph = axk::build_relationship_graph(catalog);

    const auto comparison = std::ranges::find(graph.bitmap_comparisons, source.key, &axk::BitmapComparison::object_key);

    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    EXPECT_EQ(comparison->object_type, axk::ObjectType::sbac);
    EXPECT_TRUE(comparison->direct_assignment_programs.empty());
    EXPECT_TRUE(comparison->bitmap_programs.empty());
    EXPECT_EQ(comparison->status, "match");
    EXPECT_TRUE(axk::validate_program_bitmaps(catalog, graph).empty());

    axk::PackageImportPlan plan;
    axk::PlannedPackageObject bank;
    bank.object_type = "SBAC";
    bank.destination_name = source.object.header.name;
    bank.existing_object_key = source.key;
    bank.actions = {axk::PackageImportObjectAction::reuse};
    plan.objects.push_back(std::move(bank));
    const std::vector packages{package};
    const auto retained = existing();

    const auto planned = axk::package_import_internal::plan_program_links(packages, retained, plan);

    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_TRUE(plan.valid());
    EXPECT_TRUE(plan.objects.front().target_program_numbers.empty());
}

INSTANTIATE_TEST_SUITE_P(StoredHandles, PackageProgramSourceLoad,
                         testing::Values(std::uint32_t{0U}, std::uint32_t{0x12345678U}));

class PackageProgramDormantCollision
    : public testing::TestWithParam<std::tuple<axk::MediaKind, const char *, std::uint32_t>> {};

TEST_P(PackageProgramDormantCollision, RejectsSourceHandlesAndExplicitlyClearsZeroHandles) {
    const auto &[media, object_type, handle] = GetParam();
    axk::ObjectSnapshot program;
    program.key = "existing-program";
    program.object.header.type = axk::ObjectType::prog;
    program.object.header.raw_type = "PROG";
    program.object.header.name = "008";
    program.object.format = axk::ObjectFormat::current;
    axk::CurrentProg payload;
    payload.program_name = "Unresolved";
    axk::ProgAssignment row;
    row.name = "Missing Object";
    row.kind = std::string_view{object_type} == "SBAC" ? std::uint8_t{0x11U} : std::uint8_t{0x10U};
    row.raw_handle = handle;
    payload.assignments.push_back(std::move(row));
    program.object.payload = std::move(payload);
    program.placement.emplace();
    program.placement->volume_name = "Target";

    axk::PackageRootDestination destination;
    destination.partition_index = 0U;
    destination.volume_name = "Target";
    if (media == axk::MediaKind::iso9660) {
        program.scope_key = "iso:test";
        program.placement->container_directory = "G001/F001";
        destination.raw_group = "G001";
        destination.raw_volume = "F001";
    } else if (media == axk::MediaKind::fat12_floppy) {
        program.scope_key = "fat12:test";
        destination.volume_name = "FAT root";
    } else {
        program.scope_key = "partition:0";
    }
    axk::PortablePackage package;
    axk::PackageNode node;
    node.node_id = "incoming-object";
    node.object_type = object_type;
    node.name = "Missing Object";
    std::vector<axk::package_import_internal::Candidate> incoming{
        {&package, &node, &destination, node.name, "incoming-identity", {}, {}}};
    const std::vector<axk::package_import_internal::ExistingObject> retained{{&program, {}, {}, {}}};
    axk::PackageImportPlan plan;
    plan.target_kind = media;

    const auto planned = axk::package_import_internal::plan_program_assignment_adjustments(incoming, retained, plan);

    ASSERT_TRUE(planned) << planned.error().message;
    if (handle == 0U) {
        EXPECT_TRUE(plan.valid());
        ASSERT_EQ(plan.program_assignment_adjustments.size(), 1U);
        const auto &adjustment = plan.program_assignment_adjustments.front();
        EXPECT_EQ(adjustment.origin, axk::PackageProgramAssignmentOrigin::existing_program);
        EXPECT_EQ(adjustment.existing_object_key, program.key);
        EXPECT_EQ(adjustment.assignment_ordinal, 0U);
        EXPECT_EQ(adjustment.target_object_type, object_type);
    } else {
        EXPECT_FALSE(plan.valid());
        ASSERT_EQ(plan.conflicts.size(), 1U);
        EXPECT_EQ(plan.conflicts.front().node_id, node.node_id);
        EXPECT_EQ(plan.conflicts.front().volume_name, destination.volume_name);
        EXPECT_TRUE(plan.program_assignment_adjustments.empty());
    }
    const auto &preserved = std::get<axk::CurrentProg>(program.object.payload).assignments.front();
    EXPECT_EQ(preserved.name, node.name);
    EXPECT_EQ(preserved.raw_handle, handle);
}

INSTANTIATE_TEST_SUITE_P(
    AllContainersAndTargets, PackageProgramDormantCollision,
    testing::Combine(testing::Values(axk::MediaKind::sfs, axk::MediaKind::fat12_floppy, axk::MediaKind::iso9660),
                     testing::Values("SBNK", "SBAC"), testing::Values(std::uint32_t{0U}, std::uint32_t{0x12345678U})));

} // namespace
