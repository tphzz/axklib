#pragma once

#include "axklib/conversion/plan.hpp"

namespace axk::conversion::tx16w_wave_disk {
[[nodiscard]] WaveWriterAdapter writer();
[[nodiscard]] WaveWriterAdapter set_writer();
} // namespace axk::conversion::tx16w_wave_disk
