#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/program_formats.hpp"
#include "axklib/application/sample_formats.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

using Json = nlohmann::json;

axk::ObjectSnapshot program_snapshot(std::uint32_t revision) {
    const auto written = axk::detail::prepare_prog_payload(axk::ProgramSpec{33U, "Format", {}});
    EXPECT_TRUE(written);
    if (!written)
        return {};
    auto bytes = *written;
    if (revision != 4U) {
        bytes.resize(bytes.size() - 0xb0U);
        axk::ByteWriter writer{bytes};
        EXPECT_TRUE(writer.write_be32(0x14U, revision));
        EXPECT_TRUE(writer.write_be32(0x18U, static_cast<std::uint32_t>(bytes.size() - 0x30U)));
        EXPECT_TRUE(writer.write_be32(0x1cU, 0U));
    }
    const auto decoded = axk::decode_object(bytes);
    EXPECT_TRUE(decoded);
    if (!decoded)
        return {};
    const axk::ObjectPlacement placement{
        axk::PartitionIndex{0U}, "Partition", axk::SfsId{1U}, "Programs", "PROG", "033", ""};
    return {"program",
            axk::PartitionIndex{0U},
            axk::SfsId{17U},
            "partition:0",
            *decoded,
            placement,
            std::move(bytes),
            {},
            axk::PlacementResolution::exact};
}

Json metadata(std::uint32_t revision) {
    return {{"format", revision == 4U ? "a4000_a5000" : "a3000"},
            {"structurallyValid", true},
            {"headerRevision", revision},
            {"logicalSize", revision == 4U ? 0x390U : 0x2e0U},
            {"storedAssignmentCount", 0U},
            {"assignmentCapacity", 8U},
            {"parameterTailBytes", revision == 4U ? 176U : 0U}};
}

TEST(ProgramFormats, GenericConversionUsesProgramMetadataAndOppositeGenerationPreviews) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        const auto snapshot = program_snapshot(revision);
        const auto *program = std::get_if<axk::CurrentProg>(&snapshot.object.payload);
        ASSERT_NE(program, nullptr);
        EXPECT_EQ(axk::app::program_format_metadata(snapshot.object), metadata(revision));
        const auto conversion = axk::app::object_format_conversion(snapshot, snapshot.raw_payload, true);
        ASSERT_TRUE(conversion.is_object());
        EXPECT_EQ(conversion.at("kind"), "program");
        EXPECT_EQ(conversion.at("partitionIndex"), 0U);
        EXPECT_EQ(conversion.at("volumeName"), "Programs");
        EXPECT_EQ(conversion.at("programNumber"), 33U);
        EXPECT_EQ(conversion.at("programName"), "Format");
        EXPECT_EQ(conversion.at("programFormat"), metadata(revision));
        EXPECT_FALSE(conversion.contains("sampleFormat"));
        EXPECT_EQ(conversion.at("canConvertFormat"), true);
        EXPECT_EQ(conversion.at("reason"), "");
        EXPECT_EQ(conversion.at("payloadSha256"),
                  axk::package_internal::hex_digest(axk::package_internal::sha256(snapshot.raw_payload)));
        const auto &previews = conversion.at("formatConversions");
        ASSERT_EQ(previews.size(), 1U);
        EXPECT_EQ(previews.front().at("targetFormat"), revision == 4U ? "a3000" : "a4000_a5000");
        EXPECT_EQ(previews.front().at("allowed"), true);
        EXPECT_FALSE(previews.front().at("changes").empty());
        EXPECT_TRUE(previews.front().at("blockers").empty());
    }
}

TEST(ProgramFormats, ReadOnlyUnplacedAndInvalidSlotProgramsRetainInspectionButCannotConvert) {
    auto snapshot = program_snapshot(4U);
    const auto read_only = axk::app::object_format_conversion(snapshot, snapshot.raw_payload, false);
    EXPECT_EQ(read_only.at("canConvertFormat"), false);
    EXPECT_FALSE(read_only.at("reason").get<std::string>().empty());
    EXPECT_EQ(read_only.at("programFormat"), metadata(4U));
    EXPECT_EQ(read_only.at("formatConversions").front().at("allowed"), true);
    snapshot.placement.reset();
    const auto unplaced = axk::app::object_format_conversion(snapshot, snapshot.raw_payload, true);
    EXPECT_EQ(unplaced.at("canConvertFormat"), false);
    EXPECT_EQ(unplaced.at("volumeName"), "");
    EXPECT_FALSE(unplaced.at("reason").get<std::string>().empty());
    for (const auto *slot : {"", "000", "129", "33suffix"}) {
        snapshot = program_snapshot(4U);
        snapshot.object.header.name = slot;
        const auto invalid = axk::app::object_format_conversion(snapshot, snapshot.raw_payload, true);
        EXPECT_EQ(invalid.at("canConvertFormat"), false);
        EXPECT_TRUE(invalid.at("programNumber").is_null());
        EXPECT_FALSE(invalid.at("reason").get<std::string>().empty());
        EXPECT_EQ(invalid.at("formatConversions").front().at("allowed"), true);
    }
}

TEST(ProgramFormats, BlockedPreviewReportsStructuredReasonsWithoutLosingStorageMetadata) {
    auto snapshot = program_snapshot(4U);
    snapshot.raw_payload[snapshot.raw_payload.size() - 0xb0U + 0x79U] = std::byte{64};
    const auto decoded = axk::decode_object(snapshot.raw_payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    snapshot.object = *decoded;
    const auto conversion = axk::app::object_format_conversion(snapshot, snapshot.raw_payload, true);
    EXPECT_EQ(conversion.at("programFormat"), metadata(4U));
    EXPECT_EQ(conversion.at("canConvertFormat"), true);
    const auto &preview = conversion.at("formatConversions").front();
    EXPECT_EQ(preview.at("targetFormat"), "a3000");
    EXPECT_EQ(preview.at("allowed"), false);
    ASSERT_FALSE(preview.at("blockers").empty());
    for (const auto &issue : preview.at("blockers")) {
        EXPECT_FALSE(issue.at("key").get<std::string>().empty());
        EXPECT_FALSE(issue.at("message").get<std::string>().empty());
        EXPECT_TRUE(issue.at("storedValue").is_null() || issue.at("storedValue").is_number_integer());
    }
}

class ProgramFormatSession : public testing::Test {
  protected:
    std::filesystem::path root;

    void SetUp() override {
        const auto prefix = std::string{"axklib-program-format-session-"} +
                            testing::UnitTest::GetInstance()->current_test_info()->name();
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / (prefix + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        axk::Waveform wave;
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 4U;
        wave.pcm.resize(8U);
        const auto audio = root / "tone.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Programs";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Sample";
        sample.waveform_id = "wave";
        sample.storage_format = axk::SampleStorageFormat::a3000_188;
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Sample"}});
        volume.programs.push_back({33U, "Format", {}});
        const axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(build, root / "program.hds");
        ASSERT_TRUE(written) << written.error().message;
        axk::MediaBuildManifest iso;
        iso.schema_version = "1.0";
        iso.format = axk::MediaImageFormat::iso9660;
        iso.authored_volume = volume;
        iso.volume_name = volume.name;
        const auto cd = axk::write_media_image(iso, root / "program.iso");
        ASSERT_TRUE(cd) << cd.error().message;
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }
};

TEST_F(ProgramFormatSession, SummaryAndDetailExposeConversionWithoutEnablingProgramParameterEditing) {
    for (const bool writable : {false, true}) {
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, writable}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        axk::app::ImageSessionManager sessions{*sandbox};
        const auto opened = sessions.open({"workspace", "program.hds"}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        const auto objects = sessions.objects(opened->image_id, "owner", 100U);
        ASSERT_TRUE(objects) << objects.error().message;
        const auto program = std::ranges::find_if(objects->items, [](const auto &item) { return item.type == "PROG"; });
        ASSERT_NE(program, objects->items.end());
        EXPECT_EQ(program->program_format, metadata(4U));
        EXPECT_TRUE(program->sample_format.is_null());
        const auto detail = sessions.object_detail(opened->image_id, "owner", program->id);
        ASSERT_TRUE(detail) << detail.error().message;
        EXPECT_EQ(detail->at("object").at("programFormat"), program->program_format);
        EXPECT_TRUE(detail->at("object").at("sampleFormat").is_null());
        EXPECT_TRUE(detail->at("editing").is_null());
        const auto &conversion = detail->at("formatConversion");
        EXPECT_EQ(conversion.at("kind"), "program");
        EXPECT_EQ(conversion.at("programFormat"), program->program_format);
        EXPECT_EQ(conversion.at("programNumber"), 33U);
        EXPECT_EQ(conversion.at("programName"), "Format");
        EXPECT_EQ(conversion.at("canConvertFormat"), writable);
        EXPECT_EQ(conversion.at("formatConversions").front().at("targetFormat"), "a3000");
        EXPECT_EQ(conversion.at("formatConversions").front().at("allowed"), true);
        EXPECT_EQ(conversion.at("reason").get<std::string>().empty(), writable);
        for (const auto &item : objects->items) {
            if (item.type == "PROG")
                continue;
            EXPECT_TRUE(item.program_format.is_null());
            const auto other = sessions.object_detail(opened->image_id, "owner", item.id);
            ASSERT_TRUE(other) << other.error().message;
            EXPECT_TRUE(other->at("object").contains("programFormat"));
            EXPECT_TRUE(other->at("object").at("programFormat").is_null());
            if (item.type == "SBNK" || item.type == "SBAC") {
                EXPECT_EQ(item.sample_format.at("format"), item.type == "SBNK" ? "a3000_188" : "a4000_a5000_224");
                EXPECT_EQ(other->at("object").at("sampleFormat"), item.sample_format);
            }
        }
        if (!writable)
            EXPECT_FALSE(sessions.begin_mutation(opened->image_id, "owner", opened->revision));
        ASSERT_TRUE(sessions.close(opened->image_id, "owner"));
    }
}

TEST_F(ProgramFormatSession, RevisionOneSeparatesStoredEffectIdsFromLaterLoaderValues) {
    const auto path = root / "program.iso";
    std::uint64_t offset{};
    {
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media);
        const auto objects = media->objects();
        ASSERT_TRUE(objects);
        const auto program = std::ranges::find_if(
            *objects, [](const auto &item) { return item.decoded.header.type == axk::ObjectType::prog; });
        ASSERT_NE(program, objects->end());
        offset = program->data_offset;
    }
    auto payload = program_snapshot(1U).raw_payload;
    payload[0x9fU] = std::byte{47};
    payload[0xc7U] = std::byte{52};
    payload[0xefU] = std::byte{59};
    {
        std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(file);
    }
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({"workspace", "program.iso"}, "owner");
    ASSERT_TRUE(opened);
    const auto objects = sessions.objects(opened->image_id, "owner", 100U, {}, "PROG");
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    const auto detail = sessions.object_detail(opened->image_id, "owner", objects->items.front().id);
    ASSERT_TRUE(detail);
    const auto &decoded = detail->at("object").at("decoded");
    EXPECT_EQ(decoded.at("effectTypeInterpretation"), "a3000-v2-and-later-load");
    const auto &effects = decoded.at("effectBlocks");
    EXPECT_EQ(effects.at(0).at("storedType"), 47U);
    EXPECT_EQ(effects.at(0).at("type"), 0U);
    EXPECT_EQ(effects.at(1).at("storedType"), 52U);
    EXPECT_EQ(effects.at(1).at("type"), 47U);
    EXPECT_EQ(effects.at(2).at("storedType"), 59U);
    EXPECT_EQ(effects.at(2).at("type"), 54U);
    EXPECT_EQ(detail->at("formatConversion").at("formatConversions").front().at("allowed"), false);
    EXPECT_TRUE(detail->at("editing").is_null());
    ASSERT_TRUE(sessions.close(opened->image_id, "owner"));
}

TEST_F(ProgramFormatSession, ScopedInventoryRetainsRealProgramsWithDefaultNamesAndNoAssignments) {
    for (const auto *filename : {"program.hds", "program.iso"}) {
        const auto path = root / filename;
        std::uint64_t offset{};
        {
            const auto media = axk::open_media(path);
            ASSERT_TRUE(media);
            const auto objects = media->objects();
            ASSERT_TRUE(objects);
            const auto program = std::ranges::find_if(
                *objects, [](const auto &item) { return item.decoded.header.type == axk::ObjectType::prog; });
            ASSERT_NE(program, objects->end());
            offset = program->data_offset;
            if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
                const auto &partition = sfs->partitions().front();
                const auto record = std::ranges::find_if(partition.records,
                                                         [](const auto &item) { return item.object_type == "PROG"; });
                ASSERT_NE(record, partition.records.end());
                ASSERT_EQ(record->extents.size(), 1U);
                offset = (static_cast<std::uint64_t>(partition.start_sector) +
                          static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                              partition.sectors_per_cluster) *
                         sfs->superblock().sector_size_bytes;
            }
        }
        {
            std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
            file.seekp(static_cast<std::streamoff>(offset + 0x78U));
            file.write("Pgm 033 ", 8);
            ASSERT_TRUE(file);
        }
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, false}});
        ASSERT_TRUE(sandbox);
        axk::app::ImageSessionManager sessions{*sandbox};
        const auto opened = sessions.open({"workspace", filename}, "owner");
        ASSERT_TRUE(opened);
        const auto all = sessions.objects(opened->image_id, "owner", 100U, {}, "PROG");
        ASSERT_TRUE(all);
        ASSERT_EQ(all->items.size(), 1U);
        const auto roots = sessions.content(opened->image_id, "owner", 100U);
        ASSERT_TRUE(roots);
        const auto volumes = sessions.content(opened->image_id, "owner", 100U, {}, roots->items.front().id);
        ASSERT_TRUE(volumes);
        const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
        ASSERT_NE(volume, volumes->items.end());
        const auto scoped = sessions.objects(opened->image_id, "owner", 1U, {}, "PROG", volume->id);
        ASSERT_TRUE(scoped);
        ASSERT_EQ(scoped->items.size(), 1U);
        EXPECT_EQ(scoped->items.front().id, all->items.front().id);
        EXPECT_EQ(scoped->items.front().program_format, metadata(4U));
        ASSERT_TRUE(sessions.close(opened->image_id, "owner"));
    }
}

TEST_F(ProgramFormatSession, MalformedProgramsRemainVisibleWithUnknownStorageInSfsAndIso) {
    for (const auto *filename : {"program.hds", "program.iso"}) {
        const auto path = root / filename;
        std::uint64_t absolute{};
        {
            const auto media = axk::open_media(path);
            ASSERT_TRUE(media) << media.error().message;
            const auto objects = media->objects();
            ASSERT_TRUE(objects) << objects.error().message;
            const auto program = std::ranges::find_if(
                *objects, [](const auto &item) { return item.decoded.header.type == axk::ObjectType::prog; });
            ASSERT_NE(program, objects->end());
            absolute = program->data_offset;
            if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
                const auto &partition = sfs->partitions().front();
                const auto record = std::ranges::find_if(partition.records,
                                                         [](const auto &item) { return item.object_type == "PROG"; });
                ASSERT_NE(record, partition.records.end());
                ASSERT_EQ(record->extents.size(), 1U);
                absolute = (static_cast<std::uint64_t>(partition.start_sector) +
                            static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                                partition.sectors_per_cluster) *
                           sfs->superblock().sector_size_bytes;
            }
        }
        const std::vector<std::vector<std::pair<unsigned, unsigned>>> faults{
            {{0x17U, 3U}},
            {{0x30U, 0x10U}},
            {{0x1fU, 0U}},
            {{0x97U, 9U}},
            {{0x19U, 1U}, {0x1dU, 1U}},        // Consistent size views beyond the stored extent.
            {{0x1bU, 0xb1U}, {0x1fU, 0x61U}}}; // Consistent lengths with a partial row.
        for (const auto &fault : faults) {
            SCOPED_TRACE(std::string{filename} + ":" + std::to_string(fault.front().first));
            std::vector<char> original;
            {
                std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
                for (const auto &[offset, value] : fault) {
                    char byte{};
                    file.seekg(static_cast<std::streamoff>(absolute + offset));
                    file.get(byte);
                    original.push_back(byte);
                    file.seekp(static_cast<std::streamoff>(absolute + offset));
                    file.put(static_cast<char>(value));
                    ASSERT_TRUE(file);
                }
            }
            {
                auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, false}});
                ASSERT_TRUE(sandbox);
                axk::app::ImageSessionManager sessions{*sandbox};
                const auto opened = sessions.open({"workspace", filename}, "owner");
                ASSERT_TRUE(opened) << opened.error().message;
                const auto objects = sessions.objects(opened->image_id, "owner", 100U);
                ASSERT_TRUE(objects);
                const auto program =
                    std::ranges::find_if(objects->items, [](const auto &item) { return item.type == "PROG"; });
                ASSERT_NE(program, objects->items.end());
                ASSERT_TRUE(program->program_format.is_object());
                EXPECT_EQ(program->program_format.at("format"), "unknown");
                EXPECT_EQ(program->program_format.at("structurallyValid"), false);
                for (const auto *metric :
                     {"logicalSize", "storedAssignmentCount", "assignmentCapacity", "parameterTailBytes"})
                    EXPECT_TRUE(program->program_format.at(metric).is_null());
                const auto detail = sessions.object_detail(opened->image_id, "owner", program->id);
                ASSERT_TRUE(detail) << detail.error().message;
                EXPECT_EQ(detail->at("object").at("programFormat"), program->program_format);
                EXPECT_TRUE(detail->at("formatConversion").is_null());
                EXPECT_TRUE(detail->at("editing").is_null());
                const auto roots = sessions.content(opened->image_id, "owner", 100U);
                ASSERT_TRUE(roots);
                const auto volumes =
                    sessions.content(opened->image_id, "owner", 100U, std::nullopt, roots->items.front().id);
                ASSERT_TRUE(volumes);
                const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
                ASSERT_NE(volume, volumes->items.end());
                const auto scoped =
                    sessions.objects(opened->image_id, "owner", 1U, std::nullopt, std::nullopt, volume->id);
                ASSERT_TRUE(scoped);
                auto page = *scoped;
                std::size_t program_count{};
                do {
                    for (const auto &item : page.items)
                        if (item.type == "PROG") {
                            ++program_count;
                            EXPECT_EQ(item.id, program->id);
                        }
                    if (!page.next_cursor)
                        break;
                    const auto next =
                        sessions.objects(opened->image_id, "owner", 1U, *page.next_cursor, std::nullopt, volume->id);
                    ASSERT_TRUE(next);
                    page = *next;
                } while (true);
                EXPECT_EQ(program_count, 1U);
            }
            std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
            for (std::size_t index = 0; index < fault.size(); ++index) {
                file.seekp(static_cast<std::streamoff>(absolute + fault[index].first));
                file.put(original[index]);
                ASSERT_TRUE(file);
            }
        }
    }
}

} // namespace
