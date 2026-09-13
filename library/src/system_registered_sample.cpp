#include "axklib/system_file.hpp"

namespace axk {

Result<DecodedSampleParameters> decode_system_registered_sample(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Registered Sample System File kind is unsupported")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Registered Sample System File layout is invalid")};
    return decode_sample_parameter_block(
        std::span{file.system_bulk_bytes}.subspan(native ? 0x1c8U : 0x3ecU, native ? 0xbcU : 0xe0U),
        native ? SampleParameterGeneration::a3000 : SampleParameterGeneration::current);
}

} // namespace axk
