#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/writer.hpp"

#include "../src/package_import_program_slots.hpp"

namespace {

std::filesystem::path test_root(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::filesystem::path{name};
}

axk::Waveform waveform() {
    axk::Waveform value;
    value.format = {1U, 2U, 44100U};
    value.frame_count = 4U;
    value.pcm.resize(8U);
    return value;
}

axk::VolumeSpec program_volume(std::string name, std::string_view bank_prefix,
                               std::span<const std::uint8_t> program_slots, const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = std::move(name);
    for (const auto slot : program_slots) {
        const auto waveform_id = std::format("W{}{:03}", bank_prefix, slot);
        const auto banked_name = std::format("G{}{:03}", bank_prefix, slot);
        const auto direct_name = std::format("D{}{:03}", bank_prefix, slot);
        const auto bank_name = std::format("B{}{:03}", bank_prefix, slot);
        volume.waveforms.push_back({waveform_id, waveform_id, audio_path, 60U, {}});
        for (const auto &sample_name : {banked_name, direct_name}) {
            axk::SampleSpec sample;
            sample.name = sample_name;
            sample.waveform_id = waveform_id;
            sample.parameters.root_key = 60U;
            sample.parameters.key_high = 127U;
            volume.samples.push_back(std::move(sample));
        }
        volume.sample_banks.push_back({bank_name, {banked_name}});
        volume.programs.push_back(
            {slot, std::format("Pgm {:03}", slot), {{"SBAC", bank_name, 1U}, {"SBNK", direct_name, 2U}}});
    }
    return volume;
}

std::vector<std::uint8_t> slots(std::uint8_t count) {
    std::vector<std::uint8_t> result;
    result.reserve(count);
    for (std::uint8_t slot = 1U; slot <= count; ++slot)
        result.push_back(slot);
    return result;
}

std::vector<axk::PackageRootSelector> program_roots(std::uint8_t count) {
    std::vector<axk::PackageRootSelector> roots;
    roots.reserve(count);
    for (std::uint8_t slot = 1U; slot <= count; ++slot) {
        axk::PackageRootSelector root;
        root.kind = axk::PackageRootKind::prog;
        root.partition_index = 0U;
        root.volume_name = "Source";
        root.object_name = std::format("{:03}", slot);
        roots.push_back(std::move(root));
    }
    return roots;
}

axk::PackageImportRequest import_request(std::size_t root_count) {
    axk::PackageImportRequest request;
    request.root_destinations.reserve(root_count);
    for (std::size_t root_index = 0U; root_index < root_count; ++root_index) {
        axk::PackageRootDestination destination;
        destination.package_index = 0U;
        destination.root_index = root_index;
        destination.partition_index = 0U;
        destination.volume_name = "Target";
        request.root_destinations.push_back(std::move(destination));
    }
    return request;
}

TEST(PackageProgramSlots, FallsBackToTheLowestFragmentedFreeSlots) {
    axk::package_import_internal::ProgramSlotPlanningInput input;
    const axk::package_import_internal::DestinationKey destination{0U, "Target"};
    input.candidates = {
        {0U, "program-1", destination, 1U, false},
        {0U, "program-2", destination, 2U, false},
        {0U, "program-3", destination, 3U, false},
    };
    for (std::uint16_t slot = 2U; slot <= 128U; slot += 2U)
        input.occupied_slots[destination].insert(static_cast<std::uint8_t>(slot));

    const auto placements = axk::package_import_internal::plan_program_slot_placements(input);

    ASSERT_EQ(placements.size(), 1U);
    const auto &placement = placements.front();
    EXPECT_EQ(placement.mode, axk::PackageProgramSlotPlacementMode::fragmented);
    EXPECT_FALSE(placement.applied);
    EXPECT_EQ(placement.destination_ranges, (std::vector<axk::PackageProgramSlotRange>{{1U, 1U}, {3U, 3U}, {5U, 5U}}));
    ASSERT_EQ(placement.mappings.size(), 3U);
    EXPECT_EQ(placement.mappings[0].destination_slot, 1U);
    EXPECT_EQ(placement.mappings[1].destination_slot, 3U);
    EXPECT_EQ(placement.mappings[2].destination_slot, 5U);
}

TEST(PackageProgramSlots, KeepsProgramsGroupedInPackageOrder) {
    axk::package_import_internal::ProgramSlotPlanningInput input;
    const axk::package_import_internal::DestinationKey destination{0U, "Target"};
    for (std::size_t package_index = 0U; package_index < 2U; ++package_index) {
        for (std::uint8_t source_slot = 1U; source_slot <= 4U; ++source_slot) {
            input.candidates.push_back({package_index, std::format("package-{}-program-{}", package_index, source_slot),
                                        destination, source_slot, false});
        }
    }

    const auto placements = axk::package_import_internal::plan_program_slot_placements(input);

    ASSERT_EQ(placements.size(), 1U);
    const auto &placement = placements.front();
    ASSERT_EQ(placement.mappings.size(), 8U);
    for (std::size_t index = 0U; index < placement.mappings.size(); ++index) {
        const auto &mapping = placement.mappings[index];
        EXPECT_EQ(mapping.package_index, index / 4U);
        EXPECT_EQ(mapping.source_slot, index % 4U + 1U);
        EXPECT_EQ(mapping.destination_slot, index + 1U);
    }
}

TEST(PackageProgramSlots, ExcludesExactReuseFromCompactPlacement) {
    axk::package_import_internal::ProgramSlotPlanningInput input;
    const axk::package_import_internal::DestinationKey destination{0U, "Target"};
    input.candidates = {
        {0U, "reused-program", destination, 1U, true},
        {0U, "conflicting-program", destination, 2U, false},
    };
    input.occupied_slots[destination] = {1U, 2U};

    const auto placements = axk::package_import_internal::plan_program_slot_placements(input);

    ASSERT_EQ(placements.size(), 1U);
    const auto &placement = placements.front();
    EXPECT_EQ(placement.required_slot_count, 1U);
    EXPECT_EQ(placement.source_ranges, (std::vector<axk::PackageProgramSlotRange>{{2U, 2U}}));
    EXPECT_EQ(placement.destination_ranges, (std::vector<axk::PackageProgramSlotRange>{{3U, 3U}}));
    ASSERT_EQ(placement.mappings.size(), 1U);
    EXPECT_EQ(placement.mappings.front().node_id, "conflicting-program");
}

TEST(PackageProgramSlots, ReportsIncomingCountsLargerThanTheDestinationCapacity) {
    axk::package_import_internal::ProgramSlotPlanningInput input;
    const axk::package_import_internal::DestinationKey destination{0U, "Target"};
    for (std::size_t index = 0U; index < 129U; ++index) {
        input.candidates.push_back({index, std::format("program-{}", index), destination,
                                    static_cast<std::uint8_t>(index % 128U + 1U), false});
    }

    const auto placements = axk::package_import_internal::plan_program_slot_placements(input);

    ASSERT_EQ(placements.size(), 1U);
    const auto &placement = placements.front();
    EXPECT_EQ(placement.mode, axk::PackageProgramSlotPlacementMode::unavailable);
    EXPECT_EQ(placement.required_slot_count, 129U);
    EXPECT_EQ(placement.available_slot_count, 128U);
    EXPECT_TRUE(placement.mappings.empty());
    EXPECT_TRUE(placement.destination_ranges.empty());
    EXPECT_FALSE(placement.suggested_start_slot);
}

TEST(PackageProgramSlots, SuggestsAndAcceptsACompactRangeAfterOccupiedTargetSlots) {
    const auto root = test_root("axklib-package-program-slot-compact");
    const auto audio_path = root / "tone.wav";
    const auto source_path = root / "source.hds";
    const auto target_path = root / "target.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform()));
    axk::HdsBuildManifest source_manifest{"1.0", 8U * 1024U * 1024U, {}};
    const auto source_slots = slots(33U);
    source_manifest.partitions.push_back({"P1", {program_volume("Source", "S", source_slots, audio_path)}});
    const auto source_written = axk::write_hds_image(source_manifest, source_path);
    ASSERT_TRUE(source_written) << source_written.error().message;
    const std::array occupied{std::uint8_t{1U}, std::uint8_t{2U}, std::uint8_t{3U}, std::uint8_t{4U}};
    axk::HdsBuildManifest target_manifest{"1.0", 8U * 1024U * 1024U, {}};
    target_manifest.partitions.push_back({"P1", {program_volume("Target", "T", occupied, audio_path)}});
    const auto target_written = axk::write_hds_image(target_manifest, target_path);
    ASSERT_TRUE(target_written) << target_written.error().message;

    const auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const auto built = axk::build_portable_package(*source, program_roots(33U));
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    auto request = import_request(built->package.roots.size());
    const auto suggested = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(suggested) << suggested.error().message;
    ASSERT_FALSE(suggested->valid());
    ASSERT_EQ(suggested->program_slot_placements.size(), 1U);
    const auto &placement = suggested->program_slot_placements.front();
    EXPECT_EQ(placement.mode, axk::PackageProgramSlotPlacementMode::contiguous);
    EXPECT_FALSE(placement.applied);
    EXPECT_EQ(placement.required_slot_count, 33U);
    EXPECT_EQ(placement.available_slot_count, 124U);
    ASSERT_EQ(placement.occupied_ranges.size(), 1U);
    EXPECT_EQ(placement.occupied_ranges.front(), (axk::PackageProgramSlotRange{1U, 4U}));
    ASSERT_EQ(placement.source_ranges.size(), 1U);
    EXPECT_EQ(placement.source_ranges.front(), (axk::PackageProgramSlotRange{1U, 33U}));
    ASSERT_EQ(placement.destination_ranges.size(), 1U);
    EXPECT_EQ(placement.destination_ranges.front(), (axk::PackageProgramSlotRange{5U, 37U}));
    ASSERT_EQ(placement.mappings.size(), 33U);
    auto partial_request = import_request(built->package.roots.size());
    partial_request.policy.program_slot_assignments.push_back({placement.mappings.front().package_index,
                                                               placement.mappings.front().node_id,
                                                               placement.mappings.front().destination_slot});
    const auto partial = axk::plan_package_import(target_path, packages, partial_request);
    ASSERT_TRUE(partial) << partial.error().message;
    EXPECT_TRUE(std::ranges::any_of(partial->conflicts, [](const auto &conflict) {
        return conflict.code == "SFS_PROGRAM_SLOT_ASSIGNMENTS_INCOMPLETE";
    }));
    for (std::size_t index = 0U; index < placement.mappings.size(); ++index) {
        EXPECT_EQ(placement.mappings[index].source_slot, index + 1U);
        EXPECT_EQ(placement.mappings[index].destination_slot, index + 5U);
        request.policy.program_slot_assignments.push_back({placement.mappings[index].package_index,
                                                           placement.mappings[index].node_id,
                                                           placement.mappings[index].destination_slot});
    }

    const auto accepted = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(accepted) << accepted.error().message;
    ASSERT_TRUE(accepted->valid());
    ASSERT_EQ(accepted->program_slot_placements.size(), 1U);
    EXPECT_TRUE(accepted->program_slot_placements.front().applied);
    EXPECT_EQ(accepted->program_slot_placements.front().destination_ranges, placement.destination_ranges);
    std::filesystem::remove_all(root, error);
}

} // namespace
