#include <array>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/server/workspaces.hpp"

namespace {

class WorkspaceStoreTest : public testing::Test {
  protected:
    void SetUp() override {
        base_ = std::filesystem::temp_directory_path() / "axklib-workspace-store-test";
        std::error_code error;
        std::filesystem::remove_all(base_, error);
        std::filesystem::create_directories(base_ / "one" / "nested");
        std::filesystem::create_directories(base_ / "two");
        store_path_ = base_ / "config" / "workspaces.json";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(base_, error);
    }

    std::filesystem::path base_;
    std::filesystem::path store_path_;
};

TEST(WorkspaceStoreDefaults, UsesVendorScopedConfigurationDirectory) {
    const auto path = axk::server::WorkspaceStore::default_path();
    ASSERT_TRUE(path) << path.error().message;
    EXPECT_EQ(path->filename(), "workspaces.json");
    EXPECT_EQ(path->parent_path().filename(), "axklib-server");
    EXPECT_EQ(path->parent_path().parent_path().filename(), "tphzz");
}

TEST_F(WorkspaceStoreTest, MissingStoreStartsWithoutAnAvailableWorkspace) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot.revision, 0U);
    EXPECT_EQ(snapshot.state, axk::server::WorkspaceConfigurationState::no_available_workspace);
    EXPECT_TRUE(snapshot.workspaces.empty());
    EXPECT_TRUE(store->sandbox().roots().empty());
    EXPECT_FALSE(std::filesystem::exists(store_path_));
}

TEST_F(WorkspaceStoreTest, PersistsStableIdsAndRevalidatesUnavailableDirectories) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    auto added = store->add("Samples", base_ / "one", true, 0U);
    ASSERT_TRUE(added) << added.error().message;
    EXPECT_TRUE(added->definition.id.starts_with("workspace-"));
    EXPECT_EQ(store->snapshot().state, axk::server::WorkspaceConfigurationState::ready);
    ASSERT_EQ(store->sandbox().roots().size(), 1U);

    const auto stable_id = added->definition.id;
    auto reopened = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto snapshot = reopened->snapshot();
    ASSERT_EQ(snapshot.workspaces.size(), 1U);
    EXPECT_EQ(snapshot.workspaces.front().definition.id, stable_id);
    EXPECT_EQ(snapshot.revision, 1U);

    std::filesystem::remove_all(base_ / "one");
    snapshot = reopened->snapshot();
    EXPECT_EQ(snapshot.state, axk::server::WorkspaceConfigurationState::no_available_workspace);
    EXPECT_EQ(snapshot.workspaces.front().status, axk::server::WorkspaceStatus::missing);
    EXPECT_TRUE(reopened->sandbox().roots().empty());
}

TEST_F(WorkspaceStoreTest, WritableInspectionLeavesNoCheckArtifacts) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    auto added = store->add("Samples", base_ / "one", true, 0U);
    ASSERT_TRUE(added) << added.error().message;
    EXPECT_TRUE(added->effective_writable);

    for (const auto &entry : std::filesystem::directory_iterator(base_ / "one")) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(".axkdeck-write-check-"));
    }
}

TEST_F(WorkspaceStoreTest, RejectsStaleMutationsAndKeepsInvalidUpdatesVisible) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    auto added = store->add("One", base_ / "one", false, 0U);
    ASSERT_TRUE(added) << added.error().message;
    auto stale = store->add("Two", base_ / "two", true, 0U);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "workspace_revision_conflict");

    auto updated = store->update(added->definition.id, std::nullopt, base_ / "missing", std::nullopt, 1U);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->status, axk::server::WorkspaceStatus::missing);
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot.state, axk::server::WorkspaceConfigurationState::no_available_workspace);
    EXPECT_EQ(snapshot.revision, 2U);
}

TEST_F(WorkspaceStoreTest, RejectsOverlappingWorkspaceTreesWithoutChangingTheStore) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    auto added = store->add("One", base_ / "one", false, 0U);
    ASSERT_TRUE(added) << added.error().message;

    for (const auto &overlap : std::array{
             base_ / "one",
             base_ / "one" / "nested",
             base_,
             base_ / "one" / ".." / "one",
         }) {
        auto rejected = store->add("Overlap", overlap, false, 1U);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, "workspace_path_overlap");
        EXPECT_EQ(store->snapshot().revision, 1U);
        EXPECT_EQ(store->snapshot().workspaces.size(), 1U);
    }

    auto disjoint = store->add("Two", base_ / "two", false, 1U);
    ASSERT_TRUE(disjoint) << disjoint.error().message;
    auto rejected_update =
        store->update(disjoint->definition.id, std::nullopt, base_ / "one" / "nested", std::nullopt, 2U);
    ASSERT_FALSE(rejected_update);
    EXPECT_EQ(rejected_update.error().code, "workspace_path_overlap");
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot.revision, 2U);
    ASSERT_EQ(snapshot.workspaces.size(), 2U);
    EXPECT_EQ(snapshot.workspaces.back().definition.path, std::filesystem::canonical(base_ / "two"));
}

TEST_F(WorkspaceStoreTest, TreatsPersistedOverlappingWorkspaceTreesAsAConfigurationError) {
    std::filesystem::create_directories(store_path_.parent_path());
    std::ofstream(store_path_) << nlohmann::json{
        {"schemaVersion", 1U},
        {"revision", 2U},
        {"workspaces",
         {{{"id", "one"}, {"displayName", "One"}, {"path", (base_ / "one").string()}, {"writable", false}},
          {{"id", "nested"},
           {"displayName", "Nested"},
           {"path", (base_ / "one" / "nested").string()},
           {"writable", false}}}},
    };

    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot.state, axk::server::WorkspaceConfigurationState::configuration_error);
    ASSERT_TRUE(snapshot.configuration_issue);
    EXPECT_EQ(*snapshot.configuration_issue, "workspace directories must not overlap");
    EXPECT_TRUE(store->sandbox().roots().empty());
}

TEST_F(WorkspaceStoreTest, RejectsWorkspacesThatOverlapProtectedServerState) {
    const auto state = base_ / "state";
    std::filesystem::create_directories(state / "alteration-journals");
    auto store = axk::server::WorkspaceStore::open(store_path_, {state, store_path_});
    ASSERT_TRUE(store) << store.error().message;

    for (const auto &protected_overlap :
         std::array{state, state / "alteration-journals", base_, store_path_.parent_path()}) {
        auto rejected = store->add("Protected", protected_overlap, false, 0U);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, "workspace_protected_path");
        EXPECT_EQ(store->snapshot().revision, 0U);
    }

    const auto alias = base_ / "state-alias";
    std::error_code error;
    std::filesystem::create_directory_symlink(state, alias, error);
    if (!error) {
        auto rejected = store->add("Alias", alias, false, 0U);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, "workspace_protected_path");
    }
}

TEST_F(WorkspaceStoreTest, TreatsPersistedProtectedWorkspaceAsConfigurationError) {
    const auto state = base_ / "state";
    std::filesystem::create_directories(state);
    std::filesystem::create_directories(store_path_.parent_path());
    std::ofstream(store_path_) << nlohmann::json{
        {"schemaVersion", 1U},
        {"revision", 1U},
        {"workspaces", {{{"id", "state"}, {"displayName", "State"}, {"path", state.string()}, {"writable", true}}}},
    };

    auto store = axk::server::WorkspaceStore::open(store_path_, {state, store_path_});
    ASSERT_TRUE(store) << store.error().message;
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot.state, axk::server::WorkspaceConfigurationState::configuration_error);
    ASSERT_TRUE(snapshot.configuration_issue);
    EXPECT_EQ(*snapshot.configuration_issue, "workspace directories must not overlap protected server state");
    EXPECT_TRUE(store->sandbox().roots().empty());
}

TEST_F(WorkspaceStoreTest, CorruptStoreRequiresExplicitArchiveAndReset) {
    std::filesystem::create_directories(store_path_.parent_path());
    std::ofstream(store_path_) << "not json";
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    EXPECT_EQ(store->snapshot().state, axk::server::WorkspaceConfigurationState::configuration_error);
    EXPECT_FALSE(store->add("One", base_ / "one", true, 0U));
    EXPECT_EQ(std::ifstream(store_path_).peek(), 'n');

    auto archived = store->archive_and_reset();
    ASSERT_TRUE(archived) << archived.error().message;
    ASSERT_TRUE(*archived);
    EXPECT_TRUE(std::filesystem::exists(**archived));
    EXPECT_EQ(store->snapshot().state, axk::server::WorkspaceConfigurationState::no_available_workspace);
    const auto document = nlohmann::json::parse(std::ifstream(store_path_));
    EXPECT_EQ(document.at("schemaVersion"), 1U);
    EXPECT_TRUE(document.at("workspaces").empty());
}

TEST_F(WorkspaceStoreTest, RefusesRecoveryResetForAHealthyStore) {
    auto store = axk::server::WorkspaceStore::open(store_path_);
    ASSERT_TRUE(store) << store.error().message;
    auto added = store->add("One", base_ / "one", true, 0U);
    ASSERT_TRUE(added) << added.error().message;

    auto reset = store->archive_and_reset();
    ASSERT_FALSE(reset);
    EXPECT_EQ(reset.error().code, "workspace_store_not_corrupt");
    EXPECT_EQ(store->snapshot().state, axk::server::WorkspaceConfigurationState::ready);
    EXPECT_EQ(store->snapshot().workspaces.size(), 1U);
}

} // namespace
