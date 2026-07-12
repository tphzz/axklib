#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

  const auto object_seed = seed("object/truncated.seed");
  EXPECT_FALSE(axk::decode_object(object_seed).has_value());

  auto sfs_seed = seed("sfs/truncated.seed");
  auto sfs_reader = std::make_shared<axk::MemoryReader>(std::move(sfs_seed));
  EXPECT_FALSE(axk::open_image(std::move(sfs_reader), "seed.hds").has_value());

  const auto media_seed = seed("media/truncated.seed");
  auto media_reader = std::make_shared<axk::MemoryReader>(media_seed);
  EXPECT_FALSE(axk::FatImage::open(media_reader, "seed.ima").has_value());
  EXPECT_FALSE(axk::IsoImage::open(media_reader, "seed.iso").has_value());

  const auto malformed = seed("manifest/malformed.seed");
  const std::string malformed_text{reinterpret_cast<const char *>(malformed.data()),
                                   malformed.size()};
  EXPECT_FALSE(axk::parse_hds_build_manifest(malformed_text).has_value());
  EXPECT_FALSE(axk::parse_alteration_manifest(malformed_text).has_value());
}

TEST(Hardening, EveryFocusedFuzzerSeedHasANormalTestReplay) {
  const auto corpus = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/fuzz/corpus";
  std::size_t replayed{};
  for (const auto &entry : std::filesystem::recursive_directory_iterator{corpus}) {
    if (!entry.is_regular_file() || entry.path().filename() == "README.md")
      continue;
    const auto relative = entry.path().lexically_relative(corpus);
    const auto target = (*relative.begin()).string();
    const auto bytes = seed(relative.generic_string());
    ASSERT_FALSE(bytes.empty()) << relative;
    ++replayed;

    if (target == "bytes") {
      const axk::ByteReader reader{bytes};
      static_cast<void>(reader.be32(0U));
    } else if (target.starts_with("sfs")) {
      auto reader = std::make_shared<axk::MemoryReader>(bytes);
      static_cast<void>(axk::open_image(std::move(reader), "seed.hds"));
    } else if (target.starts_with("object")) {
      static_cast<void>(axk::decode_object_header(bytes));
      static_cast<void>(axk::decode_object(bytes));
    } else if (target == "fat_record") {
      static_cast<void>(
          axk::FatImage::open(std::make_shared<axk::MemoryReader>(bytes), "seed.ima"));
    } else if (target == "iso_record") {
      static_cast<void>(
          axk::IsoImage::open(std::make_shared<axk::MemoryReader>(bytes), "seed.iso"));
    } else {
      const std::string json{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
      if (target == "build_manifest" || target == "manifest")
        static_cast<void>(axk::parse_hds_build_manifest(json));
      if (target == "transaction" || target == "manifest")
        static_cast<void>(axk::parse_alteration_manifest(json));
    }
  }
  EXPECT_GE(replayed, 20U);
}
