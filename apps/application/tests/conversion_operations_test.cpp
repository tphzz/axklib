#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/conversions.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;
using namespace axk::app;

class ConversionOperations : public testing::Test {
  protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axklib-conversion-operations-test";
    std::optional<Sandbox> sandbox;
    OperationRegistry registry = make_operation_registry();
    PathReservationCoordinator reservations;
    std::unique_ptr<UploadStore> uploads;
    std::unique_ptr<ImageSessionManager> images;
    std::unique_ptr<AlterationJournalStore> journals;
    std::unique_ptr<JobManager> jobs;
    std::unique_ptr<ConversionService> service;
    OperationContext context{};
    ImageSessionSummary workspace;

    virtual bool writable() const { return true; }

    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Disk", {{"Samples", {}, {}, {}, {}}}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, root / "target.hds", false));
        std::vector<char> bytes(35U);
        std::ranges::copy(std::string_view{"LM8953"}, bytes.begin());
        bytes[22] = static_cast<char>(0xc9);
        bytes[23] = bytes[24] = 2;
        bytes[32] = 0x7f;
        bytes[33] = static_cast<char>(0xf0);
        bytes[34] = static_cast<char>(0x80);
        {
            std::ofstream output{root / "TONE.W01", std::ios::binary};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        auto created = Sandbox::create({{"root", "Root", root, writable()}});
        ASSERT_TRUE(created);
        sandbox.emplace(std::move(*created));
        uploads = std::make_unique<UploadStore>(root / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U, 8U,
                                                1024U * 1024U, std::chrono::minutes{15});
        images = std::make_unique<ImageSessionManager>(*sandbox, 8U, 200U, std::chrono::minutes{15},
                                                       std::chrono::steady_clock::now, &reservations);
        journals = std::make_unique<AlterationJournalStore>(root / "journals");
        jobs = std::make_unique<JobManager>(registry, 1U, 1U, 8U, 16U);
        service = std::make_unique<ConversionService>(*sandbox, *uploads, *images, *journals, *jobs);
        ASSERT_TRUE(service->bind(registry));
        context.owner_id = "owner";
        auto opened = images->open({{{"root", "target.hds"}, {"root", "TONE.W01"}}, {}}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        workspace = *opened;
    }
    void TearDown() override {
        jobs->shutdown();
        service.reset();
        jobs.reset();
        images.reset();
        uploads.reset();
        journals.reset();
        std::filesystem::remove_all(root);
    }
    Json request(bool native = true) const {
        Json source = {{"kind", "DOCUMENT"},
                       {"imageId", workspace.image_id},
                       {"documentId", "document-tx16w"},
                       {"expectedRevision", 1}};
        if (!native)
            source = {{"kind", "SOURCES"},
                      {"sources", Json::array({{{"kind", "FILE"},
                                                {"file", {{"rootId", "root"}, {"relativePath", "TONE.W01"}}}}})}};
        return {{"profileId", "yamaha.tx16w-to-a-series"},
                {"source", source},
                {"destination",
                 {{"imageId", workspace.image_id},
                  {"documentId", "document-a-series-source-1"},
                  {"expectedRevision", 1},
                  {"selection", {{"partitionIndex", 0}, {"volumeName", "Samples"}}}}},
                {"options", {{"mode", "WAVE_DATA_ONLY"}}},
                {"fidelity", "BEST"}};
    }
    JobSnapshot wait(const std::string &id) {
        for (std::size_t count = 0U; count < 5000U; ++count) {
            auto current = jobs->status(id, "owner");
            if (!current) {
                ADD_FAILURE() << current.error().message;
                return {};
            }
            if (is_terminal(current->state))
                return *current;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        ADD_FAILURE() << "conversion job timed out";
        return {};
    }
    Json acknowledge(const Json &plan) const {
        Json ids = Json::array();
        for (const auto &issue : plan.at("report").at("issues"))
            if (issue.at("impact") == "LOSS" || issue.at("impact") == "UNCERTAINTY")
                ids.push_back(issue.at("id"));
        return {{"acknowledgedIssueIds", ids}};
    }
};

TEST_F(ConversionOperations, NativeSourceAndDestinationInOneWorkspacePrepareAndCommitWithoutDeadlock) {
    auto job = jobs->submit("conversions.prepare", request(), context);
    ASSERT_TRUE(job);
    auto prepared = wait(job->job_id);
    ASSERT_EQ(prepared.state, JobState::completed) << (prepared.error ? prepared.error->message : "");
    ASSERT_TRUE(prepared.result);
    const auto &plan = *prepared.result;
    EXPECT_TRUE(plan.at("jobId").is_null());
    EXPECT_TRUE(plan.at("report").at("executable").get<bool>());
    EXPECT_EQ(images->inspect(workspace.image_id, "owner")->revision, 1U);
    const auto id = plan.at("planId").get<std::string>();
    EXPECT_FALSE(service->execute(id, {{"acknowledgedIssueIds", Json::array()}}, context));
    auto execution = service->execute(id, acknowledge(plan), context);
    ASSERT_TRUE(execution) << execution.error().message;
    auto repeated = service->execute(id, acknowledge(plan), context);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->job_id, execution->job_id);
    auto completed = wait(execution->job_id);
    ASSERT_EQ(completed.state, JobState::completed) << (completed.error ? completed.error->message : "");
    EXPECT_EQ(completed.result->at("revision"), 2U);
    EXPECT_EQ(images->inspect(workspace.image_id, "owner")->documents.size(), 2U);
    EXPECT_EQ(service->execute(id, acknowledge(plan), context)->job_id, execution->job_id);
    EXPECT_EQ(service->plan(id, "owner")->at("jobId"), execution->job_id);
}

TEST_F(ConversionOperations, ExplicitSourcePreparationIsReadOnlyAndRejectsChangedSources) {
    auto prepared = registry.invoke("conversions.prepare", request(false), context);
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto id = prepared->at("planId").get<std::string>();
    EXPECT_FALSE(service->plan(id, "other"));
    EXPECT_TRUE(images->inspect(workspace.image_id, "owner"));
    {
        std::ofstream changed{root / "TONE.W01", std::ios::binary | std::ios::app};
        changed << 'x';
    }
    auto execution = service->execute(id, acknowledge(*prepared), context);
    ASSERT_TRUE(execution);
    auto completed = wait(execution->job_id);
    EXPECT_EQ(completed.state, JobState::failed);
    EXPECT_EQ(service->execute(id, acknowledge(*prepared), context)->job_id, execution->job_id);
}

TEST_F(ConversionOperations, NativeSnapshotDoesNotRetainAnAccessLockAndClosedWorkspaceCannotExecute) {
    auto snapshot = images->native_snapshot({workspace.image_id, "document-tx16w"}, "owner", 1U);
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(images->inspect(workspace.image_id, "owner"));
    auto prepared = registry.invoke("conversions.prepare", request(), context);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(images->close(workspace.image_id, "owner"));
    EXPECT_FALSE(snapshot->verify_current());
    auto execution = service->execute(prepared->at("planId").get<std::string>(), acknowledge(*prepared), context);
    ASSERT_TRUE(execution);
    EXPECT_EQ(wait(execution->job_id).state, JobState::failed);
}

TEST_F(ConversionOperations, RejectsObsoleteFieldsAndStrictLossAndPagesReviewDetails) {
    auto input = request();
    input["legacy"] = true;
    EXPECT_FALSE(registry.invoke("conversions.prepare", input, context));
    input.erase("legacy");
    input["fidelity"] = "STRICT";
    auto strict = registry.invoke("conversions.prepare", input, context);
    ASSERT_TRUE(strict) << strict.error().message;
    EXPECT_FALSE(strict->at("report").at("executable").get<bool>());
    EXPECT_FALSE(service->execute(strict->at("planId").get<std::string>(), acknowledge(*strict), context));
    ASSERT_TRUE(service->release(strict->at("planId").get<std::string>(), "owner"));
    auto prepared = registry.invoke("conversions.prepare", request(), context);
    ASSERT_TRUE(prepared) << prepared.error().message;
    auto page = service->plan(prepared->at("planId").get<std::string>(), "owner", 0U, 1U);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->at("report").at("issues").size(), 1U);
    EXPECT_FALSE(page->at("report").at("actions").front().at("details").empty());
    EXPECT_EQ(images->inspect(workspace.image_id, "owner")->revision, 1U);
}

TEST_F(ConversionOperations, RejectsUnknownUploadSourceFieldsBeforeResourceLookup) {
    for (const bool nested : {false, true}) {
        auto input = request();
        Json source = {{"kind", "UPLOAD"}, {"upload", {{"uploadId", "missing"}}}};
        (nested ? source["upload"] : source)["obsolete"] = true;
        input["source"] = {{"kind", "SOURCES"}, {"sources", Json::array({source})}};
        const auto prepared = registry.invoke("conversions.prepare", input, context);
        ASSERT_FALSE(prepared);
        EXPECT_EQ(prepared.error().code, "invalid_request");
    }
}

class ReadOnlyConversionOperations : public ConversionOperations {
    bool writable() const override { return false; }
};

TEST_F(ReadOnlyConversionOperations, RejectsReadOnlyDestinationBeforeOfferingAnExecutablePlan) {
    const auto prepared = registry.invoke("conversions.prepare", request(), context);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().code, "image_mutation_unsupported");
    EXPECT_EQ(images->inspect(workspace.image_id, "owner")->revision, 1U);
}

} // namespace
