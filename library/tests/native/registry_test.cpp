#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/native/registry.hpp"

namespace {

class TestDocument final : public axk::native::Document {
  public:
    TestDocument(std::string id, std::string format, std::shared_ptr<const axk::native::SourceSet> sources)
        : summary_{std::move(id), std::move(format), "test", "Test", axk::native::Completeness::complete, {}},
          sources_(std::move(sources)) {
        catalog_.objects.push_back({"one", "test", "First", {}, {}});
        catalog_.objects.push_back({"two", "test", "Second", {}, {}});
    }
    const axk::native::DocumentSummary &summary() const noexcept override { return summary_; }
    const axk::native::Catalog &catalog() const noexcept override { return catalog_; }
    const std::shared_ptr<const axk::native::SourceSet> &sources() const noexcept override { return sources_; }

  private:
    axk::native::DocumentSummary summary_;
    axk::native::Catalog catalog_;
    std::shared_ptr<const axk::native::SourceSet> sources_;
};

axk::native::FormatProvider provider(std::string format) {
    return {
        {format, format, {"test"}},
        [format](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
            return axk::native::Recognition{
                format, axk::native::RecognitionState::supported, {}, {{"member", "", "", 0U, 16U}}};
        },
        [format](auto sources, const auto &) -> axk::Result<std::vector<std::shared_ptr<const axk::native::Document>>> {
            return std::vector<std::shared_ptr<const axk::native::Document>>{
                std::make_shared<TestDocument>(format + "-first", format, sources),
                std::make_shared<TestDocument>(format + "-second", format, sources)};
        }};
}

std::shared_ptr<const axk::native::SourceSet> sources() {
    return *axk::native::SourceSet::create(
        {{"member", "disk", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(16U)), {}}});
}

} // namespace

TEST(NativeRegistry, RejectsDuplicateAndIncompleteProviders) {
    axk::native::FormatRegistry registry;
    EXPECT_FALSE(registry.add({}));
    ASSERT_TRUE(registry.add(provider("first")));
    EXPECT_FALSE(registry.add(provider("first")));
    EXPECT_EQ(registry.formats().size(), 1U);
}

TEST(NativeRegistry, SupportsSeveralDocumentsWithoutEquatingThemWithSourceMembers) {
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(provider("first")));
    const auto opened = registry.open(sources());
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->documents.size(), 2U);
    EXPECT_FALSE(opened->selection_required);
    const auto page = opened->documents.front()->objects(1U, 1U);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->total, 2U);
    ASSERT_EQ(page->items.size(), 1U);
    EXPECT_EQ(page->items.front().id, "two");
    EXPECT_FALSE(opened->documents.front()->objects(0U, 0U));
    EXPECT_FALSE(opened->documents.front()->objects(0U, 1025U));
    const auto beyond = opened->documents.front()->objects(std::numeric_limits<std::size_t>::max(), 1U);
    ASSERT_TRUE(beyond);
    EXPECT_TRUE(beyond->items.empty());
    EXPECT_EQ(beyond->total, 2U);
}

TEST(NativeRegistry, DoesNotUseRegistrationOrderToResolveAmbiguity) {
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(provider("first")));
    ASSERT_TRUE(registry.add(provider("second")));
    const auto ambiguous = registry.open(sources());
    ASSERT_TRUE(ambiguous);
    EXPECT_TRUE(ambiguous->selection_required);
    EXPECT_TRUE(ambiguous->documents.empty());
    const auto selected = registry.open(sources(), {"second"});
    ASSERT_TRUE(selected);
    ASSERT_EQ(selected->documents.size(), 2U);
    EXPECT_EQ(selected->documents.front()->summary().format_id, "second");
    EXPECT_FALSE(registry.open(sources(), {"missing"}));
}

TEST(NativeRegistry, ChecksCancellationAndKeepsUnknownSourcesBrowseable) {
    axk::native::FormatRegistry registry;
    auto opened = registry.open(sources());
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened->documents.empty());
    axk::CancellationSource cancelled;
    cancelled.cancel();
    EXPECT_FALSE(registry.open(sources(), {}, cancelled.token()));
}

TEST(NativeRegistry, OpensDisjointClaimsButRequiresSelectionForOverlappingClaims) {
    const auto scoped = [](std::string id, std::uint64_t offset) {
        auto result = provider(id);
        result.recognize = [id, offset](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
            return axk::native::Recognition{
                id, axk::native::RecognitionState::supported, {}, {{"member", "", "", offset, 8U}}};
        };
        return result;
    };
    axk::native::FormatRegistry disjoint;
    ASSERT_TRUE(disjoint.add(scoped("first", 0U)));
    ASSERT_TRUE(disjoint.add(scoped("second", 8U)));
    auto opened = disjoint.open(sources());
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_FALSE(opened->selection_required);
    EXPECT_EQ(opened->documents.size(), 4U);
    opened = disjoint.open(sources(), {"first"});
    ASSERT_TRUE(opened);
    EXPECT_FALSE(opened->selection_required);
    EXPECT_EQ(opened->documents.size(), 4U);
    ASSERT_TRUE(disjoint.add(scoped("alternative", 0U)));
    opened = disjoint.open(sources(), {"alternative"});
    ASSERT_TRUE(opened);
    EXPECT_FALSE(opened->selection_required);
    ASSERT_EQ(opened->documents.size(), 4U);
    EXPECT_EQ(opened->documents.front()->summary().format_id, "second");
    EXPECT_EQ(opened->documents.back()->summary().format_id, "alternative");
    opened = disjoint.open(sources(), {"second"});
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened->selection_required);
    EXPECT_TRUE(opened->documents.empty());

    axk::native::FormatRegistry overlap;
    ASSERT_TRUE(overlap.add(scoped("first", 0U)));
    ASSERT_TRUE(overlap.add(scoped("second", 7U)));
    opened = overlap.open(sources());
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened->selection_required);
    EXPECT_TRUE(opened->documents.empty());
}

TEST(NativeRegistry, RejectsAbsentAndOutOfBoundsRecognitionClaims) {
    for (const auto &claims : std::vector<std::vector<axk::native::SourceLocation>>{
             {},
             {{"missing", "", "", 0U, 1U}},
             {{"member", "", "", 15U, 2U}},
             {{"member", "", "", std::numeric_limits<std::uint64_t>::max(), 2U}}}) {
        axk::native::FormatRegistry registry;
        auto item = provider("first");
        item.recognize = [claims](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
            return axk::native::Recognition{"first", axk::native::RecognitionState::supported, {}, claims};
        };
        ASSERT_TRUE(registry.add(std::move(item)));
        EXPECT_FALSE(registry.open(sources()));
    }
}

TEST(NativeRegistry, ResolvesIndependentAmbiguitiesWithNonconflictingSelections) {
    axk::native::FormatRegistry registry;
    for (const auto &[id, offset] : std::vector<std::pair<std::string, std::uint64_t>>{
             {"first", 0U}, {"first-alternative", 0U}, {"second", 8U}, {"second-alternative", 8U}}) {
        auto item = provider(id);
        item.recognize = [id, offset](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
            return axk::native::Recognition{
                id, axk::native::RecognitionState::supported, {}, {{"member", "", "", offset, 8U}}};
        };
        ASSERT_TRUE(registry.add(std::move(item)));
    }
    const auto unresolved = registry.open(sources(), {"first"});
    ASSERT_TRUE(unresolved);
    EXPECT_TRUE(unresolved->selection_required);
    EXPECT_TRUE(unresolved->documents.empty());
    const auto selected = registry.open(sources(), {"first", "second"});
    ASSERT_TRUE(selected);
    EXPECT_FALSE(selected->selection_required);
    ASSERT_EQ(selected->documents.size(), 4U);
    EXPECT_EQ(selected->documents.front()->summary().format_id, "first");
    EXPECT_EQ(selected->documents.back()->summary().format_id, "second");
    EXPECT_FALSE(registry.open(sources(), {"first", "first-alternative"}));
    EXPECT_FALSE(registry.open(sources(), {"first", "first"}));
    EXPECT_FALSE(registry.open(sources(), {"first", "missing"}));
}

TEST(NativeRegistry, RejectsForeignRecognitionIdentityWithoutOpening) {
    axk::native::FormatRegistry registry;
    auto registered = provider("first");
    bool opened{};
    registered.recognize = [](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
        return axk::native::Recognition{"different", axk::native::RecognitionState::supported, {}};
    };
    registered.open =
        [&opened](auto, const auto &) -> axk::Result<std::vector<std::shared_ptr<const axk::native::Document>>> {
        opened = true;
        return std::vector<std::shared_ptr<const axk::native::Document>>{};
    };
    ASSERT_TRUE(registry.add(std::move(registered)));
    EXPECT_FALSE(registry.open(sources()));
    EXPECT_FALSE(opened);
}

TEST(NativeRegistry, RejectsDocumentsThatViolateProviderIdentityOrSourceOwnership) {
    using Documents = std::vector<std::shared_ptr<const axk::native::Document>>;
    const auto admitted = sources();
    const auto foreign = sources();
    const std::vector<Documents> invalid{
        {},
        {nullptr},
        {std::make_shared<TestDocument>("first", "format", foreign)},
        {std::make_shared<TestDocument>("", "format", admitted)},
        {std::make_shared<TestDocument>("first", "different", admitted)},
        {std::make_shared<TestDocument>("same", "format", admitted),
         std::make_shared<TestDocument>("same", "format", admitted)},
    };
    for (std::size_t index = 0U; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        axk::native::FormatRegistry registry;
        auto registered = provider("format");
        registered.open = [documents = invalid[index]](auto, const auto &) -> axk::Result<Documents> {
            return documents;
        };
        ASSERT_TRUE(registry.add(std::move(registered)));
        EXPECT_FALSE(registry.open(admitted));
    }
}

TEST(NativeRegistry, RejectsUndeclaredDocumentProfiles) {
    axk::native::FormatRegistry registry;
    auto registered = provider("format");
    registered.descriptor.profiles = {"other-profile"};
    ASSERT_TRUE(registry.add(std::move(registered)));
    EXPECT_FALSE(registry.open(sources()));
}

TEST(NativeRegistry, RetainsInvalidRecognitionWithoutCallingItsOpener) {
    axk::native::FormatRegistry registry;
    auto registered = provider("format");
    registered.recognize = [](const auto &, const auto &) -> axk::Result<axk::native::Recognition> {
        return axk::native::Recognition{"format", axk::native::RecognitionState::invalid, {}};
    };
    ASSERT_TRUE(registry.add(std::move(registered)));
    const auto result = registry.open(sources());
    ASSERT_TRUE(result);
    ASSERT_EQ(result->candidates.size(), 1U);
    EXPECT_EQ(result->candidates.front().state, axk::native::RecognitionState::invalid);
    EXPECT_TRUE(result->documents.empty());
    EXPECT_FALSE(registry.open(sources(), {"format"}));
}
