#pragma once

#include "axklib/catalog.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>

namespace axk::app::detail {
nlohmann::json a_series_sample_editor(const ObjectSnapshot &snapshot, std::span<const std::byte> bytes, bool writable,
                                      const nlohmann::json &sources);
}
