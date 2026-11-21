#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "axklib/formats/typhoon/companions.hpp"
#include <gtest/gtest.h>

namespace {
std::string be(std::uint32_t word) {
    std::string data;
    for (const unsigned shift : {24U, 16U, 8U, 0U})
        data += static_cast<char>((word >> shift) & 255U);
    return data;
}
std::string chunk(const std::string &name, std::string payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    if (n % 2U)
        payload += '\0';
    return name + be(n) + payload;
}
std::string reference(const std::string &tag, std::string name, std::uint32_t version, std::string disk) {
    name.resize(8, '\0');
    disk.resize(8, '\0');
    return chunk(tag, name + be(version) + disk);
}
axk::native::SourceMember member(std::string disk, std::string filename, std::string form, std::uint32_t version,
                                 std::string fields) {
    const auto file = chunk("FORM", form + chunk("VInf", std::string(12, '\0') + be(version)) + fields);
    std::vector<std::byte> bytes;
    for (const auto c : file)
        bytes.push_back(static_cast<std::byte>(c));
    auto r = std::make_shared<axk::MemoryReader>(std::move(bytes));
    return {disk, disk, r, {{"root", "FAT12", {{"slot", filename, false, 0U, r}}, disk}}};
}
auto open(std::vector<axk::native::SourceMember> members) {
    return axk::typhoon::Document::open(axk::native::SourceSet::create(std::move(members)).value()).value();
}
} // namespace
TEST(TyphoonDocument, FollowsVersionedReferencesRecursivelyAndNeverBreaksATieByFilename) {
    const auto setup = member("FIRST", "ALL.X01", "TYPS", 1, reference("Perf", "CONCERT", 11, "SECOND"));
    const auto perf = member("SECOND", "CONCERT.P01", "TYPP", 11, chunk("Entr", reference("Voic", "PIANO", 19, "")));
    const auto voice = member("THIRD", "PIANO.O01", "TYPV", 19, {});
    const auto alternate = member("FOURTH", "PIANO.O02", "TYPV", 19, {});
    const auto wrong = member("WRONG", "PIANO.O01", "TYPV", 18, {});
    auto seed = open({setup});
    std::vector docs{open({wrong}), open({voice}), open({perf})};
    const auto group = axk::typhoon::find_companions(*seed, docs);
    ASSERT_TRUE(group);
    EXPECT_EQ(group->candidates, (std::vector<std::size_t>{2, 1}));
    docs.push_back(open({alternate}));
    auto tied = axk::typhoon::find_companions(*seed, docs);
    ASSERT_TRUE(tied);
    EXPECT_EQ(tied->candidates, (std::vector<std::size_t>{2}));
    EXPECT_EQ(tied->ambiguous.size(), 1U);
    auto manual = open({setup, perf, voice});
    EXPECT_EQ(manual->summary().completeness, axk::native::Completeness::complete);
    EXPECT_EQ(manual->catalog().relationships.size(), 2U);
    auto ambiguous = open({setup, perf, voice, alternate});
    EXPECT_EQ(std::ranges::count(ambiguous->catalog().relationships, axk::native::Resolution::ambiguous,
                                 &axk::native::Relationship::resolution),
              1);
}

TEST(TyphoonDocument, ScopesTheCatalogWithoutReplacingTheAdmittedSources) {
    const auto sources =
        axk::native::SourceSet::create({
                                           member("A", "ALL.X01", "TYPS", 1, reference("Voic", "PIANO", 8, "")),
                                           member("B", "PIANO.O02", "TYPV", 8, {}),
                                       })
            .value();
    const auto scoped = axk::typhoon::Document::open(sources, {}, std::vector<std::string>{"A"}).value();
    EXPECT_EQ(scoped->sources(), sources);
    ASSERT_EQ(scoped->catalog().objects.size(), 1U);
    ASSERT_EQ(scoped->catalog().relationships.size(), 1U);
    EXPECT_EQ(scoped->catalog().relationships.front().resolution, axk::native::Resolution::missing);
    const auto all = axk::typhoon::Document::open(sources).value();
    EXPECT_EQ(all->catalog().relationships.front().resolution, axk::native::Resolution::resolved);
    EXPECT_NE(all->find(scoped->catalog().objects.front().id), nullptr);
}

TEST(TyphoonDocument, KeepsVelocityLayersAndClosesTheLastRegionAtAnEmptySplit) {
    std::string parm(64, 0);
    parm[0] = 40;
    parm[1] = 90;
    parm[2] = 20;
    parm[3] = 100;
    auto split = [](char key) { return chunk("Parm", std::string{key, 'x'}); };
    const auto group = chunk("Parm", parm) + chunk("Splt", split(88) + reference("Wave", "FIRST", 1, "DISK")) +
                       chunk("Splt", split(55) + reference("Wave", "NEXT", 2, "DISK")) + chunk("Splt", split(70));
    auto doc = open({member("DISK", "PIANO.O01", "TYPV", 7, chunk("Grop", group))});
    const auto *voice = doc->find(doc->catalog().objects.front().id);
    ASSERT_TRUE(voice);
    EXPECT_TRUE(voice->mapping_error.empty());
    ASSERT_EQ(voice->regions.size(), 2U);
    EXPECT_EQ(voice->regions[0].midi_low, 52U);
    EXPECT_EQ(voice->regions[0].midi_high, 66U);
    EXPECT_EQ(voice->regions[1].midi_low, 67U);
    EXPECT_EQ(voice->regions[1].midi_high, 81U);
    EXPECT_EQ(voice->regions[1].velocity_low, 20U);
    EXPECT_EQ(voice->regions[1].velocity_high, 100U);
    EXPECT_FALSE(voice->references[1].target);
}

TEST(TyphoonDocument, DistinguishesEmptySplitsFromMissingWaveAndUnknownLayout) {
    const auto group = chunk("Parm", std::string(64, 0)) + chunk("Splt", {});
    auto empty = open({member("A", "NOT-INIT.O01", "TYPV", 1, chunk("Grop", group))});
    auto missing = open({member(
        "A", "INIT.O01", "TYPV", 1,
        chunk("Grop", chunk("Parm", std::string(64, 0)) + chunk("Splt", reference("Wave", "NO WAVE", 4, "B"))))});
    auto unknown = open({member("A", "INIT.O01", "TYPV", 1, {})});
    const auto get = [](const auto &doc) { return doc->find(doc->catalog().objects.at(0).id); };
    EXPECT_TRUE(get(empty)->has_split_table);
    EXPECT_TRUE(get(empty)->references.empty());
    EXPECT_TRUE(get(missing)->has_split_table);
    ASSERT_EQ(get(missing)->references.size(), 1U);
    EXPECT_FALSE(get(missing)->references.front().target);
    EXPECT_FALSE(get(unknown)->has_split_table);
}

TEST(TyphoonDocument, UsesDosSpellingWithoutRelaxingVersionDiskOrUniqueness) {
    const auto setup = member("SEED", "ALL.X01", "TYPS", 3, reference("Voic", "A.B/C+,", 9, "PART"));
    const auto correct = member("PART", "A^B@C$~.O01", "TYPV", 9, {});
    const auto wrong_version = member("PART", "A^B@C$~.O01", "TYPV", 8, {});
    const auto wrong_disk = member("ELSE", "A^B@C$~.O01", "TYPV", 9, {});
    const auto wrong_name = member("PART", "OTHER.O01", "TYPV", 9, {});
    auto seed = open({setup});
    std::vector neighbors{open({wrong_version}), open({wrong_disk}), open({wrong_name}), open({correct})};
    const auto group = axk::typhoon::find_companions(*seed, neighbors);
    ASSERT_TRUE(group);
    EXPECT_EQ(group->candidates, (std::vector<std::size_t>{3}));
    const auto resolved = open({setup, correct});
    const auto &edge = resolved->catalog().relationships.at(0);
    EXPECT_EQ(edge.resolution, axk::native::Resolution::resolved);
    EXPECT_NE(edge.native_reference.find("A.B/C+,"), std::string::npos);
    EXPECT_EQ(open({setup, wrong_version})->catalog().relationships.at(0).resolution, axk::native::Resolution::missing);
    neighbors.push_back(open({correct}));
    EXPECT_TRUE(axk::typhoon::find_companions(*seed, neighbors)->candidates.empty());
}
