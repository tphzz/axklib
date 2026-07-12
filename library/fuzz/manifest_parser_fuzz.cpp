#include <cstddef>
#include <cstdint>
#include <string_view>

#include "axklib/alteration.hpp"
#include "axklib/writer.hpp"

#ifndef AXK_FUZZ_MANIFEST_KIND
#error "AXK_FUZZ_MANIFEST_KIND must identify build or transaction parsing"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size > 4U * 1024U * 1024U)
    return 0;
  const auto json = std::string_view{reinterpret_cast<const char *>(data), size};
  if constexpr (AXK_FUZZ_MANIFEST_KIND == 0)
    static_cast<void>(axk::parse_hds_build_manifest(json));
  else
    static_cast<void>(axk::parse_alteration_manifest(json));
  return 0;
}
