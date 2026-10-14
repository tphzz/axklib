#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../media_test_fixtures.hpp"
#include "axklib/formats/a_series/document.hpp"
#include "axklib/formats/a_series/native_document.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace {

class ObservedReader final : public axk::RandomAccessReader {
  public:
    explicit ObservedReader(std::vector<std::byte> bytes) : reader_(std::move(bytes)) {}
    std::uint64_t size() const noexcept override { return reader_.size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        bytes_read += bytes.size();
        return reader_.read_exact_at(offset, bytes);
    }
    mutable std::size_t bytes_read{};

  private:
    axk::MemoryReader reader_;
};

} // namespace

TEST(ASeriesDocument, PreservesInventoryGraphNavigationAndValidation) {
    auto reader = std::make_shared<ObservedReader>(nested_fat_fixture());
    auto media = axk::open_media(reader, "characterization.img");
    ASSERT_TRUE(media) << media.error().message;
    const auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(inventory) << inventory.error().message;
    const auto graph = axk::build_relationship_graph(inventory->catalog);
    const auto tree = axk::build_content_tree(*media, inventory->catalog, graph);
    const auto validation = axk::validate_semantics(*media, *inventory, graph);
    const auto document = axk::formats::a_series::Document::open(std::move(*media));
    ASSERT_TRUE(document) << document.error().message;
    const auto &state = (*document)->state();
    ASSERT_EQ(state.inventory.catalog.objects.size(), inventory->catalog.objects.size());
    ASSERT_EQ(state.inventory.objects.size(), inventory->objects.size());
    for (std::size_t index = 0; index < inventory->catalog.objects.size(); ++index) {
        EXPECT_EQ(state.inventory.catalog.objects[index].key, inventory->catalog.objects[index].key);
        EXPECT_EQ(state.inventory.catalog.objects[index].object.header.raw_type,
                  inventory->catalog.objects[index].object.header.raw_type);
        EXPECT_EQ(state.inventory.objects[index].size, inventory->objects[index].size);
        EXPECT_EQ(state.descriptors.at(state.inventory.objects[index].key), &state.inventory.objects[index]);
    }
    ASSERT_EQ(state.graph.relationships.size(), graph.relationships.size());
    for (std::size_t index = 0; index < graph.relationships.size(); ++index) {
        EXPECT_EQ(state.graph.relationships[index].key, graph.relationships[index].key);
        EXPECT_EQ(state.graph.relationships[index].target_key, graph.relationships[index].target_key);
        EXPECT_EQ(state.graph.relationships[index].candidate_keys, graph.relationships[index].candidate_keys);
        EXPECT_EQ(state.graph.relationships[index].quality, graph.relationships[index].quality);
        EXPECT_EQ(state.graph.relationships[index].basis, graph.relationships[index].basis);
    }
    ASSERT_EQ(state.tree.roots.size(), tree.roots.size());
    EXPECT_EQ(state.validation.issues.size(), validation.issues.size());
    EXPECT_EQ(state.validation.valid(), validation.valid());
    const auto reads = reader->bytes_read;
    EXPECT_FALSE(state.inventory.catalog.objects.empty());
    EXPECT_EQ((*document)->state().inventory.catalog.objects.front().key, inventory->catalog.objects.front().key);
    EXPECT_EQ(reader->bytes_read, reads);
}

TEST(ASeriesDocument, CancellationPreventsPublication) {
    auto media = axk::open_media(std::make_shared<axk::MemoryReader>(nested_fat_fixture()), "cancel.img");
    ASSERT_TRUE(media);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto document = axk::formats::a_series::Document::open(std::move(*media), cancellation.token());
    ASSERT_FALSE(document);
    EXPECT_EQ(document.error().code, axk::ErrorCode::operation_cancelled);
}

TEST(ASeriesDocument, NativeInterfaceRetainsDecodedOwnerWithoutReadingAgain) {
    auto reader = std::make_shared<ObservedReader>(nested_fat_fixture());
    auto media = axk::open_media(reader, "native.img");
    ASSERT_TRUE(media);
    auto decoded = axk::formats::a_series::Document::open(std::move(*media));
    ASSERT_TRUE(decoded);
    auto sources = axk::native::SourceSet::create({{"source-1", "native.img", reader, {}, {}}});
    ASSERT_TRUE(sources);
    const auto reads = reader->bytes_read;
    auto document = axk::formats::a_series::NativeDocument::create(*decoded, *sources, "document-1");
    ASSERT_TRUE(document);
    EXPECT_EQ((*document)->decoded(), *decoded);
    EXPECT_EQ((*document)->sources(), *sources);
    EXPECT_EQ((*document)->summary().format_id, "yamaha.a-series");
    EXPECT_EQ((*document)->catalog().objects.size(), (*decoded)->state().inventory.catalog.objects.size());
    EXPECT_EQ((*document)->catalog().relationships.size(), (*decoded)->state().graph.relationships.size());
    decoded->reset();
    std::shared_ptr<const axk::native::Document> generic = *document;
    ASSERT_TRUE(generic->objects(0U, 1U));
    EXPECT_EQ(reader->bytes_read, reads);
}
