#include "fixture_sampler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "session_formats.hpp"

namespace fixture {
namespace {
using namespace axk;

constexpr char format_id[] = "fixture.instrument";
constexpr std::array magic{std::byte{'F'}, std::byte{'X'}, std::byte{'0'}, std::byte{'1'}};

class FixtureDocument final : public native::Document {
  public:
    FixtureDocument(std::shared_ptr<const native::SourceSet> sources, native::SourceLocation location, std::string name)
        : sources_(std::move(sources)), summary_{location.member_id + ":" + location.entry_id,
                                                 format_id,
                                                 "native",
                                                 name,
                                                 native::Completeness::complete,
                                                 {}} {
        catalog_.objects.push_back({summary_.id + ":0", "tone", std::move(name), {std::move(location)}, {}});
    }
    const native::DocumentSummary &summary() const noexcept override { return summary_; }
    const native::Catalog &catalog() const noexcept override { return catalog_; }
    const std::shared_ptr<const native::SourceSet> &sources() const noexcept override { return sources_; }

  private:
    std::shared_ptr<const native::SourceSet> sources_;
    native::DocumentSummary summary_;
    native::Catalog catalog_;
};

class FixtureState final : public app::NativeDocumentState {
  public:
    explicit FixtureState(std::shared_ptr<const native::Document> document) {
        native_document = std::move(document);
        const auto &source = native_document->catalog().objects.front();
        app::ImageObjectItem item{};
        item.id = source.id;
        item.name = source.name;
        item.type = source.type;
        item.format = format_id();
        item.stored_size_bytes = source.locations.front().length;
        native_object_ids.emplace(item.id, item.id);
        object_indices_by_id.emplace(item.id, 0U);
        object_indices_by_type[item.type].push_back(0U);
        objects.push_back(std::move(item));
    }
    std::string_view format_id() const noexcept override { return fixture::format_id; }
    std::string_view profile() const noexcept override { return "native"; }
    std::vector<std::string> available_operations(bool, bool) const override { return {"images.objects"}; }
    app::NativeFilesystemBindings filesystem_bindings() const override { return {}; }
    app::Result<nlohmann::ordered_json> object_detail(const app::ImageDocumentContext &,
                                                      std::string_view) const override {
        return std::unexpected(app::Error{"native_format_unsupported", "Fixture has no typed detail"});
    }
    app::Result<void> preserve_identity(const app::NativeDocumentState &previous) override {
        if (previous.format_id() != format_id())
            return std::unexpected(app::Error{"native_format_changed", "Unexpected fixture format"});
        return {};
    }
};
} // namespace

app::Result<void> with_sampler(app::SessionFormats &formats, const app::SessionFormatContext &context) {
    if (auto defaults = app::configure_builtin_session_formats(formats, context); !defaults)
        return defaults;
    native::FormatProvider provider;
    provider.descriptor = {format_id, "Fixture sampler", {"native"}};
    using Documents = std::vector<std::shared_ptr<const native::Document>>;
    auto documents = std::make_shared<Documents>();
    provider.recognize = [documents, retained = context.sources.native_sources](
                             const native::SourceSet &sources,
                             const CancellationToken &token) -> Result<native::Recognition> {
        native::Recognition result{};
        result.format_id = format_id;
        for (const auto &member : sources.members())
            for (const auto &root : member.roots)
                for (const auto &entry : root.entries) {
                    if (entry.directory || !entry.reader || entry.reader->size() != 8U)
                        continue;
                    auto location = sources.locate(member.id, root.id, entry.id);
                    if (!location)
                        return std::unexpected(location.error());
                    auto bytes = sources.read(*location, token);
                    if (!bytes)
                        return std::unexpected(bytes.error());
                    if (!std::equal(magic.begin(), magic.end(), bytes->begin()))
                        continue;
                    const std::string name(reinterpret_cast<const char *>(bytes->data() + magic.size()), 4U);
                    documents->push_back(std::make_shared<FixtureDocument>(retained, *location, name));
                    result.claims.push_back(*location);
                    result.state = native::RecognitionState::supported;
                }
        return result;
    };
    provider.open = [documents](std::shared_ptr<const native::SourceSet>,
                                const CancellationToken &) -> Result<Documents> { return *documents; };
    return formats.add(std::move(provider),
                       [&sources = context.sources](const auto &document) -> app::Result<app::NativeDocumentAdmission> {
                           app::NativeDocumentAdmission admission;
                           admission.id = document->summary().id;
                           admission.format = format_id;
                           admission.member_ids = {document->catalog().objects.front().locations.front().member_id};
                           const auto member = std::ranges::find_if(
                               sources.members, [&](const auto &m) { return m->id == admission.member_ids.front(); });
                           if (member == sources.members.end())
                               return std::unexpected(app::Error{"document_invalid", "Missing fixture source"});
                           admission.primary = *member;
                           admission.native = std::make_unique<FixtureState>(document);
                           return admission;
                       });
}
} // namespace fixture
