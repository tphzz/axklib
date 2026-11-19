#pragma once
#include "axklib/conversion/plan.hpp"
#include "axklib/formats/a_series/authoring/bundle.hpp"
#include "axklib/formats/typhoon/document.hpp"
#include <map>
#include <set>

namespace axk::conversion::typhoon_to_a_series {
using Ids = std::set<std::string, std::less<>>;
using Waves = std::map<std::string, WaveformSpec, std::less<>>;
using Notice = std::function<void(Impact, std::string, std::string, std::string)>;
void select_instruments(const typhoon::Document &, bool subset, Ids &, Report &, const Notice &);
void map_instruments(const typhoon::Document &, const Ids &, const Waves &, const a_series::authoring::Inventory &,
                     a_series::authoring::Bundle &, Report &, const Notice &);
} // namespace axk::conversion::typhoon_to_a_series
