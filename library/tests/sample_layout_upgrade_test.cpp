#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/io.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sample_format_conversion.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Json = nlohmann::json;

class SampleLayoutUpgrade : public testing::Test {
  protected:
    std::vector<std::byte> payload;

    void SetUp() override {
        axk::SampleSpec sample;
        sample.name = "Short";
        const auto prepared = axk::detail::prepare_sbnk_payload(sample, {"Left", 0x100U, 44'100U, 400U});
        ASSERT_TRUE(prepared) << prepared.error().message;
        payload = *prepared;
        payload.resize(0x164U);
        axk::ByteWriter writer{payload};
        ASSERT_TRUE(writer.write_be32(0x14U, 2U));
        ASSERT_TRUE(writer.write_be32(0x18U, 0x134U));
        ASSERT_TRUE(writer.write_be32(0x1cU, 0U));
        for (std::size_t slot = 0U; slot < 6U; ++slot) {
            const auto offset = 0xa8U + slot * 4U;
            payload[offset] = static_cast<std::byte>(20U + slot);
            payload[offset + 1U] = static_cast<std::byte>(4U + slot);
            payload[offset + 2U] = static_cast<std::byte>(slot % 4U);
            payload[offset + 3U] = static_cast<std::byte>(236U + slot);
        }
        payload[0x14dU] = std::byte{4};
        payload[0x14eU] = std::byte{87};
        payload[0x14fU] = std::byte{3};
        payload[0x150U] = std::byte{99};
    }
};

TEST_F(SampleLayoutUpgrade, OrdinaryEditsDoNotChooseALaterStorageFormat) {
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x18U, 0x134U));
    const auto original = payload;
    axk::SampleParameters edit;
    edit.portamento_time = 73U;
    EXPECT_FALSE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload, original);
}

TEST_F(SampleLayoutUpgrade, ExplicitConversionPreservesPrefixAndInitializesOnlyTheExtension) {
    for (const auto flags : {0U, 1U, 8U, 9U}) {
        payload[0xd1U] = static_cast<std::byte>(flags);
        const auto original = payload;
        const auto plan = axk::plan_sample_format_conversion(payload, axk::SampleStorageFormat::a4000_a5000_224);
        ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
        EXPECT_EQ(payload, original);
        const auto &result = plan.converted_payload;
        ASSERT_EQ(result.size(), 392U);
        EXPECT_TRUE(std::equal(payload.begin() + 0x20, payload.end(), result.begin() + 0x20));
        EXPECT_TRUE(std::equal(payload.begin() + 0xa8, payload.begin() + 0xc0, result.begin() + 0x164));
        EXPECT_EQ(result[0x17c], (flags & 8U) ? std::byte{5} : std::byte{0});
        EXPECT_EQ(result[0x17d], result[0x17c]);
        EXPECT_EQ(result[0x182], static_cast<std::byte>(flags & 1U));
        EXPECT_EQ(result[0x183], std::byte{90});
        EXPECT_EQ(result[0x184], std::byte{90});
        EXPECT_EQ(axk::inspect_sample_storage(result).format, axk::SampleStorageFormat::a4000_a5000_224);
        const auto reverse = axk::plan_sample_format_conversion(result, axk::SampleStorageFormat::a3000_188);
        ASSERT_TRUE(reverse.allowed()) << (reverse.blockers.empty() ? "" : reverse.blockers.front().message);
        EXPECT_EQ(reverse.converted_payload, original);
    }
}

TEST_F(SampleLayoutUpgrade, NativeOutputPortamentoAndCrossfadeEditsRetainTheFormat) {
    axk::SampleParameters edit;
    edit.output1_destination = 3U;
    edit.portamento_type = 1U;
    edit.velocity_crossfade = true;
    ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload.size(), 356U);
    EXPECT_EQ(payload[0x14d], std::byte{3});
    EXPECT_EQ(std::to_integer<unsigned>(payload[0xd1]) & 9U, 9U);
    EXPECT_EQ(axk::inspect_sample_storage(payload).format, axk::SampleStorageFormat::a3000_188);
}

TEST_F(SampleLayoutUpgrade, ConversionPreservesUninterpretedHeaderDataInBothDirections) {
    for (std::size_t offset = 0x20U; offset < 0x30U; ++offset)
        payload[offset] = static_cast<std::byte>(offset);
    const auto original = payload;
    const auto later = axk::plan_sample_format_conversion(payload, axk::SampleStorageFormat::a4000_a5000_224);
    ASSERT_TRUE(later.allowed());
    EXPECT_TRUE(std::equal(payload.begin() + 0x20U, payload.begin() + 0x30U, later.converted_payload.begin() + 0x20U));
    const auto native =
        axk::plan_sample_format_conversion(later.converted_payload, axk::SampleStorageFormat::a3000_188);
    ASSERT_TRUE(native.allowed());
    EXPECT_EQ(native.converted_payload, original);
    payload[0x1cU] = std::byte{1};
    EXPECT_FALSE(axk::plan_sample_format_conversion(payload, axk::SampleStorageFormat::a4000_a5000_224).allowed());
}

TEST_F(SampleLayoutUpgrade, PaddingCannotChangeIdentityAndInvalidValuesCannotReclassifyIt) {
    payload.resize(512U, std::byte{0xa5});
    payload[0x143] = std::byte{2};
    const auto storage = axk::inspect_sample_storage(payload);
    EXPECT_EQ(storage.format, axk::SampleStorageFormat::a3000_188);
    EXPECT_EQ(storage.parameter_bytes, 188U);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<axk::CurrentSbnk>(decoded->payload).raw_parameter_window.size(), 188U);
    const auto plan = axk::plan_sample_format_conversion(payload, axk::SampleStorageFormat::a4000_a5000_224);
    EXPECT_FALSE(plan.allowed());
    axk::SampleParameters independent;
    independent.level = 80U;
    EXPECT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, independent));
    EXPECT_EQ(payload[0x143], std::byte{2});
}

TEST_F(SampleLayoutUpgrade, TruncationConflictingLengthsAndUnknownHeadersRemainUnknown) {
    for (const auto revision : {1U, 3U, 5U}) {
        auto unsupported = payload;
        ASSERT_TRUE(axk::ByteWriter{unsupported}.write_be32(0x14U, revision));
        EXPECT_EQ(axk::inspect_sample_storage(unsupported).format, axk::SampleStorageFormat::unknown);
    }
    auto malformed = payload;
    ASSERT_TRUE(axk::ByteWriter{malformed}.write_be32(0x18U, 0x110U));
    EXPECT_EQ(axk::inspect_sample_storage(malformed).format, axk::SampleStorageFormat::unknown);
    payload.pop_back();
    EXPECT_EQ(axk::inspect_sample_storage(payload).format, axk::SampleStorageFormat::unknown);
}

TEST_F(SampleLayoutUpgrade, DowngradeBlocksEveryNonrepresentableOrUnknownTailValue) {
    const auto up = axk::plan_sample_format_conversion(payload, axk::SampleStorageFormat::a4000_a5000_224);
    ASSERT_TRUE(up.allowed());
    for (const auto &[offset, value] : std::array<std::pair<std::size_t, unsigned>, 8>{
             {{0x17c, 3}, {0x182, 2}, {0x183, 89}, {0x184, 91}, {0x185, 1}, {0x17e, 6}, {0x165, 22}, {0x143, 2}}}) {
        auto later = up.converted_payload;
        later[offset] = static_cast<std::byte>(value);
        const auto plan = axk::plan_sample_format_conversion(later, axk::SampleStorageFormat::a3000_188);
        EXPECT_FALSE(plan.allowed()) << offset;
        EXPECT_FALSE(plan.blockers.empty());
    }
}

TEST_F(SampleLayoutUpgrade, NativeWiderRangesBlockUpconversionWithoutClamping) {
    for (const auto &[offset, value] : std::array<std::pair<std::size_t, unsigned>, 2>{{{0xd3, 13}, {0xd5, 127}}}) {
        auto native = payload;
        native[offset] = static_cast<std::byte>(value);
        const auto plan = axk::plan_sample_format_conversion(native, axk::SampleStorageFormat::a4000_a5000_224);
        EXPECT_FALSE(plan.allowed());
        EXPECT_EQ(plan.source.format, axk::SampleStorageFormat::a3000_188);
        EXPECT_TRUE(plan.converted_payload.empty());
    }
}

TEST_F(SampleLayoutUpgrade, RejectedTailOrDependentEditCannotPublishPartialConversion) {
    const auto original = payload;
    axk::SampleParameters edit;
    edit.portamento_time = 0U;
    edit.level = 87U;
    EXPECT_FALSE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload, original);
    edit.portamento_time = 73U;
    edit.key_low = 100U;
    edit.key_high = 20U;
    EXPECT_FALSE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload, original);
}

TEST(SampleStereoSparseExpansion, ChangesOnlyNamedStereoScalarsWithoutConvertingItsTopology) {
    axk::SampleSpec spec;
    spec.name = "Stereo";
    const axk::detail::PreparedWaveformMember left{"Left", 0x100U, 44'100U, 400U};
    const axk::detail::PreparedWaveformMember right{"Right", 0x200U, 44'100U, 400U};
    const auto prepared = axk::detail::prepare_sbnk_payload(spec, left, right);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_EQ(std::to_integer<unsigned>((*prepared)[0xd0U]) & 6U, 0U);
    for (const bool short_layout : {false, true}) {
        auto payload = *prepared;
        if (short_layout) {
            payload.resize(0x164U);
            axk::ByteWriter header{payload};
            ASSERT_TRUE(header.write_be32(0x14U, 2U));
            ASSERT_TRUE(header.write_be32(0x18U, 0x134U));
            ASSERT_TRUE(header.write_be32(0x1cU, 0U));
        }
        auto expected = payload;
        expected[0x112U] = std::byte{253};
        expected[0x113U] = std::byte{27};
        axk::SampleParameters edit;
        edit.expand_detune = -3;
        edit.expand_dephase = 27;
        const auto applied = axk::detail::apply_sample_parameters_to_payload(payload, edit);
        ASSERT_TRUE(applied) << applied.error().message;
        EXPECT_EQ(payload, expected);
    }
}

TEST(SampleStereoSparseExpansion, RejectsDuplicateSourcesAndRetainedExpandedFlagsAtomically) {
    axk::SampleSpec spec;
    spec.name = "Retained";
    const axk::detail::PreparedWaveformMember left{"Left", 0x100U, 44'100U, 400U};
    const axk::detail::PreparedWaveformMember right{"Right", 0x200U, 44'100U, 400U};
    for (const bool duplicate : {false, true}) {
        const auto prepared = axk::detail::prepare_sbnk_payload(spec, left, duplicate ? left : right);
        ASSERT_TRUE(prepared);
        auto payload = *prepared;
        if (!duplicate)
            payload[0xd0U] |= std::byte{4};
        const auto original = payload;
        axk::SampleParameters edit;
        edit.expand_detune = 2;
        edit.level = 87U;
        EXPECT_FALSE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
        EXPECT_EQ(payload, original);
    }
}

std::vector<char> read_image(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
    const auto image = axk::open_image(path);
    if (!image)
        return std::unexpected(image.error());
    return axk::build_object_catalog(*image);
}

class SampleLayoutUpgradeTransaction : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    axk::ObjectCatalog before;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-layout-upgrade-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
        for (std::size_t attempt = 0U; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / (prefix + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        source = root / "source.hds";
        axk::Waveform wave;
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 4U;
        wave.pcm = {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{2},
                    std::byte{3}, std::byte{4}, std::byte{0}, std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(root / "tone.wav", wave));
        axk::VolumeSpec volume;
        volume.name = "Samples";
        volume.waveforms.push_back({"wave", "Wave", root / "tone.wav", 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Short";
        sample.waveform_id = "wave";
        volume.samples.push_back(sample);
        sample.name = "Direct";
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Short"}});
        volume.programs.push_back({1U, "Links", {{"SBAC", "Bank", {}}, {"SBNK", "Direct", {}}}});
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, source);
        ASSERT_TRUE(written) << written.error().message;
        const auto objects = catalog(source);
        ASSERT_TRUE(objects);
        const auto found =
            std::ranges::find_if(objects->objects, [](const auto &item) { return item.object.header.name == "Short"; });
        ASSERT_NE(found, objects->objects.end());
        auto shortened = found->raw_payload;
        axk::ByteWriter header{shortened};
        ASSERT_TRUE(header.write_be32(0x14U, 2U));
        ASSERT_TRUE(header.write_be32(0x18U, 0x134U));
        ASSERT_TRUE(header.write_be32(0x1cU, 0U));
        std::fill(shortened.begin() + 0x164U, shortened.end(), std::byte{0x5a});
        const auto bytes = read_image(source);
        const auto position = std::search(
            bytes.begin(), bytes.end(), found->raw_payload.begin(), found->raw_payload.end(),
            [](char a, std::byte b) { return static_cast<unsigned char>(a) == std::to_integer<unsigned char>(b); });
        ASSERT_NE(position, bytes.end());
        {
            std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
            ASSERT_TRUE(file);
            file.seekp(static_cast<std::streamoff>(position - bytes.begin()));
            for (const auto byte : shortened)
                file.put(static_cast<char>(std::to_integer<unsigned char>(byte)));
            ASSERT_TRUE(file);
        }
        const auto patched = catalog(source);
        ASSERT_TRUE(patched) << patched.error().message;
        before = *patched;
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    static const axk::ObjectSnapshot &sample(const axk::ObjectCatalog &objects) {
        return *std::ranges::find_if(objects.objects,
                                     [](const auto &item) { return item.object.header.name == "Short"; });
    }

    static axk::Result<axk::AlterationManifest> edit(const axk::ObjectSnapshot &current, Json parameters) {
        const auto hash = axk::package_internal::hex_digest(axk::package_internal::sha256(current.raw_payload));
        const Json operation{{"id", "upgrade"},
                             {"type", "update_sbnk_parameters"},
                             {"partition_index", 0},
                             {"volume_name", "Samples"},
                             {"sample_name", "Short"},
                             {"expected_payload_sha256", hash},
                             {"parameters", std::move(parameters)}};
        return axk::parse_alteration_manifest(
            Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
    }

    static axk::Result<axk::AlterationManifest> convert(const axk::ObjectSnapshot &current) {
        const auto hash = axk::package_internal::hex_digest(axk::package_internal::sha256(current.raw_payload));
        const Json operation{{"id", "convert"},
                             {"type", "convert_sbnk_format"},
                             {"partition_index", 0},
                             {"volume_name", "Samples"},
                             {"sample_name", "Short"},
                             {"target_format", "a4000_a5000_224"},
                             {"expected_payload_sha256", hash}};
        return axk::parse_alteration_manifest(
            Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
    }

    void expect_preserved(const axk::ObjectCatalog &after) {
        ASSERT_EQ(after.objects.size(), before.objects.size());
        for (const auto &old : before.objects) {
            const auto current = std::ranges::find(after.objects, old.key, &axk::ObjectSnapshot::key);
            ASSERT_NE(current, after.objects.end());
            EXPECT_EQ(current->sfs_id, old.sfs_id);
            EXPECT_EQ(current->object.header.name, old.object.header.name);
            if (old.object.header.name == "Short")
                EXPECT_TRUE(std::equal(old.raw_payload.begin() + 0x20U, old.raw_payload.begin() + 0xa8U,
                                       current->raw_payload.begin() + 0x20U));
            else
                EXPECT_EQ(current->raw_payload, old.raw_payload) << old.object.header.name;
        }
    }
};

TEST_F(SampleLayoutUpgradeTransaction, ReopensAndEditsTwiceWithoutReplacingIdentityOrRelatedObjects) {
    const auto original = read_image(source);
    const auto first = convert(sample(before));
    ASSERT_TRUE(first) << first.error().message;
    const auto output = root / "saved.hds";
    const auto applied = axk::alter_hds(source, *first, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(read_image(source), original);
    const auto reopened = catalog(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    expect_preserved(*reopened);
    EXPECT_EQ(sample(*reopened).raw_payload.size(), sample(before).raw_payload.size() + 36U);
    EXPECT_TRUE(std::equal(sample(before).raw_payload.begin() + 0x164U, sample(before).raw_payload.end(),
                           sample(*reopened).raw_payload.begin() + 0x188U));
    EXPECT_EQ(sample(*reopened).raw_payload[0x184U], std::byte{90});
    const auto second = edit(sample(*reopened), {{"portamento_time", 64}});
    ASSERT_TRUE(second);
    const auto again = root / "saved-again.hds";
    const auto reapplied = axk::alter_hds(output, *second, again);
    ASSERT_TRUE(reapplied) << reapplied.error().message;
    const auto final = catalog(again);
    ASSERT_TRUE(final);
    expect_preserved(*final);
    auto expected = sample(*reopened).raw_payload;
    expected[0x184U] = std::byte{64};
    EXPECT_EQ(sample(*final).raw_payload, expected);
}

TEST_F(SampleLayoutUpgradeTransaction, StaleBaselineRejectsWithoutTouchingSourceOrExistingDestination) {
    const auto stale = convert(sample(before));
    ASSERT_TRUE(stale);
    const auto output = root / "saved.hds";
    ASSERT_TRUE(axk::alter_hds(source, *stale, output));
    const auto original = read_image(output);
    const auto destination = root / "existing.hds";
    {
        std::ofstream file{destination, std::ios::binary};
        file << "keep existing output";
    }
    const auto sentinel = read_image(destination);
    const auto rejected = axk::alter_hds(output, *stale, destination, {}, nullptr, true);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_NE(rejected.error().message.find("baseline"), std::string::npos);
    EXPECT_EQ(read_image(output), original);
    EXPECT_EQ(read_image(destination), sentinel);
}

class CancelUpgradeProgress final : public axk::ProgressSink {
  public:
    explicit CancelUpgradeProgress(axk::CancellationSource &source) : source_(source) {}
    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::allocating && progress.completed == 1U)
            source_.cancel();
    }

  private:
    axk::CancellationSource &source_;
};

TEST_F(SampleLayoutUpgradeTransaction, CancellationAfterUpgradePublishesNothing) {
    const auto operation = convert(sample(before));
    ASSERT_TRUE(operation);
    const auto original = read_image(source);
    const auto destination = root / "existing.hds";
    {
        std::ofstream file{destination, std::ios::binary};
        file << "keep existing output";
    }
    const auto sentinel = read_image(destination);
    axk::CancellationSource cancellation;
    CancelUpgradeProgress progress{cancellation};
    const auto result = axk::alter_hds(source, *operation, destination, cancellation.token(), &progress, true);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(destination), sentinel);
}
} // namespace
