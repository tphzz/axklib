#include <array>
#include <memory>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "media_test_fixtures.hpp"

TEST(PackageGraphTest, DirectGraphMatchesArchivedFloppyPayloadsAndIdentity) {
    auto floppy = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "source.img");
    ASSERT_TRUE(floppy);
    const axk::MediaContainer media{std::move(*floppy)};
    axk::PackageRootSelector root;
    root.kind = axk::PackageRootKind::smpl;
    root.object_name = "TEST";
    const std::array roots{root};
    const auto graph = axk::build_portable_graph(media, roots);
    ASSERT_TRUE(graph) << graph.error().message;
    const auto archived = axk::build_portable_package(media, roots);
    ASSERT_TRUE(archived) << archived.error().message;
    EXPECT_EQ(graph->package_id, archived->package.package_id);
    EXPECT_EQ(graph->nodes, archived->package.nodes);
    EXPECT_EQ(graph->relationships, archived->package.relationships);
    EXPECT_EQ(graph->roots, archived->package.roots);
    EXPECT_EQ(graph->issues, archived->package.issues);
    ASSERT_EQ(graph->nodes.size(), 1U);
    ASSERT_EQ(archived->package.nodes.size(), 1U);
    EXPECT_EQ(graph->nodes.front().raw_payload, archived->package.nodes.front().raw_payload);
    EXPECT_EQ(graph->nodes.front().node_id, archived->package.nodes.front().node_id);
    EXPECT_EQ(graph->nodes.front().payload_sha256, archived->package.nodes.front().payload_sha256);
    EXPECT_EQ(graph->roots.front().node_ids, archived->package.roots.front().node_ids);
    EXPECT_TRUE(graph->payloads_verified);
    EXPECT_TRUE(axk::verify_portable_package(*graph));
    EXPECT_FALSE(archived->archive.empty());
}

TEST(PackageGraphTest, RejectsEmptyMissingAndCancelledSelections) {
    auto floppy = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "source.img");
    ASSERT_TRUE(floppy);
    const axk::MediaContainer media{std::move(*floppy)};
    EXPECT_FALSE(axk::build_portable_graph(media, {}));
    axk::PackageRootSelector root;
    root.kind = axk::PackageRootKind::smpl;
    root.object_name = "MISSING";
    EXPECT_FALSE(axk::build_portable_graph(media, std::array{root}));
    axk::CancellationSource cancelled;
    cancelled.cancel();
    root.object_name = "TEST";
    EXPECT_FALSE(axk::build_portable_graph(media, std::array{root}, cancelled.token()));
}
