#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/deletion.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/relationship.hpp"
#include "axklib/writer.hpp"

namespace {

class CancelAtObject final : public axk::ProgressSink {
  public:
    CancelAtObject(axk::CancellationSource &source, std::uint64_t boundary) : source_(source), boundary_(boundary) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::writing && progress.completed == boundary_)
            source_.cancel();
    }

  private:
    axk::CancellationSource &source_;
    std::uint64_t boundary_{};
};

class PackageProgramLinks : public testing::TestWithParam<axk::SampleStorageFormat> {
  protected:
    std::filesystem::path directory;
    std::filesystem::path audio_path;

    void SetUp() override {
        auto name = std::string{testing::UnitTest::GetInstance()->current_test_info()->name()};
        std::ranges::replace(name, '/', '-');
        directory = std::filesystem::temp_directory_path() / ("axklib-package-program-links-" + name);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory);
        audio_path = directory / "tone.wav";
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44100U};
        waveform.frame_count = 4U;
        waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                        std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        const auto written = axk::write_wav_atomic(audio_path, waveform);
        ASSERT_TRUE(written) << written.error().message;
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        EXPECT_FALSE(error) << error.message();
    }

    axk::VolumeSpec volume(std::span<const std::uint8_t> program_numbers) const {
        axk::VolumeSpec result;
        result.name = "Source";
        result.waveforms.push_back({"wave", "Bank Wave", audio_path, 60U, {}});
        axk::SampleSpec member;
        member.name = "Bank Member";
        member.waveform_id = "wave";
        member.parameters.root_key = 60U;
        member.parameters.key_high = 127U;
        member.storage_format = GetParam();
        result.samples.push_back(std::move(member));
        axk::SampleBankSpec bank;
        bank.name = "Shared Bank";
        bank.member_samples = {"Bank Member"};
        bank.storage_format = GetParam();
        bank.parameter_overrides.emplace();
        bank.parameter_overrides->level = 93U;
        bank.parameter_overrides->pan = -12;
        result.sample_banks.push_back(std::move(bank));
        for (const auto number : program_numbers) {
            result.programs.push_back(
                {number,
                 std::format("Bank {:03}", number),
                 {{"SBAC", "Shared Bank", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}}}});
        }
        return result;
    }

    void write_sfs(const std::filesystem::path &path, axk::VolumeSpec content) const {
        axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
        manifest.partitions.push_back({"P1", {std::move(content)}});
        const auto written = axk::write_hds_image(manifest, path);
        ASSERT_TRUE(written) << written.error().message;
    }

    axk::Result<axk::PackageBuild> package(const std::filesystem::path &source,
                                           std::span<const std::uint8_t> program_numbers) const {
        const auto opened = axk::open_media(source);
        if (!opened)
            return std::unexpected{opened.error()};
        std::vector<axk::PackageRootSelector> roots;
        if (program_numbers.empty()) {
            axk::PackageRootSelector root;
            root.kind = axk::PackageRootKind::sbac;
            root.partition_index = 0U;
            root.volume_name = "Source";
            root.object_name = "Shared Bank";
            roots.push_back(std::move(root));
        }
        for (const auto number : program_numbers) {
            axk::PackageRootSelector root;
            root.kind = axk::PackageRootKind::prog;
            root.partition_index = 0U;
            root.volume_name = "Source";
            root.object_name = std::format("{:03}", number);
            roots.push_back(std::move(root));
        }
        return axk::build_portable_package(*opened, roots);
    }

    static axk::PackageImportRequest request(const axk::PortablePackage &source,
                                             std::span<const std::uint8_t> slots = {}) {
        axk::PackageImportRequest result;
        for (std::size_t index = 0U; index < source.roots.size(); ++index) {
            axk::PackageRootDestination destination;
            destination.root_index = index;
            destination.partition_index = 0U;
            destination.volume_name = "Target";
            result.root_destinations.push_back(std::move(destination));
        }
        std::vector<const axk::PackageNode *> programs;
        for (const auto &node : source.nodes) {
            if (node.object_type == "PROG")
                programs.push_back(&node);
        }
        std::ranges::sort(programs, {}, [](const auto *node) { return node->placement_hint.entry_name; });
        EXPECT_TRUE(slots.empty() || slots.size() == programs.size());
        for (std::size_t index = 0U; index < slots.size() && index < programs.size(); ++index)
            result.policy.program_slot_assignments.push_back({0U, programs[index]->node_id, slots[index]});
        return result;
    }

    static void expect_bank_links(const std::filesystem::path &path, std::span<const std::uint8_t> expected) {
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media) << media.error().message;
        const auto objects = media->objects();
        ASSERT_TRUE(objects) << objects.error().message;
        std::size_t banks{};
        for (const auto &object : *objects) {
            if (object.decoded.header.raw_type != "SBAC")
                continue;
            ++banks;
            std::vector<std::uint8_t> actual;
            const axk::ByteReader reader{object.raw_payload};
            for (std::uint16_t number = 1U; number <= 128U; ++number) {
                const auto bitmap = reader.be32(0x90U + ((number - 1U) / 32U) * 4U);
                ASSERT_TRUE(bitmap) << bitmap.error().message;
                if ((*bitmap & (std::uint32_t{1U} << ((number - 1U) % 32U))) != 0U)
                    actual.push_back(static_cast<std::uint8_t>(number));
            }
            EXPECT_EQ(actual, std::vector<std::uint8_t>(expected.begin(), expected.end()));
        }
        EXPECT_EQ(banks, 1U);
    }

    static std::string conflicts(const axk::PackageImportPlan &plan) {
        std::string result;
        for (const auto &conflict : plan.conflicts)
            result += conflict.code + ": " + conflict.message + "\n";
        return result;
    }

    static std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        if (!input || input.tellg() < 0)
            return {};
        const auto size = static_cast<std::size_t>(input.tellg());
        std::vector<std::byte> bytes(size);
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        EXPECT_TRUE(input);
        return bytes;
    }

    static void corrupt_bank_link(const std::filesystem::path &path) {
        std::uint64_t offset{};
        {
            const auto container = axk::open_image(path);
            ASSERT_TRUE(container) << container.error().message;
            ASSERT_EQ(container->partitions().size(), 1U);
            const auto &partition = container->partitions().front();
            const auto bank = std::ranges::find_if(partition.records,
                                                   [](const auto &record) { return record.object_type == "SBAC"; });
            ASSERT_NE(bank, partition.records.end());
            ASSERT_EQ(bank->extents.size(), 1U);
            offset =
                (static_cast<std::uint64_t>(partition.start_sector) +
                 static_cast<std::uint64_t>(bank->extents.front().cluster_offset) * partition.sectors_per_cluster) *
                    512U +
                0x90U;
        }
        std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(image);
        image.seekp(static_cast<std::streamoff>(offset));
        const std::array<char, 4> wrong_link{0, 0, 0, '\x80'};
        image.write(wrong_link.data(), static_cast<std::streamsize>(wrong_link.size()));
        ASSERT_TRUE(image);
    }

    static void rename_stored_bank_target(const std::filesystem::path &path, std::string_view name) {
        std::uint64_t offset{};
        {
            const auto container = axk::open_image(path);
            ASSERT_TRUE(container) << container.error().message;
            const auto &partition = container->partitions().front();
            const auto program = std::ranges::find_if(partition.records,
                                                      [](const auto &record) { return record.object_type == "PROG"; });
            ASSERT_NE(program, partition.records.end());
            ASSERT_EQ(program->extents.size(), 1U);
            offset =
                (static_cast<std::uint64_t>(partition.start_sector) +
                 static_cast<std::uint64_t>(program->extents.front().cluster_offset) * partition.sectors_per_cluster) *
                    512U +
                0x120U + 0x38U;
        }
        std::array<std::byte, 20> row{};
        ASSERT_TRUE(axk::ByteWriter{row}.write_ascii_field(0U, 16U, name));
        std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(image);
        image.seekp(static_cast<std::streamoff>(offset));
        image.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
        ASSERT_TRUE(image);
    }

    static void expect_preserved_sound(const std::filesystem::path &source, const std::filesystem::path &target) {
        const auto source_media = axk::open_media(source);
        ASSERT_TRUE(source_media) << source_media.error().message;
        const auto target_media = axk::open_media(target);
        ASSERT_TRUE(target_media) << target_media.error().message;
        const auto source_objects = source_media->objects();
        ASSERT_TRUE(source_objects) << source_objects.error().message;
        const auto target_objects = target_media->objects();
        ASSERT_TRUE(target_objects) << target_objects.error().message;
        const auto source_bank = std::ranges::find_if(
            *source_objects, [](const auto &object) { return object.decoded.header.raw_type == "SBAC"; });
        const auto target_bank = std::ranges::find_if(
            *target_objects, [](const auto &object) { return object.decoded.header.raw_type == "SBAC"; });
        ASSERT_NE(source_bank, source_objects->end());
        ASSERT_NE(target_bank, target_objects->end());
        const auto *before = std::get_if<axk::CurrentSbac>(&source_bank->decoded.payload);
        const auto *after = std::get_if<axk::CurrentSbac>(&target_bank->decoded.payload);
        ASSERT_NE(before, nullptr);
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(before->storage.format, after->storage.format);
        EXPECT_EQ(before->override_enable_words, after->override_enable_words);
        auto before_parameters = before->raw_sample_parameter_block;
        auto after_parameters = after->raw_sample_parameter_block;
        std::fill(before_parameters.begin() + 0x18U, before_parameters.begin() + 0x28U, std::byte{0});
        std::fill(after_parameters.begin() + 0x18U, after_parameters.begin() + 0x28U, std::byte{0});
        EXPECT_EQ(before_parameters, after_parameters);
        const auto source_wave = std::ranges::find_if(
            *source_objects, [](const auto &object) { return object.decoded.header.raw_type == "SMPL"; });
        const auto target_wave = std::ranges::find_if(
            *target_objects, [](const auto &object) { return object.decoded.header.raw_type == "SMPL"; });
        ASSERT_NE(source_wave, source_objects->end());
        ASSERT_NE(target_wave, target_objects->end());
        const auto *before_wave = std::get_if<axk::CurrentSmpl>(&source_wave->decoded.payload);
        const auto *after_wave = std::get_if<axk::CurrentSmpl>(&target_wave->decoded.payload);
        ASSERT_NE(before_wave, nullptr);
        ASSERT_NE(after_wave, nullptr);
        const auto before_pcm = axk::ByteReader{source_wave->raw_payload}.slice(before_wave->stored_pcm_offset,
                                                                                before_wave->stored_pcm_bytes);
        const auto after_pcm = axk::ByteReader{target_wave->raw_payload}.slice(after_wave->stored_pcm_offset,
                                                                               after_wave->stored_pcm_bytes);
        ASSERT_TRUE(before_pcm) << before_pcm.error().message;
        ASSERT_TRUE(after_pcm) << after_pcm.error().message;
        EXPECT_GT(before_wave->stored_pcm_bytes, 0U);
        EXPECT_TRUE(std::ranges::equal(*before_pcm, *after_pcm));
    }
};

TEST_P(PackageProgramLinks, RenumbersBankFrom008To021WithoutBlockingUnrelatedProgramDeletion) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const auto imported_path = directory / "imported.hds";
    const auto deleted_path = directory / "deleted.hds";
    const std::array source_slots{std::uint8_t{8U}};
    const std::array destination_slots{std::uint8_t{21U}};
    write_sfs(source_path, volume(source_slots));
    ASSERT_FALSE(HasFatalFailure());
    auto target = volume({});
    target.name = "Target";
    target.sample_banks.clear();
    target.samples.front().name = "909 Closed HH";
    target.programs.push_back(
        {8U, "909 HH", {{"SBNK", "909 Closed HH", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}}}});
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    const auto built = package(source_path, source_slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(target_path, packages, request(built->package, destination_slots));
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflicts(*plan);
    const auto applied = axk::apply_package_import(target_path, packages, *plan, imported_path);
    ASSERT_TRUE(applied) << applied.error().message;
    expect_bank_links(imported_path, destination_slots);
    expect_preserved_sound(source_path, imported_path);

    const auto container = axk::open_image(imported_path);
    ASSERT_TRUE(container) << container.error().message;
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto program = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.raw_type == "PROG" && object.object.header.name == "008";
    });
    ASSERT_NE(program, catalog->objects.end());
    axk::ObjectDeletionSelection selection{.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {}};
    const auto preview = axk::inspect_object_deletion(*container, *catalog, graph, selection);
    ASSERT_TRUE(preview) << preview.error().message;
    for (const auto &impact : preview->impacts) {
        if (impact.role == axk::ObjectDeletionRole::dependency && impact.status == axk::ObjectDeletionStatus::optional)
            selection.cleanup_keys.push_back(impact.object_key);
    }
    const auto deletion = axk::inspect_object_deletion(*container, *catalog, graph, selection);
    ASSERT_TRUE(deletion) << deletion.error().message;
    ASSERT_TRUE(deletion->can_apply);
    const auto deleted = axk::alter_hds(imported_path, deletion->manifest, deleted_path);
    ASSERT_TRUE(deleted) << deleted.error().message;
    expect_bank_links(deleted_path, destination_slots);
    const auto remaining = axk::open_media(deleted_path);
    ASSERT_TRUE(remaining) << remaining.error().message;
    const auto objects = remaining->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    EXPECT_FALSE(std::ranges::any_of(*objects, [](const auto &object) {
        return (object.decoded.header.raw_type == "PROG" && object.decoded.header.name == "008") ||
               object.decoded.header.name == "909 Closed HH";
    }));
    EXPECT_TRUE(std::ranges::any_of(*objects, [](const auto &object) {
        return object.decoded.header.raw_type == "SBNK" && object.decoded.header.name == "Bank Member";
    }));
}

TEST_P(PackageProgramLinks, ClearsSourceOnlyLinksWhenImportingABankIntoEveryContainer) {
    const auto source_path = directory / "source.hds";
    const std::array slots{std::uint8_t{8U}, std::uint8_t{128U}};
    write_sfs(source_path, volume(slots));
    ASSERT_FALSE(HasFatalFailure());
    const auto built = package(source_path, {});
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    for (const auto kind : {axk::MediaKind::sfs, axk::MediaKind::fat12_floppy, axk::MediaKind::iso9660}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto extension = kind == axk::MediaKind::sfs            ? ".hds"
                               : kind == axk::MediaKind::fat12_floppy ? ".ima"
                                                                      : ".iso";
        const auto target_path = directory / ("target" + std::string{extension});
        const auto imported_path = directory / ("imported" + std::string{extension});
        axk::VolumeSpec target;
        target.name = "Target";
        auto import_request = request(built->package);
        if (kind == axk::MediaKind::sfs) {
            write_sfs(target_path, target);
            ASSERT_FALSE(HasFatalFailure());
        } else {
            target.waveforms.push_back({"retained", "Retained Wave", audio_path, 60U, {}});
            axk::MediaBuildManifest manifest;
            manifest.schema_version = "1.0";
            manifest.format = kind == axk::MediaKind::fat12_floppy ? axk::MediaImageFormat::fat12_floppy
                                                                   : axk::MediaImageFormat::iso9660;
            manifest.authored_volume = target;
            manifest.volume_name = "Target";
            const auto written = axk::write_media_image(manifest, target_path);
            ASSERT_TRUE(written) << written.error().message;
            auto &destination = import_request.root_destinations.front();
            if (kind == axk::MediaKind::fat12_floppy) {
                destination.volume_name = "FAT root";
            } else {
                destination.group_name = "AXKLIB";
                destination.raw_group = "GROUP";
                destination.raw_volume = "F001";
            }
        }
        const auto plan = axk::plan_package_import(target_path, packages, import_request);
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_TRUE(plan->valid()) << conflicts(*plan);
        const auto applied = axk::apply_package_import(target_path, packages, *plan, imported_path);
        ASSERT_TRUE(applied) << applied.error().message;
        expect_bank_links(imported_path, {});
        expect_preserved_sound(source_path, imported_path);
    }
}

TEST_P(PackageProgramLinks, MergesSharedBankLinksAcrossCombinedAndSequentialImports) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const auto combined_path = directory / "combined.hds";
    const auto first_path = directory / "first.hds";
    const auto second_path = directory / "second.hds";
    const std::array source_slots{std::uint8_t{8U}, std::uint8_t{9U}};
    const std::array destination_slots{std::uint8_t{21U}, std::uint8_t{22U}};
    write_sfs(source_path, volume(source_slots));
    axk::VolumeSpec target;
    target.name = "Target";
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    const auto built = package(source_path, source_slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto combined = axk::plan_package_import(target_path, packages, request(built->package, destination_slots));
    ASSERT_TRUE(combined) << combined.error().message;
    ASSERT_TRUE(combined->valid()) << conflicts(*combined);
    const auto applied = axk::apply_package_import(target_path, packages, *combined, combined_path);
    ASSERT_TRUE(applied) << applied.error().message;
    expect_bank_links(combined_path, destination_slots);

    for (std::size_t index = 0U; index < source_slots.size(); ++index) {
        const auto single = package(source_path, std::span{source_slots}.subspan(index, 1U));
        ASSERT_TRUE(single) << single.error().message;
        const std::vector single_package{single->package};
        const auto &input = index == 0U ? target_path : first_path;
        const auto &output = index == 0U ? first_path : second_path;
        const auto plan = axk::plan_package_import(
            input, single_package, request(single->package, std::span{destination_slots}.subspan(index, 1U)));
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_TRUE(plan->valid()) << conflicts(*plan);
        if (index == 1U) {
            const auto bank =
                std::ranges::find(plan->objects, std::string{"SBAC"}, &axk::PlannedPackageObject::object_type);
            ASSERT_NE(bank, plan->objects.end());
            EXPECT_TRUE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::reuse));
            EXPECT_FALSE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::insert));
        }
        const auto committed = axk::apply_package_import(input, single_package, *plan, output);
        ASSERT_TRUE(committed) << committed.error().message;
        expect_bank_links(output, std::span{destination_slots}.first(index + 1U));
    }
}

TEST_P(PackageProgramLinks, MergesExistingBankReferencesIntoEveryContainer) {
    const auto source_path = directory / "source.hds";
    const std::array source_slots{std::uint8_t{8U}, std::uint8_t{128U}};
    const std::array selected_slot{std::uint8_t{8U}};
    const std::array target_slot{std::uint8_t{21U}};
    const std::array expected_slots{std::uint8_t{8U}, std::uint8_t{21U}};
    write_sfs(source_path, volume(source_slots));
    ASSERT_FALSE(HasFatalFailure());
    const auto built = package(source_path, selected_slot);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    for (const auto kind : {axk::MediaKind::sfs, axk::MediaKind::fat12_floppy, axk::MediaKind::iso9660}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto extension = kind == axk::MediaKind::sfs            ? ".hds"
                               : kind == axk::MediaKind::fat12_floppy ? ".ima"
                                                                      : ".iso";
        const auto target_path = directory / ("target" + std::string{extension});
        const auto imported_path = directory / ("imported" + std::string{extension});
        auto target = volume(target_slot);
        target.name = "Target";
        auto import_request = request(built->package);
        if (kind == axk::MediaKind::sfs) {
            write_sfs(target_path, target);
            ASSERT_FALSE(HasFatalFailure());
        } else {
            axk::MediaBuildManifest manifest;
            manifest.schema_version = "1.0";
            manifest.format = kind == axk::MediaKind::fat12_floppy ? axk::MediaImageFormat::fat12_floppy
                                                                   : axk::MediaImageFormat::iso9660;
            manifest.authored_volume = target;
            manifest.volume_name = "Target";
            const auto written = axk::write_media_image(manifest, target_path);
            ASSERT_TRUE(written) << written.error().message;
            auto &destination = import_request.root_destinations.front();
            if (kind == axk::MediaKind::fat12_floppy) {
                destination.volume_name = "FAT root";
            } else {
                destination.group_name = "AXKLIB";
                destination.raw_group = "GROUP";
                destination.raw_volume = "F001";
            }
        }
        const auto plan = axk::plan_package_import(target_path, packages, import_request);
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_TRUE(plan->valid()) << conflicts(*plan);
        const auto bank =
            std::ranges::find(plan->objects, std::string{"SBAC"}, &axk::PlannedPackageObject::object_type);
        ASSERT_NE(bank, plan->objects.end());
        EXPECT_TRUE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::reuse));
        EXPECT_FALSE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::insert));
        const auto applied = axk::apply_package_import(target_path, packages, *plan, imported_path);
        ASSERT_TRUE(applied) << applied.error().message;
        expect_bank_links(imported_path, expected_slots);
        expect_preserved_sound(source_path, imported_path);
    }
}

TEST_P(PackageProgramLinks, RebuildsAllFourBitmapWordsAtProgramSlotBoundaries) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const auto imported_path = directory / "imported.hds";
    const std::array source_slots{std::uint8_t{2U}, std::uint8_t{3U}, std::uint8_t{4U}, std::uint8_t{5U},
                                  std::uint8_t{6U}, std::uint8_t{7U}, std::uint8_t{8U}, std::uint8_t{9U}};
    const std::array destination_slots{std::uint8_t{1U},  std::uint8_t{32U}, std::uint8_t{33U}, std::uint8_t{64U},
                                       std::uint8_t{65U}, std::uint8_t{96U}, std::uint8_t{97U}, std::uint8_t{128U}};
    write_sfs(source_path, volume(source_slots));
    axk::VolumeSpec target;
    target.name = "Target";
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    const auto built = package(source_path, source_slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(target_path, packages, request(built->package, destination_slots));
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflicts(*plan);
    const auto applied = axk::apply_package_import(target_path, packages, *plan, imported_path);
    ASSERT_TRUE(applied) << applied.error().message;
    expect_bank_links(imported_path, destination_slots);
}

TEST_P(PackageProgramLinks, RejectsInconsistentReusedBankWithoutRepairingItsLinks) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const std::array source_slots{std::uint8_t{9U}};
    const std::array target_slots{std::uint8_t{21U}};
    const std::array destination_slots{std::uint8_t{22U}};
    write_sfs(source_path, volume(source_slots));
    auto target = volume(target_slots);
    target.name = "Target";
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    corrupt_bank_link(target_path);
    ASSERT_FALSE(HasFatalFailure());
    const auto target_before = read_bytes(target_path);
    ASSERT_FALSE(target_before.empty());
    const auto built = package(source_path, source_slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(target_path, packages, request(built->package, destination_slots));
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(plan->conflicts, [](const auto &conflict) {
        return conflict.code == "PROGRAM_LINKS_INCONSISTENT";
    })) << conflicts(*plan);
    EXPECT_EQ(read_bytes(target_path), target_before);
}

TEST_P(PackageProgramLinks, CancelledAndStaleImportsLeaveReusedBankUntouched) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const std::array source_slots{std::uint8_t{9U}};
    const std::array target_slots{std::uint8_t{21U}};
    const std::array destination_slots{std::uint8_t{22U}};
    write_sfs(source_path, volume(source_slots));
    auto target = volume(target_slots);
    target.name = "Target";
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    const auto target_before = read_bytes(target_path);
    ASSERT_FALSE(target_before.empty());
    const auto built = package(source_path, source_slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(target_path, packages, request(built->package, destination_slots));
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflicts(*plan);
    for (std::uint64_t boundary = 0U; boundary <= plan->objects.size(); ++boundary) {
        SCOPED_TRACE(boundary);
        const auto output = directory / std::format("cancelled-{}.hds", boundary);
        axk::CancellationSource cancellation;
        if (boundary == 0U)
            cancellation.cancel();
        CancelAtObject progress{cancellation, boundary};
        const auto applied =
            axk::apply_package_import(target_path, packages, *plan, output, false, cancellation.token(), &progress);
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().code, axk::ErrorCode::operation_cancelled);
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(read_bytes(target_path), target_before);
    }
    corrupt_bank_link(target_path);
    ASSERT_FALSE(HasFatalFailure());
    const auto changed_target = read_bytes(target_path);
    ASSERT_NE(changed_target, target_before);
    const auto stale_output = directory / "stale.hds";
    const auto stale = axk::apply_package_import(target_path, packages, *plan, stale_output);
    ASSERT_FALSE(stale);
    EXPECT_FALSE(std::filesystem::exists(stale_output));
    EXPECT_EQ(read_bytes(target_path), changed_target);
}

TEST_P(PackageProgramLinks, RejectsProgramReuseWhenClearingARowWouldRemoveALiveDestinationAssignment) {
    const auto source_path = directory / "source.hds";
    const auto target_path = directory / "target.hds";
    const std::array slots{std::uint8_t{8U}};
    auto source = volume(slots);
    auto other_member = source.samples.front();
    other_member.name = "Other Member";
    source.samples.push_back(std::move(other_member));
    auto other_bank = source.sample_banks.front();
    other_bank.name = "Other Bank";
    other_bank.member_samples = {"Other Member"};
    source.sample_banks.push_back(std::move(other_bank));
    source.programs.front().assignments.push_back(
        {"SBAC", "Other Bank", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 2U}}});
    auto target = source;
    target.name = "Target";
    target.sample_banks.back().name = "Missing Bank";
    target.programs.front().assignments.back().target_name = "Missing Bank";
    write_sfs(source_path, std::move(source));
    write_sfs(target_path, std::move(target));
    ASSERT_FALSE(HasFatalFailure());
    rename_stored_bank_target(source_path, "Missing Bank");
    ASSERT_FALSE(HasFatalFailure());
    const auto before = read_bytes(target_path);
    ASSERT_FALSE(before.empty());
    const auto built = package(source_path, slots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(target_path, packages, request(built->package));
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(plan->conflicts, [](const auto &conflict) {
        return conflict.code == "PROGRAM_ASSIGNMENT_ADJUSTMENT_CONFLICT";
    })) << conflicts(*plan);
    EXPECT_EQ(read_bytes(target_path), before);
}

INSTANTIATE_TEST_SUITE_P(Generations, PackageProgramLinks,
                         testing::Values(axk::SampleStorageFormat::a3000_188,
                                         axk::SampleStorageFormat::a4000_a5000_224));

} // namespace
