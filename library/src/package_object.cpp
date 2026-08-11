#include "axklib/package_relocation.hpp"

#include <utility>
#include <variant>

namespace axk::package_internal {

bool is_opaque_sequence(const DecodedObject &object) {
    return object.header.type == ObjectType::sequ && object.format == ObjectFormat::unknown &&
           std::holds_alternative<GenericObject>(object.payload);
}

Result<DecodedObject> decode_package_object(std::span<const std::byte> payload) {
    auto decoded = decode_object(payload);
    if (decoded)
        return decoded;
    const auto semantic_error = decoded.error();
    auto header = decode_object_header(payload);
    if (!header || header->type != ObjectType::sequ)
        return std::unexpected{semantic_error};
    return DecodedObject{std::move(*header), ObjectFormat::unknown, GenericObject{{payload.begin(), payload.end()}}};
}

} // namespace axk::package_internal
