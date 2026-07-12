#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fuzz_envelopes.hpp"

#include "axklib/alteration.hpp"
#include "axklib/bytes.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

std::vector<std::byte> seed(std::string_view relative) {
  const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/fuzz/corpus" / relative;
  std::ifstream stream{path, std::ios::binary};
  const std::string text{std::istreambuf_iterator<char>{stream}, {}};
  std::vector<std::byte> bytes(text.size());
  for (std::size_t index = 0; index < text.size(); ++index)
    bytes[index] = static_cast<std::byte>(text[index]);
  return bytes;
}

} // namespace

TEST(Hardening, ReplaysEveryMaintainedMalformedSeedWithoutPartialSuccess) {
  const auto byte_seed = seed("bytes/short.seed");
  const axk::ByteReader bytes{byte_seed};
  EXPECT_FALSE(bytes.be32(1U).has_value());

  const auto object_seed = seed("object_header/truncated.seed");
  EXPECT_FALSE(axk::decode_object(object_seed).has_value());

  auto sfs_seed = seed("sfs_image/truncated.seed");
  auto sfs_reader = std::make_shared<axk::MemoryReader>(std::move(sfs_seed));
  EXPECT_FALSE(axk::open_image(std::move(sfs_reader), "seed.hds").has_value());

  const std::string malformed_text{"{"};
  EXPECT_FALSE(axk::parse_hds_build_manifest(malformed_text).has_value());
  EXPECT_FALSE(axk::parse_alteration_manifest(malformed_text).has_value());
}

TEST(Hardening, EveryFocusedFuzzerSeedHasANormalTestReplay) {
  const auto corpus = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/fuzz/corpus";
  const auto fuzz_root = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/fuzz";
  std::ifstream manifest_stream{fuzz_root / "harnesses.json"};
  ASSERT_TRUE(manifest_stream);
  const auto manifest = nlohmann::json::parse(manifest_stream);
  std::ifstream cmake_stream{std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/CMakeLists.txt"};
  const std::string cmake{std::istreambuf_iterator<char>{cmake_stream}, {}};
  std::set<std::string> registered_sources;
  for (const auto& support : manifest.at("support_sources")) {
    const auto source = support.get<std::string>();
    registered_sources.insert(source);
    EXPECT_NE(cmake.find("add_executable(axk_sfs_seed_builder fuzz/" + source + ")"),
              std::string::npos)
        << source;
  }
  std::size_t replayed{};
  for (const auto& harness : manifest.at("harnesses")) {
    const auto target = harness.at("target").get<std::string>();
    const auto source = harness.at("source").get<std::string>();
    const auto corpus_directory = corpus / harness.at("corpus").get<std::string>();
    registered_sources.insert(source);
    const auto directly_registered =
        cmake.find("axk_add_fuzzer(" + target + " " + source + ")") != std::string::npos;
    const auto typed_registered =
        source == "typed_object_fuzz.cpp" &&
        cmake.find("axk_add_fuzzer(object_${object_type} typed_object_fuzz.cpp)") !=
            std::string::npos &&
        cmake.find(target.substr(std::string{"object_"}.size())) != std::string::npos;
    EXPECT_TRUE(directly_registered || typed_registered) << target;
    ASSERT_TRUE(std::filesystem::is_directory(corpus_directory)) << target;
    std::size_t target_seed_count{};
    for (const auto& entry : std::filesystem::directory_iterator{corpus_directory}) {
      if (!entry.is_regular_file() || entry.path().extension() != ".seed")
        continue;
      const auto relative = entry.path().lexically_relative(corpus);
      const auto bytes = seed(relative.generic_string());
      ASSERT_FALSE(bytes.empty()) << relative;
      ++target_seed_count;
      ++replayed;

      if (target == "bytes") {
        const axk::ByteReader reader{bytes};
        static_cast<void>(reader.be32(0U));
      } else if (target == "sfs_image") {
        auto reader = std::make_shared<axk::MemoryReader>(bytes);
        static_cast<void>(axk::open_image(std::move(reader), "seed.hds"));
      } else if (target == "object_header") {
        static_cast<void>(axk::decode_object_header(bytes));
      } else if (target.starts_with("object_")) {
        auto type = target.substr(std::string{"object_"}.size());
        std::ranges::transform(type, type.begin(), [](unsigned char value) {
          return static_cast<char>(std::toupper(value));
        });
        const auto envelope = axk::fuzz::typed_object_envelope(bytes, type);
        static_cast<void>(axk::decode_object(envelope));
      } else if (target == "fat_record") {
        auto envelope = axk::fuzz::fat_record_envelope(bytes);
        static_cast<void>(axk::FatImage::open(
            std::make_shared<axk::MemoryReader>(std::move(envelope)), "seed.ima"));
      } else if (target == "iso_record") {
        auto envelope = axk::fuzz::iso_record_envelope(bytes);
        static_cast<void>(axk::IsoImage::open(
            std::make_shared<axk::MemoryReader>(std::move(envelope)), "seed.iso"));
      } else {
        const std::string json{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        if (target == "build_manifest")
          static_cast<void>(axk::parse_hds_build_manifest(json));
        else
          static_cast<void>(axk::parse_alteration_manifest(json));
      }
    }
    EXPECT_GT(target_seed_count, 0U) << target;
  }
  std::set<std::string> source_files;
  for (const auto& entry : std::filesystem::directory_iterator{fuzz_root}) {
    if (entry.is_regular_file() && entry.path().extension() == ".cpp")
      source_files.insert(entry.path().filename().string());
  }
  EXPECT_EQ(source_files, registered_sources);
  EXPECT_GE(replayed, manifest.at("harnesses").size());
}
