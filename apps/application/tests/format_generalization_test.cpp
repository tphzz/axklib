#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../../../library/tests/media_ex5_fixture.hpp"
#include "../src/content_digest.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/writer.hpp"

namespace {

class FormatGeneralization : public testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::format("axk-native-control-{}", std::chrono::steady_clock::now().time_since_epoch().count());

    void SetUp() override { ASSERT_TRUE(std::filesystem::create_directory(root)); }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }
    void write(const std::filesystem::path &path, const std::vector<std::byte> &bytes) const {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream output{root / path, std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        ASSERT_TRUE(output);
    }
};

TEST_F(FormatGeneralization, R0) {
    namespace app = axk::app;
    const axk::HdsBuildManifest empty{"1.0", 1'048'576U, {{"Disk", {{"Empty", {}, {}, {}, {}}}}}};
    ASSERT_TRUE(axk::write_hds_image(empty, root / "empty.hds"));
    std::vector<std::byte> wave(35U);
    ascii(wave, 0, "LM8953");
    wave[22] = std::byte{0xc9};
    wave[23] = wave[24] = std::byte{2};
    write("TONE.W01", wave);
    write("another/TONE.W01", wave);
    std::vector<std::byte> setup(1536U);
    ascii(setup, 0, "LM8953");
    ascii(setup, 0xf7, "TONE");
    write("main/SET.S01", setup);
    std::vector<std::byte> voices(8704U);
    ascii(voices, 0, "LM8953");
    write("main/SET.V01", voices);
    write("ex5.hda", ex5_fixture());
    const auto digest = [this](const char *path) { return app::detail::file_sha256(root / path).value(); };
    const auto wave_before = digest("TONE.W01");
    const auto disk_before = digest("empty.hds");
    const auto setup_before = digest("main/SET.S01");
    const auto ex5_before = digest("ex5.hda");

    auto sandbox = app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    app::PathReservationCoordinator reservations;
    app::ImageSessionManager sessions{*sandbox,     4U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now,
                                      &reservations};
    const auto mixed = sessions.open({{{"root", "empty.hds"}, {"root", "TONE.W01"}}, {}}, "owner");
    ASSERT_TRUE(mixed) << mixed.error().message;
    ASSERT_EQ(mixed->sources.size(), 2U);
    ASSERT_EQ(mixed->documents.size(), 2U);
    const auto a = std::ranges::find(mixed->documents, "yamaha.a-series", &app::ImageDocumentSummary::format_id);
    const auto t = std::ranges::find(mixed->documents, "yamaha.tx16w", &app::ImageDocumentSummary::format_id);
    ASSERT_NE(a, mixed->documents.end());
    ASSERT_NE(t, mixed->documents.end());
    EXPECT_EQ(a->source_member_ids, std::vector<std::string>{mixed->sources.front().id});
    EXPECT_EQ(t->source_member_ids, std::vector<std::string>{mixed->sources.back().id});
    EXPECT_TRUE(std::ranges::contains(a->available_operations, "images.alter.objects"));
    EXPECT_FALSE(std::ranges::contains(t->available_operations, "images.alter.objects"));
    EXPECT_EQ(a->object_count, 0U);
    const app::ImageDocumentRef tone{mixed->image_id, t->id};
    const auto objects = sessions.objects(tone, "owner", 10U);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    const auto &tone_id = objects->items.front().id;
    auto registry = app::make_operation_registry();
    app::UploadStore uploads{root / "uploads", 1024U, 1024U, 1U, 1024U, std::chrono::minutes{1}};
    app::AlterationJournalStore journals{root / "journals"};
    ASSERT_TRUE(app::bind_session_write_operations(registry, *sandbox, uploads, sessions, journals));
    const nlohmann::json rename{{"schema_version", "1.0"},
                                {"operations",
                                 {{{"id", "rename"},
                                   {"type", "rename_volume"},
                                   {"partition_index", 0U},
                                   {"volume_name", "Empty"},
                                   {"new_volume_name", "Renamed"}}}}};
    const auto edit = registry.invoke("images.alter",
                                      {{"imageId", mixed->image_id},
                                       {"documentId", a->id},
                                       {"expectedRevision", mixed->revision},
                                       {"manifest", {{"inline", rename}}}},
                                      {"owner", "control", {}, nullptr, {}});
    ASSERT_TRUE(edit) << edit.error().message;
    EXPECT_TRUE(edit->at("applied").get<bool>());
    EXPECT_NE(digest("empty.hds"), disk_before);
    EXPECT_EQ(digest("TONE.W01"), wave_before);
    EXPECT_TRUE(sessions.object_detail(tone, "owner", tone_id));
    const auto mapped = sessions.filesystem(mixed->image_id, "owner", mixed->revision + 1U,
                                            {.object_id = tone_id, .document_id = tone.document_id});
    ASSERT_TRUE(mapped) << mapped.error().message;
    ASSERT_EQ(mapped->items.size(), 1U);
    EXPECT_EQ(mapped->items.front().source_member_id, mixed->sources.back().id);

    const auto partial = sessions.open({{{"root", "main", app::ImageSourceKind::directory}}, {}}, "owner");
    ASSERT_TRUE(partial) << partial.error().message;
    ASSERT_EQ(partial->documents.size(), 1U);
    const auto &document = partial->documents.front();
    ASSERT_EQ(document.companions.requirements.size(), 1U);
    EXPECT_EQ(document.companions.requirements.front().name, "TONE.W01");
    const app::ImageDocumentRef set{partial->image_id, document.id};
    const auto before = sessions.objects(set, "owner", 100U);
    ASSERT_TRUE(before);
    ASSERT_FALSE(before->items.empty());
    const auto wave_edges = [&] {
        app::ImageRelationshipFilter filter{};
        filter.relationship_type = "wave-reference";
        return sessions.relationships(set, "owner", 10U, {}, filter);
    };
    const auto missing = wave_edges();
    ASSERT_TRUE(missing);
    ASSERT_EQ(missing->items.size(), 1U);
    EXPECT_EQ(missing->items.front().quality, "missing");
    EXPECT_FALSE(missing->items.front().target_object_id);
    EXPECT_FALSE(sessions.attach_companions(
        set, "owner", partial->revision,
        {app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}, {"root", "another/TONE.W01"}}}));
    const auto unchanged = sessions.inspect(set.image_id, "owner");
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->revision, partial->revision);
    ASSERT_EQ(unchanged->sources.size(), 1U);
    const auto complete = sessions.attach_companions(set, "owner", partial->revision,
                                                     {app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}}});
    ASSERT_TRUE(complete) << complete.error().message;
    ASSERT_EQ(complete->documents.size(), 1U);
    EXPECT_EQ(complete->documents.front().completeness, "COMPLETE");
    EXPECT_TRUE(complete->documents.front().companions.requirements.empty());
    EXPECT_EQ(complete->documents.front().object_count, document.object_count + 1U);
    for (const auto &object : before->items)
        EXPECT_TRUE(sessions.object_detail(set, "owner", object.id));
    const auto resolved = wave_edges();
    ASSERT_TRUE(resolved);
    ASSERT_EQ(resolved->items.size(), 1U);
    const auto &edge = resolved->items.front();
    EXPECT_EQ(edge.source_object_id, missing->items.front().source_object_id);
    EXPECT_EQ(edge.quality, "resolved");
    ASSERT_TRUE(edge.target_object_id);
    const auto supplier = sessions.filesystem(set.image_id, "owner", complete->revision,
                                              {.object_id = *edge.target_object_id, .document_id = set.document_id});
    ASSERT_TRUE(supplier);
    ASSERT_EQ(supplier->items.size(), 1U);
    EXPECT_EQ(supplier->items.front().source_member_id, complete->sources.back().id);
    EXPECT_EQ(digest("main/SET.S01"), setup_before);
    EXPECT_EQ(digest("TONE.W01"), wave_before);

    const auto ex5 = sessions.open({{{"root", "ex5.hda"}}, {}}, "owner");
    ASSERT_TRUE(ex5) << ex5.error().message;
    EXPECT_TRUE(ex5->documents.empty());
    ASSERT_EQ(ex5->sources.size(), 1U);
    ASSERT_EQ(ex5->sources.front().filesystem_root_ids.size(), 1U);
    const auto files =
        sessions.filesystem(ex5->image_id, "owner", ex5->revision,
                            {.root_id = ex5->sources.front().filesystem_root_ids.front(), .query = "DEMO1.S1A"});
    ASSERT_TRUE(files) << files.error().message;
    ASSERT_EQ(files->items.size(), 1U);
    EXPECT_EQ(files->items.front().name, "DEMO1.S1A");
    EXPECT_TRUE(files->items.front().bindings.empty());
    EXPECT_FALSE(sessions.objects({ex5->image_id, a->id}, "owner", 10U));
    EXPECT_EQ(digest("ex5.hda"), ex5_before);
}

} // namespace
