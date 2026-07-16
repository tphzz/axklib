#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/package_operations.hpp"
#include "axklib/writer.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> chars{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return {reinterpret_cast<const std::byte *>(chars.data()),
            reinterpret_cast<const std::byte *>(chars.data() + chars.size())};
}

void write_empty_target(const std::filesystem::path &path, std::string_view partition_name = "Target") {
    const auto manifest = axk::parse_hds_build_manifest(
        std::string{R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":")"} +
        std::string{partition_name} + R"(","volumes":[{"name":"Imported","waveforms":[],"sample_banks":[]}]}]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto written = axk::write_hds_image(*manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

class PackageOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-package-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "uploads");
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        write_empty_target(root_ / "target.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        uploads_ = std::make_unique<axk::app::UploadStore>(root_ / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                           8U, 2U * 1024U * 1024U, std::chrono::minutes{5});
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_package_operations(registry_, *sandbox_, *uploads_));
    }

    void TearDown() override {
        uploads_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::UploadStore> uploads_;
    axk::app::OperationRegistry registry_;
};

TEST_F(PackageOperationsTest, ExportInspectUploadVerifyPlanAndApplyShareOneRegistryContract) {
    for (const auto operation :
         {"package.export", "package.inspect", "package.verify", "package.plan_import", "package.import"}) {
        EXPECT_TRUE(registry_.is_implemented(operation));
    }
    const auto exported = registry_.invoke(
        "package.export",
        {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
         {"output", {{"rootId", "workspace"}, {"relativePath", "bank"}}},
         {"roots",
          {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", "sine wave"}}}}},
        context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("output").at("relativePath"), "bank.axksbnk");
    EXPECT_TRUE(exported->at("payloadsVerified").get<bool>());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "bank.axksbnk"));

    const nlohmann::json file_input = {
        {"package", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "bank.axksbnk"}}}}}};
    const auto inspected = registry_.invoke("package.inspect", file_input, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->at("payloadsVerified").get<bool>());
    EXPECT_EQ(inspected->at("packageId"), exported->at("packageId"));
    EXPECT_TRUE(inspected->at("packageKind").is_string());
    EXPECT_TRUE(inspected->at("requiredExtension").is_string());
    ASSERT_FALSE(inspected->at("roots").empty());
    EXPECT_TRUE(inspected->at("roots").front().at("kind").is_string());
    for (const auto &object : inspected->at("objects")) {
        EXPECT_TRUE(object.at("semanticSha256").is_null() || object.at("semanticSha256").is_string());
        EXPECT_TRUE(object.at("audioSha256").is_null() || object.at("audioSha256").is_string());
    }

    const auto bytes = read_bytes(root_ / "bank.axksbnk");
    auto upload = uploads_->create({.owner_id = "owner",
                                    .filename = "dropped.axksbnk",
                                    .kind = axk::app::UploadKind::package,
                                    .media_type = "application/octet-stream",
                                    .declared_size = bytes.size(),
                                    .sha256 = std::nullopt});
    ASSERT_TRUE(upload) << upload.error().message;
    upload = uploads_->append(upload->reference, "owner", 0U, bytes);
    ASSERT_TRUE(upload) << upload.error().message;
    upload = uploads_->complete(upload->reference, "owner");
    ASSERT_TRUE(upload) << upload.error().message;
    const nlohmann::json upload_input = {{"package", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}};
    const auto verified = registry_.invoke("package.verify", upload_input, context());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->at("payloadsVerified").get<bool>());
    EXPECT_EQ(verified->at("packageId"), exported->at("packageId"));

    const auto import_request = nlohmann::json{
        {"target", {{"rootId", "workspace"}, {"relativePath", "target.hds"}}},
        {"output", {{"rootId", "workspace"}, {"relativePath", "imported.hds"}}},
        {"packages", {{{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}},
        {"destinations",
         {{{"packageIndex", 0U}, {"rootIndex", 0U}, {"partitionIndex", 0U}, {"volumeName", "Imported"}}}},
    };
    const auto planned = registry_.invoke("package.plan_import", import_request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_TRUE(planned->at("valid").get<bool>());
    const auto reserved = registry_.invoke("package.plan_import", import_request, context());
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().code, "destination_reserved");

    auto other = context();
    other.owner_id = "other";
    const auto denied =
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "package_plan_not_found");

    const auto applied =
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->at("applied").get<bool>());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "imported.hds"));
    EXPECT_NE(read_bytes(root_ / "imported.hds"), read_bytes(root_ / "target.hds"));
    EXPECT_FALSE(
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context()));
}

TEST_F(PackageOperationsTest, RejectsSequenceRootsAndUploadOwnershipMismatch) {
    const auto sequence = registry_.invoke("package.export",
                                           {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
                                            {"output", {{"rootId", "workspace"}, {"relativePath", "sequence.axkseq"}}},
                                            {"roots", {{{"kind", "sequence"}, {"objectName", "Sequence"}}}}},
                                           context());
    ASSERT_FALSE(sequence);
    EXPECT_EQ(sequence.error().code, "unsupported_package_root");

    auto upload = uploads_->create({.owner_id = "owner",
                                    .filename = "private.axkvol",
                                    .kind = axk::app::UploadKind::package,
                                    .media_type = "application/octet-stream",
                                    .declared_size = 1U,
                                    .sha256 = std::nullopt});
    ASSERT_TRUE(upload) << upload.error().message;
    const std::array byte{std::byte{0}};
    ASSERT_TRUE(uploads_->append(upload->reference, "owner", 0U, byte));
    ASSERT_TRUE(uploads_->complete(upload->reference, "owner"));
    auto other = context();
    other.owner_id = "other";
    const auto denied = registry_.invoke(
        "package.inspect", {{"package", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "upload_not_found");
}

TEST_F(PackageOperationsTest, PlanTokenRejectsChangedPackageAndTargetFilesWithoutPublication) {
    const auto export_package = [&](std::string_view name, std::string_view output) {
        return registry_.invoke(
            "package.export",
            {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
             {"output", {{"rootId", "workspace"}, {"relativePath", output}}},
             {"roots",
              {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", name}}}}},
            context());
    };
    const auto sine = export_package("sine wave", "sine");
    ASSERT_TRUE(sine) << sine.error().message;
    const auto square = export_package("square", "square");
    ASSERT_TRUE(square) << square.error().message;
    const auto sine_bytes = read_bytes(root_ / "sine.axksbnk");

    const auto plan_request = [&](std::string_view target, std::string_view output, std::string_view package) {
        return nlohmann::json{
            {"target", {{"rootId", "workspace"}, {"relativePath", target}}},
            {"output", {{"rootId", "workspace"}, {"relativePath", output}}},
            {"packages", {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", package}}}}}},
            {"destinations",
             {{{"packageIndex", 0U}, {"rootIndex", 0U}, {"partitionIndex", 0U}, {"volumeName", "Imported"}}}}};
    };

    const auto package_plan = registry_.invoke(
        "package.plan_import", plan_request("target.hds", "stale-package.hds", "sine.axksbnk"), context());
    ASSERT_TRUE(package_plan) << package_plan.error().message;
    std::filesystem::copy_file(root_ / "square.axksbnk", root_ / "sine.axksbnk",
                               std::filesystem::copy_options::overwrite_existing);
    const auto stale_package = registry_.invoke(
        "package.import", {{"planToken", package_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_package);
    EXPECT_EQ(stale_package.error().code, "package_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-package.hds"));

    std::ofstream sine_output{root_ / "sine.axksbnk", std::ios::binary | std::ios::trunc};
    sine_output.write(reinterpret_cast<const char *>(sine_bytes.data()),
                      static_cast<std::streamsize>(sine_bytes.size()));
    sine_output.close();
    std::filesystem::copy_file(root_ / "target.hds", root_ / "stale-target.hds");
    write_empty_target(root_ / "changed-target.hds", "Changed");
    const auto target_plan = registry_.invoke(
        "package.plan_import", plan_request("stale-target.hds", "stale-target-output.hds", "sine.axksbnk"), context());
    ASSERT_TRUE(target_plan) << target_plan.error().message;
    std::filesystem::copy_file(root_ / "changed-target.hds", root_ / "stale-target.hds",
                               std::filesystem::copy_options::overwrite_existing);
    const auto stale_target =
        registry_.invoke("package.import", {{"planToken", target_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_target);
    EXPECT_EQ(stale_target.error().code, "package_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-target-output.hds"));
}

} // namespace
