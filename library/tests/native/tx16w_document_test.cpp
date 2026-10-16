#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/formats/tx16w/document.hpp"

namespace {

void text(std::vector<std::byte> &bytes, std::size_t offset, std::string_view value) {
    for (std::size_t index = 0U; index < value.size(); ++index)
        bytes.at(offset + index) = static_cast<std::byte>(value[index]);
}

std::vector<std::byte> file(std::size_t size) {
    std::vector<std::byte> bytes(size);
    text(bytes, 0U, "LM8953");
    return bytes;
}

std::vector<std::byte> wave() {
    auto bytes = file(100000U);
    bytes[22U] = std::byte{0xc9};
    bytes[23U] = std::byte{2U};
    bytes[24U] = std::byte{2U};
    bytes[32U] = std::byte{0x7f};
    bytes[33U] = std::byte{0xf0};
    bytes[34U] = std::byte{0x80};
    return bytes;
}

class CountingReader final : public axk::RandomAccessReader {
  public:
    explicit CountingReader(std::vector<std::byte> bytes) : reader_(std::move(bytes)) {}
    void replace(std::vector<std::byte> bytes) { reader_ = axk::MemoryReader{std::move(bytes)}; }
    std::uint64_t size() const noexcept override { return reader_.size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        bytes_read += bytes.size();
        return reader_.read_exact_at(offset, bytes);
    }
    mutable std::size_t bytes_read{};

  private:
    axk::MemoryReader reader_;
};

axk::native::SourceMember member(std::string id, std::vector<axk::filesystems::Entry> entries) {
    return {std::move(id),
            "Disk",
            std::make_shared<axk::MemoryReader>(std::vector<std::byte>{}),
            {{"fat", "FAT", std::move(entries)}}};
}

axk::filesystems::Entry entry(std::string name, std::vector<std::byte> bytes) {
    return {name, name, false, 0U, std::make_shared<axk::MemoryReader>(std::move(bytes))};
}

} // namespace

TEST(Tx16wDocument, BrowsesWaveHeadersWithoutReadingPcmAndKeepsDuplicateOccurrences) {
    auto counted = std::make_shared<CountingReader>(wave());
    const auto sources = axk::native::SourceSet::create(
        {member("one", {{"wave", "TONE.W01", false, 0U, counted}}), member("two", {entry("TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ((*opened)->native_objects().size(), 2U);
    EXPECT_EQ(counted->bytes_read, 32U);
    EXPECT_NE((*opened)->native_objects()[0U].id, (*opened)->native_objects()[1U].id);
    const auto decoded = (*opened)->read_wave((*opened)->native_objects().front().id);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->pcm, (std::vector<std::int16_t>{32752, -32768}));
    EXPECT_EQ(counted->bytes_read, 67U);
    EXPECT_FALSE((*opened)->read_wave("missing"));
}

TEST(Tx16wDocument, WaveDisplayNamesExcludePathsWithoutChangingSourceLocationsOrLazyReads) {
    for (const auto *path : {"Tone.W01", "/Tone.W01", "/Folder/Tone.W01", "Folder\\Tone.W01"}) {
        SCOPED_TRACE(path);
        auto counted = std::make_shared<CountingReader>(wave());
        const auto sources = axk::native::SourceSet::create({member("one", {{"wave", path, false, 0U, counted}})});
        ASSERT_TRUE(sources);
        const auto opened = axk::tx16w::Document::open(*sources);
        ASSERT_TRUE(opened) << opened.error().message;
        ASSERT_EQ((*opened)->native_objects().size(), 1U);
        const auto &object = (*opened)->native_objects().front();
        const auto *header = std::get_if<axk::tx16w::WaveHeader>(&object.value);
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->name, "Tone.W01");
        EXPECT_EQ(header->native_slot, std::optional<std::uint8_t>{0U});
        EXPECT_EQ((*opened)->catalog().objects.front().name, "Tone.W01");
        EXPECT_EQ((*opened)->catalog().nodes.back().name, "Tone.W01");
        EXPECT_EQ(object.id, "tx16w:3:one:3:fat:4:wave:0");
        EXPECT_EQ(object.location.member_id, "one");
        EXPECT_EQ(object.location.root_id, "fat");
        EXPECT_EQ(object.location.entry_id, "wave");
        EXPECT_EQ(object.location.offset, 0U);
        EXPECT_EQ(object.location.length, 100000U);
        EXPECT_EQ((*sources)->members().front().roots.front().entries.front().path, path);
        EXPECT_EQ(counted->bytes_read, 32U);
        const auto frames = (*opened)->read_wave_frames(object.id, 0U, 2U);
        ASSERT_TRUE(frames) << frames.error().message;
        EXPECT_EQ(*frames, (std::vector<std::int16_t>{32752, -32768}));
        const auto decoded = (*opened)->read_wave(object.id);
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded->name, header->name);
        EXPECT_EQ(decoded->source_member, "one");
        EXPECT_EQ(decoded->pcm, *frames);
    }
}

TEST(Tx16wDocument, PreservesEveryInitializedPerformanceSlotRegardlessOfNameOrContent) {
    auto performances = file(5120U);
    for (std::size_t slot = 0U; slot < 32U; ++slot)
        text(performances, 16U + slot * 146U + 0x78U, "init performance");
    performances[16U + 12U * 146U + 66U] = std::byte{99U};
    const auto sources = axk::native::SourceSet::create({member("one", {entry("/SET.U01", std::move(performances))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ((*opened)->native_objects().size(), 32U);
    ASSERT_EQ((*opened)->catalog().objects.size(), 32U);
    std::set<std::string> ids;
    for (std::size_t slot = 0U; slot < 32U; ++slot) {
        const auto &summary = (*opened)->catalog().objects[slot];
        EXPECT_EQ(summary.name, "init performance");
        EXPECT_TRUE(ids.insert(summary.id).second);
        const auto object = (*opened)->object(summary.id);
        ASSERT_TRUE(object);
        const auto *performance = std::get_if<axk::tx16w::Performance>(&(*object)->value);
        ASSERT_NE(performance, nullptr);
        EXPECT_EQ(performance->slot, static_cast<std::uint8_t>(slot));
        EXPECT_EQ(performance->name, summary.name);
        EXPECT_EQ(performance->voices.front().volume, static_cast<std::uint8_t>(slot == 12U ? 99U : 0U));
        EXPECT_EQ((*object)->location.entry_id, "/SET.U01");
        EXPECT_EQ((*object)->location.offset, 16U + slot * 146U);
        EXPECT_EQ((*object)->location.length, 146U);
    }
}

TEST(Tx16wDocument, KeepsAnUnmatchedSetupAndItsMissingNativeReferencesBrowseable) {
    auto setup = file(1536U);
    text(setup, 0xf0U + 7U, "MISSING");
    const auto sources = axk::native::SourceSet::create({member("one", {entry("SET.S01", std::move(setup))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_EQ((*opened)->summary().completeness, axk::native::Completeness::incomplete);
    ASSERT_EQ((*opened)->native_objects().size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::tx16w::NativeSetup>((*opened)->native_objects().front().value));
    EXPECT_TRUE(std::ranges::any_of((*opened)->catalog().relationships, [](const auto &relationship) {
        return relationship.type == "wave-reference" && relationship.resolution == axk::native::Resolution::missing;
    }));
    EXPECT_TRUE(std::ranges::none_of((*opened)->catalog().objects,
                                     [](const auto &object) { return object.type == "program"; }));
}

TEST(Tx16wDocument, DoesNotResolveIdenticalWaveFilesByContentOrMemberOrder) {
    auto setup = file(1536U);
    text(setup, 0xf0U + 7U, "TONE");
    const auto sources =
        axk::native::SourceSet::create({member("one", {entry("SET.S01", std::move(setup)), entry("/TONE.W01", wave())}),
                                        member("two", {entry("/Folder/TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    const auto &relationships = (*opened)->catalog().relationships;
    const auto reference = std::ranges::find(relationships, "wave-reference", &axk::native::Relationship::type);
    ASSERT_NE(reference, relationships.end());
    EXPECT_EQ(reference->resolution, axk::native::Resolution::ambiguous);
    EXPECT_FALSE(reference->target_id);
    ASSERT_EQ(reference->candidates.size(), 2U);
    EXPECT_NE(reference->candidates.front(), reference->candidates.back());
    for (const auto &id : reference->candidates) {
        const auto summary = std::ranges::find((*opened)->catalog().objects, id, &axk::native::ObjectSummary::id);
        ASSERT_NE(summary, (*opened)->catalog().objects.end());
        EXPECT_EQ(summary->name, "TONE.W01");
    }
}

TEST(Tx16wDocument, IgnoresExtensionOnlyMatchesAndReportsMalformedNativeFiles) {
    const auto false_sources =
        axk::native::SourceSet::create({member("one", {entry("TEXT.W01", std::vector<std::byte>(32U))})});
    ASSERT_TRUE(false_sources);
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(axk::tx16w::provider()));
    const auto unrecognized = registry.open(*false_sources);
    ASSERT_TRUE(unrecognized);
    EXPECT_TRUE(unrecognized->documents.empty());
    const auto malformed_sources = axk::native::SourceSet::create({member("one", {entry("BAD.W01", file(32U))})});
    ASSERT_TRUE(malformed_sources);
    const auto malformed = registry.open(*malformed_sources);
    ASSERT_TRUE(malformed);
    ASSERT_EQ(malformed->documents.size(), 1U);
    EXPECT_EQ(malformed->documents.front()->summary().completeness, axk::native::Completeness::invalid);
    EXPECT_FALSE(malformed->documents.front()->catalog().diagnostics.empty());
}

TEST(Tx16wDocument, DoesNotTreatAnEmptyVoiceCompanionAsAMissingFile) {
    const auto sources =
        axk::native::SourceSet::create({member("one", {entry("SET.S01", file(1536U)), entry("SET.V01", file(8704U))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_EQ((*opened)->summary().completeness, axk::native::Completeness::complete);
}

TEST(Tx16wDocument, WarningsAndInternalReferencesDoNotImplyMissingMedia) {
    auto voices = file(8704U);
    text(voices, 0x1250U + 46U, "Timbre");
    voices[0x1250U] = std::byte{63U};
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("SET.S01", file(1536U)), entry("SET.V01", std::move(voices)), entry("SET.F01", file(32U)),
                        entry("TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_EQ((*opened)->summary().completeness, axk::native::Completeness::complete);
    EXPECT_FALSE((*opened)->catalog().diagnostics.empty());
    EXPECT_TRUE((*opened)->summary().source_requirements.empty());
    EXPECT_TRUE(std::ranges::any_of((*opened)->catalog().relationships, [](const auto &relationship) {
        return relationship.type == "timbre-wave" && relationship.resolution == axk::native::Resolution::missing;
    }));
}

TEST(Tx16wDocument, ReportsPresentUnsupportedWaveSeparatelyFromMissingCompanions) {
    auto setup = file(1536U);
    text(setup, 0xf0U + 7U, "TONE");
    auto compressed = std::vector<std::byte>(32U);
    text(compressed, 0U, "FORM");
    text(compressed, 8U, "AIFC");
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("SET.S01", std::move(setup)), entry("SET.V01", file(8704U)),
                        entry("TONE.C01", std::move(compressed))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_EQ((*opened)->summary().completeness, axk::native::Completeness::incomplete);
    ASSERT_EQ((*opened)->summary().source_requirements.size(), 1U);
    EXPECT_EQ((*opened)->summary().source_requirements.front().state, axk::native::DependencyState::unsupported);
    EXPECT_EQ((*opened)->summary().source_requirements.front().name, "TONE.W01");
    EXPECT_TRUE(std::ranges::any_of((*opened)->catalog().objects, [](const auto &object) {
        return object.name == "TONE.C01" && object.type == "native-file";
    }));
}

TEST(Tx16wDocument, DiscoveryGroupsOnlyUniqueCompanionsAndKeepsIndependentSetupsSeparate) {
    auto first = file(1536U);
    text(first, 0xf0U + 7U, "TONE");
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("TONE.W01", wave()), entry("NOTES.S01", std::vector<std::byte>(32U))}),
         member("two", {entry("SET.S01", std::move(first)), entry("SET.V01", file(8704U))}),
         member("three", {entry("SET.S01", file(1536U)), entry("SET.V01", file(8704U))})});
    ASSERT_TRUE(sources);
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(axk::tx16w::provider()));
    const auto opened = registry.open(*sources);
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->documents.size(), 2U);
    const auto &joined = opened->documents.front();
    EXPECT_EQ(joined->summary().id, "tx16w-one");
    EXPECT_EQ(joined->sources(), *sources);
    EXPECT_EQ(joined->catalog().objects.size(), 2U);
    EXPECT_EQ(joined->summary().completeness, axk::native::Completeness::complete);
    EXPECT_EQ(opened->documents.back()->summary().id, "tx16w-three");
}

TEST(Tx16wDocument, UnsupportedAudioDoesNotResolveNativeReferencesOrCompeteWithNativeWaves) {
    auto setup = file(1536U);
    text(setup, 0xf0U + 16U + 7U, "TONE");
    auto compressed = std::vector<std::byte>(32U);
    text(compressed, 0U, "FORM");
    text(compressed, 8U, "AIFC");
    auto entries = std::vector{entry("SET.S01", std::move(setup)), entry("SET.V01", file(8704U)),
                               entry("TONE.C01", std::move(compressed))};
    auto sources = axk::native::SourceSet::create({member("one", entries)});
    ASSERT_TRUE(sources);
    auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    ASSERT_EQ((*opened)->summary().source_requirements.size(), 1U);
    EXPECT_EQ((*opened)->summary().source_requirements.front().state, axk::native::DependencyState::unsupported);
    EXPECT_EQ((*opened)->summary().source_requirements.front().name, "TONE.W02");
    EXPECT_EQ((*opened)->catalog().relationships.front().resolution, axk::native::Resolution::missing);

    entries.push_back(entry("TONE.W02", wave()));
    sources = axk::native::SourceSet::create({member("one", std::move(entries))});
    ASSERT_TRUE(sources);
    opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_TRUE((*opened)->summary().source_requirements.empty());
}

TEST(Tx16wDocument, DoesNotSilentlyReadWavePcmUnderChangedMetadata) {
    auto reader = std::make_shared<CountingReader>(wave());
    const auto sources = axk::native::SourceSet::create({member("one", {{"wave", "TONE.W01", false, 0U, reader}})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    auto changed = wave();
    changed[23U] = std::byte{3U};
    reader->replace(std::move(changed));
    const auto read = (*opened)->read_wave((*opened)->native_objects().front().id);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, axk::ErrorCode::transaction_stale);
}

TEST(Tx16wDocument, DiscoveryPreservesAmbiguousSuppliersWithoutMergingThem) {
    auto setup = file(1536U);
    text(setup, 0xf7U, "TONE");
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("SET.S01", std::move(setup)), entry("SET.V01", file(8704U))}),
         member("two", {entry("TONE.W01", wave())}), member("three", {entry("TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(axk::tx16w::provider()));
    const auto opened = registry.open(*sources);
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 3U);
    const auto &target = opened->documents.front();
    ASSERT_EQ(target->summary().source_requirements.size(), 1U);
    EXPECT_EQ(target->summary().source_requirements.front().state, axk::native::DependencyState::ambiguous);
    EXPECT_EQ(target->catalog().objects.size(), 1U);
}

TEST(Tx16wDocument, ExistingReferencedFileWithDamagedMagicIsInvalidNotMissing) {
    auto setup = file(1536U);
    text(setup, 0xf7U, "TONE");
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("SET.S01", std::move(setup)), entry("SET.V01", file(8704U))}),
         member("two", {entry("TONE.W01", std::vector<std::byte>(35U))})});
    ASSERT_TRUE(sources);
    axk::native::FormatRegistry registry;
    ASSERT_TRUE(registry.add(axk::tx16w::provider()));
    const auto opened = registry.open(*sources);
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 1U);
    const auto &document = opened->documents.front();
    ASSERT_EQ(document->summary().source_requirements.size(), 1U);
    EXPECT_EQ(document->summary().source_requirements.front().state, axk::native::DependencyState::invalid);
    EXPECT_EQ(document->summary().completeness, axk::native::Completeness::invalid);
}

TEST(Tx16wDocument, KeepsOrphanedEmptyNativeFilesExplicitlyIncomplete) {
    const auto sources =
        axk::native::SourceSet::create({member("one", {entry("SET.V01", file(8704U)), entry("SET.U01", file(5120U))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    EXPECT_EQ((*opened)->summary().completeness, axk::native::Completeness::incomplete);
    EXPECT_FALSE((*opened)->catalog().diagnostics.empty());
}

TEST(Tx16wDocument, DoesNotDiscardAnUnresolvedAlternativeSetupWhenResolvingATimbre) {
    auto first = file(1536U);
    text(first, 0xf0U + 7U, "TONE");
    auto second = file(1536U);
    text(second, 0xf0U + 7U, "ABSENT");
    auto voices = file(8704U);
    text(voices, 0x1250U + 46U, "Timbre");
    const auto sources =
        axk::native::SourceSet::create({member("one", {entry("SET.S01", std::move(first)),
                                                       entry("SET.V01", std::move(voices)), entry("TONE.W01", wave())}),
                                        member("two", {entry("SET.S01", std::move(second))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    const auto &relationships = (*opened)->catalog().relationships;
    const auto owner = std::ranges::find(relationships, "companion-setup", &axk::native::Relationship::type);
    ASSERT_NE(owner, relationships.end());
    EXPECT_EQ(owner->resolution, axk::native::Resolution::ambiguous);
    EXPECT_EQ(owner->candidates.size(), 2U);
    const auto target = std::ranges::find(relationships, "timbre-wave", &axk::native::Relationship::type);
    ASSERT_NE(target, relationships.end());
    EXPECT_NE(target->resolution, axk::native::Resolution::resolved);
    EXPECT_FALSE(target->target_id);
}

TEST(Tx16wDocument, DoesNotDiscardAnEmptyAlternativeVoiceFileWhenResolvingAPerformance) {
    auto voices = file(8704U);
    text(voices, 0x10U + 0x86U, "Voice");
    auto performances = file(5120U);
    text(performances, 0x10U + 0x78U, "Performance");
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("SET.S01", file(1536U)), entry("SET.V01", std::move(voices)),
                        entry("SET.U01", std::move(performances))}),
         member("two", {entry("SET.V01", file(8704U))})});
    ASSERT_TRUE(sources);
    const auto opened = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(opened);
    const auto &relationships = (*opened)->catalog().relationships;
    const auto target = std::ranges::find(relationships, "performance-voice", &axk::native::Relationship::type);
    ASSERT_NE(target, relationships.end());
    EXPECT_EQ(target->resolution, axk::native::Resolution::ambiguous);
    EXPECT_FALSE(target->target_id);
    EXPECT_EQ(target->candidates.size(), 1U);
}
