#include <array>

#include <gtest/gtest.h>

#include "axklib/application/path_reservations.hpp"

namespace {

using axk::app::PathAccess;
using axk::app::PathAccessMode;
using axk::app::PathReservationCoordinator;

TEST(PathReservationCoordinatorTest, SharedAncestorsCanCoexist) {
    PathReservationCoordinator coordinator;
    auto first = coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(first) << first.error().message;

    auto second = coordinator.try_acquire(PathAccess{{"workspace", "images"}, PathAccessMode::shared});
    EXPECT_TRUE(second) << second.error().message;
}

TEST(PathReservationCoordinatorTest, ExclusiveReservationConflictsWithAncestorsAndDescendants) {
    PathReservationCoordinator coordinator;
    auto active = coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(active) << active.error().message;

    for (const auto &path : {"", "images", "images/disk.hds", "images/disk.hds/member"}) {
        auto mutation = coordinator.try_acquire(PathAccess{{"workspace", path}, PathAccessMode::exclusive});
        ASSERT_FALSE(mutation) << path;
        EXPECT_EQ(mutation.error().code, "entry_in_use");
    }
    EXPECT_TRUE(coordinator.try_acquire(PathAccess{{"other", "images"}, PathAccessMode::exclusive}));
}

TEST(PathReservationCoordinatorTest, BatchAcquisitionIsAtomicAndReleasedByLeaseLifetime) {
    PathReservationCoordinator coordinator;
    auto blocker = coordinator.try_acquire(PathAccess{{"workspace", "destination"}, PathAccessMode::shared});
    ASSERT_TRUE(blocker) << blocker.error().message;

    const std::array batch{PathAccess{{"workspace", "source"}, PathAccessMode::exclusive},
                           PathAccess{{"workspace", "destination"}, PathAccessMode::exclusive}};
    EXPECT_FALSE(coordinator.try_acquire(batch));

    auto source = coordinator.try_acquire(PathAccess{{"workspace", "source"}, PathAccessMode::exclusive});
    EXPECT_TRUE(source) << source.error().message;
    source = {};
    blocker = {};

    auto acquired = coordinator.try_acquire(batch);
    ASSERT_TRUE(acquired) << acquired.error().message;
    acquired = {};
    EXPECT_TRUE(coordinator.try_acquire(batch));
}

TEST(PathReservationCoordinatorTest, SessionLeaseUpgradeIsAtomicAndReversible) {
    PathReservationCoordinator coordinator;
    auto session = coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(session) << session.error().message;
    auto reader = coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(reader) << reader.error().message;
    EXPECT_FALSE(session->try_upgrade());
    reader = {};
    ASSERT_TRUE(session->try_upgrade());
    EXPECT_FALSE(coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared}));
    session->downgrade();
    EXPECT_TRUE(coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::shared}));
}

TEST(PathReservationCoordinatorTest, AllRootsReservationSerializesWorkspaceCatalogChanges) {
    PathReservationCoordinator coordinator;
    auto image = coordinator.try_acquire(PathAccess{{"workspace-a", "image.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(image) << image.error().message;

    const auto all_roots = PathAccess{.reference = {}, .mode = PathAccessMode::exclusive, .all_roots = true};
    EXPECT_FALSE(coordinator.try_acquire(all_roots));
    EXPECT_TRUE(coordinator.try_acquire(PathAccess{{"workspace-b", "image.hds"}, PathAccessMode::exclusive}));
    image = {};

    auto catalog_change = coordinator.try_acquire(all_roots);
    ASSERT_TRUE(catalog_change) << catalog_change.error().message;
    EXPECT_FALSE(coordinator.try_acquire(PathAccess{{"workspace-b", "image.hds"}, PathAccessMode::shared}));
}

#if defined(_WIN32) || defined(__APPLE__)
TEST(PathReservationCoordinatorTest, ConservativelyMatchesCaseAndUnicodeOnCaseFoldingPlatforms) {
    PathReservationCoordinator coordinator;
    auto mixed_case = coordinator.try_acquire(PathAccess{{"workspace", "Images/Disk.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(mixed_case) << mixed_case.error().message;
    EXPECT_FALSE(coordinator.try_acquire(PathAccess{{"workspace", "images/disk.hds"}, PathAccessMode::exclusive}));

    auto unicode = coordinator.try_acquire(PathAccess{{"workspace", "images/\xC3\x84.hds"}, PathAccessMode::shared});
    ASSERT_TRUE(unicode) << unicode.error().message;
    EXPECT_FALSE(coordinator.try_acquire(PathAccess{{"workspace", "unrelated.hds"}, PathAccessMode::exclusive}));
}
#endif

} // namespace
