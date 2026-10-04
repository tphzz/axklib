#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/program_format_conversion.hpp"
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;

Json update() {
    return {{"id", "parameters"},
            {"type", "update_program_parameters"},
            {"partition_index", 0},
            {"volume_name", "Programs"},
            {"program_number", 33},
            {"model", "A4000"},
            {"parameters", {{"level", 87}, {"effects", {{"1", {{"enabled", false}}}}}}},
            {"assignments", Json::array({{{"ordinal", 1},
                                          {"expected_target_kind", "SBNK"},
                                          {"expected_target_name", "Direct"},
                                          {"parameters", {{"pan_offset", 100}, {"receive", "basic"}}}}})}};
}

axk::Result<axk::AlterationManifest> manifest(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
    const auto image = axk::open_image(path);
    if (!image)
        return std::unexpected{image.error()};
    return axk::build_object_catalog(*image);
}

const axk::ObjectSnapshot *find_program(const axk::ObjectCatalog &objects) {
    const auto found = std::ranges::find_if(objects.objects, [](const auto &item) {
        return item.object.header.type == axk::ObjectType::prog && item.object.header.name == "033";
    });
    return found == objects.objects.end() ? nullptr : &*found;
}

std::string payload_hash(const axk::ObjectSnapshot &program) {
    return axk::package_internal::hex_digest(axk::package_internal::sha256(program.raw_payload));
}

class ProgramParameterAlteration : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-program-parameters-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / (prefix + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        source = root / "source.hds";
        output = root / "output.hds";
        const auto audio = root / "tone.wav";
        axk::Waveform wave;
        wave.format = {1U, 2U, 44100U};
        wave.frame_count = 4U;
        wave.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Programs";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Member";
        sample.waveform_id = "wave";
        volume.samples.push_back(sample);
        sample.name = "Direct";
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Member"}});
        volume.programs.push_back(
            {33U,
             "Params",
             {{"SBAC", "Bank", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}},
              {"SBNK", "Direct", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 2U}}}}});
        axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
        const auto written = axk::write_hds_image(build, source);
        ASSERT_TRUE(written) << written.error().message;
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void patch_program(std::size_t offset, std::string_view patch) {
        const auto image = bytes(source);
        constexpr std::string_view magic{"FSFSDEV3SPLXPROG"};
        const auto found = std::search(image.begin(), image.end(), magic.begin(), magic.end());
        ASSERT_NE(found, image.end());
        ASSERT_EQ(
            std::search(found + static_cast<std::ptrdiff_t>(magic.size()), image.end(), magic.begin(), magic.end()),
            image.end());
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(file);
        file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
        file.write(patch.data(), static_cast<std::streamsize>(patch.size()));
        ASSERT_TRUE(file);
    }
};

class CancelAfterUpdate final : public axk::ProgressSink {
  public:
    explicit CancelAfterUpdate(axk::CancellationSource &source) : source_(source) {}
    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::allocating && progress.completed == 1U)
            source_.cancel();
    }

  private:
    axk::CancellationSource &source_;
};
} // namespace

TEST(ProgramParameterManifest, AcceptsExplicitModelSparseGlobalAndGuardedAssignmentUpdates) {
    const auto parsed = manifest(update());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data), "update_program_parameters");
    auto global = update();
    global.erase("assignments");
    EXPECT_TRUE(manifest(global));
    auto rows = update();
    rows.erase("parameters");
    EXPECT_TRUE(manifest(rows));
    rows["assignments"][0]["expected_target_name"] = " Leading";
    EXPECT_TRUE(manifest(rows));
    auto native = update();
    native["model"] = "A3000";
    EXPECT_TRUE(manifest(native));
}

TEST(ProgramParameterManifest, RejectsMissingModelEmptyLeavesInvalidIdentitiesAndNarrowing) {
    auto invalid = update();
    invalid.erase("model");
    EXPECT_FALSE(manifest(invalid));
    for (const auto &value : {Json("a3000"), Json("a4000"), Json(nullptr), Json(4)}) {
        invalid = update();
        invalid["model"] = value;
        EXPECT_FALSE(manifest(invalid));
    }
    invalid = update();
    invalid["parameters"] = {{"effects", {{"1", Json::object()}}}};
    invalid["assignments"] = Json::array();
    EXPECT_FALSE(manifest(invalid));
    invalid = update();
    invalid["assignments"][0]["parameters"] = Json::object();
    EXPECT_FALSE(manifest(invalid));
    invalid = update();
    invalid["assignments"].push_back(invalid["assignments"][0]);
    EXPECT_FALSE(manifest(invalid));
    for (const auto &value : {Json(-1), Json(999), Json(1.5), Json(true), Json(18446744073709551615ULL)}) {
        invalid = update();
        invalid["assignments"][0]["ordinal"] = value;
        EXPECT_FALSE(manifest(invalid));
        invalid = update();
        invalid["program_number"] = value;
        EXPECT_FALSE(manifest(invalid));
    }
    invalid = update();
    invalid["assignments"][0]["expected_target_kind"] = "SMPL";
    EXPECT_FALSE(manifest(invalid));
    invalid = update();
    invalid["assignments"][0]["expected_target_name"] = "";
    EXPECT_FALSE(manifest(invalid));
    invalid = update();
    invalid["retarget"] = "Other";
    EXPECT_FALSE(manifest(invalid));
}

TEST(ProgramParameterManifest, RejectsMalformedPayloadGuards) {
    for (const auto &digest : {Json(""), Json(std::string(63U, 'a')), Json(std::string(65U, 'a')),
                               Json(std::string(64U, 'A')), Json(std::string(64U, 'g')), Json(nullptr), Json(42)}) {
        SCOPED_TRACE(digest.dump());
        auto operation = update();
        operation["expected_payload_sha256"] = digest;
        const auto parsed = manifest(operation);
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().code, axk::ErrorCode::manifest_invalid);
    }
}

TEST_F(ProgramParameterAlteration, PayloadGuardAllowsMatchingBytesAndRejectsStaleWritesWithoutPublishing) {
    const auto before = catalog(source);
    ASSERT_TRUE(before);
    const auto *program = find_program(*before);
    ASSERT_NE(program, nullptr);
    const auto original = bytes(source);
    auto operation = update();
    operation["expected_payload_sha256"] = payload_hash(*program);
    const auto parsed = manifest(operation);
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto applied = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(bytes(source), original);
    const auto saved = bytes(output);
    EXPECT_NE(saved, original);

    const auto unpublished = root / "stale.hds";
    const auto stale = axk::alter_hds(output, *parsed, unpublished);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_FALSE(std::filesystem::exists(unpublished));
    EXPECT_EQ(bytes(output), saved);

    const auto overwrite = axk::alter_hds(output, *parsed, source, {}, nullptr, true);
    ASSERT_FALSE(overwrite);
    EXPECT_EQ(overwrite.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_EQ(bytes(source), original);
    EXPECT_EQ(bytes(output), saved);
}

TEST_F(ProgramParameterAlteration, MalformedTypedPayloadGuardPreservesSourceAndExistingDestination) {
    std::filesystem::copy_file(source, output);
    const auto original = bytes(source);
    auto parsed = manifest(update());
    ASSERT_TRUE(parsed);
    auto &operation = std::get<axk::UpdateProgramParametersOperation>(parsed->operations.front().data);
    for (const auto &digest :
         {std::string{}, std::string(63U, 'a'), std::string(65U, 'a'), std::string(64U, 'A'), std::string(64U, 'g')}) {
        SCOPED_TRACE(digest);
        operation.expected_payload_sha256 = digest;
        const auto applied = axk::alter_hds(source, *parsed, output, {}, nullptr, true);
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().code, axk::ErrorCode::manifest_invalid);
        EXPECT_EQ(bytes(source), original);
        EXPECT_EQ(bytes(output), original);
    }
}

TEST_F(ProgramParameterAlteration, NativeRevisionTwoEditsPreserveLayoutOpaqueBytesAndOtherObjects) {
    const auto current = catalog(source);
    ASSERT_TRUE(current);
    const auto *current_program = find_program(*current);
    ASSERT_NE(current_program, nullptr);
    const auto conversion = manifest({{"id", "native-source"},
                                      {"type", "convert_prog_format"},
                                      {"partition_index", 0},
                                      {"volume_name", "Programs"},
                                      {"program_number", 33},
                                      {"target_format", "a3000"},
                                      {"expected_payload_sha256", payload_hash(*current_program)}});
    ASSERT_TRUE(conversion);
    const auto native = root / "native.hds";
    const auto converted = axk::alter_hds(source, *conversion, native);
    ASSERT_TRUE(converted) << converted.error().message;
    source = native;
    patch_program(0x43U, std::string(1U, '\x5a'));
    patch_program(0x120U + 0x10U, std::string(1U, '\x6b'));
    patch_program(0x120U + 7U * 0x38U + 0x17U, std::string(1U, '\x77'));
    const auto original = bytes(source);
    const auto before = catalog(source);
    ASSERT_TRUE(before);
    const auto *program = find_program(*before);
    ASSERT_NE(program, nullptr);
    const auto storage = axk::inspect_program_storage(program->raw_payload);
    ASSERT_EQ(storage.header_revision, std::uint32_t{2U});
    ASSERT_EQ(storage.format, axk::ProgramStorageFormat::a3000);

    auto operation = update();
    operation["model"] = "A3000";
    operation["expected_payload_sha256"] = payload_hash(*program);
    operation["assignments"][0]["parameters"] = {{"pan_offset", 31}};
    const auto parsed = manifest(operation);
    ASSERT_TRUE(parsed);
    const auto applied = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(bytes(source), original);
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    ASSERT_EQ(after->objects.size(), before->objects.size());
    for (const auto &old : before->objects) {
        const auto found = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(found, after->objects.end());
        auto expected = old.raw_payload;
        if (old.key == program->key) {
            expected[0x8bU] = std::byte{87};
            expected[0x98U] = std::byte{};
            expected[0x120U + 0x38U + 0x18U] = std::byte{31};
        }
        EXPECT_EQ(found->sfs_id, old.sfs_id);
        EXPECT_EQ(found->raw_payload, expected) << old.object.header.name;
    }
    const auto *saved = find_program(*after);
    ASSERT_NE(saved, nullptr);
    const auto saved_storage = axk::inspect_program_storage(saved->raw_payload);
    EXPECT_EQ(saved_storage.header_revision, storage.header_revision);
    EXPECT_EQ(saved_storage.format, storage.format);
    EXPECT_EQ(saved_storage.assignment_capacity, storage.assignment_capacity);
    EXPECT_EQ(saved_storage.parameter_tail_bytes, storage.parameter_tail_bytes);
}

TEST_F(ProgramParameterAlteration, ChangesOnlyRequestedBytesAndIsIdempotent) {
    const auto parsed = manifest(update());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = bytes(source);
    const auto source_image = axk::open_image(source);
    ASSERT_TRUE(source_image);
    const auto before = axk::build_object_catalog(*source_image);
    ASSERT_TRUE(before);
    const auto applied = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(bytes(source), original);
    const auto output_image = axk::open_image(output);
    ASSERT_TRUE(output_image);
    const auto after = axk::build_object_catalog(*output_image);
    ASSERT_TRUE(after);
    ASSERT_EQ(before->objects.size(), after->objects.size());
    for (std::size_t index = 0; index < before->objects.size(); ++index) {
        const auto &old = before->objects[index];
        const auto &current = after->objects[index];
        auto expected = old.raw_payload;
        if (old.object.header.type == axk::ObjectType::prog) {
            expected[0x8b] = std::byte{87};
            expected[0x98] = std::byte{0};
            expected[0x120 + 0x38 + 0x15] = std::byte{16};
            expected[0x120 + 0x38 + 0x18] = std::byte{100};
        }
        EXPECT_EQ(current.raw_payload, expected) << old.object.header.name;
    }
    const auto again = root / "again.hds";
    ASSERT_TRUE(axk::alter_hds(output, *parsed, again));
    EXPECT_EQ(bytes(output), bytes(again));
}

TEST_F(ProgramParameterAlteration, RejectsStaleRowsAndInvalidMergedValuesWithoutPublishing) {
    const auto original = bytes(source);
    auto row = update();
    const std::vector<Json> invalid_rows{
        {{"ordinal", 1},
         {"expected_target_kind", "SBNK"},
         {"expected_target_name", "Renamed"},
         {"parameters", {{"pan_offset", 1}}}},
        {{"ordinal", 1},
         {"expected_target_kind", "SBAC"},
         {"expected_target_name", "Direct"},
         {"parameters", {{"pan_offset", 1}}}},
        {{"ordinal", 2},
         {"expected_target_kind", "SBNK"},
         {"expected_target_name", "Direct"},
         {"parameters", {{"pan_offset", 1}}}},
        {{"ordinal", 1},
         {"expected_target_kind", "SBNK"},
         {"expected_target_name", "Direct"},
         {"parameters", {{"filter_q_offset", 32}}}},
        {{"ordinal", 1},
         {"expected_target_kind", "SBNK"},
         {"expected_target_name", "Direct"},
         {"parameters", {{"key_low", 100}, {"key_high", 50}}}},
        {{"ordinal", 1},
         {"expected_target_kind", "SBNK"},
         {"expected_target_name", "Direct"},
         {"parameters", {{"receive", {{"port", "b"}, {"channel", 1}}}}}},
    };
    for (const auto &invalid : invalid_rows) {
        SCOPED_TRACE(invalid.dump());
        row["assignments"][0] = invalid;
        const auto parsed = manifest(row);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_FALSE(axk::alter_hds(source, *parsed, output));
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(bytes(source), original);
    }
    row = update();
    row["parameters"]["effects"]["1"]["type"] = 97;
    const auto parsed = manifest(row);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(axk::alter_hds(source, *parsed, output));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(bytes(source), original);
}

TEST_F(ProgramParameterAlteration, CancellationAndLaterFailurePreserveSourceAndExistingDestination) {
    std::filesystem::copy_file(source, output);
    const auto original = bytes(source);
    const auto parsed = manifest(update());
    ASSERT_TRUE(parsed);
    axk::CancellationSource cancellation;
    CancelAfterUpdate progress{cancellation};
    const auto cancelled = axk::alter_hds(source, *parsed, output, cancellation.token(), &progress, true);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(bytes(source), original);
    EXPECT_EQ(bytes(output), original);

    auto later = *parsed;
    later.operations.push_back({"missing", axk::DeleteProgramOperation{axk::PartitionIndex{0}, "Programs", 128}});
    EXPECT_FALSE(axk::alter_hds(source, later, output, {}, nullptr, true));
    EXPECT_EQ(bytes(source), original);
    EXPECT_EQ(bytes(output), original);
}

TEST_F(ProgramParameterAlteration, QueuedEditsUseMergedStateAndExplicitFalseIsNotEmpty) {
    auto first = update();
    first["assignments"][0]["parameters"] = {{"key_low", 50}};
    auto second = update();
    second["id"] = "second";
    second.erase("parameters");
    second["assignments"][0]["parameters"] = {{"key_high", 40}};
    const auto invalid = axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({first, second})}}.dump());
    ASSERT_TRUE(invalid);
    EXPECT_FALSE(axk::alter_hds(source, *invalid, output));
    EXPECT_FALSE(std::filesystem::exists(output));

    auto zero = update();
    zero["parameters"] = {{"controller_reset", {{"a", {{"1", false}}}}}};
    zero.erase("assignments");
    const auto valid = manifest(zero);
    ASSERT_TRUE(valid);
    EXPECT_TRUE(axk::alter_hds(source, *valid, output));
}

TEST_F(ProgramParameterAlteration, GlobalEditsPreserveUnresolvedTargetsAndUnsupportedEffectWords) {
    patch_program(0x120 + 0x38, std::string{"Absent"} + std::string(10, '\0'));
    patch_program(0x98 + 6, std::string(1, '\x61'));
    patch_program(0x98 + 8, std::string(32, '\xaa'));
    const auto original = bytes(source);
    auto global = update();
    global.erase("assignments");
    const auto parsed = manifest(global);
    ASSERT_TRUE(parsed);
    const auto applied = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(bytes(source), original);
    const auto source_image = axk::open_image(source);
    const auto output_image = axk::open_image(output);
    ASSERT_TRUE(source_image);
    ASSERT_TRUE(output_image);
    const auto before = axk::build_object_catalog(*source_image);
    const auto after = axk::build_object_catalog(*output_image);
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    ASSERT_EQ(before->objects.size(), after->objects.size());
    for (std::size_t index = 0; index < before->objects.size(); ++index) {
        auto expected = before->objects[index].raw_payload;
        if (before->objects[index].object.header.type == axk::ObjectType::prog) {
            expected[0x8b] = std::byte{87};
            expected[0x98] = std::byte{0};
        }
        EXPECT_EQ(after->objects[index].raw_payload, expected);
    }
}

TEST_F(ProgramParameterAlteration, SupportsZeroCountWithoutCompactingUnusedRows) {
    patch_program(0x96, std::string(2, '\0'));
    auto global = update();
    global.erase("assignments");
    const auto parsed = manifest(global);
    ASSERT_TRUE(parsed);
    const auto applied = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(applied) << applied.error().message;
    const auto again = root / "again.hds";
    ASSERT_TRUE(axk::alter_hds(output, *parsed, again));
    EXPECT_EQ(bytes(output), bytes(again));
}
