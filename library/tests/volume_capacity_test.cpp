#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "axklib/volume_capacity.hpp"
#include "axklib/volume_capacity_internal.hpp"

namespace {

axk::detail::CapacityObject object(axk::ObjectType type, unsigned name, std::uint32_t bytes = 128U) {
    axk::detail::CapacityObject result;
    result.type = type;
    result.name.fill(std::byte{' '});
    const auto text = "N" + std::to_string(name);
    std::ranges::transform(text, result.name.begin(), [](char value) { return static_cast<std::byte>(value); });
    result.selector = 4U;
    result.encoded_body_bytes = bytes;
    result.older_body_bytes = bytes;
    return result;
}

axk::detail::CapacityVolume volume() { return {{0U}, {9U}, "Test", {}, {}}; }

axk::VolumeCapacityReport analyze(const axk::detail::CapacityVolume &input) {
    const auto report = axk::detail::analyze_volume_capacity(input);
    EXPECT_TRUE(report) << (report ? "" : report.error().message);
    return report.value();
}

} // namespace

TEST(VolumeCapacity, PublishesSeparatePoolsAndFreshWipeReservations) {
    const auto report = analyze(volume());
    ASSERT_EQ(report.profiles.size(), 2U);
    EXPECT_EQ(report.profiles[0].parameter_byte_limit, 524288U);
    EXPECT_EQ(report.profiles[0].shared_object_slot_limit, 1024U);
    EXPECT_EQ(report.profiles[0].baseline_bytes, 87720U);
    EXPECT_EQ(report.profiles[0].baseline_slots, 129U);
    EXPECT_EQ(report.profiles[1].parameter_byte_limit, 786432U);
    EXPECT_EQ(report.profiles[1].shared_object_slot_limit, 2048U);
    EXPECT_EQ(report.profiles[1].baseline_bytes, 111280U);
    EXPECT_EQ(report.profiles[1].baseline_slots, 130U);
}

TEST(VolumeCapacity, CompleteSequenceResidencyProvesBothTargetsCannotLoad) {
    auto input = volume();
    input.objects.push_back(object(axk::ObjectType::sequ, 1U, 786444U));
    const auto report = analyze(input);
    for (const auto &profile : report.profiles) {
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::does_not_fit);
        ASSERT_TRUE(profile.minimum_resident_bytes);
        EXPECT_GT(*profile.minimum_resident_bytes, profile.parameter_byte_limit);
        EXPECT_FALSE(profile.resident_bytes);
        EXPECT_FALSE(profile.peak_bytes);
        EXPECT_FALSE(profile.peak_slots);
    }
}

TEST(VolumeCapacity, TinySequenceReturnsAnActionableError) {
    auto input = volume();
    input.objects.push_back(object(axk::ObjectType::sequ, 1U, 64U));
    const auto report = axk::detail::analyze_volume_capacity(input);
    ASSERT_FALSE(report);
    EXPECT_NE(report.error().message.find("Sequence body"), std::string::npos);
}

TEST(VolumeCapacity, StoredNameReplacementKeepsTheIncomingAllocationOverlap) {
    auto input = volume();
    auto first = object(axk::ObjectType::sequ, 1U, 128U);
    first.filesystem_name = first.name;
    auto second = first;
    second.filesystem_name = object(axk::ObjectType::sequ, 2U).name;
    input.objects = {first, second};
    for (const auto &profile : analyze(input).profiles) {
        ASSERT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + 120U);
        EXPECT_EQ(profile.peak_bytes, profile.baseline_bytes + 240U);
    }
}

TEST(VolumeCapacity, SkippedMissingWaveDoesNotHideALaterSequenceAllocationFailure) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 2U).name});
    input.objects.push_back(sample);
    input.objects.push_back(object(axk::ObjectType::sequ, 3U, 786444U));
    const auto report = analyze(input);
    for (const auto &profile : report.profiles) {
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::does_not_fit);
        ASSERT_TRUE(profile.minimum_resident_bytes);
        EXPECT_GT(*profile.minimum_resident_bytes, profile.parameter_byte_limit);
    }
}

TEST(VolumeCapacity, AmbiguousVolumePlacementReturnsAnErrorNotAFitClaim) {
    auto input = volume();
    input.issues.push_back({"AMBIGUOUS_VOLUME", "The volume has multiple physical placements."});
    const auto report = axk::detail::analyze_volume_capacity(input);
    ASSERT_FALSE(report);
    EXPECT_NE(report.error().message.find("multiple physical placements"), std::string::npos);
}

TEST(VolumeCapacity, RegisteredNameNormalizationUsesFixedWidthAndReplacesCollisions) {
    auto input = volume();
    auto sequence = object(axk::ObjectType::sequ, 1U, 128U);
    sequence.filesystem_name = sequence.name;
    sequence.name.back() = std::byte{};
    input.objects.push_back(sequence);
    auto other = sequence;
    other.filesystem_name = object(axk::ObjectType::sequ, 2U).name;
    other.name.back() = std::byte{'_'};
    input.objects.push_back(other);
    const auto report = analyze(input);
    for (const auto &profile : report.profiles) {
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + 120U);
        EXPECT_EQ(profile.peak_bytes, profile.baseline_bytes + 240U);
    }
}

TEST(VolumeCapacity, FilenameLookupSkipsAnAlreadyRegisteredSequenceBeforeAllocation) {
    auto input = volume();
    auto first = object(axk::ObjectType::sequ, 1U, 128U);
    first.filesystem_name = object(axk::ObjectType::sequ, 2U).name;
    auto second = object(axk::ObjectType::sequ, 3U, 786444U);
    second.filesystem_name = first.name;
    input.objects = {first, second};
    for (const auto &profile : analyze(input).profiles) {
        ASSERT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + 120U);
        EXPECT_EQ(profile.peak_bytes, profile.resident_bytes);
    }
}

TEST(VolumeCapacity, BlankStoredOwnNameFallsBackToFilesystemNameBeforeNormalization) {
    auto input = volume();
    auto first = object(axk::ObjectType::sequ, 1U, 128U);
    first.filesystem_name = first.name;
    first.name.fill(std::byte{});
    auto second = object(axk::ObjectType::sequ, 2U, 786444U);
    second.filesystem_name = first.filesystem_name;
    input.objects = {first, second};
    for (const auto &profile : analyze(input).profiles) {
        ASSERT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + 120U);
    }
}

TEST(VolumeCapacity, A3000UsesOlderLengthViewIndependentlyOfLaterGeneration) {
    auto input = volume();
    auto sequence = object(axk::ObjectType::sequ, 1U, 786444U);
    sequence.older_body_bytes = 128U;
    input.objects.push_back(sequence);
    const auto report = analyze(input);
    EXPECT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::does_not_fit);
}

TEST(VolumeCapacity, CompleteSequencesReserveBaselineAtBothByteBoundaries) {
    for (const auto index : {0U, 1U}) {
        const auto baseline = index == 0U ? 87720U : 111280U;
        const auto limit = index == 0U ? 524288U : 786432U;
        auto input = volume();
        input.objects.push_back(object(axk::ObjectType::sequ, 1U, limit - baseline + 8U));
        auto row = analyze(input).profiles[index];
        EXPECT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(row.resident_bytes, limit);
        EXPECT_EQ(row.peak_bytes, limit);
        input.objects.front().encoded_body_bytes += 4U;
        input.objects.front().older_body_bytes += 4U;
        row = analyze(input).profiles[index];
        EXPECT_EQ(row.status, axk::VolumeCapacityStatus::does_not_fit);
        EXPECT_FALSE(row.resident_bytes);
        EXPECT_FALSE(row.peak_bytes);
    }
}

TEST(VolumeCapacity, SharedSlotLimitIncludesFreshPrograms) {
    auto input = volume();
    for (unsigned i = 0U; i < 895U; ++i)
        input.objects.push_back(object(axk::ObjectType::sequ, i));
    EXPECT_EQ(analyze(input).profiles[0].status, axk::VolumeCapacityStatus::fits);
    input.objects.push_back(object(axk::ObjectType::sequ, 895U));
    const auto report = analyze(input);
    EXPECT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::does_not_fit);
    EXPECT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
}

TEST(VolumeCapacity, TargetSpecificSequenceLengthMustFitPhysicalRecord) {
    auto input = volume();
    auto sequence = object(axk::ObjectType::sequ, 1U, 256U);
    sequence.older_body_bytes = 128U;
    sequence.physical_body_bytes = 128U;
    input.objects.push_back(sequence);
    const auto report = axk::detail::analyze_volume_capacity(input);
    ASSERT_FALSE(report);
    EXPECT_NE(report.error().message.find("A4000_A5000"), std::string::npos);
}

TEST(VolumeCapacity, SamplesShareWavesAndUseTargetSpecificNativeFootprints) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 308U);
    sample.selector = 2U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    input.objects.push_back(sample);
    sample.name = object(axk::ObjectType::sbnk, 2U).name;
    input.objects.push_back(sample);
    input.objects.push_back(object(axk::ObjectType::smpl, 9U));
    const auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 2U * 300U + 72U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 2U * 336U + 72U);
    EXPECT_EQ(report.profiles[1].peak_slots, 133U);
}

TEST(VolumeCapacity, WaveOnlyVolumeDoesNotLoadUnreferencedWavesIntoParameterMemory) {
    auto input = volume();
    input.objects.push_back(object(axk::ObjectType::smpl, 1U));
    for (const auto &row : analyze(input).profiles) {
        EXPECT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes);
        EXPECT_EQ(row.peak_slots, row.baseline_slots);
    }
}

TEST(VolumeCapacity, WaveLoadPeakIncludesTheTemporaryMetadataBeforeShrink) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 308U);
    sample.selector = 2U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    input.objects = {sample, object(axk::ObjectType::smpl, 9U)};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(row.peak_bytes, *row.resident_bytes + 48U);
    }
}

TEST(VolumeCapacity, CountedEmptyProgramRowsWithinEightPreservePhysicalAllocation) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 856U + 8U);
    program.older_body_bytes = 680U + 8U;
    program.name.fill(std::byte{' '});
    program.name[0] = std::byte{'0'};
    program.name[1] = std::byte{'0'};
    program.name[2] = std::byte{'1'};
    program.physical_rows = 8U;
    program.counted_rows = program.empty_counted_rows = 1U;
    input.objects.push_back(program);
    const auto row = analyze(input).profiles[1];
    EXPECT_EQ(row.status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(row.resident_bytes, 111280U);
    EXPECT_EQ(row.peak_bytes, 111280U + 856U);
}

TEST(VolumeCapacity, AdmissionRejectsFailuresAndAChangedTarget) {
    axk::VolumeCapacityAdmission admission;
    admission.allowed = false;
    axk::VolumeCapacityPolicy policy{admission.target};
    EXPECT_FALSE(axk::enforce_volume_capacity_admission(admission, policy));
    admission.allowed = true;
    EXPECT_TRUE(axk::enforce_volume_capacity_admission(admission, policy));
    policy = {axk::ASeriesLoadTarget::a3000};
    EXPECT_FALSE(axk::enforce_volume_capacity_admission(admission, policy));
}

TEST(VolumeCapacity, NativeBankPhysicalCapacityAndProgramReplacementOverlapAreRetained) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.bank_member = true;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    auto bank = object(axk::ObjectType::sbac, 2U, 312U + 20U * 8U + 8U);
    bank.older_body_bytes = bank.encoded_body_bytes - 36U;
    bank.counted_rows = 1U;
    bank.physical_rows = 8U;
    bank.references.push_back({axk::ObjectType::sbnk, sample.name});
    auto program = object(axk::ObjectType::prog, 3U, 408U + 56U * 8U + 8U);
    program.name.fill(std::byte{' '});
    program.name[0] = std::byte{'0'};
    program.name[1] = std::byte{'0'};
    program.name[2] = std::byte{'1'};
    program.older_body_bytes = program.encoded_body_bytes - 176U;
    program.counted_rows = 1U;
    program.physical_rows = 8U;
    program.references.push_back({axk::ObjectType::sbac, bank.name});
    input.objects = {program, bank, sample, object(axk::ObjectType::smpl, 9U)};
    const auto row = analyze(input).profiles[1];
    ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(row.resident_bytes, 111280U + 336U + 72U + 472U);
    EXPECT_EQ(row.peak_bytes, *row.resident_bytes + 856U);
    EXPECT_EQ(row.peak_slots, 134U);
    program.references = {{axk::ObjectType::sbnk, sample.name}};
    input.objects[0] = program;
    const auto direct = analyze(input).profiles[1];
    EXPECT_EQ(direct.status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(direct.resident_bytes, *row.resident_bytes + 336U);
    EXPECT_EQ(direct.peak_bytes, row.peak_bytes);
}

TEST(VolumeCapacity, LaterObjectsLoadOnBothGenerationsWithoutAnExtraExtensionBody) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    auto bank = object(axk::ObjectType::sbac, 2U, 480U);
    bank.older_body_bytes = 444U;
    bank.physical_rows = 8U;
    bank.counted_rows = 1U;
    bank.references.push_back({axk::ObjectType::sbnk, sample.name});
    input.objects = {sample, bank, object(axk::ObjectType::smpl, 9U)};
    const auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U + 72U + 436U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 336U + 72U + 472U);
}

TEST(VolumeCapacity, RepeatedRowsShareOneOwnerButDistinctBanksCopyWholeSamples) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    auto bank = object(axk::ObjectType::sbac, 2U, 480U);
    bank.older_body_bytes = 444U;
    bank.physical_rows = 8U;
    bank.counted_rows = 2U;
    bank.references = {{axk::ObjectType::sbnk, sample.name}, {axk::ObjectType::sbnk, sample.name}};
    auto other = bank;
    other.name = object(axk::ObjectType::sbac, 3U).name;
    input.objects = {sample, bank, other, object(axk::ObjectType::smpl, 9U)};
    const auto report = analyze(input);
    for (const auto &row : report.profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U;
        const auto bank_bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 436U : 472U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + bytes * 2U + bank_bytes * 2U + 72U);
        EXPECT_EQ(row.peak_slots, row.baseline_slots + 5U);
    }
}

TEST(VolumeCapacity, ArbitraryProgramNamesAddAnObjectRatherThanReplacingADefault) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 864U);
    program.older_body_bytes = 688U;
    program.physical_rows = 8U;
    input.objects.push_back(program);
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 680U : 856U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + bytes);
        EXPECT_EQ(row.peak_slots, row.baseline_slots + 1U);
    }
}

TEST(VolumeCapacity, CountChangesAboveEightShrinkButUnchangedCountsKeepSpareRows) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 408U + 56U * 16U + 8U);
    program.older_body_bytes = program.encoded_body_bytes - 176U;
    program.physical_rows = 16U;
    program.counted_rows = program.empty_counted_rows = 9U;
    input.objects.push_back(program);
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto body = row.target == axk::ASeriesLoadTarget::a3000 ? 232U : 408U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + body + 56U * 8U);
        EXPECT_EQ(row.peak_bytes, row.baseline_bytes + body + 56U * 16U);
    }
    input.objects.front().counted_rows = input.objects.front().empty_counted_rows = 0U;
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto body = row.target == axk::ASeriesLoadTarget::a3000 ? 232U : 408U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + body + 56U * 16U);
    }
}

TEST(VolumeCapacity, MembershipCopiesCannotUseSavingsFromLaterEmptyRowCleanup) {
    for (const auto index : {0U, 1U}) {
        auto input = volume();
        auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
        sample.older_body_bytes = 308U;
        sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
        auto bank = object(axk::ObjectType::sbac, 2U, 480U);
        bank.older_body_bytes = 444U;
        bank.physical_rows = 8U;
        bank.counted_rows = 1U;
        bank.references.push_back({axk::ObjectType::sbnk, sample.name});
        auto other = bank;
        other.name = object(axk::ObjectType::sbac, 3U).name;
        other.physical_rows = 16U;
        other.counted_rows = 9U;
        other.empty_counted_rows = 8U;
        other.encoded_body_bytes += 160U;
        other.older_body_bytes += 160U;
        const auto baseline = index == 0U ? 87720U : 111280U;
        const auto limit = index == 0U ? 524288U : 786432U;
        const auto sample_bytes = index == 0U ? 300U : 336U;
        const auto bank_bytes = index == 0U ? 436U : 472U;
        auto sequence =
            object(axk::ObjectType::sequ, 4U, limit - baseline - sample_bytes * 2U - 72U - bank_bytes * 2U - 160U + 8U);
        input.objects = {sample, bank, other, sequence, object(axk::ObjectType::smpl, 9U)};
        const auto fit = analyze(input).profiles[index];
        ASSERT_EQ(fit.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(fit.peak_bytes, limit);
        EXPECT_EQ(fit.resident_bytes, limit - 160U);
        input.objects[3].encoded_body_bytes += 4U;
        input.objects[3].older_body_bytes += 4U;
        const auto fail = analyze(input).profiles[index];
        EXPECT_EQ(fail.status, axk::VolumeCapacityStatus::does_not_fit);
        ASSERT_FALSE(fail.reasons.empty());
        EXPECT_NE(fail.reasons.back().message.find("member copy"), std::string::npos);
    }
}

TEST(VolumeCapacity, InactiveRowsWithRetainedPointersAreNotRemovedByCleanup) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 408U + 56U * 16U + 8U);
    program.older_body_bytes = program.encoded_body_bytes - 176U;
    program.physical_rows = 16U;
    program.counted_rows = program.empty_counted_rows = 9U;
    program.inactive_row_handles.assign(9U, 1U);
    input.objects.push_back(program);
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto body = row.target == axk::ASeriesLoadTarget::a3000 ? 232U : 408U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + body + 56U * 16U);
    }
}

TEST(VolumeCapacity, BothGenerationsClearSavedActiveBankHandlesBeforeMembershipReconciliation) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
    auto bank = object(axk::ObjectType::sbac, 2U, 480U);
    bank.older_body_bytes = 444U;
    bank.physical_rows = 8U;
    bank.counted_rows = 1U;
    bank.references = {{axk::ObjectType::sbnk, sample.name, 1U}};
    auto other = bank;
    other.name = object(axk::ObjectType::sbac, 3U).name;
    other.references.front().raw_handle = 0U;
    input.objects = {sample, bank, other, object(axk::ObjectType::smpl, 9U)};
    auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U * 2U + 72U + 436U * 2U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 336U * 2U + 72U + 472U * 2U);
    input.objects[1].references.front().raw_handle = 0x09130f20U + 129U * 24U;
    report = analyze(input);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U * 2U + 72U + 436U * 2U);
}

TEST(VolumeCapacity, CountedInactiveHandlesCanRequireTheFirstBankToCopyItsSample) {
    for (const auto index : {0U, 1U}) {
        auto input = volume();
        auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
        sample.older_body_bytes = 308U;
        sample.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 9U).name});
        auto bank = object(axk::ObjectType::sbac, 2U, 480U);
        bank.older_body_bytes = 444U;
        bank.physical_rows = 8U;
        bank.counted_rows = 1U;
        bank.references = {{axk::ObjectType::sbnk, sample.name}};
        auto inactive = bank;
        inactive.name = object(axk::ObjectType::sbac, 3U).name;
        inactive.references.clear();
        inactive.empty_counted_rows = 1U;
        inactive.inactive_row_handles = {index == 0U ? 0x09130f20U + 143U * 24U : 0x01443000U + 144U * 24U};
        input.objects = {sample, bank, inactive, object(axk::ObjectType::smpl, 9U)};
        const auto row = analyze(input).profiles[index];
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(row.resident_bytes,
                  row.baseline_bytes + (index == 0U ? 300U : 336U) * 2U + 72U + (index == 0U ? 436U : 472U) * 2U);
    }
}

TEST(VolumeCapacity, A3000SkipsFilenameMatchedBanksWhileLaterGenerationsLoadTheirBodies) {
    auto input = volume();
    auto first = object(axk::ObjectType::sbac, 1U, 480U);
    first.older_body_bytes = 444U;
    first.physical_rows = 8U;
    first.filesystem_name = object(axk::ObjectType::sbac, 2U).name;
    auto second = first;
    second.name = object(axk::ObjectType::sbac, 3U).name;
    second.filesystem_name = first.name;
    input.objects = {first, second};
    const auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 436U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 472U * 2U);
}

TEST(VolumeCapacity, MismatchedWaveOwnNameDoesNotShrinkAndOnlyLaterTargetsDeleteUnlinkedSamples) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    auto wave = object(axk::ObjectType::smpl, 2U);
    wave.filesystem_name = object(axk::ObjectType::smpl, 3U).name;
    sample.references = {{axk::ObjectType::smpl, *wave.filesystem_name}};
    input.objects = {sample, wave};
    const auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U + 120U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 120U);
    EXPECT_EQ(report.profiles[1].peak_bytes, 111280U + 336U + 120U);
}

TEST(VolumeCapacity, WaveReplacementRetargetsPriorSamplesWithoutShrinkingItsNewBody) {
    auto input = volume();
    auto first = object(axk::ObjectType::sbnk, 1U, 344U);
    first.older_body_bytes = 308U;
    auto first_wave = object(axk::ObjectType::smpl, 2U);
    first.references = {{axk::ObjectType::smpl, first_wave.name}};
    auto second = first;
    second.name = object(axk::ObjectType::sbnk, 3U).name;
    auto second_wave = first_wave;
    second_wave.filesystem_name = object(axk::ObjectType::smpl, 4U).name;
    second.references.front().name = *second_wave.filesystem_name;
    input.objects = {first, second, first_wave, second_wave};
    const auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U * 2U + 120U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 336U + 120U);
    EXPECT_EQ(report.profiles[1].peak_bytes, 111280U + 336U * 2U + 72U + 120U);
}

TEST(VolumeCapacity, LaterLeftNullCleanupReleasesOnlyASoleOwnedRightWave) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    auto left = object(axk::ObjectType::smpl, 2U);
    left.filesystem_name = object(axk::ObjectType::smpl, 3U).name;
    auto right = object(axk::ObjectType::smpl, 4U);
    sample.references = {{axk::ObjectType::smpl, *left.filesystem_name}, {axk::ObjectType::smpl, right.name}};
    input.objects = {sample, left, right};
    auto report = analyze(input);
    ASSERT_EQ(report.profiles[0].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[0].resident_bytes, 87720U + 300U + 120U + 72U);
    ASSERT_EQ(report.profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 120U);
    auto prior = sample;
    prior.name = object(axk::ObjectType::sbnk, 5U).name;
    prior.references = {{axk::ObjectType::smpl, right.name}};
    input.objects.insert(input.objects.begin(), prior);
    report = analyze(input);
    EXPECT_EQ(report.profiles[1].resident_bytes, 111280U + 336U + 120U + 72U);
}

TEST(VolumeCapacity, MissingLeftWaveSkipsTheSampleWithoutTryingItsRightWave) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    auto right = object(axk::ObjectType::smpl, 3U);
    sample.references = {{axk::ObjectType::smpl, object(axk::ObjectType::smpl, 2U).name},
                         {axk::ObjectType::smpl, right.name}};
    input.objects = {sample, right};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes);
        EXPECT_EQ(row.peak_bytes, row.baseline_bytes + (row.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U));
        ASSERT_FALSE(row.reasons.empty());
        EXPECT_EQ(row.reasons.front().code, "SAMPLE_SKIPPED_MISSING_WAVE");
    }
}

TEST(VolumeCapacity, MissingRightWaveDeletesEvenASharedLeftWaveButKeepsPriorSampleMetadata) {
    auto input = volume();
    auto first = object(axk::ObjectType::sbnk, 1U, 344U);
    first.older_body_bytes = 308U;
    auto left = object(axk::ObjectType::smpl, 3U);
    first.references = {{axk::ObjectType::smpl, left.name}};
    auto second = first;
    second.name = object(axk::ObjectType::sbnk, 2U).name;
    second.references.push_back({axk::ObjectType::smpl, object(axk::ObjectType::smpl, 4U).name});
    input.objects = {first, second, left};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + bytes);
        EXPECT_EQ(row.peak_bytes, row.baseline_bytes + bytes * 2U + 72U);
        EXPECT_EQ(row.peak_slots, row.baseline_slots + 3U);
    }
}

TEST(VolumeCapacity, LaterDeclaredSampleExtentIsRetainedIncludingInMembershipCopies) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 348U);
    sample.older_body_bytes = 312U;
    auto wave = object(axk::ObjectType::smpl, 9U);
    sample.references = {{axk::ObjectType::smpl, wave.name}};
    auto bank = object(axk::ObjectType::sbac, 2U, 480U);
    bank.older_body_bytes = 444U;
    bank.physical_rows = 8U;
    bank.counted_rows = 1U;
    bank.references = {{axk::ObjectType::sbnk, sample.name}};
    auto other = bank;
    other.name = object(axk::ObjectType::sbac, 3U).name;
    input.objects = {sample, bank, other, wave};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 304U : 340U;
        const auto bank_bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 436U : 472U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + bytes * 2U + bank_bytes * 2U + 72U);
    }
}

TEST(VolumeCapacity, OlderSampleExtraBytesAreOverwrittenWithinTheFixedLaterAllocation) {
    for (const auto declared : {308U, 312U, 344U}) {
        SCOPED_TRACE(declared);
        auto input = volume();
        auto sample = object(axk::ObjectType::sbnk, 1U, declared);
        sample.selector = 2U;
        auto wave = object(axk::ObjectType::smpl, 9U);
        sample.references = {{axk::ObjectType::smpl, wave.name}};
        input.objects = {sample, wave};
        const auto report = axk::detail::analyze_volume_capacity(input);
        ASSERT_TRUE(report) << report.error().message;
        for (const auto &profile : report->profiles) {
            ASSERT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
            const auto bytes = profile.target == axk::ASeriesLoadTarget::a3000 ? declared - 8U : 336U;
            EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + bytes + 72U);
        }
    }
    auto input = volume();
    auto oversized = object(axk::ObjectType::sbnk, 1U, 348U);
    oversized.selector = 2U;
    input.objects = {oversized};
    EXPECT_FALSE(axk::detail::analyze_volume_capacity(input));
}

TEST(VolumeCapacity, ReservedTempProgramReturnsAnExplicitLoadError) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 864U);
    program.older_body_bytes = 688U;
    program.physical_rows = 8U;
    program.name.fill(std::byte{' '});
    program.name[0U] = std::byte{'T'};
    program.name[1U] = std::byte{'E'};
    program.name[2U] = std::byte{'M'};
    program.name[3U] = std::byte{'P'};
    input.objects = {program};
    const auto result = axk::detail::analyze_volume_capacity(input);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("rename this Program"), std::string::npos);
}

TEST(VolumeCapacity, TemporaryCopyRegistrationBackfillsProgramRowsBeforeRenameAndCleanup) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    auto wave = object(axk::ObjectType::smpl, 9U);
    sample.references = {{axk::ObjectType::smpl, wave.name}};
    auto bank = object(axk::ObjectType::sbac, 2U, 480U);
    bank.older_body_bytes = 444U;
    bank.physical_rows = 8U;
    bank.counted_rows = 1U;
    bank.references = {{axk::ObjectType::sbnk, sample.name}};
    auto program = object(axk::ObjectType::prog, 3U, 408U + 56U * 16U + 8U);
    program.older_body_bytes = program.encoded_body_bytes - 176U;
    program.physical_rows = 16U;
    program.counted_rows = 10U;
    program.references = {{axk::ObjectType::sbnk, sample.name}};
    auto temporary = sample.name;
    temporary.fill(std::byte{' '});
    const std::string name = "New Sample";
    std::ranges::transform(name, temporary.begin(), [](char byte) { return static_cast<std::byte>(byte); });
    for (unsigned row = 0U; row < 9U; ++row)
        program.references.push_back({axk::ObjectType::sbnk, temporary});
    input.objects = {sample, bank, program, wave};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U;
        const auto bank_bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 436U : 472U;
        const auto program_bytes = row.target == axk::ASeriesLoadTarget::a3000 ? 1128U : 1304U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + bytes * 2U + bank_bytes + program_bytes + 72U);
    }
}

TEST(VolumeCapacity, UnknownActiveProgramRowKindsAreDiscardedByRuntimeCleanup) {
    auto input = volume();
    auto program = object(axk::ObjectType::prog, 1U, 408U + 56U * 16U + 8U);
    program.older_body_bytes = program.encoded_body_bytes - 176U;
    program.physical_rows = 16U;
    program.counted_rows = 9U;
    program.references.assign(9U, {axk::ObjectType::unknown, object(axk::ObjectType::sbnk, 2U).name});
    input.objects = {program};
    for (const auto &row : analyze(input).profiles) {
        ASSERT_EQ(row.status, axk::VolumeCapacityStatus::fits);
        const auto fixed = row.target == axk::ASeriesLoadTarget::a3000 ? 232U : 408U;
        EXPECT_EQ(row.resident_bytes, row.baseline_bytes + fixed + 56U * 8U);
        EXPECT_EQ(row.peak_bytes, row.baseline_bytes + fixed + 56U * 16U);
    }
}

TEST(VolumeCapacity, AllSpaceCopyNameIsNotRestoredOrBoundToAdditionalBanks) {
    auto input = volume();
    auto sample = object(axk::ObjectType::sbnk, 1U, 344U);
    sample.older_body_bytes = 308U;
    sample.name.fill(std::byte{' '});
    sample.filesystem_name = object(axk::ObjectType::sbnk, 4U).name;
    auto wave = object(axk::ObjectType::smpl, 9U);
    sample.references = {{axk::ObjectType::smpl, wave.name}};
    input.objects = {sample, wave};
    for (unsigned name = 1U; name <= 3U; ++name) {
        auto bank = object(axk::ObjectType::sbac, name, 480U);
        bank.older_body_bytes = 444U;
        bank.physical_rows = 8U;
        bank.counted_rows = 1U;
        bank.references = {{axk::ObjectType::sbnk, sample.name}};
        input.objects.push_back(bank);
    }
    for (const auto &profile : analyze(input).profiles) {
        ASSERT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        const auto sample_bytes = profile.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U;
        const auto bank_bytes = profile.target == axk::ASeriesLoadTarget::a3000 ? 436U : 472U;
        EXPECT_EQ(profile.resident_bytes, profile.baseline_bytes + sample_bytes * 2U + bank_bytes * 3U + 72U);
        EXPECT_EQ(profile.minimum_resident_slots, profile.baseline_slots + 6U);
    }
}
