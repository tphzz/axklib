#include "sequence_diagnostics.hpp"

#include <format>
#include <string>

namespace axk {

Error sequence_channel_size_error(std::uint8_t status, std::size_t expected, std::size_t observed) {
    return make_error(ErrorCode::object_malformed, ErrorCategory::object,
                      std::format("native Sequence channel event status 0x{:02X} expects {} bytes but observed {}",
                                  status, expected, observed));
}

Error sequence_event_error(Error error, std::string_view name, std::size_t offset, std::size_t block_index,
                           std::size_t event_index, std::uint32_t tick) {
    const auto object_name = name.empty() ? std::string{"<unnamed>"} : std::string{name};
    error.message = std::format("Sequence '{}' event at object offset 0x{:x} (block {}, event {}, tick {}): {}",
                                object_name, offset, block_index, event_index, tick, error.message);
    error.context.object_type = "SEQU";
    error.context.object_name = object_name;
    error.context.raw_offset = offset;
    return error;
}

} // namespace axk
