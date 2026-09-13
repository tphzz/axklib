#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

#include "axklib/bytes.hpp"
#include "system_recording_fields.hpp"

namespace axk {
namespace {

Error invalid(const std::string &message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message);
}

Result<void> apply_fields(std::span<std::byte> bytes, const SystemRecordingParameters &patch, bool native) {
    if (native && patch.ad_input_gain)
        return std::unexpected{invalid("A3000 has no saved A/D input gain setting")};
    Result<void> result;
    ByteWriter writer{bytes};
    detail::visit_system_recording_fields(native, [&]<typename T>(std::size_t offset,
                                                                  std::optional<T> SystemRecordingParameters::*member,
                                                                  int minimum, int maximum, const char *name) {
        const auto &value = patch.*member;
        if (!result || !value)
            return;
        if (static_cast<int>(*value) < minimum || static_cast<int>(*value) > maximum) {
            result = std::unexpected{invalid(std::string{"Recording "} + name + " is out of range")};
            return;
        }
        if constexpr (std::is_same_v<T, std::uint16_t>)
            result = writer.write_be16(offset, *value);
        else
            result = writer.write_u8(offset, static_cast<std::uint8_t>(*value));
    });
    return result;
}

Result<void> input_dependencies(const SystemRecordingParameters &patch, const SystemRecordingParameters &values,
                                SystemRecordingParameters &dependent) {
    if (!patch.input && !patch.stereo && !patch.frequency_selection && !patch.monitor_output && !patch.monitor_level)
        return {};
    if (!values.input)
        return std::unexpected{invalid("Recording input is undecoded; supply it with the dependent settings")};
    if (*values.input <= 2U) {
        if (patch.input && (!values.stereo || !values.frequency_selection || !values.monitor_output))
            return std::unexpected{invalid("Recording input dependents are undecoded; supply their replacements")};
        return {};
    }
    if (patch.monitor_level)
        return std::unexpected{invalid("Recording monitor level cannot be edited with digital/optical input")};
    if (patch.input) {
        if (patch.stereo == false || (patch.frequency_selection && *patch.frequency_selection > 3U) ||
            (patch.monitor_output && *patch.monitor_output != 0U))
            return std::unexpected{invalid("Explicit recording settings conflict with digital/optical input")};
        if (!values.frequency_selection)
            return std::unexpected{invalid("Recording frequency is undecoded; supply its replacement")};
        dependent.stereo = true;
        dependent.frequency_selection = std::min<std::uint8_t>(*values.frequency_selection, 3U);
        dependent.monitor_output = 0;
    } else if (values.stereo != true || !values.frequency_selection || *values.frequency_selection > 3U ||
               values.monitor_output != 0U) {
        return std::unexpected{
            invalid("Digital/optical recording requires stereo, frequency 0..3 and monitor output 0")};
    }
    return {};
}

Result<void> map_dependencies(const SystemRecordingParameters &patch, const SystemRecordingParameters &values,
                              SystemRecordingParameters &dependent) {
    if (!patch.record_type && !patch.map_destination)
        return {};
    if (!values.record_type)
        return std::unexpected{invalid("Recording type is undecoded; supply it with map destination")};
    if (patch.map_destination && (*values.record_type == 0U || *values.record_type == 3U))
        return std::unexpected{invalid("Map destination cannot be edited in Replc or Save mode")};
    if (*values.record_type == 1U || *values.record_type == 2U) {
        if (!values.map_destination)
            return std::unexpected{invalid("Recording map destination is undecoded; supply its replacement")};
        if (*values.record_type == 1U && *values.map_destination > 1) {
            if (patch.map_destination)
                return std::unexpected{invalid("New recording mode requires map destination 0..1")};
            dependent.map_destination = 1;
        }
    }
    return {};
}

Result<void> key_dependencies(const SystemRecordingParameters &patch, const SystemRecordingParameters &values) {
    if (!patch.key_low && !patch.key_high && !patch.original_key)
        return {};
    if (!values.key_low || !values.key_high)
        return std::unexpected{invalid("Recording key endpoints are undecoded; supply their replacements")};
    int low = *values.key_low;
    int high = *values.key_high;
    if (low == -1 || high == 128) {
        if (!values.original_key)
            return std::unexpected{invalid("Recording original key is undecoded; supply it with sentinel endpoints")};
        if (low == -1)
            low = *values.original_key;
        if (high == 128)
            high = *values.original_key;
    }
    if (low > high)
        return std::unexpected{invalid("Recording effective low key exceeds high key; patch both endpoints together")};
    return {};
}

} // namespace

Result<DecodedSystemFile> patch_system_recording_configuration(const DecodedSystemFile &file,
                                                               const SystemRecordingParameters &patch,
                                                               ASeriesModel model) {
    const auto native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Recording target model does not match the System File")};
    if (const auto valid = encode_system_file(file); !valid)
        return std::unexpected{valid.error()};
    auto result = file;
    auto bytes = std::span{result.system_bulk_bytes}.subspan(native ? 0x188U : 0x398U, native ? 34U : 56U);
    if (const auto applied = apply_fields(bytes, patch, native); !applied)
        return std::unexpected{applied.error()};
    const auto decoded = decode_system_recording(result);
    if (!decoded)
        return std::unexpected{decoded.error()};
    SystemRecordingParameters dependent;
    if (const auto checked = input_dependencies(patch, decoded->parameters, dependent); !checked)
        return std::unexpected{checked.error()};
    if (const auto checked = map_dependencies(patch, decoded->parameters, dependent); !checked)
        return std::unexpected{checked.error()};
    if (const auto checked = key_dependencies(patch, decoded->parameters); !checked)
        return std::unexpected{checked.error()};
    if (const auto applied = apply_fields(bytes, dependent, native); !applied)
        return std::unexpected{applied.error()};
    return result;
}

} // namespace axk
