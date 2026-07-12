#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "axklib/media.hpp"

namespace {

std::shared_ptr<axk::MemoryReader> reader(const std::uint8_t *data, std::size_t size) {
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index) {
    bytes[index] = static_cast<std::byte>(data[index]);
  }
  return std::make_shared<axk::MemoryReader>(std::move(bytes));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  constexpr std::size_t maximum_input = 4U * 1024U * 1024U;
  if (data == nullptr || size > maximum_input)
    return 0;

  if (auto fat = axk::FatImage::open(reader(data, size), "fuzz.ima"); fat) {
    static_cast<void>(fat->objects(maximum_input));
  }
  if (auto iso = axk::IsoImage::open(reader(data, size), "fuzz.iso"); iso) {
    static_cast<void>(iso->objects(maximum_input));
  }
  static_cast<void>(axk::StandaloneObject::open(reader(data, size), "fuzz.obj", maximum_input));
  return 0;
}
