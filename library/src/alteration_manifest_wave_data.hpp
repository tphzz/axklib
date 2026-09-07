#pragma once

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {
Result<WaveDataParameters> parse_wave_data_parameters_json(const nlohmann::json &value);
Result<void> validate_sample_retarget(const RetargetSampleWaveDataOperation &operation);
Result<RetargetSampleWaveDataOperation> parse_sample_retarget_json(const nlohmann::json &value,
                                                                   PartitionSelector selector);
} // namespace axk::detail
