#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "axklib/io.hpp"
#include "axklib/sfs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size > 4U * 1024U * 1024U)
    return 0;
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index)
    bytes[index] = static_cast<std::byte>(data[index]);
  auto reader = std::make_shared<axk::MemoryReader>(std::move(bytes));
  static_cast<void>(axk::open_image(std::move(reader), "fuzz.hds"));
  if (size >= 12U) {
    const auto value = [](const std::uint8_t *source) {
      return (static_cast<std::uint32_t>(source[0]) << 24U) |
             (static_cast<std::uint32_t>(source[1]) << 16U) |
             (static_cast<std::uint32_t>(source[2]) << 8U) | source[3];
    };
    static_cast<void>(axk::calculate_sfs_free_space(value(data), value(data + 4U),
                                                     value(data + 8U)));
  }
  return 0;
}
