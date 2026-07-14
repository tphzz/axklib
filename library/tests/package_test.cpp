#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/writer.hpp"

#include "../src/package_internal.hpp"
#include "../src/writer_internal.hpp"
#include "media_test_fixtures.hpp"

namespace {

std::filesystem::path fixture(std::string_view name) {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored" / name;
}

std::filesystem::path publication_root(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::filesystem::path{name};
}

axk::PackageRootSelector root(axk::PackageRootKind kind, std::string volume, std::string object = {}) {
    axk::PackageRootSelector result;
    result.kind = kind;
    result.partition_index = 0U;
    result.volume_name = std::move(volume);
    result.object_name = std::move(object);
    return result;
}

axk::PackageRootDestination destination(std::size_t package_index, std::string volume) {
    axk::PackageRootDestination result;
    result.package_index = package_index;
    result.root_index = 0U;
    result.partition_index = 0U;
    result.volume_name = std::move(volume);
    return result;
}

axk::PackageRootDestination destination(std::size_t package_index, std::uint8_t partition_index, std::string volume) {
    auto result = destination(package_index, std::move(volume));
    result.partition_index = partition_index;
    return result;
}

axk::Result<axk::PackageBuild> fat_smpl_package() {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    if (!image)
        return std::unexpected{image.error()};
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    return axk::build_portable_package(media, roots);
}

class CancellingPackageProgress final : public axk::ProgressSink {
  public:
    CancellingPackageProgress(axk::CancellationSource &source, std::uint64_t cancel_after)
        : source_(source), cancel_after_(cancel_after) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::writing && progress.completed == cancel_after_)
            source_.cancel();
    }

  private:
    axk::CancellationSource &source_;
    std::uint64_t cancel_after_{};
};

axk::Result<axk::PackageBuild> standalone_smpl_package(std::vector<std::byte> payload) {
    auto object =
        axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(std::move(payload)), "portable-sample.bin");
    if (!object)
        return std::unexpected{object.error()};
    const axk::MediaContainer media{std::move(*object)};
    axk::PackageRootSelector selector;
    selector.kind = axk::PackageRootKind::smpl;
    selector.volume_name = "Standalone object";
    selector.object_name = "TEST";
    selector.object_key = "standalone";
    const std::vector selectors{std::move(selector)};
    return axk::build_portable_package(media, selectors);
}

axk::VolumeSpec graph_volume(const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = "Graph Volume";
    volume.waveforms.push_back({"wave", "Graph Wave", audio_path, 60U, {}});
    axk::SampleBankSpec grouped;
    grouped.name = "Grouped Bank";
    grouped.waveform_id = "wave";
    grouped.root_key = 60U;
    grouped.key_high = 127U;
    volume.sample_banks.push_back(std::move(grouped));
    axk::SampleBankSpec direct;
    direct.name = "Direct Bank";
    direct.waveform_id = "wave";
    direct.root_key = 60U;
    direct.key_high = 127U;
    volume.sample_banks.push_back(std::move(direct));
    volume.sample_bank_groups.push_back({"Graph Group", {"Grouped Bank"}});
    volume.programs.push_back({1U, {{"SBAC", "Graph Group", 1U}, {"SBNK", "Direct Bank", 2U}}});
    return volume;
}

axk::VolumeSpec single_bank_volume(const std::filesystem::path &audio_path, std::string volume_name,
                                   std::string waveform_name, std::string bank_name) {
    axk::VolumeSpec volume;
    volume.name = std::move(volume_name);
    volume.waveforms.push_back({"wave", std::move(waveform_name), audio_path, 60U, {}});
    axk::SampleBankSpec bank;
    bank.name = std::move(bank_name);
    bank.waveform_id = "wave";
    bank.root_key = 60U;
    bank.key_high = 127U;
    volume.sample_banks.push_back(std::move(bank));
    return volume;
}

axk::Waveform tiny_waveform(std::int16_t peak) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm.resize(8U);
    axk::ByteWriter writer{waveform.pcm};
    static_cast<void>(writer.write_le16(2U, static_cast<std::uint16_t>(peak)));
    static_cast<void>(writer.write_le16(4U, static_cast<std::uint16_t>(-peak)));
    return waveform;
}

axk::Result<std::vector<axk::PortablePackage>> mixed_source_packages(const std::filesystem::path &root_path) {
    const auto graph_audio = root_path / "graph.wav";
    const auto iso_audio = root_path / "iso.wav";
    const auto sfs_path = root_path / "source.hds";
    const auto iso_path = root_path / "source.iso";
    if (const auto written = axk::write_wav_atomic(graph_audio, tiny_waveform(1000)); !written)
        return std::unexpected{written.error()};
    if (const auto written = axk::write_wav_atomic(iso_audio, tiny_waveform(2000)); !written)
        return std::unexpected{written.error()};

    axk::HdsBuildManifest sfs_manifest{"1.0", 4U * 1024U * 1024U, {}};
    sfs_manifest.partitions.push_back({"P1", {graph_volume(graph_audio)}});
    if (const auto written = axk::write_hds_image(sfs_manifest, sfs_path); !written)
        return std::unexpected{written.error()};
    auto sfs = axk::open_media(sfs_path);
    if (!sfs)
        return std::unexpected{sfs.error()};
    const std::vector program_root{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    auto program = axk::build_portable_package(*sfs, program_root);
    if (!program)
        return std::unexpected{program.error()};

    auto fat = fat_smpl_package();
    if (!fat)
        return std::unexpected{fat.error()};

    axk::MediaBuildManifest iso_manifest;
    iso_manifest.schema_version = "1.0";
    iso_manifest.format = axk::MediaImageFormat::iso9660;
    iso_manifest.iso_volume_id = "MIXED_SOURCE";
    iso_manifest.group_name = "ISO Group";
    iso_manifest.raw_group = "GROUP";
    iso_manifest.volume_name = "ISO Volume";
    iso_manifest.raw_volume = "F001";
    iso_manifest.authored_volume = single_bank_volume(iso_audio, "ISO Volume", "ISO Wave", "ISO Bank");
    if (const auto written = axk::write_media_image(iso_manifest, iso_path); !written)
        return std::unexpected{written.error()};
    auto iso = axk::open_media(iso_path);
    if (!iso)
        return std::unexpected{iso.error()};
    auto iso_catalog = axk::build_object_catalog(*iso);
    if (!iso_catalog)
        return std::unexpected{iso_catalog.error()};
    const auto bank = std::ranges::find_if(iso_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbnk && object.object.header.name == "ISO Bank" &&
               object.placement.has_value();
    });
    if (bank == iso_catalog->objects.end()) {
        return std::unexpected{axk::make_error(axk::ErrorCode::object_missing, axk::ErrorCategory::object,
                                               "generated ISO bank is missing")};
    }
    axk::PackageRootSelector iso_selector;
    iso_selector.kind = axk::PackageRootKind::sbnk;
    iso_selector.partition_index = bank->placement->partition.value;
    iso_selector.group_name = bank->placement->partition_name;
    iso_selector.volume_name = bank->placement->volume_name;
    iso_selector.object_name = bank->object.header.name;
    iso_selector.object_key = bank->key;
    const std::vector iso_roots{std::move(iso_selector)};
    auto iso_bank = axk::build_portable_package(*iso, iso_roots);
    if (!iso_bank)
        return std::unexpected{iso_bank.error()};

    std::vector<axk::PortablePackage> packages;
    packages.push_back(std::move(program->package));
    packages.push_back(std::move(fat->package));
    packages.push_back(std::move(iso_bank->package));
    return packages;
}

axk::Result<std::vector<std::string>> normalized_media_signature(const axk::MediaContainer &media) {
    auto objects = media.objects();
    if (!objects)
        return std::unexpected{objects.error()};
    std::vector<std::string> result;
    result.reserve(objects->size());
    for (const auto &object : *objects) {
        auto profile = axk::package_internal::build_relocation_profile(object.decoded, object.raw_payload);
        if (!profile)
            return std::unexpected{profile.error()};
        result.push_back(
            std::format("{}\0{}\0{}", object.decoded.header.raw_type, object.decoded.header.name,
                        axk::package_internal::hex_digest(axk::package_internal::sha256(profile->normalized_payload))));
    }
    std::ranges::sort(result);
    return result;
}

void expect_package_audio(const axk::MediaContainer &media, std::span<const axk::PortablePackage> packages) {
    const auto objects = media.objects();
    ASSERT_TRUE(objects) << objects.error().message;
    for (const auto &package : packages) {
        for (const auto &node : package.nodes) {
            if (node.object_type != "SMPL")
                continue;
            ASSERT_TRUE(node.audio_sha256);
            const auto object = std::ranges::find_if(*objects, [&](const auto &candidate) {
                return candidate.decoded.header.type == axk::ObjectType::smpl &&
                       candidate.decoded.header.name == node.name;
            });
            ASSERT_NE(object, objects->end()) << node.name;
            const auto waveform = axk::decode_waveform(*object);
            ASSERT_TRUE(waveform) << waveform.error().message;
            EXPECT_EQ(axk::package_internal::hex_digest(axk::package_internal::sha256(waveform->pcm)),
                      *node.audio_sha256)
                << node.name;
        }
    }
}

std::string conflict_summary(const axk::PackageImportPlan &plan) {
    std::string result;
    for (const auto &conflict : plan.conflicts) {
        result += std::format("{}: {} [package={}, node={}]\n", conflict.code, conflict.message,
                              conflict.package_index.value_or(0U), conflict.node_id);
    }
    return result;
}

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size < 0)
        return {};
    const auto byte_count = static_cast<std::streamsize>(size);
    std::vector<std::byte> result(static_cast<std::size_t>(byte_count));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(result.data()), byte_count);
    return result;
}

bool write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

nlohmann::json archive_manifest(std::span<const std::byte> archive) {
    const auto entries = axk::package_internal::read_archive(archive);
    if (!entries)
        return {};
    const auto manifest =
        std::ranges::find(*entries, std::string{"manifest.json"}, &axk::package_internal::ArchiveEntry::path);
    if (manifest == entries->end())
        return {};
    return nlohmann::json::parse(
        std::string_view{reinterpret_cast<const char *>(manifest->bytes.data()), manifest->bytes.size()});
}

std::set<std::string> object_keys(const nlohmann::json &value) {
    std::set<std::string> result;
    for (const auto &[key, unused] : value.items()) {
        static_cast<void>(unused);
        result.insert(key);
    }
    return result;
}

std::set<std::string> string_set(const nlohmann::json &value) {
    std::set<std::string> result;
    for (const auto &item : value)
        result.insert(item.get<std::string>());
    return result;
}

void expect_schema_object_shape(const nlohmann::json &schema, const nlohmann::json &value) {
    ASSERT_TRUE(value.is_object());
    ASSERT_EQ(schema.value("type", ""), "object");
    ASSERT_TRUE(schema.contains("required"));
    ASSERT_TRUE(schema.contains("properties"));
    EXPECT_EQ(object_keys(value), string_set(schema.at("required")));
    EXPECT_EQ(object_keys(schema.at("properties")), string_set(schema.at("required")));
    EXPECT_EQ(schema.value("additionalProperties", true), false);
}

#if defined(AXK_TEST_ISOINFO) || defined(AXK_TEST_XORRISO)
std::string shell_quote(const std::filesystem::path &path) {
    std::string value = path.string();
    std::string result{"'"};
    for (const char character : value)
        result += character == '\'' ? "'\\''" : std::string(1U, character);
    result += '\'';
    return result;
}
#endif

void expect_external_iso_tools_accept(const std::filesystem::path &path) {
    static_cast<void>(path);
#ifdef AXK_TEST_ISOINFO
    const auto isoinfo = shell_quote(AXK_TEST_ISOINFO) + " -i " + shell_quote(path) + " -f >/dev/null 2>&1";
    EXPECT_EQ(std::system(isoinfo.c_str()), 0) << "isoinfo rejected " << path;
#endif
#ifdef AXK_TEST_XORRISO
    const auto xorriso = shell_quote(AXK_TEST_XORRISO) + " -indev " + shell_quote(path) + " -toc >/dev/null 2>&1";
    EXPECT_EQ(std::system(xorriso.c_str()), 0) << "xorriso rejected " << path;
#endif
}

template <typename Mutation>
std::vector<std::byte> mutate_manifest(std::span<const std::byte> archive, Mutation mutation) {
    auto entries = axk::package_internal::read_archive(archive);
    if (!entries)
        return {};
    auto manifest =
        std::ranges::find(*entries, std::string{"manifest.json"}, &axk::package_internal::ArchiveEntry::path);
    if (manifest == entries->end())
        return {};
    const std::string text(reinterpret_cast<const char *>(manifest->bytes.data()), manifest->bytes.size());
    auto json = nlohmann::json::parse(text);
    mutation(json);
    json.erase("package_id");
    const auto identity = json.dump() + '\n';
    const auto source = std::as_bytes(std::span{identity});
    json["package_id"] = axk::package_internal::hex_digest(axk::package_internal::sha256(source));
    const auto output = json.dump() + '\n';
    const auto bytes = std::as_bytes(std::span{output});
    manifest->bytes.assign(bytes.begin(), bytes.end());
    const auto rewritten = axk::package_internal::write_archive(std::move(*entries));
    return rewritten ? std::move(*rewritten) : std::vector<std::byte>{};
}

} // namespace

TEST(PortablePackage, ExportsAndVerifiesTypedSmplFromFat12) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_EQ(built->package.kind, axk::PackageKind::smpl);
    EXPECT_EQ(built->required_extension, ".axksmpl");
    ASSERT_EQ(built->package.roots.size(), 1U);
    ASSERT_EQ(built->package.nodes.size(), 1U);
    EXPECT_EQ(built->package.nodes.front().object_type, "SMPL");
    EXPECT_EQ(built->package.nodes.front().relocations.size(), 2U);
    ASSERT_TRUE(built->package.nodes.front().semantic_sha256);
    ASSERT_TRUE(built->package.nodes.front().audio_sha256);
    EXPECT_EQ(built->package.nodes.front().semantic_sha256->size(), 64U);
    EXPECT_EQ(built->package.nodes.front().audio_sha256->size(), 64U);
    EXPECT_NE(built->package.nodes.front().semantic_sha256, built->package.nodes.front().normalized_sha256);
    EXPECT_NE(built->package.nodes.front().audio_sha256, built->package.nodes.front().normalized_sha256);
    EXPECT_EQ(*built->package.nodes.front().semantic_sha256,
              "f0bc9a18c0c14d35e0cb81c375fd43ef8cd6528bb52eb29aee67ef8bbfe82b82");
    EXPECT_EQ(*built->package.nodes.front().audio_sha256,
              "2e0f5102be7b10596b272fd8df2cb288a304bd6f8a1b9c7603d13494cd13a024");
    EXPECT_EQ(built->package.nodes.front().normalized_sha256,
              "e64305ce4d25f82f73b5512fa6945d74ba554e0cf481e42accb76fd397117d37");
    EXPECT_EQ(built->package.package_id, "f3ce5bc88b01566c80b9dbfe1d630869ec65b90b0a9c4674ef0406a662f9ca4a");

    const auto reopened = axk::open_portable_package(built->archive, "TEST.axksmpl");
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->package_id, built->package.package_id);
    EXPECT_EQ(reopened->nodes, built->package.nodes);
    EXPECT_TRUE(reopened->issues.empty());
}

TEST(PortablePackage, BuildsStrictSbnkClosureAndDeduplicatesMultiRootDependencies) {
    auto source = axk::open_media(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave")};
    const auto bank = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(bank) << bank.error().message;
    EXPECT_EQ(bank->package.kind, axk::PackageKind::sbnk);
    EXPECT_EQ(bank->required_extension, ".axksbnk");
    EXPECT_EQ(bank->package.nodes.size(), 2U);
    EXPECT_EQ(bank->package.relationships.size(), 1U);
    EXPECT_EQ(std::ranges::count(bank->package.nodes, std::string{"SBNK"}, &axk::PackageNode::object_type), 1);
    EXPECT_EQ(std::ranges::count(bank->package.nodes, std::string{"SMPL"}, &axk::PackageNode::object_type), 1);

    std::vector bundle_roots{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave"),
                             root(axk::PackageRootKind::smpl, "New Volume", "sine wave")};
    const auto bundle = axk::build_portable_package(*source, bundle_roots);
    ASSERT_TRUE(bundle) << bundle.error().message;
    EXPECT_EQ(bundle->package.kind, axk::PackageKind::bundle);
    EXPECT_EQ(bundle->required_extension, ".axkpkg");
    EXPECT_EQ(bundle->package.nodes.size(), bank->package.nodes.size());
    EXPECT_EQ(bundle->package.relationships, bank->package.relationships);
    EXPECT_EQ(bundle->package.roots.size(), 2U);
}

TEST(PortablePackage, IgnoresAmbiguousInactiveProgramDiagnosticsButKeepsExactRows) {
    axk::Relationship relationship;
    relationship.assignment_state = axk::AssignmentState::visible_off;
    relationship.quality = axk::RelationshipQuality::tentative;
    EXPECT_FALSE(axk::package_internal::portable_inactive_program_relationship(relationship));

    relationship.assignment_state = axk::AssignmentState::source_load;
    relationship.quality = axk::RelationshipQuality::known;
    relationship.target_key = "target";
    EXPECT_TRUE(axk::package_internal::portable_inactive_program_relationship(relationship));
}

TEST(PortablePackage, ExportsACompleteSupportedVolumeWithOnePayloadPerDigest) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector volume_root{root(axk::PackageRootKind::volume, "FAT root")};
    const auto volume = axk::build_portable_package(media, volume_root);
    ASSERT_TRUE(volume) << volume.error().message;
    EXPECT_EQ(volume->package.kind, axk::PackageKind::volume);
    EXPECT_EQ(volume->required_extension, ".axkvol");
    EXPECT_EQ(volume->package.nodes.size(), 1U);
    EXPECT_TRUE(volume->package.relationships.empty());
    const auto reopened = axk::open_portable_package(volume->archive, "volume.axkvol");
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->nodes.size(), 1U);
}

TEST(PortablePackage, TypedSuffixFollowsSelectedRootRatherThanDependencyClosure) {
    const auto output_root = publication_root("axklib-package-typed-roots");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    auto authored_volume = graph_volume(audio_path);
    axk::SampleBankSpec grouped_two;
    grouped_two.name = "Grouped Bank 2";
    grouped_two.waveform_id = "wave";
    grouped_two.root_key = 60U;
    grouped_two.key_high = 127U;
    authored_volume.sample_banks.push_back(std::move(grouped_two));
    axk::SampleBankSpec direct_two;
    direct_two.name = "Direct Bank 2";
    direct_two.waveform_id = "wave";
    direct_two.root_key = 60U;
    direct_two.key_high = 127U;
    authored_volume.sample_banks.push_back(std::move(direct_two));
    authored_volume.sample_bank_groups.push_back({"Graph Group 2", {"Grouped Bank 2"}});
    authored_volume.programs.push_back({2U, {{"SBAC", "Graph Group 2", 1U}, {"SBNK", "Direct Bank 2", 2U}}});
    manifest.partitions.push_back({"P1", {std::move(authored_volume)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source_path));
    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;

    struct Case {
        axk::PackageRootKind root_kind;
        std::string object_name;
        axk::PackageKind package_kind;
        std::string_view extension;
        std::size_t minimum_nodes;
    };
    const std::vector cases{
        Case{axk::PackageRootKind::volume, "", axk::PackageKind::volume, ".axkvol", 5U},
        Case{axk::PackageRootKind::prog, "001", axk::PackageKind::program, ".axkprg", 5U},
        Case{axk::PackageRootKind::sbac, "Graph Group", axk::PackageKind::sbac, ".axksbac", 3U},
        Case{axk::PackageRootKind::sbnk, "Grouped Bank", axk::PackageKind::sbnk, ".axksbnk", 2U},
        Case{axk::PackageRootKind::smpl, "Graph Wave", axk::PackageKind::smpl, ".axksmpl", 1U},
    };
    for (const auto &test : cases) {
        const std::vector selectors{root(test.root_kind, "Graph Volume", test.object_name)};
        const auto built = axk::build_portable_package(*source, selectors);
        ASSERT_TRUE(built) << test.extension << ": " << built.error().message;
        EXPECT_EQ(built->package.kind, test.package_kind) << test.extension;
        EXPECT_EQ(built->required_extension, test.extension) << test.extension;
        EXPECT_GE(built->package.nodes.size(), test.minimum_nodes) << test.extension;
        const auto published =
            axk::publish_portable_package(*built, output_root / (std::string{"typed"} + std::string{test.extension}));
        ASSERT_TRUE(published) << test.extension << ": " << published.error().message;
        const auto inspected = axk::inspect_portable_package(published->output_path);
        ASSERT_TRUE(inspected) << test.extension << ": " << inspected.error().message;
        EXPECT_EQ(inspected->kind, test.package_kind) << test.extension;
        EXPECT_FALSE(inspected->payloads_verified) << test.extension;
        EXPECT_TRUE(inspected->issues.empty()) << test.extension;
        const auto reopened = axk::open_portable_package(published->output_path);
        ASSERT_TRUE(reopened) << test.extension << ": " << reopened.error().message;
        EXPECT_EQ(reopened->kind, test.package_kind) << test.extension;
        EXPECT_TRUE(reopened->payloads_verified) << test.extension;
        EXPECT_TRUE(axk::verify_portable_package(*reopened)) << test.extension;
        EXPECT_TRUE(reopened->issues.empty()) << test.extension;
    }

    const std::vector bundle_roots{
        root(axk::PackageRootKind::sbac, "Graph Volume", "Graph Group"),
        root(axk::PackageRootKind::sbnk, "Graph Volume", "Direct Bank"),
    };
    const auto bundle = axk::build_portable_package(*source, bundle_roots);
    ASSERT_TRUE(bundle) << bundle.error().message;
    EXPECT_EQ(bundle->package.kind, axk::PackageKind::bundle);
    EXPECT_EQ(bundle->required_extension, ".axkpkg");
    EXPECT_EQ(bundle->package.roots.size(), 2U);
    const auto published_bundle = axk::publish_portable_package(*bundle, output_root / "typed.axkpkg");
    ASSERT_TRUE(published_bundle) << published_bundle.error().message;
    const auto inspected_bundle = axk::inspect_portable_package(published_bundle->output_path);
    ASSERT_TRUE(inspected_bundle) << inspected_bundle.error().message;
    EXPECT_EQ(inspected_bundle->kind, axk::PackageKind::bundle);
    EXPECT_FALSE(inspected_bundle->payloads_verified);
    const auto verified_bundle = axk::open_portable_package(published_bundle->output_path);
    ASSERT_TRUE(verified_bundle) << verified_bundle.error().message;
    EXPECT_TRUE(axk::verify_portable_package(*verified_bundle));

    const std::vector same_type_bundle_roots{
        root(axk::PackageRootKind::prog, "Graph Volume", "001"),
        root(axk::PackageRootKind::prog, "Graph Volume", "002"),
    };
    const auto same_type_bundle = axk::build_portable_package(*source, same_type_bundle_roots);
    ASSERT_TRUE(same_type_bundle) << same_type_bundle.error().message;
    EXPECT_EQ(same_type_bundle->package.kind, axk::PackageKind::bundle);
    EXPECT_EQ(same_type_bundle->required_extension, ".axkpkg");
    EXPECT_EQ(same_type_bundle->package.roots.size(), 2U);
    EXPECT_EQ(std::ranges::count(same_type_bundle->package.nodes, std::string{"SMPL"}, &axk::PackageNode::object_type),
              1);

    const auto copied_source_path = output_root / "copied-source.hds";
    ASSERT_TRUE(std::filesystem::copy_file(source_path, copied_source_path));
    const auto copied_source = axk::open_media(copied_source_path);
    ASSERT_TRUE(copied_source) << copied_source.error().message;
    const std::vector program_root{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    const auto original_program = axk::build_portable_package(*source, program_root);
    const auto copied_program = axk::build_portable_package(*copied_source, program_root);
    ASSERT_TRUE(original_program) << original_program.error().message;
    ASSERT_TRUE(copied_program) << copied_program.error().message;
    EXPECT_EQ(copied_program->package.package_id, original_program->package.package_id);
    EXPECT_EQ(copied_program->archive, original_program->archive);

    std::vector<std::byte> sequence_payload(0x80U);
    axk::ByteWriter writer{sequence_payload};
    ASSERT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0cU, 4U, "SEQU", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x32U, 16U, "Sequence", std::byte{}));
    auto sequence_object =
        axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(std::move(sequence_payload)), "sequence.bin");
    ASSERT_TRUE(sequence_object) << sequence_object.error().message;
    const axk::MediaContainer sequence_media{std::move(*sequence_object)};
    const std::vector sequence{root(axk::PackageRootKind::sequ, "Standalone object", "Sequence")};
    const auto unsupported = axk::build_portable_package(sequence_media, sequence);
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, axk::ErrorCode::unsupported_profile);
    EXPECT_NE(unsupported.error().message.find("SEQU"), std::string::npos);
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, SbacRelationshipOrdinalsPreserveSourceSlotOrder) {
    const auto output_root = publication_root("axklib-package-sbac-slot-order");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, tiny_waveform(1000)));

    axk::VolumeSpec volume;
    volume.name = "Slot Order";
    volume.waveforms.push_back({"wave", "Shared Wave", audio_path, 60U, {}});
    for (const auto &name : {std::string{"Z Bank"}, std::string{"A Bank"}}) {
        axk::SampleBankSpec bank;
        bank.name = name;
        bank.waveform_id = "wave";
        bank.root_key = 60U;
        bank.key_high = 127U;
        volume.sample_banks.push_back(std::move(bank));
    }
    axk::SampleBankSpec direct;
    direct.name = "Direct Bank";
    direct.waveform_id = "wave";
    direct.root_key = 60U;
    direct.key_high = 127U;
    volume.sample_banks.push_back(std::move(direct));
    volume.sample_bank_groups.push_back({"Ordered Group", {"Z Bank", "A Bank"}});
    volume.programs.push_back({1U, {{"SBAC", "Ordered Group", 1U}, {"SBNK", "Direct Bank", 2U}}});
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {std::move(volume)}});
    const auto written = axk::write_hds_image(manifest, source_path);
    ASSERT_TRUE(written) << written.error().message;
    const auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;

    const std::vector roots{root(axk::PackageRootKind::sbac, "Slot Order", "Ordered Group")};
    const auto built = axk::build_portable_package(*source, roots);
    ASSERT_TRUE(built) << built.error().message;
    std::vector<axk::PackageRelationship> edges;
    for (const auto &edge : built->package.relationships) {
        if (edge.role == "SBAC_SLOT_TO_SBNK")
            edges.push_back(edge);
    }
    std::ranges::sort(edges, {}, &axk::PackageRelationship::ordinal);
    ASSERT_EQ(edges.size(), 2U);
    const auto target_name = [&](const axk::PackageRelationship &edge) {
        const auto target = std::ranges::find(built->package.nodes, edge.target_node_id, &axk::PackageNode::node_id);
        EXPECT_NE(target, built->package.nodes.end());
        return target->name;
    };
    EXPECT_EQ(edges[0].ordinal, 0U);
    EXPECT_EQ(target_name(edges[0]), "Z Bank");
    EXPECT_EQ(edges[1].ordinal, 1U);
    EXPECT_EQ(target_name(edges[1]), "A Bank");
    EXPECT_TRUE(axk::verify_portable_package(built->package));
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, NormativeJsonSchemaMatchesCanonicalManifestShapeAndEnums) {
    const auto output_root = publication_root("axklib-package-schema-drift");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, tiny_waveform(1000)));
    axk::HdsBuildManifest source_manifest{"1.0", 4U * 1024U * 1024U, {}};
    source_manifest.partitions.push_back({"P1", {graph_volume(audio_path)}});
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source_path));
    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector roots{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    const auto built = axk::build_portable_package(*source, roots);
    ASSERT_TRUE(built) << built.error().message;

    std::ifstream schema_input{std::filesystem::path{AXK_SOURCE_ROOT} / "docs/axklib/portable-package-v1.schema.json"};
    ASSERT_TRUE(schema_input);
    const auto schema = nlohmann::json::parse(schema_input);
    const auto manifest = archive_manifest(built->archive);
    ASSERT_FALSE(manifest.empty());
    expect_schema_object_shape(schema, manifest);
    expect_schema_object_shape(schema.at("properties").at("provenance"), manifest.at("provenance"));
    const auto &definitions = schema.at("$defs");
    for (const auto &item : manifest.at("roots"))
        expect_schema_object_shape(definitions.at("root"), item);
    for (const auto &item : manifest.at("objects")) {
        expect_schema_object_shape(definitions.at("object"), item);
        expect_schema_object_shape(definitions.at("placementHint"), item.at("placement_hint"));
        for (const auto &relocation : item.at("relocations"))
            expect_schema_object_shape(definitions.at("relocation"), relocation);
    }
    for (const auto &item : manifest.at("relationships"))
        expect_schema_object_shape(definitions.at("relationship"), item);
    for (const auto &item : manifest.at("payloads"))
        expect_schema_object_shape(definitions.at("payload"), item);

    EXPECT_EQ(string_set(schema.at("properties").at("package_kind").at("enum")),
              (std::set<std::string>{"bundle", "program", "sbnk", "sbac", "sequence", "smpl", "volume"}));
    EXPECT_EQ(string_set(definitions.at("root").at("properties").at("kind").at("enum")),
              (std::set<std::string>{"prog", "sbnk", "sbac", "sequ", "smpl", "volume"}));
    EXPECT_EQ(string_set(definitions.at("object").at("properties").at("object_type").at("enum")),
              (std::set<std::string>{"PRF3", "PROG", "SBAC", "SBNK", "SEQU", "SMPL"}));
    EXPECT_EQ(string_set(definitions.at("relocation").at("properties").at("role").at("enum")),
              (std::set<std::string>{"PROG_ASSIGNMENT_HANDLE", "SBAC_SLOT_HANDLE", "SBNK_GROUP_MEMBERSHIP",
                                     "SBNK_LEFT_MEMBER_LINK", "SBNK_PROGRAM_BITMAP", "SBNK_RIGHT_MEMBER_LINK",
                                     "SMPL_GROUP_ID", "SMPL_LINK_ID"}));
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, ExportsCompleteVolumeWithRelocatableNonzeroSbacHandles) {
    auto source = axk::open_media(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(source) << source.error().message;
    const std::vector volume_root{root(axk::PackageRootKind::volume, "New Volume")};
    const auto volume = axk::build_portable_package(*source, volume_root);
    ASSERT_TRUE(volume) << volume.error().message;
    EXPECT_TRUE(axk::verify_portable_package(volume->package));
    EXPECT_TRUE(std::ranges::any_of(volume->package.nodes, [](const auto &node) {
        return node.object_type == "SBAC" && std::ranges::any_of(node.relocations, [](const auto &relocation) {
                   return relocation.role == "SBAC_SLOT_HANDLE" && relocation.expected_hex != "00000000";
               });
    }));
}

TEST(PortablePackage, ManifestKindOverridesARecognizedWrongFilenameExtension) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const auto reopened = axk::open_portable_package(built->archive, "renamed.axkprg");
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->kind, axk::PackageKind::smpl);
    ASSERT_EQ(reopened->issues.size(), 1U);
    EXPECT_EQ(reopened->issues.front().code, "PACKAGE_EXTENSION_MISMATCH");
    EXPECT_FALSE(reopened->issues.front().fatal);

    const auto bundle_named = axk::open_portable_package(built->archive, "bundle.axkpkg");
    ASSERT_TRUE(bundle_named) << bundle_named.error().message;
    EXPECT_EQ(bundle_named->kind, axk::PackageKind::smpl);
    EXPECT_EQ(bundle_named->package_id, built->package.package_id);
    ASSERT_EQ(bundle_named->issues.size(), 1U);
    EXPECT_EQ(bundle_named->issues.front().code, "PACKAGE_EXTENSION_MISMATCH");
    EXPECT_FALSE(bundle_named->issues.front().fatal);
    EXPECT_TRUE(axk::verify_portable_package(*bundle_named));

    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "New Volume"));
    const std::vector packages{*bundle_named};
    const auto plan = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflict_summary(*plan);
    ASSERT_EQ(plan->warnings.size(), 1U);
    EXPECT_EQ(plan->warnings.front().code, "PACKAGE_EXTENSION_MISMATCH");
    EXPECT_FALSE(plan->warnings.front().fatal);
}

TEST(PortablePackage, RejectsMissingAmbiguousAndUnsupportedSelectorsWithoutArchive) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector missing{root(axk::PackageRootKind::smpl, "FAT root", "MISSING")};
    EXPECT_FALSE(axk::build_portable_package(media, missing));
    const std::vector sequence{root(axk::PackageRootKind::sequ, "FAT root", "TEST")};
    EXPECT_FALSE(axk::build_portable_package(media, sequence));
    EXPECT_FALSE(axk::build_portable_package(media, {}));
}

TEST(PortablePackage, RejectsRehashedSemanticTampering) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;

    const auto wrong_kind =
        mutate_manifest(built->archive, [](nlohmann::json &manifest) { manifest["package_kind"] = "program"; });
    ASSERT_FALSE(wrong_kind.empty());
    EXPECT_FALSE(axk::open_portable_package(wrong_kind, "TEST.axkprg"));

    const auto wrong_node = mutate_manifest(built->archive, [](nlohmann::json &manifest) {
        const auto replacement = "n-"
                                 "0000000000000000000000000000000000000000000000000000000000000000";
        manifest["objects"][0]["node_id"] = replacement;
        manifest["roots"][0]["node_ids"][0] = replacement;
    });
    ASSERT_FALSE(wrong_node.empty());
    EXPECT_FALSE(axk::open_portable_package(wrong_node, "TEST.axksmpl"));

    const auto wrong_relocation = mutate_manifest(
        built->archive, [](nlohmann::json &manifest) { manifest["objects"][0]["relocations"][0]["offset"] = 1; });
    ASSERT_FALSE(wrong_relocation.empty());
    EXPECT_FALSE(axk::open_portable_package(wrong_relocation, "TEST.axksmpl"));

    const auto wrong_semantic = mutate_manifest(built->archive, [](nlohmann::json &manifest) {
        manifest["objects"][0]["semantic_sha256"] = std::string(64U, '0');
    });
    ASSERT_FALSE(wrong_semantic.empty());
    EXPECT_FALSE(axk::open_portable_package(wrong_semantic, "TEST.axksmpl"));

    const auto wrong_audio = mutate_manifest(built->archive, [](nlohmann::json &manifest) {
        manifest["objects"][0]["audio_sha256"] = std::string(64U, '0');
    });
    ASSERT_FALSE(wrong_audio.empty());
    EXPECT_FALSE(axk::open_portable_package(wrong_audio, "TEST.axksmpl"));
}

TEST(PortablePackage, RevalidatesInMemoryPackagesBeforePlanning) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_TRUE(axk::verify_portable_package(built->package));

    auto changed_id = built->package;
    changed_id.package_id.front() = changed_id.package_id.front() == '0' ? '1' : '0';
    EXPECT_FALSE(axk::verify_portable_package(changed_id));

    auto changed_payload = built->package;
    changed_payload.nodes.front().raw_payload.back() ^= std::byte{0x01};
    EXPECT_FALSE(axk::verify_portable_package(changed_payload));
}

TEST(PortablePackage, PublishesWithDerivedTypedSuffixAndReopensFromPath) {
    const auto output_root = publication_root("axklib-package-publish");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};

    const auto published = axk::export_portable_package(media, roots, output_root / "TEST", false);
    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(published->output_path.filename(), "TEST.axksmpl");
    EXPECT_TRUE(std::filesystem::is_regular_file(published->output_path));
    EXPECT_EQ(published->size_bytes, std::filesystem::file_size(published->output_path));
    const auto inspected = axk::inspect_portable_package(published->output_path);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->payloads_verified);
    ASSERT_FALSE(inspected->nodes.empty());
    EXPECT_TRUE(inspected->nodes.front().raw_payload.empty());
    EXPECT_FALSE(axk::verify_portable_package(*inspected));

    const auto reopened = axk::open_portable_package(published->output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->package_id, published->package_id);
    EXPECT_TRUE(reopened->issues.empty());
    EXPECT_TRUE(reopened->payloads_verified);
    EXPECT_FALSE(reopened->nodes.front().raw_payload.empty());

    auto corrupted_archive = read_file(published->output_path);
    const axk::MemoryReader archive_reader{corrupted_archive};
    const auto archive_summary = axk::package_internal::inspect_archive(archive_reader);
    ASSERT_TRUE(archive_summary) << archive_summary.error().message;
    ASSERT_GT(archive_summary->entries.size(), 1U);
    const auto payload_offset = archive_summary->entries[1].data_offset;
    ASSERT_LT(payload_offset, corrupted_archive.size());
    corrupted_archive[static_cast<std::size_t>(payload_offset)] ^= std::byte{0x01};
    const auto corrupted_path = output_root / "CORRUPTED.axksmpl";
    ASSERT_TRUE(write_file(corrupted_path, corrupted_archive));
    const auto inspected_corruption = axk::inspect_portable_package(corrupted_path);
    ASSERT_TRUE(inspected_corruption) << inspected_corruption.error().message;
    EXPECT_FALSE(inspected_corruption->payloads_verified);
    EXPECT_FALSE(axk::open_portable_package(corrupted_path));
    for (const auto &entry : std::filesystem::directory_iterator(output_root))
        EXPECT_EQ(entry.path().filename().string().find(".package."), std::string::npos);
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, RejectsWrongOutputSuffixAndPreservesExistingTarget) {
    const auto output_root = publication_root("axklib-package-preflight");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};

    EXPECT_FALSE(axk::export_portable_package(media, roots, output_root / "wrong.axkprg"));
    EXPECT_FALSE(axk::export_portable_package(media, roots, output_root / "wrong.axkpkg"));
    EXPECT_FALSE(axk::export_portable_package(media, roots, output_root / "wrong.zip"));
    EXPECT_FALSE(std::filesystem::exists(output_root / "wrong.axkprg"));
    EXPECT_FALSE(std::filesystem::exists(output_root / "wrong.axkpkg"));
    EXPECT_FALSE(std::filesystem::exists(output_root / "wrong.zip"));

    const auto target = output_root / "TEST.axksmpl";
    {
        std::ofstream existing{target, std::ios::binary};
        existing << "preserve-me";
    }
    const auto rejected = axk::export_portable_package(media, roots, target, false);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::io_open_failed);
    std::ifstream existing{target, std::ios::binary};
    const std::string retained{std::istreambuf_iterator<char>{existing}, {}};
    EXPECT_EQ(retained, "preserve-me");

    const auto replaced = axk::export_portable_package(media, roots, target, true);
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_TRUE(axk::open_portable_package(target));
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, CancellationAndInvalidBuildPublishNothingAndCleanTemporaryFiles) {
    const auto output_root = publication_root("axklib-package-cancel");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;

    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled =
        axk::publish_portable_package(*built, output_root / "cancelled", false, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(output_root));

    built->archive.front() ^= std::byte{0xff};
    const auto invalid = axk::publish_portable_package(*built, output_root / "invalid");
    ASSERT_FALSE(invalid);
    EXPECT_FALSE(std::filesystem::exists(output_root / "invalid.axksmpl"));
    if (std::filesystem::exists(output_root)) {
        EXPECT_TRUE(std::filesystem::is_empty(output_root));
        std::filesystem::remove_all(output_root, error);
    }
}

TEST(PortablePackage, ConcurrentNoOverwritePublishersProduceOneCompletePackage) {
    const auto output_root = publication_root("axklib-package-concurrent");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const auto target = output_root / "shared";

    auto publish = [&] { return axk::publish_portable_package(*built, target); };
    auto first = std::async(std::launch::async, publish);
    auto second = std::async(std::launch::async, publish);
    const auto first_result = first.get();
    const auto second_result = second.get();
    EXPECT_NE(first_result.has_value(), second_result.has_value());
    const auto reopened = axk::open_portable_package(output_root / "shared.axksmpl");
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->package_id, built->package.package_id);
    std::size_t file_count{};
    for (const auto &entry : std::filesystem::directory_iterator(output_root)) {
        ++file_count;
        EXPECT_EQ(entry.path().filename(), "shared.axksmpl");
    }
    EXPECT_EQ(file_count, 1U);
    std::filesystem::remove_all(output_root, error);
}

TEST(PortablePackage, NormalizationIgnoresOnlyDeclaredRelocationBytes) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const auto &node = built->package.nodes.front();
    auto decoded = axk::decode_object(node.raw_payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original = axk::package_internal::build_relocation_profile(*decoded, node.raw_payload);
    ASSERT_TRUE(original) << original.error().message;
    ASSERT_FALSE(original->relocations.empty());

    auto relocated = node.raw_payload;
    relocated[original->relocations.front().offset] ^= std::byte{0x01};
    auto relocated_decoded = axk::decode_object(relocated);
    ASSERT_TRUE(relocated_decoded) << relocated_decoded.error().message;
    const auto relocated_profile = axk::package_internal::build_relocation_profile(*relocated_decoded, relocated);
    ASSERT_TRUE(relocated_profile) << relocated_profile.error().message;
    EXPECT_NE(axk::package_internal::sha256(relocated), axk::package_internal::sha256(node.raw_payload));
    EXPECT_EQ(relocated_profile->normalized_payload, original->normalized_payload);

    auto unknown_changed = node.raw_payload;
    unknown_changed[0x30U] ^= std::byte{0x01};
    auto unknown_decoded = axk::decode_object(unknown_changed);
    ASSERT_TRUE(unknown_decoded) << unknown_decoded.error().message;
    const auto unknown_profile = axk::package_internal::build_relocation_profile(*unknown_decoded, unknown_changed);
    ASSERT_TRUE(unknown_profile) << unknown_profile.error().message;
    EXPECT_NE(unknown_profile->normalized_payload, original->normalized_payload);
}

TEST(PortablePackage, RelocationProfilesCoverEveryAdmittedObjectAndOnlyDeclaredBytes) {
    const auto output_root = publication_root("axklib-package-relocation-profiles");
    const auto audio = output_root / "tone.wav";
    const auto image_path = output_root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    ASSERT_TRUE(std::filesystem::create_directories(output_root));
    ASSERT_TRUE(axk::write_wav_atomic(audio, tiny_waveform(1234)));
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {graph_volume(audio)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, image_path));
    auto media = axk::open_media(image_path);
    ASSERT_TRUE(media) << media.error().message;
    const std::vector roots{root(axk::PackageRootKind::volume, "Graph Volume")};
    const auto built = axk::build_portable_package(*media, roots);
    ASSERT_TRUE(built) << built.error().message;

    const std::map<std::string, std::string, std::less<>> destination_names{
        {"PROG", "Reloc Program"},       {"SBAC", "Reloc Group"}, {"Grouped Bank", "Reloc Grouped"},
        {"Direct Bank", "Reloc Direct"}, {"SMPL", "Reloc Wave"},
    };
    const auto destination_name = [&](const axk::PackageNode &node) -> const std::string & {
        if (node.object_type == "SBNK")
            return destination_names.at(node.name);
        return destination_names.at(node.object_type);
    };
    const auto target_node = [&](std::string_view node_id) -> const axk::PackageNode & {
        const auto found = std::ranges::find(built->package.nodes, node_id, &axk::PackageNode::node_id);
        EXPECT_NE(found, built->package.nodes.end());
        return *found;
    };
    const auto be32 = [](std::span<const std::byte> bytes, std::size_t offset) {
        return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
               std::to_integer<std::uint32_t>(bytes[offset + 3U]);
    };
    const auto in_range = [](std::size_t offset, std::size_t begin, std::size_t width) {
        return offset >= begin && offset - begin < width;
    };

    std::set<std::string> object_types;
    for (const auto &node : built->package.nodes) {
        object_types.insert(node.object_type);
        const auto decoded = axk::decode_object(node.raw_payload);
        ASSERT_TRUE(decoded) << decoded.error().message;
        const auto profile = axk::package_internal::build_relocation_profile(*decoded, node.raw_payload);
        ASSERT_TRUE(profile) << profile.error().message;
        ASSERT_EQ(profile->relocations.size(), node.relocations.size());
        for (std::size_t index = 0; index < profile->relocations.size(); ++index) {
            EXPECT_EQ(profile->relocations[index].offset, node.relocations[index].offset);
            EXPECT_EQ(profile->relocations[index].width, node.relocations[index].width);
            EXPECT_EQ(profile->relocations[index].role, node.relocations[index].role);
            EXPECT_EQ(profile->relocations[index].mask_hex, node.relocations[index].mask_hex);
            EXPECT_EQ(profile->relocations[index].expected_hex, node.relocations[index].expected_hex);
        }

        if (node.object_type == "SMPL") {
            ASSERT_EQ(profile->relocations.size(), 2U);
            EXPECT_EQ(profile->relocations[0].offset, 0x6cU);
            EXPECT_EQ(profile->relocations[0].width, 4U);
            EXPECT_EQ(profile->relocations[0].role, "SMPL_GROUP_ID");
            EXPECT_TRUE(profile->relocations[0].mask_hex.empty());
            EXPECT_EQ(profile->relocations[1].offset, 0x78U);
            EXPECT_EQ(profile->relocations[1].width, 4U);
            EXPECT_EQ(profile->relocations[1].role, "SMPL_LINK_ID");
            EXPECT_TRUE(profile->relocations[1].mask_hex.empty());
        } else if (node.object_type == "SBNK") {
            ASSERT_EQ(profile->relocations.size(), 4U);
            EXPECT_EQ(profile->relocations[0].offset, 0xa0U);
            EXPECT_EQ(profile->relocations[0].width, 4U);
            EXPECT_EQ(profile->relocations[0].role, "SBNK_LEFT_MEMBER_LINK");
            EXPECT_EQ(profile->relocations[1].offset, 0xa4U);
            EXPECT_EQ(profile->relocations[1].width, 4U);
            EXPECT_EQ(profile->relocations[1].role, "SBNK_RIGHT_MEMBER_LINK");
            EXPECT_EQ(profile->relocations[2].offset, 0xc0U);
            EXPECT_EQ(profile->relocations[2].width, 16U);
            EXPECT_EQ(profile->relocations[2].role, "SBNK_PROGRAM_BITMAP");
            EXPECT_EQ(profile->relocations[3].offset, 0xd0U);
            EXPECT_EQ(profile->relocations[3].width, 1U);
            EXPECT_EQ(profile->relocations[3].role, "SBNK_GROUP_MEMBERSHIP");
            EXPECT_EQ(profile->relocations[3].mask_hex, "01");
        } else if (node.object_type == "SBAC") {
            const auto *group = std::get_if<axk::CurrentSbac>(&decoded->payload);
            ASSERT_NE(group, nullptr);
            ASSERT_EQ(profile->relocations.size(), group->slots.size());
            for (std::size_t index = 0; index < group->slots.size(); ++index) {
                EXPECT_EQ(profile->relocations[index].offset, group->slots[index].offset + 16U);
                EXPECT_EQ(profile->relocations[index].width, 4U);
                EXPECT_EQ(profile->relocations[index].role, "SBAC_SLOT_HANDLE");
                EXPECT_TRUE(profile->relocations[index].mask_hex.empty());
                ASSERT_EQ(node.relocations[index].edge_ids.size(), 1U);
                const auto edge =
                    std::ranges::find(built->package.relationships, node.relocations[index].edge_ids.front(),
                                      &axk::PackageRelationship::edge_id);
                ASSERT_NE(edge, built->package.relationships.end());
                EXPECT_EQ(edge->source_node_id, node.node_id);
                EXPECT_EQ(edge->role, "SBAC_SLOT_TO_SBNK");
                EXPECT_EQ(edge->ordinal, index);
            }
            auto nonzero_handles = node.raw_payload;
            for (std::size_t index = 0; index < group->slots.size(); ++index) {
                const auto offset = static_cast<std::size_t>(group->slots[index].offset) + 16U;
                const auto value = static_cast<std::uint32_t>(0x10203040U + index);
                nonzero_handles[offset] = static_cast<std::byte>(value >> 24U);
                nonzero_handles[offset + 1U] = static_cast<std::byte>(value >> 16U);
                nonzero_handles[offset + 2U] = static_cast<std::byte>(value >> 8U);
                nonzero_handles[offset + 3U] = static_cast<std::byte>(value);
            }
            const auto nonzero_decoded = axk::decode_object(nonzero_handles);
            ASSERT_TRUE(nonzero_decoded) << nonzero_decoded.error().message;
            const auto nonzero_profile =
                axk::package_internal::build_relocation_profile(*nonzero_decoded, nonzero_handles);
            ASSERT_TRUE(nonzero_profile) << nonzero_profile.error().message;
            EXPECT_EQ(nonzero_profile->normalized_payload, profile->normalized_payload);
            auto mutable_nonzero_profile = *nonzero_profile;
            for (std::size_t index = 0; index < group->slots.size(); ++index) {
                EXPECT_EQ(nonzero_profile->relocations[index].expected_hex, std::format("{:08x}", 0x10203040U + index));
                mutable_nonzero_profile.relocations[index].edge_ids = node.relocations[index].edge_ids;
            }
            auto nonzero_node = node;
            nonzero_node.raw_payload = std::move(nonzero_handles);
            nonzero_node.relocations = std::move(mutable_nonzero_profile.relocations);
            axk::package_internal::PackageNodeRelocationContext nonzero_context;
            nonzero_context.destination_name = node.name;
            for (const auto &edge : built->package.relationships) {
                if (edge.source_node_id == node.node_id) {
                    nonzero_context.edge_target_names.emplace(edge.edge_id, target_node(edge.target_node_id).name);
                }
            }
            const auto normalized =
                axk::package_internal::relocate_package_node(built->package, nonzero_node, nonzero_context);
            ASSERT_TRUE(normalized) << normalized.error().message;
            const auto normalized_decoded = axk::decode_object(*normalized);
            ASSERT_TRUE(normalized_decoded) << normalized_decoded.error().message;
            const auto *normalized_group = std::get_if<axk::CurrentSbac>(&normalized_decoded->payload);
            ASSERT_NE(normalized_group, nullptr);
            EXPECT_TRUE(
                std::ranges::all_of(normalized_group->slots, [](const auto &slot) { return slot.raw_handle == 0U; }));
        } else if (node.object_type == "PROG") {
            const auto *program = std::get_if<axk::CurrentProg>(&decoded->payload);
            ASSERT_NE(program, nullptr);
            std::vector<std::size_t> portable_rows;
            for (std::size_t index = 0; index < program->assignments.size(); ++index) {
                const auto &assignment = program->assignments[index];
                if (!assignment.name.empty() && (assignment.kind == 0x10U || assignment.kind == 0x11U))
                    portable_rows.push_back(index);
            }
            ASSERT_EQ(profile->relocations.size(), portable_rows.size());
            auto nonzero_handles = node.raw_payload;
            for (std::size_t relocation_index = 0; relocation_index < portable_rows.size(); ++relocation_index) {
                const auto row_index = portable_rows[relocation_index];
                const auto offset = 0x130U + row_index * 0x38U;
                const auto value = static_cast<std::uint32_t>(0x50607080U + relocation_index);
                nonzero_handles[offset] = static_cast<std::byte>(value >> 24U);
                nonzero_handles[offset + 1U] = static_cast<std::byte>(value >> 16U);
                nonzero_handles[offset + 2U] = static_cast<std::byte>(value >> 8U);
                nonzero_handles[offset + 3U] = static_cast<std::byte>(value);
                EXPECT_EQ(profile->relocations[relocation_index].offset, offset);
                EXPECT_EQ(profile->relocations[relocation_index].width, 4U);
                EXPECT_EQ(profile->relocations[relocation_index].role, "PROG_ASSIGNMENT_HANDLE");
                EXPECT_TRUE(profile->relocations[relocation_index].mask_hex.empty());
                ASSERT_EQ(node.relocations[relocation_index].edge_ids.size(), 1U);
                const auto edge =
                    std::ranges::find(built->package.relationships, node.relocations[relocation_index].edge_ids.front(),
                                      &axk::PackageRelationship::edge_id);
                ASSERT_NE(edge, built->package.relationships.end());
                EXPECT_EQ(edge->source_node_id, node.node_id);
                EXPECT_EQ(edge->ordinal, row_index);
            }
            const auto nonzero_decoded = axk::decode_object(nonzero_handles);
            ASSERT_TRUE(nonzero_decoded) << nonzero_decoded.error().message;
            const auto nonzero_profile =
                axk::package_internal::build_relocation_profile(*nonzero_decoded, nonzero_handles);
            ASSERT_TRUE(nonzero_profile) << nonzero_profile.error().message;
            EXPECT_EQ(nonzero_profile->normalized_payload, profile->normalized_payload);
            auto mutable_nonzero_profile = *nonzero_profile;
            for (std::size_t index = 0; index < portable_rows.size(); ++index) {
                EXPECT_EQ(nonzero_profile->relocations[index].expected_hex, std::format("{:08x}", 0x50607080U + index));
                mutable_nonzero_profile.relocations[index].edge_ids = node.relocations[index].edge_ids;
            }
            auto nonzero_node = node;
            nonzero_node.raw_payload = std::move(nonzero_handles);
            nonzero_node.relocations = std::move(mutable_nonzero_profile.relocations);
            axk::package_internal::PackageNodeRelocationContext nonzero_context;
            nonzero_context.destination_name = node.name;
            for (const auto &edge : built->package.relationships) {
                if (edge.source_node_id == node.node_id) {
                    nonzero_context.edge_target_names.emplace(edge.edge_id, target_node(edge.target_node_id).name);
                }
            }
            const auto normalized =
                axk::package_internal::relocate_package_node(built->package, nonzero_node, nonzero_context);
            ASSERT_TRUE(normalized) << normalized.error().message;
            const auto normalized_decoded = axk::decode_object(*normalized);
            ASSERT_TRUE(normalized_decoded) << normalized_decoded.error().message;
            const auto *normalized_program = std::get_if<axk::CurrentProg>(&normalized_decoded->payload);
            ASSERT_NE(normalized_program, nullptr);
            for (const auto index : portable_rows)
                EXPECT_EQ(normalized_program->assignments[index].raw_handle, 0U);

            const auto empty_row = std::ranges::find_if(program->assignments,
                                                        [](const auto &assignment) { return assignment.name.empty(); });
            ASSERT_NE(empty_row, program->assignments.end());
            const auto empty_index = static_cast<std::size_t>(std::distance(program->assignments.begin(), empty_row));
            auto invalid_handle = node.raw_payload;
            invalid_handle[0x130U + empty_index * 0x38U] = std::byte{0x01};
            const auto invalid_decoded = axk::decode_object(invalid_handle);
            ASSERT_TRUE(invalid_decoded) << invalid_decoded.error().message;
            const auto invalid_profile =
                axk::package_internal::build_relocation_profile(*invalid_decoded, invalid_handle);
            ASSERT_TRUE(invalid_profile) << invalid_profile.error().message;
            EXPECT_NE(invalid_profile->normalized_payload, profile->normalized_payload);
        } else {
            EXPECT_TRUE(profile->relocations.empty()) << node.object_type;
        }

        for (const auto &relocation : profile->relocations) {
            const auto first = static_cast<std::size_t>(relocation.offset);
            const auto width = static_cast<std::size_t>(relocation.width);
            if (relocation.mask_hex.empty()) {
                for (std::size_t index = 0; index < width; ++index) {
                    auto changed = node.raw_payload;
                    changed[first + index] ^= std::byte{0x01};
                    const auto changed_decoded = axk::decode_object(changed);
                    ASSERT_TRUE(changed_decoded) << relocation.role << '[' << index << ']';
                    const auto changed_profile =
                        axk::package_internal::build_relocation_profile(*changed_decoded, changed);
                    ASSERT_TRUE(changed_profile) << changed_profile.error().message;
                    EXPECT_EQ(changed_profile->normalized_payload, profile->normalized_payload)
                        << relocation.role << '[' << index << ']';
                }
            } else {
                auto declared_changed = node.raw_payload;
                declared_changed[first] ^= std::byte{0x01};
                const auto declared_decoded = axk::decode_object(declared_changed);
                ASSERT_TRUE(declared_decoded);
                const auto declared_profile =
                    axk::package_internal::build_relocation_profile(*declared_decoded, declared_changed);
                ASSERT_TRUE(declared_profile);
                EXPECT_EQ(declared_profile->normalized_payload, profile->normalized_payload);

                auto undeclared_mask_changed = node.raw_payload;
                undeclared_mask_changed[first] ^= std::byte{0x02};
                const auto undeclared_mask_decoded = axk::decode_object(undeclared_mask_changed);
                ASSERT_TRUE(undeclared_mask_decoded);
                const auto undeclared_mask_profile =
                    axk::package_internal::build_relocation_profile(*undeclared_mask_decoded, undeclared_mask_changed);
                ASSERT_TRUE(undeclared_mask_profile);
                EXPECT_NE(undeclared_mask_profile->normalized_payload, profile->normalized_payload);
            }
        }

        auto unknown_changed = node.raw_payload;
        unknown_changed[0x30U] ^= std::byte{0x01};
        const auto unknown_decoded = axk::decode_object(unknown_changed);
        ASSERT_TRUE(unknown_decoded) << node.object_type;
        const auto unknown_profile = axk::package_internal::build_relocation_profile(*unknown_decoded, unknown_changed);
        ASSERT_TRUE(unknown_profile) << unknown_profile.error().message;
        EXPECT_NE(unknown_profile->normalized_payload, profile->normalized_payload) << node.object_type;

        axk::package_internal::PackageNodeRelocationContext context;
        context.destination_name = destination_name(node);
        if (node.object_type == "SMPL")
            context.smpl_link_id = 0x234U;
        if (node.object_type == "SBNK") {
            context.linked_program_numbers = {2U, 33U, 96U, 128U};
            context.grouped =
                std::ranges::any_of(built->package.relationships, [&](const axk::PackageRelationship &edge) {
                    return edge.role == "SBAC_SLOT_TO_SBNK" && edge.target_node_id == node.node_id;
                });
        }
        for (const auto &edge : built->package.relationships) {
            if (edge.source_node_id != node.node_id)
                continue;
            const auto &target = target_node(edge.target_node_id);
            context.edge_target_names.emplace(edge.edge_id, destination_name(target));
            if (target.object_type == "SMPL")
                context.edge_target_link_ids.emplace(edge.edge_id, 0x234U);
        }
        const auto relocated = axk::package_internal::relocate_package_node(built->package, node, context);
        ASSERT_TRUE(relocated) << node.object_type << ' ' << node.name << ": " << relocated.error().message;
        ASSERT_EQ(relocated->size(), node.raw_payload.size());
        for (std::size_t offset = 0; offset < relocated->size(); ++offset) {
            if ((*relocated)[offset] == node.raw_payload[offset])
                continue;
            bool allowed = in_range(offset, 0x32U, 16U);
            for (const auto &relocation : node.relocations)
                allowed = allowed || in_range(offset, relocation.offset, relocation.width);
            for (const auto &edge : built->package.relationships) {
                if (edge.source_node_id != node.node_id)
                    continue;
                if (edge.role == "SBNK_LEFT_MEMBER_TO_SMPL")
                    allowed = allowed || in_range(offset, 0x78U, 16U);
                else if (edge.role == "SBNK_RIGHT_MEMBER_TO_SMPL")
                    allowed = allowed || in_range(offset, 0x88U, 16U);
                else if (edge.role == "SBAC_SLOT_TO_SBNK")
                    allowed = allowed || in_range(offset, 0x14cU + edge.ordinal * 0x14U, 16U);
                else if (edge.role == "PROG_ASSIGNMENT_TO_SBAC" || edge.role == "PROG_ASSIGNMENT_TO_SBNK")
                    allowed = allowed || in_range(offset, 0x120U + edge.ordinal * 0x38U, 16U);
            }
            EXPECT_TRUE(allowed) << node.object_type << ' ' << node.name << " changed byte " << offset;
        }
        if (node.object_type == "SMPL") {
            EXPECT_EQ(be32(*relocated, 0x6cU), 0x234U - 0xbaU);
            EXPECT_EQ(be32(*relocated, 0x78U), 0x234U);
        } else if (node.object_type == "SBNK") {
            EXPECT_EQ(be32(*relocated, 0xa0U), 0x234U);
            EXPECT_EQ(be32(*relocated, 0xc0U), 0x00000002U);
            EXPECT_EQ(be32(*relocated, 0xc4U), 0x00000001U);
            EXPECT_EQ(be32(*relocated, 0xc8U), 0x80000000U);
            EXPECT_EQ(be32(*relocated, 0xccU), 0x80000000U);
        } else if (node.object_type == "SBAC") {
            const auto relocated_decoded = axk::decode_object(*relocated);
            ASSERT_TRUE(relocated_decoded) << relocated_decoded.error().message;
            const auto *group = std::get_if<axk::CurrentSbac>(&relocated_decoded->payload);
            ASSERT_NE(group, nullptr);
            EXPECT_TRUE(std::ranges::all_of(group->slots, [](const auto &slot) { return slot.raw_handle == 0U; }));
        } else if (node.object_type == "PROG") {
            const auto relocated_decoded = axk::decode_object(*relocated);
            ASSERT_TRUE(relocated_decoded) << relocated_decoded.error().message;
            const auto *program = std::get_if<axk::CurrentProg>(&relocated_decoded->payload);
            ASSERT_NE(program, nullptr);
            EXPECT_TRUE(std::ranges::all_of(program->assignments, [](const auto &assignment) {
                return assignment.name.empty() || (assignment.kind != 0x10U && assignment.kind != 0x11U) ||
                       assignment.raw_handle == 0U;
            }));
        }
    }
    EXPECT_EQ(object_types, (std::set<std::string>{"PROG", "SBAC", "SBNK", "SMPL"}));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportPlanner, ReusesAnExactExistingSfsClosureWithoutAllocation) {
    auto source = axk::open_media(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave")};
    const auto built = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(built) << built.error().message;

    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "New Volume"));
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflict_summary(*plan);
    ASSERT_EQ(plan->objects.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    ASSERT_EQ(plan->allocation.size(), 1U);
    EXPECT_EQ(plan->allocation.front().inserted_object_count, 0U);
    EXPECT_EQ(plan->allocation.front().reused_object_count, 2U);
    EXPECT_EQ(plan->allocation.front().payload_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().directory_growth_bytes, 0U);
    EXPECT_EQ(plan->target_snapshot_id.size(), 64U);
    EXPECT_EQ(plan->policy_digest.size(), 64U);
    EXPECT_EQ(plan->plan_id.size(), 64U);
}

TEST(PackageImportPlanner, ReservesOneSfsObjectForSharedIncomingRoots) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;

    axk::PackageImportRequest request;
    request.root_destinations = {destination(0U, "New Volume"), destination(1U, "New Volume")};
    const std::vector packages{built->package, built->package};
    const auto first = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, request);
    const auto second = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, request);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(first->valid());
    EXPECT_EQ(first->plan_id, second->plan_id);
    ASSERT_EQ(first->objects.size(), 2U);
    EXPECT_EQ(std::ranges::count_if(first->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              1);
    EXPECT_EQ(std::ranges::count_if(first->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              1);
    const auto inserted = std::ranges::find_if(first->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    });
    const auto reused = std::ranges::find_if(first->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse);
    });
    ASSERT_NE(inserted, first->objects.end());
    ASSERT_NE(reused, first->objects.end());
    ASSERT_TRUE(inserted->target_sfs_id);
    ASSERT_TRUE(inserted->target_link_id);
    EXPECT_EQ(reused->canonical_action_id, inserted->action_id);
    EXPECT_EQ(reused->target_sfs_id, inserted->target_sfs_id);
    EXPECT_EQ(reused->target_link_id, inserted->target_link_id);
    const auto expect_rejected_canonical = [&](auto mutate) {
        auto tampered = *first;
        const auto tampered_reuse = std::ranges::find_if(
            tampered.objects, [](const auto &object) { return object.canonical_action_id.has_value(); });
        ASSERT_NE(tampered_reuse, tampered.objects.end());
        mutate(*tampered_reuse);
        const auto verified = axk::verify_package_import_plan(tampered);
        ASSERT_FALSE(verified);
    };
    expect_rejected_canonical([](auto &object) { object.volume_name = "Other Volume"; });
    expect_rejected_canonical([](auto &object) { object.object_type = "SBNK"; });
    expect_rejected_canonical([](auto &object) { object.destination_name = "Other Name"; });
    expect_rejected_canonical(
        [](auto &object) { object.normalized_sha256.front() = object.normalized_sha256.front() == '0' ? '1' : '0'; });
    ASSERT_EQ(first->allocation.size(), 1U);
    EXPECT_EQ(first->allocation.front().inserted_object_count, 1U);
    EXPECT_EQ(first->allocation.front().reused_object_count, 1U);
    EXPECT_EQ(first->allocation.front().directory_growth_bytes, 32U);
}

TEST(PackageImportPlanner, CollectsIncomingNameConflictsAndAcceptsExplicitRename) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->size(), 1U);
    auto original = standalone_smpl_package(objects->front().raw_payload);
    ASSERT_TRUE(original) << original.error().message;
    auto changed_payload = objects->front().raw_payload;
    changed_payload.back() ^= std::byte{0x01};
    auto changed = standalone_smpl_package(std::move(changed_payload));
    ASSERT_TRUE(changed) << changed.error().message;
    ASSERT_NE(original->package.nodes.front().normalized_sha256, changed->package.nodes.front().normalized_sha256);

    const std::vector packages{original->package, changed->package};
    axk::PackageImportRequest conflicting;
    conflicting.root_destinations = {destination(0U, "New Volume"), destination(1U, "New Volume")};
    const auto rejected = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, conflicting);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_FALSE(rejected->valid());
    EXPECT_TRUE(std::ranges::any_of(rejected->conflicts,
                                    [](const auto &conflict) { return conflict.code == "SFS_NAME_CONFLICT"; }));
    EXPECT_EQ(std::ranges::count_if(rejected->conflicts,
                                    [](const auto &conflict) { return conflict.code == "SFS_NAME_CONFLICT"; }),
              2);
    EXPECT_GE(std::ranges::count_if(rejected->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::conflict);
                                    }),
              2);

    auto renamed = conflicting;
    renamed.policy.renames.push_back({1U, changed->package.nodes.front().node_id, "TEST ALT"});
    const auto accepted = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, renamed);
    ASSERT_TRUE(accepted) << accepted.error().message;
    ASSERT_TRUE(accepted->valid());
    EXPECT_EQ(std::ranges::count_if(accepted->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              2);
    EXPECT_TRUE(std::ranges::any_of(accepted->objects, [](const auto &object) {
        return object.destination_name == "TEST ALT" &&
               std::ranges::contains(object.actions, axk::PackageImportObjectAction::rename);
    }));

    const std::vector equal_pcm_packages{original->package, original->package};
    auto distinct_names = conflicting;
    distinct_names.policy.renames.push_back({1U, original->package.nodes.front().node_id, "TEST ALT"});
    const auto name_sensitive =
        axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), equal_pcm_packages, distinct_names);
    ASSERT_TRUE(name_sensitive) << name_sensitive.error().message;
    ASSERT_TRUE(name_sensitive->valid()) << conflict_summary(*name_sensitive);
    EXPECT_EQ(std::ranges::count_if(name_sensitive->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              2);
    EXPECT_EQ(std::ranges::count_if(name_sensitive->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              0);

    const auto fat_root = publication_root("axklib-package-fat12-name-conflicts");
    const auto fat_target = fat_root / "target.ima";
    std::error_code error;
    std::filesystem::remove_all(fat_root, error);
    std::filesystem::create_directories(fat_root);
    ASSERT_TRUE(write_file(fat_target, fat_fixture()));
    axk::PackageImportRequest fat_conflicting;
    fat_conflicting.root_destinations = {destination(0U, "FAT root"), destination(1U, "FAT root")};
    const auto fat_rejected = axk::plan_package_import(fat_target, packages, fat_conflicting);
    ASSERT_TRUE(fat_rejected) << fat_rejected.error().message;
    EXPECT_FALSE(fat_rejected->valid());
    EXPECT_TRUE(std::ranges::any_of(fat_rejected->conflicts,
                                    [](const auto &conflict) { return conflict.code == "FAT12_NAME_CONFLICT"; }));

    auto fat_distinct_names = fat_conflicting;
    fat_distinct_names.policy.renames.push_back({1U, original->package.nodes.front().node_id, "TEST ALT"});
    const auto fat_name_sensitive = axk::plan_package_import(fat_target, equal_pcm_packages, fat_distinct_names);
    ASSERT_TRUE(fat_name_sensitive) << fat_name_sensitive.error().message;
    ASSERT_TRUE(fat_name_sensitive->valid()) << conflict_summary(*fat_name_sensitive);
    EXPECT_EQ(std::ranges::count_if(fat_name_sensitive->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              1);
    EXPECT_EQ(std::ranges::count_if(fat_name_sensitive->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              1);
    std::filesystem::remove_all(fat_root, error);
}

TEST(PackageImportPlanner, ReportsInsufficientSfsAndFat12CapacityBeforeApply) {
    const auto output_root = publication_root("axklib-package-capacity-conflict");
    const auto audio_path = output_root / "large.wav";
    const auto source_path = output_root / "source.hds";
    const auto target_path = output_root / "target.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 1024U * 1024U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));

    axk::VolumeSpec source_volume;
    source_volume.name = "Source";
    source_volume.waveforms.push_back({"large", "Large Wave", audio_path, 60U, {}});
    axk::HdsBuildManifest source_manifest{"1.0", 4U * 1024U * 1024U, {}};
    source_manifest.partitions.push_back({"P1", {std::move(source_volume)}});
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source_path));

    axk::VolumeSpec target_volume;
    target_volume.name = "Target";
    axk::HdsBuildManifest target_manifest{"1.0", 1024U * 1024U, {}};
    target_manifest.partitions.push_back({"P1", {std::move(target_volume)}});
    ASSERT_TRUE(axk::write_hds_image(target_manifest, target_path));

    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector selectors{root(axk::PackageRootKind::smpl, "Source", "Large Wave")};
    const auto built = axk::build_portable_package(*source, selectors);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "Target"));
    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(plan->conflicts,
                                    [](const auto &conflict) { return conflict.code == "SFS_CLUSTER_EXHAUSTED"; }));
    EXPECT_TRUE(std::ranges::any_of(plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::conflict);
    }));

    const auto fat_target = output_root / "target.ima";
    ASSERT_TRUE(write_file(fat_target, fat_fixture()));
    axk::PackageImportRequest fat_request;
    fat_request.root_destinations.push_back(destination(0U, "FAT root"));
    const auto fat_plan = axk::plan_package_import(fat_target, packages, fat_request);
    ASSERT_TRUE(fat_plan) << fat_plan.error().message;
    EXPECT_FALSE(fat_plan->valid());
    EXPECT_TRUE(std::ranges::any_of(fat_plan->conflicts,
                                    [](const auto &conflict) { return conflict.code == "FAT12_CLUSTER_EXHAUSTED"; }));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportPlanner, RejectsFat12RootExhaustionAndInvalidExistingChains) {
    const auto output_root = publication_root("axklib-package-fat12-capacity");
    const auto full_target = output_root / "full.ima";
    const auto invalid_target = output_root / "invalid.ima";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);

    axk::detail::PreparedMediaImage full;
    full.manifest.schema_version = "1.0";
    full.manifest.format = axk::MediaImageFormat::fat12_floppy;
    for (std::size_t index = 0U; index < 224U; ++index)
        full.retained_files.push_back({std::format("R{:07}.DAT", index), {static_cast<std::byte>(index)}});
    const auto full_written = axk::detail::write_fat12_image(full, full_target, {});
    ASSERT_TRUE(full_written) << full_written.error().message;
    const auto full_image = axk::FatImage::open(full_target);
    ASSERT_TRUE(full_image) << full_image.error().message;
    EXPECT_EQ(full_image->files().size(), 224U);

    const auto built = fat_smpl_package();
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "FAT root"));
    const auto full_plan = axk::plan_package_import(full_target, packages, request);
    ASSERT_TRUE(full_plan) << full_plan.error().message;
    EXPECT_FALSE(full_plan->valid());
    EXPECT_TRUE(std::ranges::any_of(
        full_plan->conflicts, [](const auto &conflict) { return conflict.code == "FAT12_ROOT_ENTRY_EXHAUSTED"; }));

    const auto invalid_bytes = fat_fixture(2U);
    ASSERT_TRUE(write_file(invalid_target, invalid_bytes));
    const auto invalid_plan = axk::plan_package_import(invalid_target, packages, request);
    ASSERT_FALSE(invalid_plan);
    EXPECT_EQ(read_file(invalid_target), invalid_bytes);
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportPlanner, ReportsSfsObjectAndDirectoryCapacityBeforeApply) {
    const auto built = fat_smpl_package();
    ASSERT_TRUE(built) << built.error().message;
    constexpr std::size_t package_count = 8192U;
    const std::vector packages(package_count, built->package);
    axk::PackageImportRequest request;
    request.root_destinations.reserve(package_count);
    request.policy.renames.reserve(package_count);
    for (std::size_t index = 0U; index < package_count; ++index) {
        request.root_destinations.push_back(destination(index, "New Volume"));
        request.policy.renames.push_back({index, built->package.nodes.front().node_id, std::format("W{:04}", index)});
    }

    const auto target = fixture("HD00_512_single_sbnk_authored.hds");
    const auto before = read_file(target);
    const auto plan = axk::plan_package_import(target, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(plan->conflicts,
                                    [](const auto &conflict) { return conflict.code == "SFS_OBJECT_ID_EXHAUSTED"; }));
    EXPECT_TRUE(std::ranges::any_of(
        plan->conflicts, [](const auto &conflict) { return conflict.code == "SFS_DIRECTORY_CAPACITY_EXHAUSTED"; }));
    EXPECT_EQ(read_file(target), before);
}

TEST(PackageImportPlanner, ReportsIsoDirectoryCapacityBeforeApply) {
    const auto output_root = publication_root("axklib-package-iso-capacity");
    const auto audio_path = output_root / "tone.wav";
    const auto target_path = output_root / "target.iso";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, tiny_waveform(1000)));
    axk::MediaBuildManifest target_manifest;
    target_manifest.schema_version = "1.0";
    target_manifest.format = axk::MediaImageFormat::iso9660;
    target_manifest.iso_volume_id = "CAPACITY_TEST";
    target_manifest.group_name = "Target Group";
    target_manifest.raw_group = "GROUP";
    target_manifest.volume_name = "Target Volume";
    target_manifest.raw_volume = "F001";
    target_manifest.authored_volume = single_bank_volume(audio_path, "Target Volume", "Target Wave", "Target Bank");
    ASSERT_TRUE(axk::write_media_image(target_manifest, target_path));

    const auto built = fat_smpl_package();
    ASSERT_TRUE(built) << built.error().message;
    constexpr std::size_t package_count = 50U;
    const std::vector packages(package_count, built->package);
    axk::PackageImportRequest request;
    request.root_destinations.reserve(package_count);
    request.policy.renames.reserve(package_count);
    for (std::size_t index = 0U; index < package_count; ++index) {
        auto target = destination(index, 0U, "Target Volume");
        target.group_name = "Target Group";
        target.raw_group = "GROUP";
        target.raw_volume = "F001";
        request.root_destinations.push_back(std::move(target));
        request.policy.renames.push_back({index, built->package.nodes.front().node_id, std::format("W{:04}", index)});
    }

    const auto before = read_file(target_path);
    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(
        plan->conflicts, [](const auto &conflict) { return conflict.code == "ISO9660_DIRECTORY_CAPACITY_EXHAUSTED"; }));
    EXPECT_EQ(read_file(target_path), before);
    std::filesystem::remove_all(output_root, error);
}

TEST(MediaWriter, RejectsIsoSectorCountOverflowWithoutAllocatingAnImage) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const std::array<std::uint64_t, 1> boundary{(static_cast<std::uint64_t>(maximum) - 21U) * 2048U};
    const auto accepted = axk::detail::checked_iso9660_sector_count(1U, boundary);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_EQ(*accepted, maximum);

    const std::array<std::uint64_t, 1> overflow{(static_cast<std::uint64_t>(maximum) - 20U) * 2048U};
    const auto rejected = axk::detail::checked_iso9660_sector_count(1U, overflow);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::unsupported_profile);
}

TEST(PackageImportPlanner, ReportsMissingDestinationMappings) {
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;

    axk::PackageImportRequest request;
    const std::vector packages{built->package};
    const auto plan = axk::plan_package_import(fixture("HD00_512_single_sbnk_authored.hds"), packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->valid());
    EXPECT_TRUE(std::ranges::any_of(plan->conflicts,
                                    [](const auto &conflict) { return conflict.code == "DESTINATION_ROOT_MISSING"; }));
}

TEST(PackageImportApply, AtomicallyInsertsAndThenReusesAnExactSmpl) {
    const auto output_root = publication_root("axklib-package-import-apply");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "New Volume"));
    const auto source_path = fixture("HD00_512_single_sbnk_authored.hds");
    const auto source_before = read_file(source_path);

    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->objects.size(), 1U);
    ASSERT_TRUE(plan->objects.front().target_sfs_id);
    ASSERT_TRUE(plan->objects.front().target_link_id);
    EXPECT_TRUE(std::ranges::contains(plan->objects.front().actions, axk::PackageImportObjectAction::insert));

    const auto first_output = output_root / "first.hds";
    const auto applied = axk::apply_package_import(source_path, packages, *plan, first_output, false);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->applied);
    EXPECT_EQ(applied->plan_id, plan->plan_id);
    EXPECT_EQ(applied->source_snapshot_id, plan->target_snapshot_id);
    EXPECT_EQ(applied->output_snapshot_id.size(), 64U);
    EXPECT_EQ(read_file(source_path), source_before);

    auto reopened = axk::open_media(first_output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    std::vector<const axk::ObjectSnapshot *> imported;
    for (const auto &object : catalog->objects) {
        if (object.placement && object.placement->volume_name == "New Volume" &&
            object.object.header.raw_type == "SMPL" && object.object.header.name == "TEST") {
            imported.push_back(&object);
        }
    }
    ASSERT_EQ(imported.size(), 1U);
    EXPECT_EQ(imported.front()->sfs_id.value, *plan->objects.front().target_sfs_id);
    const auto *sample = std::get_if<axk::CurrentSmpl>(&imported.front()->object.payload);
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->link_id.value, *plan->objects.front().target_link_id);

    const auto repeat_plan = axk::plan_package_import(first_output, packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    ASSERT_EQ(repeat_plan->objects.size(), 1U);
    EXPECT_TRUE(std::ranges::contains(repeat_plan->objects.front().actions, axk::PackageImportObjectAction::reuse));
    ASSERT_EQ(repeat_plan->allocation.size(), 1U);
    EXPECT_EQ(repeat_plan->allocation.front().inserted_object_count, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().payload_clusters, 0U);
    const auto second_output = output_root / "second.hds";
    const auto repeated = axk::apply_package_import(first_output, packages, *repeat_plan, second_output, false);
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(read_file(second_output), read_file(first_output));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, AppliesVolumeLocalReuseAndPartitionIsolationInOnePlan) {
    const auto output_root = publication_root("axklib-package-import-scope-matrix");
    const auto target_path = output_root / "target.hds";
    const auto output_path = output_root / "imported.hds";
    const auto repeated_path = output_root / "repeated.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);

    axk::VolumeSpec volume_a;
    volume_a.name = "Volume A";
    axk::VolumeSpec volume_b;
    volume_b.name = "Volume B";
    axk::VolumeSpec volume_c;
    volume_c.name = "Volume C";
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {std::move(volume_a), std::move(volume_b)}});
    manifest.partitions.push_back({"P2", {std::move(volume_c)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, target_path));

    const auto built = fat_smpl_package();
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package, built->package, built->package, built->package};
    axk::PackageImportRequest request;
    request.root_destinations = {destination(0U, 0U, "Volume A"), destination(1U, 0U, "Volume A"),
                                 destination(2U, 0U, "Volume B"), destination(3U, 1U, "Volume C")};

    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    EXPECT_EQ(std::ranges::count_if(plan->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              3);
    EXPECT_EQ(std::ranges::count_if(plan->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              1);
    ASSERT_EQ(plan->allocation.size(), 3U);
    EXPECT_EQ(std::ranges::count_if(plan->allocation,
                                    [](const auto &allocation) { return allocation.inserted_object_count == 1U; }),
              3);

    const auto applied = axk::apply_package_import(target_path, packages, *plan, output_path);
    ASSERT_TRUE(applied) << applied.error().message;
    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto count_sample = [&](std::uint8_t partition, std::string_view volume) {
        return std::ranges::count_if(catalog->objects, [&](const auto &object) {
            return object.partition.value == partition && object.placement && object.placement->volume_name == volume &&
                   object.object.header.raw_type == "SMPL" && object.object.header.name == "TEST";
        });
    };
    EXPECT_EQ(count_sample(0U, "Volume A"), 1);
    EXPECT_EQ(count_sample(0U, "Volume B"), 1);
    EXPECT_EQ(count_sample(1U, "Volume C"), 1);

    const auto repeat_plan = axk::plan_package_import(output_path, packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    ASSERT_TRUE(axk::apply_package_import(output_path, packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(output_path), read_file(repeated_path));
}

TEST(PackageImportApply, RelocatesACompleteRenamedSbnkClosureAndReopensItsGraph) {
    const auto output_root = publication_root("axklib-package-import-sbnk");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    const auto source_path = fixture("HD00_512_single_sbnk_authored.hds");
    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave")};
    const auto built = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "New Volume"));
    for (const auto &node : built->package.nodes) {
        request.policy.renames.push_back({0U, node.node_id, node.object_type == "SBNK" ? "Bank Copy" : "Wave Copy"});
    }
    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_EQ(plan->objects.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::rename) &&
               std::ranges::contains(object.actions, axk::PackageImportObjectAction::relocate) &&
               std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));

    const auto output = output_root / "renamed-bank.hds";
    const auto applied = axk::apply_package_import(source_path, packages, *plan, output);
    ASSERT_TRUE(applied) << applied.error().message;
    auto reopened = axk::open_media(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto bank = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "New Volume" &&
               object.object.header.raw_type == "SBNK" && object.object.header.name == "Bank Copy";
    });
    const auto sample = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "New Volume" &&
               object.object.header.raw_type == "SMPL" && object.object.header.name == "Wave Copy";
    });
    ASSERT_NE(bank, catalog->objects.end());
    ASSERT_NE(sample, catalog->objects.end());
    const auto *decoded_bank = std::get_if<axk::CurrentSbnk>(&bank->object.payload);
    const auto *decoded_sample = std::get_if<axk::CurrentSmpl>(&sample->object.payload);
    ASSERT_NE(decoded_bank, nullptr);
    ASSERT_NE(decoded_sample, nullptr);
    EXPECT_EQ(decoded_bank->left.sample_name, "Wave Copy");
    EXPECT_EQ(decoded_bank->left.smpl_link_id, decoded_sample->link_id.value);
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto children = graph.children(bank->key);
    EXPECT_TRUE(std::ranges::any_of(children, [&](const axk::Relationship *edge) {
        return edge->type == "SBNK_LEFT_MEMBER_TO_SMPL" && edge->quality == axk::RelationshipQuality::known &&
               edge->target_key == sample->key;
    }));

    const auto repeat_plan = axk::plan_package_import(output, packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    const auto repeated = output_root / "renamed-bank-repeat.hds";
    ASSERT_TRUE(axk::apply_package_import(output, packages, *repeat_plan, repeated));
    EXPECT_EQ(read_file(output), read_file(repeated));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, PreservesStereoClosureAndExactPhysicalPcm) {
    const auto output_root = publication_root("axklib-package-import-stereo");
    const auto left_path = output_root / "left.wav";
    const auto right_path = output_root / "right.wav";
    const auto source_path = output_root / "source.hds";
    const auto output_path = output_root / "imported.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);

    axk::Waveform left;
    left.format = {1U, 2U, 44100U};
    left.frame_count = 4U;
    left.pcm = {std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00},
                std::byte{0x03}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00}};
    auto right = left;
    right.pcm = {std::byte{0xf1}, std::byte{0xff}, std::byte{0xf2}, std::byte{0xff},
                 std::byte{0xf3}, std::byte{0xff}, std::byte{0xf4}, std::byte{0xff}};
    ASSERT_TRUE(axk::write_wav_atomic(left_path, left));
    ASSERT_TRUE(axk::write_wav_atomic(right_path, right));

    axk::VolumeSpec volume;
    volume.name = "Stereo Source";
    volume.waveforms.push_back({"left", "Stereo L", left_path, 60U, {}});
    volume.waveforms.push_back({"right", "Stereo R", right_path, 60U, {}});
    axk::SampleBankSpec bank;
    bank.name = "Stereo Bank";
    bank.waveform_id = "left";
    bank.right_waveform_id = "right";
    bank.root_key = 60U;
    bank.key_high = 127U;
    volume.sample_banks.push_back(std::move(bank));
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source_path));

    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "Stereo Source", "Stereo Bank")};
    const auto built = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(built) << built.error().message;
    ASSERT_EQ(built->package.nodes.size(), 3U);
    ASSERT_EQ(built->package.relationships.size(), 2U);
    const auto &source_container = std::get<axk::Container>(source->storage());
    const auto source_catalog = axk::build_object_catalog(source_container);
    ASSERT_TRUE(source_catalog) << source_catalog.error().message;
    const auto source_left = std::ranges::find_if(source_catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Stereo Source" &&
               object.object.header.raw_type == "SMPL" && object.object.header.name == "Stereo L";
    });
    const auto source_right = std::ranges::find_if(source_catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Stereo Source" &&
               object.object.header.raw_type == "SMPL" && object.object.header.name == "Stereo R";
    });
    ASSERT_NE(source_left, source_catalog->objects.end());
    ASSERT_NE(source_right, source_catalog->objects.end());
    const auto source_left_waveform = axk::decode_waveform(source_container, *source_left);
    const auto source_right_waveform = axk::decode_waveform(source_container, *source_right);
    ASSERT_TRUE(source_left_waveform) << source_left_waveform.error().message;
    ASSERT_TRUE(source_right_waveform) << source_right_waveform.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    auto target = destination(0U, "Stereo Imported");
    target.create_destination = true;
    request.root_destinations.push_back(std::move(target));
    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_TRUE(axk::apply_package_import(source_path, packages, *plan, output_path));

    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto imported_bank = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Stereo Imported" &&
               object.object.header.raw_type == "SBNK" && object.object.header.name == "Stereo Bank";
    });
    const auto imported_left = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Stereo Imported" &&
               object.object.header.raw_type == "SMPL" && object.object.header.name == "Stereo L";
    });
    const auto imported_right = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Stereo Imported" &&
               object.object.header.raw_type == "SMPL" && object.object.header.name == "Stereo R";
    });
    ASSERT_NE(imported_bank, catalog->objects.end());
    ASSERT_NE(imported_left, catalog->objects.end());
    ASSERT_NE(imported_right, catalog->objects.end());
    const auto *decoded_bank = std::get_if<axk::CurrentSbnk>(&imported_bank->object.payload);
    ASSERT_NE(decoded_bank, nullptr);
    ASSERT_TRUE(decoded_bank->right);
    EXPECT_EQ(decoded_bank->left.sample_name, "Stereo L");
    EXPECT_EQ(decoded_bank->right->sample_name, "Stereo R");
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto children = graph.children(imported_bank->key);
    EXPECT_TRUE(std::ranges::any_of(children, [&](const axk::Relationship *edge) {
        return edge->type == "SBNK_LEFT_MEMBER_TO_SMPL" && edge->quality == axk::RelationshipQuality::known &&
               edge->target_key == imported_left->key;
    }));
    EXPECT_TRUE(std::ranges::any_of(children, [&](const axk::Relationship *edge) {
        return edge->type == "SBNK_RIGHT_MEMBER_TO_SMPL" && edge->quality == axk::RelationshipQuality::known &&
               edge->target_key == imported_right->key;
    }));
    const auto &container = std::get<axk::Container>(reopened->storage());
    const auto decoded_left = axk::decode_waveform(container, *imported_left);
    const auto decoded_right = axk::decode_waveform(container, *imported_right);
    ASSERT_TRUE(decoded_left) << decoded_left.error().message;
    ASSERT_TRUE(decoded_right) << decoded_right.error().message;
    EXPECT_EQ(decoded_left->pcm, source_left_waveform->pcm);
    EXPECT_EQ(decoded_right->pcm, source_right_waveform->pcm);

    const auto fat_target_path = output_root / "target.ima";
    const auto fat_output_path = output_root / "imported.ima";
    ASSERT_TRUE(write_file(fat_target_path, fat_fixture()));
    axk::PackageImportRequest fat_request;
    fat_request.root_destinations.push_back(destination(0U, "FAT root"));
    const auto fat_plan = axk::plan_package_import(fat_target_path, packages, fat_request);
    ASSERT_TRUE(fat_plan) << fat_plan.error().message;
    ASSERT_TRUE(fat_plan->valid()) << conflict_summary(*fat_plan);
    ASSERT_TRUE(axk::apply_package_import(fat_target_path, packages, *fat_plan, fat_output_path, false));
    auto fat_reopened = axk::open_media(fat_output_path);
    ASSERT_TRUE(fat_reopened) << fat_reopened.error().message;
    EXPECT_TRUE(fat_reopened->validation_issues().empty());
    expect_package_audio(*fat_reopened, packages);
    const auto fat_catalog = axk::build_object_catalog(*fat_reopened);
    ASSERT_TRUE(fat_catalog) << fat_catalog.error().message;
    const auto fat_bank = std::ranges::find_if(fat_catalog->objects, [](const auto &object) {
        return object.object.header.raw_type == "SBNK" && object.object.header.name == "Stereo Bank";
    });
    ASSERT_NE(fat_bank, fat_catalog->objects.end());
    const auto fat_graph = axk::build_relationship_graph(*fat_catalog);
    EXPECT_EQ(std::ranges::count_if(fat_graph.children(fat_bank->key),
                                    [](const axk::Relationship *edge) {
                                        return (edge->type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
                                                edge->type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
                                               edge->quality == axk::RelationshipQuality::known &&
                                               edge->target_key.has_value();
                                    }),
              2);
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, CreatesAnExplicitDestinationWithFullyPlannedScaffolding) {
    const auto output_root = publication_root("axklib-package-import-new-volume");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    auto new_destination = destination(0U, "Imported Volume");
    new_destination.create_destination = true;
    request.root_destinations.push_back(std::move(new_destination));
    const auto source_path = fixture("HD00_512_single_sbnk_authored.hds");
    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_EQ(plan->destinations.size(), 1U);
    EXPECT_TRUE(plan->destinations.front().create);
    EXPECT_EQ(plan->destinations.front().infrastructure_sfs_ids.size(), 6U);
    EXPECT_EQ(plan->destinations.front().infrastructure_clusters, 12U);
    EXPECT_EQ(plan->destinations.front().root_directory_growth_bytes, 32U);
    ASSERT_EQ(plan->allocation.size(), 1U);
    EXPECT_EQ(plan->allocation.front().directory_growth_bytes, 64U);

    const auto output = output_root / "new-volume.hds";
    const auto applied = axk::apply_package_import(source_path, packages, *plan, output);
    ASSERT_TRUE(applied) << applied.error().message;
    auto reopened = axk::open_media(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    EXPECT_EQ(std::ranges::count_if(catalog->objects,
                                    [](const auto &object) {
                                        return object.placement && object.placement->volume_name == "Imported Volume" &&
                                               object.object.header.raw_type == "SMPL" &&
                                               object.object.header.name == "TEST";
                                    }),
              1);
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, ImportsACompleteProgramGroupBankAndWaveformGraph) {
    const auto output_root = publication_root("axklib-package-import-program");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    const auto output_path = output_root / "imported.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {graph_volume(audio_path)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source_path));
    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector program_root{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    const auto built = axk::build_portable_package(*source, program_root);
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_EQ(built->package.kind, axk::PackageKind::program);
    EXPECT_EQ(built->required_extension, ".axkprg");
    EXPECT_EQ(built->package.nodes.size(), 5U);
    EXPECT_EQ(built->package.relationships.size(), 5U);

    const auto program_node =
        std::ranges::find(built->package.nodes, std::string{"PROG"}, &axk::PackageNode::object_type);
    ASSERT_NE(program_node, built->package.nodes.end());
    axk::PackageImportRequest invalid_program_name;
    auto invalid_target = destination(0U, "Invalid Program Name");
    invalid_target.create_destination = true;
    invalid_program_name.root_destinations.push_back(std::move(invalid_target));
    invalid_program_name.policy.renames.push_back({0U, program_node->node_id, "1"});
    const std::vector packages{built->package};
    const auto rejected = axk::plan_package_import(source_path, packages, invalid_program_name);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_FALSE(rejected->valid());
    EXPECT_TRUE(std::ranges::any_of(rejected->conflicts, [](const auto &conflict) {
        return conflict.code == "SFS_PROGRAM_SLOT_INVALID" &&
               conflict.message.find("001 through 128") != std::string::npos;
    }));

    axk::PackageImportRequest request;
    auto target = destination(0U, "Imported Graph");
    target.create_destination = true;
    request.root_destinations.push_back(std::move(target));
    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    EXPECT_EQ(plan->objects.size(), 5U);
    const auto applied = axk::apply_package_import(source_path, packages, *plan, output_path);
    ASSERT_TRUE(applied) << applied.error().message;

    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto in_imported_volume = [](const auto &object) {
        return object.placement && object.placement->volume_name == "Imported Graph";
    };
    EXPECT_EQ(std::ranges::count_if(catalog->objects, in_imported_volume), 5);
    const auto grouped_bank = std::ranges::find_if(catalog->objects, [&](const auto &object) {
        return in_imported_volume(object) && object.object.header.raw_type == "SBNK" &&
               object.object.header.name == "Grouped Bank";
    });
    const auto direct_bank = std::ranges::find_if(catalog->objects, [&](const auto &object) {
        return in_imported_volume(object) && object.object.header.raw_type == "SBNK" &&
               object.object.header.name == "Direct Bank";
    });
    ASSERT_NE(grouped_bank, catalog->objects.end());
    ASSERT_NE(direct_bank, catalog->objects.end());
    const auto *grouped = std::get_if<axk::CurrentSbnk>(&grouped_bank->object.payload);
    const auto *direct = std::get_if<axk::CurrentSbnk>(&direct_bank->object.payload);
    ASSERT_NE(grouped, nullptr);
    ASSERT_NE(direct, nullptr);
    EXPECT_NE(grouped->sample_flags & 1U, 0U);
    EXPECT_TRUE(direct->linked_program_numbers == std::vector<std::uint8_t>{1U});
    const auto graph = axk::build_relationship_graph(*catalog);
    for (const auto &object : plan->objects) {
        const auto actual = std::ranges::find_if(catalog->objects, [&](const auto &snapshot) {
            return object.target_sfs_id && snapshot.partition.value == object.partition_index &&
                   snapshot.sfs_id.value == *object.target_sfs_id;
        });
        const auto expected_children = std::ranges::count_if(
            built->package.relationships, [&](const auto &edge) { return edge.source_node_id == object.node_id; });
        ASSERT_NE(actual, catalog->objects.end()) << object.object_type << ' ' << object.destination_name;
        const auto actual_children = graph.children(actual->key);
        const auto package_children = std::ranges::count_if(actual_children, [&](const auto *edge) {
            return std::ranges::any_of(built->package.relationships, [&](const auto &expected) {
                return expected.source_node_id == object.node_id && expected.role == edge->type;
            });
        });
        EXPECT_EQ(package_children, static_cast<std::size_t>(expected_children))
            << object.object_type << ' ' << object.destination_name;
    }

    const std::vector imported_program_root{root(axk::PackageRootKind::prog, "Imported Graph", "001")};
    const auto reexported = axk::build_portable_package(*reopened, imported_program_root);
    ASSERT_TRUE(reexported) << reexported.error().message;
    EXPECT_EQ(reexported->package.nodes.size(), built->package.nodes.size());
    EXPECT_EQ(reexported->package.relationships, built->package.relationships);
    const auto reexported_program =
        std::ranges::find(reexported->package.nodes, std::string{"PROG"}, &axk::PackageNode::object_type);
    ASSERT_NE(reexported_program, reexported->package.nodes.end());
    ASSERT_EQ(reexported_program->relocations.size(), program_node->relocations.size());
    for (const auto &relocation : reexported_program->relocations) {
        EXPECT_EQ(relocation.role, "PROG_ASSIGNMENT_HANDLE");
        EXPECT_EQ(relocation.edge_ids.size(), 1U);
    }
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, CancellationAtEveryGraphBoundaryPublishesNothing) {
    const auto output_root = publication_root("axklib-package-import-cancellation-boundaries");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"P1", {graph_volume(audio_path)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source_path));
    const auto source_before = read_file(source_path);

    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector program_root{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    const auto built = axk::build_portable_package(*source, program_root);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    auto target = destination(0U, "Cancelled Graph");
    target.create_destination = true;
    request.root_destinations.push_back(std::move(target));
    const auto plan = axk::plan_package_import(source_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_EQ(plan->objects.size(), 5U);

    for (std::uint64_t boundary = 0U; boundary <= plan->objects.size(); ++boundary) {
        SCOPED_TRACE(boundary);
        axk::CancellationSource cancellation;
        CancellingPackageProgress progress{cancellation, boundary};
        const auto output_path = output_root / ("cancelled-" + std::to_string(boundary) + ".hds");
        const auto applied = axk::apply_package_import(source_path, packages, *plan, output_path, false,
                                                       cancellation.token(), &progress);
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().code, axk::ErrorCode::operation_cancelled);
        EXPECT_FALSE(std::filesystem::exists(output_path));
        EXPECT_EQ(read_file(source_path), source_before);
    }
    for (const auto &entry : std::filesystem::directory_iterator(output_root)) {
        EXPECT_TRUE(entry.path() == audio_path || entry.path() == source_path) << entry.path().string();
    }
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, MergesPlannedGraphMetadataIntoReusedSampleBanks) {
    const auto output_root = publication_root("axklib-package-import-reused-banks");
    const auto audio_path = output_root / "tone.wav";
    const auto source_path = output_root / "source.hds";
    const auto target_path = output_root / "target.hds";
    const auto output_path = output_root / "imported.hds";
    const auto repeated_path = output_root / "repeated.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));

    axk::HdsBuildManifest source_manifest{"1.0", 4U * 1024U * 1024U, {}};
    source_manifest.partitions.push_back({"P1", {graph_volume(audio_path)}});
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source_path));
    auto target_volume = graph_volume(audio_path);
    target_volume.name = "Target Volume";
    target_volume.sample_bank_groups.clear();
    target_volume.programs.clear();
    axk::HdsBuildManifest target_manifest{"1.0", 4U * 1024U * 1024U, {}};
    target_manifest.partitions.push_back({"P1", {std::move(target_volume)}});
    ASSERT_TRUE(axk::write_hds_image(target_manifest, target_path));

    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector program_root{root(axk::PackageRootKind::prog, "Graph Volume", "001")};
    const auto built = axk::build_portable_package(*source, program_root);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "Target Volume"));
    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    EXPECT_EQ(std::ranges::count_if(plan->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              2);
    const auto grouped_bank = std::ranges::find_if(plan->objects, [](const auto &object) {
        return object.object_type == "SBNK" && object.destination_name == "Grouped Bank";
    });
    const auto direct_bank = std::ranges::find_if(plan->objects, [](const auto &object) {
        return object.object_type == "SBNK" && object.destination_name == "Direct Bank";
    });
    ASSERT_NE(grouped_bank, plan->objects.end());
    ASSERT_NE(direct_bank, plan->objects.end());
    EXPECT_TRUE(grouped_bank->target_grouped);
    EXPECT_TRUE(grouped_bank->target_program_numbers.empty());
    EXPECT_FALSE(direct_bank->target_grouped);
    EXPECT_TRUE(direct_bank->target_program_numbers == std::vector<std::uint8_t>{1U});
    for (const auto *bank : {&*grouped_bank, &*direct_bank}) {
        EXPECT_TRUE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::reuse));
        EXPECT_TRUE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::relocate));
        EXPECT_FALSE(std::ranges::contains(bank->actions, axk::PackageImportObjectAction::insert));
    }

    const auto applied = axk::apply_package_import(target_path, packages, *plan, output_path);
    ASSERT_TRUE(applied) << applied.error().message;
    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto imported_grouped = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Target Volume" &&
               object.object.header.raw_type == "SBNK" && object.object.header.name == "Grouped Bank";
    });
    const auto imported_direct = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Target Volume" &&
               object.object.header.raw_type == "SBNK" && object.object.header.name == "Direct Bank";
    });
    ASSERT_NE(imported_grouped, catalog->objects.end());
    ASSERT_NE(imported_direct, catalog->objects.end());
    const auto *grouped = std::get_if<axk::CurrentSbnk>(&imported_grouped->object.payload);
    const auto *direct = std::get_if<axk::CurrentSbnk>(&imported_direct->object.payload);
    ASSERT_NE(grouped, nullptr);
    ASSERT_NE(direct, nullptr);
    EXPECT_NE(grouped->sample_flags & 1U, 0U);
    EXPECT_TRUE(direct->linked_program_numbers == std::vector<std::uint8_t>{1U});

    const auto repeat_plan = axk::plan_package_import(output_path, packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::relocate);
    }));
    ASSERT_TRUE(axk::apply_package_import(output_path, packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(output_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, RejectsTamperedStaleAndCancelledPlansWithoutPublication) {
    const auto output_root = publication_root("axklib-package-import-reject");
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
    ASSERT_TRUE(image) << image.error().message;
    const axk::MediaContainer media{std::move(*image)};
    const std::vector roots{root(axk::PackageRootKind::smpl, "FAT root", "TEST")};
    const auto built = axk::build_portable_package(media, roots);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package};
    axk::PackageImportRequest request;
    request.root_destinations.push_back(destination(0U, "New Volume"));
    const auto mutable_target = output_root / "target.hds";
    ASSERT_TRUE(std::filesystem::copy_file(fixture("HD00_512_single_sbnk_authored.hds"), mutable_target));
    const auto plan = axk::plan_package_import(mutable_target, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());

    auto tampered = *plan;
    tampered.objects.front().destination_name = "ALTERED";
    EXPECT_FALSE(axk::verify_package_import_plan(tampered));
    const auto tampered_output = output_root / "tampered.hds";
    EXPECT_FALSE(axk::apply_package_import(mutable_target, packages, tampered, tampered_output, false));
    EXPECT_FALSE(std::filesystem::exists(tampered_output));

    {
        std::fstream target{mutable_target, std::ios::binary | std::ios::in | std::ios::out};
        target.seekg(-1, std::ios::end);
        char value{};
        target.read(&value, 1);
        value ^= 1;
        target.seekp(-1, std::ios::end);
        target.write(&value, 1);
    }
    const auto stale_output = output_root / "stale.hds";
    const auto stale = axk::apply_package_import(mutable_target, packages, *plan, stale_output, false);
    ASSERT_FALSE(stale);
    EXPECT_NE(stale.error().message.find("stale"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(stale_output));

    ASSERT_TRUE(std::filesystem::copy_file(fixture("HD00_512_single_sbnk_authored.hds"), mutable_target,
                                           std::filesystem::copy_options::overwrite_existing));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled_output = output_root / "cancelled.hds";
    const auto cancelled =
        axk::apply_package_import(mutable_target, packages, *plan, cancelled_output, false, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_output));
    for (const auto &entry : std::filesystem::directory_iterator(output_root))
        EXPECT_EQ(entry.path().extension(), ".hds");
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageRegressionMatrix, MixesSfsFat12AndIsoSourcesIntoSfsDeterministically) {
    const auto output_root = publication_root("axklib-package-mixed-sfs-matrix");
    const auto target_path = output_root / "target.hds";
    const auto first_path = output_root / "first.hds";
    const auto reversed_path = output_root / "reversed.hds";
    const auto repeated_path = output_root / "repeated.hds";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    auto packages = mixed_source_packages(output_root);
    ASSERT_TRUE(packages) << packages.error().message;
    ASSERT_EQ(packages->size(), 3U);
    EXPECT_EQ((*packages)[0].source_media_kind, "sfs");
    EXPECT_EQ((*packages)[1].source_media_kind, "fat12-floppy");
    EXPECT_EQ((*packages)[2].source_media_kind, "iso9660");
    EXPECT_EQ((*packages)[0].kind, axk::PackageKind::program);
    EXPECT_EQ((*packages)[1].kind, axk::PackageKind::smpl);
    EXPECT_EQ((*packages)[2].kind, axk::PackageKind::sbnk);

    axk::VolumeSpec target_volume;
    target_volume.name = "Mixed";
    axk::HdsBuildManifest target_manifest{"1.0", 8U * 1024U * 1024U, {}};
    target_manifest.partitions.push_back({"P1", {std::move(target_volume)}});
    ASSERT_TRUE(axk::write_hds_image(target_manifest, target_path));

    axk::PackageImportRequest request;
    request.root_destinations = {destination(0U, "Mixed"), destination(1U, "Mixed"), destination(2U, "Mixed")};
    const auto plan = axk::plan_package_import(target_path, *packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->objects.size(), 8U);
    ASSERT_EQ(plan->allocation.size(), 1U);
    EXPECT_EQ(plan->allocation.front().inserted_object_count, 8U);
    EXPECT_EQ(plan->allocation.front().reused_object_count, 0U);
    EXPECT_EQ(plan->allocation.front().payload_clusters, 16U);
    EXPECT_EQ(plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().directory_growth_bytes, 256U);
    const auto report = axk::apply_package_import(target_path, *packages, *plan, first_path);
    ASSERT_TRUE(report) << report.error().message;

    auto first = axk::open_media(first_path);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->validation_issues().empty());
    const auto first_signature = normalized_media_signature(*first);
    ASSERT_TRUE(first_signature) << first_signature.error().message;
    EXPECT_EQ(first_signature->size(), 8U);
    expect_package_audio(*first, *packages);

    std::vector<axk::PortablePackage> reversed_packages{(*packages)[2], (*packages)[1], (*packages)[0]};
    axk::PackageImportRequest reversed_request;
    reversed_request.root_destinations = {destination(0U, "Mixed"), destination(1U, "Mixed"), destination(2U, "Mixed")};
    const auto reversed_plan = axk::plan_package_import(target_path, reversed_packages, reversed_request);
    ASSERT_TRUE(reversed_plan) << reversed_plan.error().message;
    ASSERT_TRUE(reversed_plan->valid());
    ASSERT_EQ(reversed_plan->allocation.size(), 1U);
    EXPECT_EQ(reversed_plan->allocation.front().inserted_object_count, 8U);
    EXPECT_EQ(reversed_plan->allocation.front().reused_object_count, 0U);
    ASSERT_TRUE(axk::apply_package_import(target_path, reversed_packages, *reversed_plan, reversed_path));
    auto reversed = axk::open_media(reversed_path);
    ASSERT_TRUE(reversed) << reversed.error().message;
    const auto reversed_signature = normalized_media_signature(*reversed);
    ASSERT_TRUE(reversed_signature) << reversed_signature.error().message;
    EXPECT_EQ(*reversed_signature, *first_signature);
    expect_package_audio(*reversed, reversed_packages);

    const auto repeat_plan = axk::plan_package_import(first_path, *packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    ASSERT_EQ(repeat_plan->allocation.size(), 1U);
    EXPECT_EQ(repeat_plan->allocation.front().inserted_object_count, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().reused_object_count, 8U);
    EXPECT_EQ(repeat_plan->allocation.front().payload_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().directory_growth_bytes, 0U);
    ASSERT_TRUE(axk::apply_package_import(first_path, *packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(first_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageRegressionMatrix, MixesSfsFat12AndIsoSourcesIntoFat12Deterministically) {
    const auto output_root = publication_root("axklib-package-mixed-fat12-matrix");
    const auto target_path = output_root / "target.ima";
    const auto first_path = output_root / "first.ima";
    const auto reversed_path = output_root / "reversed.ima";
    const auto repeated_path = output_root / "repeated.ima";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    auto packages = mixed_source_packages(output_root);
    ASSERT_TRUE(packages) << packages.error().message;
    ASSERT_EQ(packages->size(), 3U);

    auto seed = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "seed.ima");
    ASSERT_TRUE(seed) << seed.error().message;
    const auto seed_objects = seed->objects();
    ASSERT_TRUE(seed_objects) << seed_objects.error().message;
    ASSERT_EQ(seed_objects->size(), 1U);
    axk::detail::PreparedMediaImage target_prepared;
    target_prepared.manifest.schema_version = "1.0";
    target_prepared.manifest.format = axk::MediaImageFormat::fat12_floppy;
    target_prepared.objects.push_back({seed_objects->front().decoded.header.type,
                                       seed_objects->front().decoded.header.name, seed_objects->front().raw_payload});
    const std::vector retained_file_payload{std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
                                            std::byte{'a'}, std::byte{'i'}, std::byte{'n'}};
    target_prepared.retained_files.push_back({"README.TXT", retained_file_payload});
    ASSERT_TRUE(axk::detail::write_prepared_media_image(target_prepared, target_path, false, {}));

    auto target = axk::open_media(target_path);
    ASSERT_TRUE(target) << target.error().message;
    const auto target_objects = target->objects();
    ASSERT_TRUE(target_objects) << target_objects.error().message;
    ASSERT_EQ(target_objects->size(), 1U);
    const auto retained_payload = target_objects->front().raw_payload;
    const auto target_profile = axk::package_internal::build_relocation_profile(target_objects->front().decoded,
                                                                                target_objects->front().raw_payload);
    ASSERT_TRUE(target_profile) << target_profile.error().message;
    ASSERT_EQ((*packages)[1].nodes.size(), 1U);
    EXPECT_EQ((*packages)[1].nodes.front().normalized_sha256,
              axk::package_internal::hex_digest(axk::package_internal::sha256(target_profile->normalized_payload)));

    axk::PackageImportRequest request;
    request.root_destinations = {destination(0U, "FAT root"), destination(1U, "FAT root"), destination(2U, "FAT root")};
    const auto plan = axk::plan_package_import(target_path, *packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflict_summary(*plan);
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->objects.size(), 8U);
    ASSERT_EQ(plan->allocation.size(), 1U);
    EXPECT_EQ(plan->allocation.front().inserted_object_count, 7U);
    EXPECT_EQ(plan->allocation.front().reused_object_count, 1U);
    EXPECT_EQ(plan->allocation.front().payload_clusters, 11U);
    EXPECT_EQ(plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().directory_growth_bytes, 224U);
    ASSERT_TRUE(axk::apply_package_import(target_path, *packages, *plan, first_path));

    auto first = axk::open_media(first_path);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->validation_issues().empty());
    const auto first_signature = normalized_media_signature(*first);
    ASSERT_TRUE(first_signature) << first_signature.error().message;
    EXPECT_EQ(first_signature->size(), 8U);
    expect_package_audio(*first, *packages);
    const auto first_objects = first->objects();
    ASSERT_TRUE(first_objects) << first_objects.error().message;
    EXPECT_TRUE(std::ranges::any_of(*first_objects, [&](const auto &object) {
        return object.decoded.header.raw_type == "SMPL" && object.decoded.header.name == "TEST" &&
               object.raw_payload == retained_payload;
    }));
    const auto &first_fat = std::get<axk::FatImage>(first->storage());
    const auto retained_file = std::ranges::find(first_fat.files(), std::string{"README.TXT"}, &axk::FatFile::path);
    ASSERT_NE(retained_file, first_fat.files().end());
    const auto retained_file_bytes = first_fat.read_file(*retained_file);
    ASSERT_TRUE(retained_file_bytes) << retained_file_bytes.error().message;
    EXPECT_EQ(*retained_file_bytes, retained_file_payload);

    std::vector<axk::PortablePackage> reversed_packages{(*packages)[2], (*packages)[1], (*packages)[0]};
    axk::PackageImportRequest reversed_request;
    reversed_request.root_destinations = {destination(0U, "FAT root"), destination(1U, "FAT root"),
                                          destination(2U, "FAT root")};
    const auto reversed_plan = axk::plan_package_import(target_path, reversed_packages, reversed_request);
    ASSERT_TRUE(reversed_plan) << reversed_plan.error().message;
    ASSERT_TRUE(reversed_plan->valid());
    ASSERT_EQ(reversed_plan->allocation.size(), 1U);
    EXPECT_EQ(reversed_plan->allocation.front().inserted_object_count, 7U);
    EXPECT_EQ(reversed_plan->allocation.front().reused_object_count, 1U);
    ASSERT_TRUE(axk::apply_package_import(target_path, reversed_packages, *reversed_plan, reversed_path));
    EXPECT_EQ(read_file(first_path), read_file(reversed_path));

    const auto repeat_plan = axk::plan_package_import(first_path, *packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    ASSERT_EQ(repeat_plan->allocation.size(), 1U);
    EXPECT_EQ(repeat_plan->allocation.front().inserted_object_count, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().reused_object_count, 8U);
    EXPECT_EQ(repeat_plan->allocation.front().payload_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().directory_growth_bytes, 0U);
    ASSERT_TRUE(axk::apply_package_import(first_path, *packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(first_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, RebuildsFat12GraphAndRepeatsByteIdentically) {
    const auto output_root = publication_root("axklib-package-import-fat12");
    const auto target_path = output_root / "target.ima";
    const auto output_path = output_root / "imported.ima";
    const auto repeated_path = output_root / "repeated.ima";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    const auto target_bytes = fat_fixture();
    ASSERT_TRUE(write_file(target_path, target_bytes));

    auto target_before = axk::open_media(target_path);
    ASSERT_TRUE(target_before) << target_before.error().message;
    const auto objects_before = target_before->objects();
    ASSERT_TRUE(objects_before) << objects_before.error().message;
    ASSERT_EQ(objects_before->size(), 1U);
    const auto retained_payload = objects_before->front().raw_payload;

    const auto source_path = fixture("HD00_512_single_sbnk_authored.hds");
    auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave")};
    const auto built = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package, built->package};
    axk::PackageImportRequest request;
    request.root_destinations = {destination(0U, 0U, "FAT root"), destination(1U, 0U, "FAT root")};

    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->objects.size(), 4U);
    EXPECT_EQ(std::ranges::count_if(plan->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              2);
    EXPECT_EQ(std::ranges::count_if(plan->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              2);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled_path = output_root / "cancelled.ima";
    const auto cancelled =
        axk::apply_package_import(target_path, packages, *plan, cancelled_path, false, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_path));
    ASSERT_TRUE(axk::apply_package_import(target_path, packages, *plan, output_path));

    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->kind(), axk::MediaKind::fat12_floppy);
    const auto media_objects = reopened->objects();
    ASSERT_TRUE(media_objects) << media_objects.error().message;
    EXPECT_TRUE(std::ranges::any_of(*media_objects, [&](const auto &object) {
        return object.decoded.header.name == "TEST" && object.raw_payload == retained_payload;
    }));
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto bank = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.raw_type == "SBNK" && object.object.header.name == "sine wave";
    });
    ASSERT_NE(bank, catalog->objects.end());
    const auto graph = axk::build_relationship_graph(*catalog);
    EXPECT_TRUE(std::ranges::any_of(graph.children(bank->key), [](const axk::Relationship *edge) {
        return edge->type == "SBNK_LEFT_MEMBER_TO_SMPL" && edge->quality == axk::RelationshipQuality::known &&
               edge->target_key.has_value();
    }));

    const auto repeat_plan = axk::plan_package_import(output_path, packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    ASSERT_TRUE(axk::apply_package_import(output_path, packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(output_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageRegressionMatrix, MixesSfsFat12AndIsoSourcesIntoIso9660Deterministically) {
    const auto output_root = publication_root("axklib-package-mixed-iso9660-matrix");
    const auto target_audio = output_root / "target.wav";
    const auto target_path = output_root / "target.iso";
    const auto first_path = output_root / "first.iso";
    const auto reversed_path = output_root / "reversed.iso";
    const auto repeated_path = output_root / "repeated.iso";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    auto packages = mixed_source_packages(output_root);
    ASSERT_TRUE(packages) << packages.error().message;
    ASSERT_EQ(packages->size(), 3U);
    ASSERT_TRUE(axk::write_wav_atomic(target_audio, tiny_waveform(3000)));

    axk::MediaBuildManifest target_manifest;
    target_manifest.schema_version = "1.0";
    target_manifest.format = axk::MediaImageFormat::iso9660;
    target_manifest.iso_volume_id = "MIXED_TARGET";
    target_manifest.group_name = "Target Group";
    target_manifest.raw_group = "GROUP";
    target_manifest.volume_name = "Target Volume";
    target_manifest.raw_volume = "F001";
    target_manifest.authored_volume = single_bank_volume(target_audio, "Target Volume", "Target Wave", "Target Bank");
    auto prepared = axk::detail::prepare_media_image(target_manifest, {});
    ASSERT_TRUE(prepared) << prepared.error().message;
    const std::vector retained_payload{std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
                                       std::byte{'a'}, std::byte{'i'}, std::byte{'n'}};
    prepared->retained_files.push_back({"README", retained_payload});
    ASSERT_TRUE(axk::detail::write_prepared_media_image(*prepared, target_path, false, {}));

    auto target = axk::open_media(target_path);
    ASSERT_TRUE(target) << target.error().message;
    const auto target_objects = target->objects();
    ASSERT_TRUE(target_objects) << target_objects.error().message;
    ASSERT_EQ(target_objects->size(), 2U);
    std::map<std::pair<std::string, std::string>, std::vector<std::byte>> retained_objects;
    for (const auto &object : *target_objects) {
        retained_objects.emplace(std::pair{object.decoded.header.raw_type, object.decoded.header.name},
                                 object.raw_payload);
    }

    axk::PackageImportRequest request;
    for (std::size_t package_index = 0U; package_index < packages->size(); ++package_index) {
        auto target_destination = destination(package_index, 0U, "Target Volume");
        target_destination.group_name = "Target Group";
        target_destination.raw_group = "GROUP";
        target_destination.raw_volume = "F001";
        request.root_destinations.push_back(std::move(target_destination));
    }
    const auto plan = axk::plan_package_import(target_path, *packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid()) << conflict_summary(*plan);
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->objects.size(), 8U);
    ASSERT_EQ(plan->allocation.size(), 1U);
    EXPECT_EQ(plan->allocation.front().inserted_object_count, 8U);
    EXPECT_EQ(plan->allocation.front().reused_object_count, 0U);
    EXPECT_EQ(plan->allocation.front().payload_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().payload_sectors, 8U);
    EXPECT_EQ(plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(plan->allocation.front().directory_growth_bytes, 256U);
    EXPECT_TRUE(std::ranges::all_of(plan->objects, [](const auto &object) {
        return object.payload_clusters == 0U && object.payload_sectors == 1U && object.continuation_clusters == 0U;
    }));
    EXPECT_GT(plan->allocation.front().projected_image_sectors, 20U);
    EXPECT_EQ(plan->allocation.front().projected_image_size_bytes,
              plan->allocation.front().projected_image_sectors * 2048U);
    const auto report = axk::apply_package_import(target_path, *packages, *plan, first_path);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(std::filesystem::file_size(first_path), plan->allocation.front().projected_image_size_bytes);
    expect_external_iso_tools_accept(first_path);

    auto first = axk::open_media(first_path);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->validation_issues().empty());
    const auto first_signature = normalized_media_signature(*first);
    ASSERT_TRUE(first_signature) << first_signature.error().message;
    EXPECT_EQ(first_signature->size(), 10U);
    expect_package_audio(*first, *packages);
    const auto first_objects = first->objects();
    ASSERT_TRUE(first_objects) << first_objects.error().message;
    for (const auto &[identity, payload] : retained_objects) {
        EXPECT_TRUE(std::ranges::any_of(*first_objects,
                                        [&](const auto &object) {
                                            return object.decoded.header.raw_type == identity.first &&
                                                   object.decoded.header.name == identity.second &&
                                                   object.raw_payload == payload;
                                        }))
            << identity.first << ' ' << identity.second;
    }
    const auto &first_iso = std::get<axk::IsoImage>(first->storage());
    const auto retained = std::ranges::find(first_iso.files(), std::string{"README"}, &axk::IsoFile::path);
    ASSERT_NE(retained, first_iso.files().end());
    const auto retained_bytes = first_iso.read_file(*retained);
    ASSERT_TRUE(retained_bytes) << retained_bytes.error().message;
    EXPECT_EQ(*retained_bytes, retained_payload);

    std::vector<axk::PortablePackage> reversed_packages{(*packages)[2], (*packages)[1], (*packages)[0]};
    axk::PackageImportRequest reversed_request;
    for (std::size_t package_index = 0U; package_index < reversed_packages.size(); ++package_index) {
        auto target_destination = destination(package_index, 0U, "Target Volume");
        target_destination.group_name = "Target Group";
        target_destination.raw_group = "GROUP";
        target_destination.raw_volume = "F001";
        reversed_request.root_destinations.push_back(std::move(target_destination));
    }
    const auto reversed_plan = axk::plan_package_import(target_path, reversed_packages, reversed_request);
    ASSERT_TRUE(reversed_plan) << reversed_plan.error().message;
    ASSERT_TRUE(reversed_plan->valid()) << conflict_summary(*reversed_plan);
    ASSERT_EQ(reversed_plan->allocation.size(), 1U);
    EXPECT_EQ(reversed_plan->allocation.front().projected_image_sectors,
              plan->allocation.front().projected_image_sectors);
    EXPECT_EQ(reversed_plan->allocation.front().projected_image_size_bytes,
              plan->allocation.front().projected_image_size_bytes);
    ASSERT_TRUE(axk::apply_package_import(target_path, reversed_packages, *reversed_plan, reversed_path));
    EXPECT_EQ(read_file(first_path), read_file(reversed_path));

    const auto repeat_plan = axk::plan_package_import(first_path, *packages, request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid()) << conflict_summary(*repeat_plan);
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::relocate);
    }));
    ASSERT_EQ(repeat_plan->allocation.size(), 1U);
    EXPECT_EQ(repeat_plan->allocation.front().inserted_object_count, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().reused_object_count, 8U);
    EXPECT_EQ(repeat_plan->allocation.front().payload_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().payload_sectors, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().continuation_clusters, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().directory_growth_bytes, 0U);
    EXPECT_EQ(repeat_plan->allocation.front().projected_image_sectors,
              plan->allocation.front().projected_image_sectors);
    EXPECT_EQ(repeat_plan->allocation.front().projected_image_size_bytes, std::filesystem::file_size(first_path));
    ASSERT_TRUE(axk::apply_package_import(first_path, *packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(first_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}

TEST(PackageImportApply, RebuildsIsoWithVolumeLocalReuseAndByteIdenticalRepeat) {
    const auto output_root = publication_root("axklib-package-import-iso9660");
    const auto audio_path = output_root / "tone.wav";
    const auto target_path = output_root / "target.iso";
    const auto output_path = output_root / "imported.iso";
    const auto repeated_path = output_root / "repeated.iso";
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    axk::MediaBuildManifest target_manifest;
    target_manifest.schema_version = "1.0";
    target_manifest.format = axk::MediaImageFormat::iso9660;
    target_manifest.iso_volume_id = "PACKAGE_TEST";
    target_manifest.group_name = "Target Group";
    target_manifest.raw_group = "GROUP";
    target_manifest.volume_name = "Target Volume";
    target_manifest.raw_volume = "F001";
    target_manifest.authored_volume = graph_volume(audio_path);
    auto target_prepared = axk::detail::prepare_media_image(target_manifest, {});
    ASSERT_TRUE(target_prepared) << target_prepared.error().message;
    const std::vector retained_payload{std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
                                       std::byte{'a'}, std::byte{'i'}, std::byte{'n'}};
    target_prepared->retained_files.push_back({"README", retained_payload});
    const auto target_written = axk::detail::write_prepared_media_image(*target_prepared, target_path, false, {});
    ASSERT_TRUE(target_written) << target_written.error().message;

    auto source = axk::open_media(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(source) << source.error().message;
    const std::vector bank_root{root(axk::PackageRootKind::sbnk, "New Volume", "sine wave")};
    const auto built = axk::build_portable_package(*source, bank_root);
    ASSERT_TRUE(built) << built.error().message;
    const std::vector packages{built->package, built->package};
    axk::PackageImportRequest request;
    auto first = destination(0U, 0U, "Target Volume");
    first.group_name = "Target Group";
    first.raw_group = "GROUP";
    first.raw_volume = "F001";
    auto second = destination(1U, 0U, "Second Volume");
    second.group_name = "Target Group";
    second.raw_group = "GROUP";
    second.create_destination = true;
    request.root_destinations = {std::move(first), std::move(second)};

    auto shared_creation_request = request;
    shared_creation_request.root_destinations[0].volume_name = "Second Volume";
    shared_creation_request.root_destinations[0].raw_volume = "F002";
    shared_creation_request.root_destinations[0].create_destination = true;
    shared_creation_request.root_destinations[1].raw_volume = "F002";
    const auto shared_creation = axk::plan_package_import(target_path, packages, shared_creation_request);
    ASSERT_TRUE(shared_creation) << shared_creation.error().message;
    ASSERT_TRUE(shared_creation->valid());
    ASSERT_EQ(shared_creation->destinations.size(), 1U);
    EXPECT_TRUE(shared_creation->destinations.front().create);
    EXPECT_EQ(std::ranges::count_if(shared_creation->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::insert);
                                    }),
              2);
    EXPECT_EQ(std::ranges::count_if(shared_creation->objects,
                                    [](const auto &object) {
                                        return std::ranges::contains(object.actions,
                                                                     axk::PackageImportObjectAction::reuse);
                                    }),
              2);

    const auto plan = axk::plan_package_import(target_path, packages, request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->valid());
    ASSERT_TRUE(axk::verify_package_import_plan(*plan));
    ASSERT_EQ(plan->destinations.size(), 2U);
    EXPECT_EQ(plan->destinations[0].raw_volume, "F001");
    EXPECT_EQ(plan->destinations[1].raw_volume, "F002");
    EXPECT_TRUE(plan->destinations[1].create);
    ASSERT_EQ(plan->objects.size(), 4U);
    EXPECT_TRUE(std::ranges::all_of(plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert);
    }));
    EXPECT_TRUE(std::ranges::any_of(
        plan->warnings, [](const auto &warning) { return warning.code == "ISO9660_CROSS_VOLUME_DUPLICATE"; }));

    auto conflicting_request = request;
    const auto sample_node =
        std::ranges::find_if(packages[0].nodes, [](const auto &node) { return node.object_type == "SMPL"; });
    ASSERT_NE(sample_node, packages[0].nodes.end());
    conflicting_request.policy.renames.push_back({0U, sample_node->node_id, "Graph Wave"});
    const auto conflicting = axk::plan_package_import(target_path, packages, conflicting_request);
    ASSERT_TRUE(conflicting) << conflicting.error().message;
    EXPECT_FALSE(conflicting->valid());
    EXPECT_TRUE(std::ranges::any_of(conflicting->conflicts,
                                    [](const auto &conflict) { return conflict.code == "ISO9660_NAME_CONFLICT"; }));

    auto invalid_creation_request = request;
    invalid_creation_request.root_destinations[1].raw_volume = "F003";
    const auto invalid_creation = axk::plan_package_import(target_path, packages, invalid_creation_request);
    ASSERT_TRUE(invalid_creation) << invalid_creation.error().message;
    EXPECT_FALSE(invalid_creation->valid());
    EXPECT_TRUE(std::ranges::any_of(invalid_creation->conflicts, [](const auto &conflict) {
        return conflict.code == "ISO9660_RAW_VOLUME_ALLOCATION_INVALID";
    }));

    axk::CancellationSource cancellation;
    CancellingPackageProgress cancel_after_first{cancellation, 1U};
    const auto cancelled_path = output_root / "cancelled.iso";
    const auto cancelled = axk::apply_package_import(target_path, packages, *plan, cancelled_path, false,
                                                     cancellation.token(), &cancel_after_first);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_path));
    ASSERT_TRUE(axk::apply_package_import(target_path, packages, *plan, output_path));
    expect_external_iso_tools_accept(output_path);

    auto reopened = axk::open_media(output_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(reopened->validation_issues().empty());
    const auto objects = reopened->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    for (const auto raw_volume : {std::string_view{"F001"}, std::string_view{"F002"}}) {
        EXPECT_TRUE(std::ranges::any_of(*objects, [&](const auto &object) {
            return object.raw_group == "GROUP" && object.raw_volume == raw_volume &&
                   object.decoded.header.type == axk::ObjectType::sbnk && object.decoded.header.name == "sine wave";
        }));
        EXPECT_TRUE(std::ranges::any_of(*objects, [&](const auto &object) {
            return object.raw_group == "GROUP" && object.raw_volume == raw_volume &&
                   object.decoded.header.type == axk::ObjectType::smpl;
        }));
    }
    EXPECT_TRUE(std::ranges::any_of(*objects, [](const auto &object) {
        return object.raw_volume == "F001" && object.decoded.header.name == "Graph Wave";
    }));
    const auto &reopened_iso = std::get<axk::IsoImage>(reopened->storage());
    const auto retained = std::ranges::find(reopened_iso.files(), std::string{"README"}, &axk::IsoFile::path);
    ASSERT_NE(retained, reopened_iso.files().end());
    const auto retained_bytes = reopened_iso.read_file(*retained);
    ASSERT_TRUE(retained_bytes) << retained_bytes.error().message;
    EXPECT_EQ(*retained_bytes, retained_payload);

    auto repeat_request = request;
    repeat_request.root_destinations[1].raw_volume = "F002";
    repeat_request.root_destinations[1].create_destination = false;
    const auto repeat_plan = axk::plan_package_import(output_path, packages, repeat_request);
    ASSERT_TRUE(repeat_plan) << repeat_plan.error().message;
    ASSERT_TRUE(repeat_plan->valid());
    EXPECT_TRUE(std::ranges::all_of(repeat_plan->objects, [](const auto &object) {
        return std::ranges::contains(object.actions, axk::PackageImportObjectAction::reuse) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::insert) &&
               !std::ranges::contains(object.actions, axk::PackageImportObjectAction::relocate);
    }));
    ASSERT_TRUE(axk::apply_package_import(output_path, packages, *repeat_plan, repeated_path, false));
    EXPECT_EQ(read_file(output_path), read_file(repeated_path));
    std::filesystem::remove_all(output_root, error);
}
