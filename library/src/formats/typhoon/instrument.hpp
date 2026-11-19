#pragma once
#include "axklib/formats/typhoon/document.hpp"
#include "iff.hpp"

namespace axk::typhoon {
[[nodiscard]] Result<void> instrument_layout(Item &item, iff::Bytes body);
}
