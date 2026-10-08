#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/package.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/program_format_conversion.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

using Json = nlohmann::json;
using Format = axk::ProgramStorageFormat;

Json conversion(std::string hash, std::string_view target = "a3000", std::uint8_t number = 33U) {
    return {{"id", "program-format"},
            {"type", "convert_prog_format"},
            {"partition_index", 0},
            {"volume_name", "Programs"},
            {"program_number", number},
            {"target_format", target},
            {"expected_payload_sha256", std::move(hash)}};
}

axk::Result<axk::AlterationManifest> parse_conversion(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> read_image(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
    const auto image = axk::open_image(path);
    if (!image)
        return std::unexpected{image.error()};
    return axk::build_object_catalog(*image);
}

const axk::ObjectSnapshot *find_program(const axk::ObjectCatalog &objects, std::string_view slot = "033") {
    const auto found = std::ranges::find_if(objects.objects, [&](const auto &item) {
        return item.object.header.type == axk::ObjectType::prog && item.object.header.name == slot;
    });
    return found == objects.objects.end() ? nullptr : &*found;
}

std::string payload_hash(const axk::ObjectSnapshot &program) {
    return axk::package_internal::hex_digest(axk::package_internal::sha256(program.raw_payload));
}

void expect_same_relationships(const axk::ObjectCatalog &before, const axk::ObjectCatalog &after) {
    const auto old_graph = axk::build_relationship_graph(before);
    const auto current_graph = axk::build_relationship_graph(after);
    ASSERT_FALSE(old_graph.relationships.empty());
    ASSERT_EQ(current_graph.relationships.size(), old_graph.relationships.size());
    for (const auto &old : old_graph.relationships) {
        const auto found = std::ranges::find(current_graph.relationships, old.key, &axk::Relationship::key);
        ASSERT_NE(found, current_graph.relationships.end());
        EXPECT_EQ(found->source_key, old.source_key);
        EXPECT_EQ(found->target_key, old.target_key);
        EXPECT_EQ(found->candidate_keys, old.candidate_keys);
        EXPECT_EQ(found->type, old.type);
        EXPECT_EQ(found->scope_key, old.scope_key);
        EXPECT_EQ(found->assignment_index, old.assignment_index);
        EXPECT_EQ(found->assignment_name, old.assignment_name);
        EXPECT_EQ(found->assignment_state, old.assignment_state);
        EXPECT_EQ(found->receive_selector.has_value(), old.receive_selector.has_value());
        if (found->receive_selector && old.receive_selector)
            EXPECT_EQ(found->receive_selector->raw_value, old.receive_selector->raw_value);
    }
}

class ProgramFormatAlteration : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-program-format-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 4U;
        wave.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Programs";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        for (const auto *name : {"Member", "Direct", "Standalone"}) {
            axk::SampleSpec sample;
            sample.name = name;
            sample.waveform_id = "wave";
            volume.samples.push_back(sample);
        }
        volume.sample_banks.push_back({"Bank", {"Member"}});
        axk::ProgramSpec program{33U,
                                 "Format",
                                 {{"SBAC", "Bank", {.receive = axk::ProgramReceiveInherit{}}},
                                  {"SBNK", "Direct", {.pan_offset = -20}},
                                  {"SBNK", "Standalone", {.level_offset = 11}}}};
        program.parameters.level = 83U;
        program.parameters.transpose = -7;
        volume.programs.push_back(program);
        volume.programs.push_back({34U, "Other", {{"SBNK", "Direct", {.receive = axk::ProgramReceiveInherit{}}}}});
        const axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(build, source);
        ASSERT_TRUE(written) << written.error().message;
        patch_program({{0x43U, '\x5a'},
                       {0x130U, '\x5a'},
                       {0x120U + 7U * 0x38U + 0x17U, '\x77'},
                       {0x120U + 7U * 0x38U + 0x34U, '\x20'}});
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }

    void patch_program(const std::vector<std::pair<std::size_t, char>> &changes) {
        const auto objects = catalog(source);
        ASSERT_TRUE(objects) << objects.error().message;
        const auto *program = find_program(*objects);
        ASSERT_NE(program, nullptr);
        const auto image = read_image(source);
        const auto found =
            std::search(image.begin(), image.end(), program->raw_payload.begin(), program->raw_payload.end(),
                        [](char left, std::byte right) {
                            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
                        });
        ASSERT_NE(found, image.end());
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(file);
        for (const auto &[offset, value] : changes) {
            ASSERT_LT(offset, program->raw_payload.size());
            file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
            file.put(value);
            ASSERT_TRUE(file);
        }
    }
};

} // namespace

TEST(ProgramFormatManifest, AcceptsOnlyCanonicalStorageTargetsAndProgramSlotBounds) {
    const std::string digest(64U, 'a');
    for (const auto format : {Format::a3000, Format::a4000_a5000}) {
        for (const auto number : {1U, 33U, 128U}) {
            auto operation = conversion(digest, axk::program_storage_format_name(format));
            operation["program_number"] = number;
            const auto parsed = parse_conversion(operation);
            ASSERT_TRUE(parsed) << parsed.error().message;
            ASSERT_EQ(parsed->operations.size(), 1U);
            EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data), "convert_prog_format");
            const auto *item = std::get_if<axk::ConvertProgramFormatOperation>(&parsed->operations.front().data);
            ASSERT_NE(item, nullptr);
            ASSERT_TRUE(std::holds_alternative<axk::PartitionIndex>(item->partition));
            EXPECT_EQ(std::get<axk::PartitionIndex>(item->partition), axk::PartitionIndex{0U});
            EXPECT_EQ(item->volume_name, "Programs");
            EXPECT_EQ(item->program_number, static_cast<std::uint8_t>(number));
            EXPECT_EQ(item->target_format, format);
            EXPECT_EQ(item->expected_payload_sha256, digest);
        }
    }
}

TEST(ProgramFormatManifest, RejectsInvalidProgramNumbersTargetsAndObsoleteIdentities) {
    const auto operation = conversion(std::string(64U, 'a'));
    for (const auto &number : {Json(-1), Json(0), Json(129), Json(256), Json(18446744073709551615ULL), Json(true),
                               Json(1.5), Json("33"), Json(nullptr)}) {
        auto invalid = operation;
        invalid["program_number"] = number;
        EXPECT_FALSE(parse_conversion(invalid)) << number.dump();
    }
    for (const auto &format : {Json("unknown"), Json("a3000_188"), Json("a4000_a5000_224"), Json("A3000"),
                               Json("a4000"), Json("a5000"), Json(""), Json(4), Json(true), Json(nullptr)}) {
        auto invalid = operation;
        invalid["target_format"] = format;
        EXPECT_FALSE(parse_conversion(invalid)) << format.dump();
    }
    for (const auto *field : {"sample_name", "sample_bank_name", "program_name", "unknown"}) {
        auto invalid = operation;
        invalid[field] = "Format";
        EXPECT_FALSE(parse_conversion(invalid));
    }
    for (const auto *field : {"program_number", "target_format", "volume_name", "expected_payload_sha256"}) {
        auto invalid = operation;
        invalid.erase(field);
        EXPECT_FALSE(parse_conversion(invalid));
    }
    auto invalid = operation;
    invalid["volume_name"] = "";
    EXPECT_FALSE(parse_conversion(invalid));
    invalid = operation;
    invalid["type"] = "convert_program_format";
    EXPECT_FALSE(parse_conversion(invalid));
}

TEST(ProgramFormatManifest, RequiresALowercaseSha256Digest) {
    for (const auto &digest :
         {Json(""), Json(std::string(63U, 'a')), Json(std::string(65U, 'a')), Json(std::string(64U, 'A')),
          Json(std::string(64U, 'g')), Json(true), Json(123), Json(nullptr)}) {
        auto invalid = conversion(std::string(64U, 'a'));
        invalid["expected_payload_sha256"] = digest;
        EXPECT_FALSE(parse_conversion(invalid)) << digest.dump();
    }
}

TEST_F(ProgramFormatAlteration, RoundTripPreservesProgramIdentityAssignmentsDependenciesAndOtherObjects) {
    const auto original_catalog = catalog(source);
    ASSERT_TRUE(original_catalog) << original_catalog.error().message;
    const auto *original_program = find_program(*original_catalog);
    ASSERT_NE(original_program, nullptr);
    auto current_path = source;
    for (const auto target : {Format::a3000, Format::a4000_a5000}) {
        const auto before = catalog(current_path);
        ASSERT_TRUE(before) << before.error().message;
        const auto *program = find_program(*before);
        ASSERT_NE(program, nullptr);
        const auto plan = axk::plan_program_format_conversion(program->raw_payload, target);
        ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
        const auto parsed =
            parse_conversion(conversion(payload_hash(*program), axk::program_storage_format_name(target)));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto inspected = axk::inspect_hds_alteration(current_path, *parsed);
        ASSERT_TRUE(inspected) << inspected.error().message;
        EXPECT_TRUE(inspected->capacity.allowed);
        ASSERT_EQ(inspected->capacity.reports.size(), 1U);
        EXPECT_EQ(inspected->capacity.reports.front().volume_name, "Programs");
        const auto destination = root / (std::string{axk::program_storage_format_name(target)} + ".hds");
        const auto original = read_image(current_path);
        const auto changed = axk::alter_hds(current_path, *parsed, destination);
        ASSERT_TRUE(changed) << changed.error().message;
        EXPECT_EQ(read_image(current_path), original);
        const auto after = catalog(destination);
        ASSERT_TRUE(after) << after.error().message;
        ASSERT_EQ(after->objects.size(), before->objects.size());
        for (const auto &old : before->objects) {
            const auto found = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
            ASSERT_NE(found, after->objects.end());
            EXPECT_EQ(found->sfs_id, old.sfs_id);
            EXPECT_EQ(found->object.header.type, old.object.header.type);
            EXPECT_EQ(found->object.header.name, old.object.header.name);
            EXPECT_EQ(found->raw_payload, old.key == program->key ? plan.converted_payload : old.raw_payload);
        }
        const auto *converted = find_program(*after);
        ASSERT_NE(converted, nullptr);
        const auto storage = axk::inspect_program_storage(converted->raw_payload);
        EXPECT_TRUE(storage.structurally_valid);
        EXPECT_EQ(storage.format, target);
        EXPECT_EQ(storage.stored_assignment_count, std::uint16_t{3U});
        EXPECT_EQ(storage.assignment_capacity, std::size_t{8U});
        const auto &previous = std::get<axk::CurrentProg>(program->object.payload);
        const auto &current = std::get<axk::CurrentProg>(converted->object.payload);
        EXPECT_EQ(current.program_name, "Format");
        ASSERT_EQ(current.assignments.size(), previous.assignments.size());
        for (std::size_t index = 0; index < previous.assignments.size(); ++index) {
            EXPECT_EQ(current.assignments[index].name, previous.assignments[index].name);
            EXPECT_EQ(current.assignments[index].kind, previous.assignments[index].kind);
            EXPECT_EQ(current.assignments[index].raw_handle, previous.assignments[index].raw_handle);
            EXPECT_EQ(current.assignments[index].raw_receive_selector,
                      previous.assignments[index].raw_receive_selector);
        }
        expect_same_relationships(*before, *after);
        current_path = destination;
    }
    const auto restored = catalog(current_path);
    ASSERT_TRUE(restored) << restored.error().message;
    const auto *restored_program = find_program(*restored);
    ASSERT_NE(restored_program, nullptr);
    EXPECT_EQ(restored_program->raw_payload, original_program->raw_payload);
}

TEST_F(ProgramFormatAlteration, StaleHashesRefuseConversionAndSameFormatWithoutPublishing) {
    const auto objects = catalog(source);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto *program = find_program(*objects);
    ASSERT_NE(program, nullptr);
    auto stale_hash = payload_hash(*program);
    stale_hash[0] = stale_hash[0] == '0' ? '1' : '0';
    const auto original = read_image(source);
    for (const auto *target : {"a3000", "a4000_a5000"}) {
        const auto parsed = parse_conversion(conversion(stale_hash, target));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto absent = axk::alter_hds(source, *parsed, output);
        ASSERT_FALSE(absent);
        EXPECT_EQ(absent.error().code, axk::ErrorCode::transaction_stale);
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(read_image(source), original);
    }
    ASSERT_TRUE(std::filesystem::copy_file(source, output));
    const auto existing = read_image(output);
    const auto parsed = parse_conversion(conversion(stale_hash));
    ASSERT_TRUE(parsed);
    const auto rejected = axk::alter_hds(source, *parsed, output, {}, nullptr, true);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(output), existing);
}

TEST_F(ProgramFormatAlteration, LaterBatchFailureRollsBackAnOtherwiseValidConversion) {
    const auto objects = catalog(source);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto *program = find_program(*objects);
    ASSERT_NE(program, nullptr);
    auto parsed = parse_conversion(conversion(payload_hash(*program)));
    ASSERT_TRUE(parsed) << parsed.error().message;
    parsed->operations.push_back(
        {"missing-program", axk::ConvertProgramFormatOperation{axk::PartitionIndex{0U}, "Programs", 128U, Format::a3000,
                                                               payload_hash(*program)}});
    ASSERT_TRUE(std::filesystem::copy_file(source, output));
    const auto original = read_image(source);
    const auto existing = read_image(output);
    const auto rejected = axk::alter_hds(source, *parsed, output, {}, nullptr, true);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(output), existing);
}

TEST_F(ProgramFormatAlteration, BlockedDowngradePreservesSourceAndExistingDestination) {
    const auto initial = catalog(source);
    ASSERT_TRUE(initial) << initial.error().message;
    const auto *program = find_program(*initial);
    ASSERT_NE(program, nullptr);
    const auto &decoded = std::get<axk::CurrentProg>(program->object.payload);
    ASSERT_TRUE(decoded.layout.parameter_tail_offset);
    patch_program({{*decoded.layout.parameter_tail_offset + 0x79U, '\x40'}});
    const auto objects = catalog(source);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto *blocked = find_program(*objects);
    ASSERT_NE(blocked, nullptr);
    const auto parsed = parse_conversion(conversion(payload_hash(*blocked)));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_image(source);
    EXPECT_FALSE(axk::alter_hds(source, *parsed, output));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_image(source), original);
    ASSERT_TRUE(std::filesystem::copy_file(source, output));
    const auto existing = read_image(output);
    EXPECT_FALSE(axk::alter_hds(source, *parsed, output, {}, nullptr, true));
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(output), existing);
}

TEST_F(ProgramFormatAlteration, ExhaustedImageAllocationRefusesUpgradeWithoutPublishing) {
    axk::ProgramSpec program;
    program.number = 128U;
    program.name = "Capacity";
    program.assignments.resize(31U, {"SBNK", "Direct", {.receive = axk::ProgramReceiveInherit{}}});
    axk::AlterationManifest insertion;
    insertion.schema_version = "1.0";
    insertion.operations.push_back(
        {"insert", axk::InsertProgramOperation{axk::PartitionIndex{0U}, "Programs", program}});
    const auto current_path = root / "capacity-current.hds";
    const auto inserted = axk::alter_hds(source, insertion, current_path);
    ASSERT_TRUE(inserted) << inserted.error().message;
    const auto current_objects = catalog(current_path);
    ASSERT_TRUE(current_objects) << current_objects.error().message;
    const auto *current_program = find_program(*current_objects, "128");
    ASSERT_NE(current_program, nullptr);
    ASSERT_EQ(current_program->raw_payload.size(), std::size_t{2200U});
    const auto down = parse_conversion(conversion(payload_hash(*current_program), "a3000", 128U));
    ASSERT_TRUE(down) << down.error().message;
    const auto native_path = root / "capacity-native.hds";
    const auto converted = axk::alter_hds(current_path, *down, native_path);
    ASSERT_TRUE(converted) << converted.error().message;
    const auto native_objects = catalog(native_path);
    ASSERT_TRUE(native_objects) << native_objects.error().message;
    const auto *native_program = find_program(*native_objects, "128");
    ASSERT_NE(native_program, nullptr);
    const auto native_storage = axk::inspect_program_storage(native_program->raw_payload);
    EXPECT_EQ(native_storage.assignment_capacity, std::size_t{31U});
    EXPECT_EQ(native_storage.logical_size, std::size_t{2024U});
    ASSERT_EQ(native_program->raw_payload.size(), std::size_t{2024U});
    std::vector<axk::PortablePackage> packages;
    {
        const auto media = axk::open_media(native_path);
        ASSERT_TRUE(media) << media.error().message;
        axk::PackageRootSelector selector;
        selector.kind = axk::PackageRootKind::prog;
        selector.partition_index = 0U;
        selector.volume_name = "Programs";
        selector.object_name = "128";
        const std::vector roots{selector};
        const auto built = axk::build_portable_graph(*media, roots);
        ASSERT_TRUE(built) << built.error().message;
        packages.push_back(*built);
    }
    axk::VolumeSpec empty_volume;
    empty_volume.name = "Programs";
    const axk::HdsBuildManifest empty_build{"1.0", 4U * 1024U * 1024U, {{"Partition", {empty_volume}}}};
    const auto empty_path = root / "capacity-empty.hds";
    const auto empty = axk::write_hds_image(empty_build, empty_path);
    ASSERT_TRUE(empty) << empty.error().message;
    axk::PackageRootDestination destination;
    destination.partition_index = 0U;
    destination.volume_name = "Programs";
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination);
    const auto import_plan = axk::plan_package_import(empty_path, packages, request);
    ASSERT_TRUE(import_plan) << import_plan.error().message;
    ASSERT_TRUE(import_plan->valid()) << (import_plan->conflicts.empty() ? "" : import_plan->conflicts.front().message);
    const auto compact_path = root / "capacity-compact.hds";
    const auto imported = axk::apply_package_import(empty_path, packages, *import_plan, compact_path);
    ASSERT_TRUE(imported) << imported.error().message;
    std::uint32_t free_clusters{};
    {
        const auto image = axk::open_image(compact_path);
        ASSERT_TRUE(image) << image.error().message;
        const auto &partition = image->partitions().front();
        const auto record = std::ranges::find_if(partition.records, [](const auto &item) {
            return item.object_type == "PROG" && item.object_name == "128";
        });
        ASSERT_NE(record, partition.records.end());
        ASSERT_EQ(record->cluster_count, std::uint16_t{2U});
        ASSERT_EQ(record->data_size, std::uint32_t{2024U});
        ASSERT_EQ(partition.sectors_per_cluster, 2U);
        ASSERT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
        ASSERT_TRUE(partition.allocation.free_space);
        free_clusters = partition.allocation.free_space->free_cluster_count;
    }
    ASSERT_GT(free_clusters, 64U);
    // Fresh records allocate at least two clusters; capacity 31 requires a third only after upgrade.
    const auto pcm_bytes = static_cast<std::size_t>(free_clusters) * 1024U - 512U - 8U;
    axk::Waveform fill;
    fill.format = {1U, 2U, 44'100U};
    fill.frame_count = pcm_bytes / 2U;
    fill.pcm.resize(pcm_bytes);
    const auto audio = root / "fill.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio, fill));
    axk::InsertWaveformSpec waveform;
    waveform.path = audio;
    waveform.waveform_names = {"Capacity Fill"};
    waveform.root_key = 60U;
    waveform.loop_mode = axk::AudioSamplerLoopMode::forward;
    axk::AlterationManifest filling;
    filling.schema_version = "1.0";
    filling.operations.push_back({"fill", axk::InsertWaveformOperation{axk::PartitionIndex{0U}, "Programs", waveform}});
    const auto filled = root / "capacity-full.hds";
    const auto filled_image =
        axk::alter_hds(compact_path, filling, filled, {}, nullptr, false, {axk::ASeriesLoadTarget::a4000_a5000});
    ASSERT_TRUE(filled_image) << filled_image.error().message;
    {
        const auto image = axk::open_image(filled);
        ASSERT_TRUE(image) << image.error().message;
        const auto &allocation = image->partitions().front().allocation;
        ASSERT_TRUE(axk::allocation_is_safe_for_mutation(allocation));
        ASSERT_TRUE(allocation.free_space);
        ASSERT_EQ(allocation.free_space->free_cluster_count, 0U);
    }
    const auto full_objects = catalog(filled);
    ASSERT_TRUE(full_objects) << full_objects.error().message;
    const auto *full_program = find_program(*full_objects, "128");
    ASSERT_NE(full_program, nullptr);
    const auto up = parse_conversion(conversion(payload_hash(*full_program), "a4000_a5000", 128U));
    ASSERT_TRUE(up) << up.error().message;
    ASSERT_TRUE(std::filesystem::copy_file(filled, output));
    const auto original = read_image(filled);
    const auto existing = read_image(output);
    const auto rejected = axk::alter_hds(filled, *up, output, {}, nullptr, true);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "partition has insufficient free clusters");
    EXPECT_EQ(read_image(filled), original);
    EXPECT_EQ(read_image(output), existing);
}
